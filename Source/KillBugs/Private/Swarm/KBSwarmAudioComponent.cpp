#include "Swarm/KBSwarmAudioComponent.h"

#include "Audio/KBListenerLocation.h"
#include "Components/AudioComponent.h"
#include "Data/KBEnemyArchetype.h"
#include "Engine/World.h"
#include "KBConsoleVariables.h"
#include "KillBugs.h"
#include "Sound/SoundBase.h"
#include "Swarm/KBEnemyDirector.h"
#include "Swarm/KBEnemyVisualizerComponent.h"

UKBSwarmAudioComponent::UKBSwarmAudioComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// After the visualizer, which is what knows where the bugs are actually being drawn - and
	// the drawn position, not the replicated one, is what a voice has to follow or the sound
	// trails the picture by a network update.
	//
	// A whole group later rather than the same one: the visualizer runs in TG_DuringPhysics, and
	// within a group the order is registration order, which is exactly the kind of thing this
	// component should not silently depend on.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UKBSwarmAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The emitters are components of this actor, so they would be torn down with it anyway -
	// but a voice that is stopped explicitly is released immediately rather than whenever the
	// audio engine gets round to the component's destruction.
	UnbindAll();

	Super::EndPlay(EndPlayReason);
}

int32 UKBSwarmAudioComponent::GetActiveEmitterCount() const
{
	int32 Count = 0;
	for (const int32 StableId : BoundStableIds)
	{
		if (StableId != INDEX_NONE)
		{
			++Count;
		}
	}
	return Count;
}

void UKBSwarmAudioComponent::EnsureEmitters()
{
	if (bPoolInitialised)
	{
		return;
	}

	bPoolInitialised = true;

	if (MaxMoveEmitters <= 0)
	{
		// Loud, because otherwise the symptom is "bug sounds do not work" with no other clue.
		UE_LOG(LogKillBugs, Warning,
			TEXT("SwarmAudio: MaxMoveEmitters is 0, so no bug will ever be heard moving"));
		return;
	}

	Emitters.Reserve(MaxMoveEmitters);
	BoundStableIds.Reserve(MaxMoveEmitters);
	BoundSounds.Reserve(MaxMoveEmitters);
	BoundRadii.Reserve(MaxMoveEmitters);
	BoundDistances.Reserve(MaxMoveEmitters);
	BoundPitches.Reserve(MaxMoveEmitters);
	BoundVolumes.Reserve(MaxMoveEmitters);
	CurrentGains.Reserve(MaxMoveEmitters);
	TargetGains.Reserve(MaxMoveEmitters);

	for (int32 Index = 0; Index < MaxMoveEmitters; ++Index)
	{
		UAudioComponent* Emitter = NewObject<UAudioComponent>(GetOwner());
		Emitter->SetupAttachment(this);
		Emitter->SetMobility(EComponentMobility::Movable);

		// Not destroyed when a sound ends. The pool outlives every individual use of it, and a
		// component that destroyed itself would take its slot with it - leaving the pool to be
		// rebuilt, one component per spurt, which is the per-actor churn this project avoids
		// everywhere else.
		Emitter->bAutoDestroy = false;
		Emitter->bAllowSpatialization = true;

		// Positional, but with the ENGINE's distance falloff switched off and replaced by one
		// computed below. Two reasons:
		//
		//   * The project has no USoundAttenuation assets at all, so there is no shared curve to
		//     point at, and authoring one would become a prerequisite for hearing a single bug.
		//   * The ARCHETYPE knows how far its bug should carry, not the sound asset. The same
		//     sound reused on a Runner and a Brute should not reach the same distance, and with
		//     the falloff on the asset the only way to express that would be two assets.
		//
		// Spatialisation and attenuation are separate flags, so switching falloff off here does
		// NOT cost the left/right sense of where a bug is - which is most of what this component
		// exists to provide.
		Emitter->bOverrideAttenuation = true;
		Emitter->AttenuationOverrides.bAttenuate = false;
		Emitter->AttenuationOverrides.bSpatialize = true;

		Emitter->RegisterComponent();
		Emitter->SetVolumeMultiplier(0.f);

		Emitters.Add(Emitter);
		BoundStableIds.Add(INDEX_NONE);
		BoundSounds.Add(nullptr);
		BoundRadii.Add(1.f);
		BoundDistances.Add(0.f);
		BoundPitches.Add(1.f);
		BoundVolumes.Add(0.f);
		CurrentGains.Add(0.f);
		TargetGains.Add(0.f);
	}

	UE_LOG(LogKillBugs, Display, TEXT("SwarmAudio: %d move emitters pooled"), Emitters.Num());
}

void UKBSwarmAudioComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                           FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	AKBEnemyDirector* Director = Cast<AKBEnemyDirector>(GetOwner());
	if (!Director)
	{
		return;
	}

	// A dedicated server never finds a listener, so this is where the whole feature costs
	// nothing there - but the check is cheap enough to redo every update rather than latch.
	FVector Listener = FVector::ZeroVector;
	const bool bHasListener = KBListener::FindLocation(GetWorld(), Listener);

	// The pool is built on the first listener rather than up front: on a dedicated server there
	// is never one, and six audio components nobody can ever hear are six components that should
	// not have been created.
	if (bHasListener)
	{
		EnsureEmitters();
	}

	TimeSinceUpdate += DeltaTime;
	if (TimeSinceUpdate >= UpdateInterval)
	{
		TimeSinceUpdate = 0.f;

		if (bHasListener)
		{
			Rebind(Director, Listener);
		}
		else
		{
			// Unbind rather than merely stop ticking: a listen server whose last player left
			// should release its voices, not hold six of them playing to an empty room.
			UnbindAll();
		}
	}

	TickAudioLog(DeltaTime, Listener, bHasListener);

	if (Emitters.Num() == 0)
	{
		return;
	}

	// Runs every frame, listener or not - the fade is what makes an unbind silent rather than a
	// cut, and it has to finish after the last rebind.
	UpdateEmitters(DeltaTime, Director, Listener, bHasListener);
}

void UKBSwarmAudioComponent::TickAudioLog(float DeltaTime, const FVector& Listener, bool bHasListener)
{
	const float Interval = CVarKBSwarmAudioLog.GetValueOnGameThread();
	if (Interval <= 0.f)
	{
		TimeSinceAudioLog = 0.f;
		return;
	}

	TimeSinceAudioLog += DeltaTime;
	if (TimeSinceAudioLog < Interval)
	{
		return;
	}

	TimeSinceAudioLog = 0.f;

	if (!bHasListener)
	{
		// The single most useful line this can produce: on a dedicated server this is correct and
		// expected, and anywhere else it is the whole answer.
		UE_LOG(LogKillBugs, Display,
			TEXT("SwarmAudio: no local listener - nothing will be played. "
			     "(Expected on a dedicated server; a bug anywhere else.)"));
		return;
	}

	UE_LOG(LogKillBugs, Display,
		TEXT("SwarmAudio: listener=(%.0f,%.0f,%.0f) drawn=%d candidates=%d silent-archetype=%d ")
		TEXT("nearest-out-of-range=%s pooled=%d bound=%d"),
		Listener.X, Listener.Y, Listener.Z,
		DebugViewCount, DebugCandidateCount, DebugNoSoundCount,
		DebugNearestOutOfRange < 0.f
			? TEXT("none")
			: *FString::Printf(TEXT("%.0f"), DebugNearestOutOfRange),
		Emitters.Num(), GetActiveEmitterCount());

	// One line per slot. A slot that is bound with a target gain of nearly zero is the exact
	// shape of "there is sound assigned, the code is running, and it is still inaudible" - which
	// is what a hearing radius smaller than the camera height produces.
	for (int32 Slot = 0; Slot < Emitters.Num(); ++Slot)
	{
		const UAudioComponent* Emitter = Emitters[Slot].Get();
		if (BoundStableIds[Slot] == INDEX_NONE)
		{
			continue;
		}

		UE_LOG(LogKillBugs, Display,
			TEXT("SwarmAudio:   slot %d: bug=%d dist=%.0f radius=%.0f target=%.3f current=%.3f ")
			TEXT("sound=%s playing=%d"),
			Slot, BoundStableIds[Slot], BoundDistances[Slot], BoundRadii[Slot],
			TargetGains[Slot], CurrentGains[Slot],
			BoundSounds[Slot] ? *BoundSounds[Slot]->GetName() : TEXT("NONE"),
			Emitter && Emitter->IsPlaying() ? 1 : 0);
	}
}

void UKBSwarmAudioComponent::Rebind(AKBEnemyDirector* Director, const FVector& Listener)
{
	const UKBEnemyVisualizerComponent* Visualizer = Director->GetVisualizer();
	if (!Visualizer)
	{
		UnbindAll();
		return;
	}

	const TMap<int32, FKBEnemyView>& Views = Visualizer->GetViews();
	const TArray<TObjectPtr<UKBEnemyArchetype>>& Archetypes = Director->GetArchetypes();

	/** One bug that is close enough to be worth a voice. */
	struct FCandidate
	{
		int32 StableId = INDEX_NONE;
		int32 ArchetypeIndex = INDEX_NONE;
		float Distance = 0.f;
	};

	TArray<FCandidate> Candidates;
	Candidates.Reserve(Views.Num());

	DebugViewCount = Views.Num();
	DebugCandidateCount = 0;
	DebugNoSoundCount = 0;
	DebugNearestOutOfRange = -1.f;

	for (const TPair<int32, FKBEnemyView>& Pair : Views)
	{
		const FKBEnemyView& View = Pair.Value;

		if (!Archetypes.IsValidIndex(View.ArchetypeIndex))
		{
			continue;
		}

		const UKBEnemyArchetype* Archetype = Archetypes[View.ArchetypeIndex].Get();
		if (!Archetype || Archetype->MoveSound.IsNull())
		{
			// Silent archetype. Note it is filtered out BEFORE the distance test, so a silent bug
			// - or a bug whose art has not landed yet - never takes an emitter away from one
			// that would actually be heard.
			++DebugNoSoundCount;
			continue;
		}

		const float Distance = KBListener::HearingDistance(Listener, View.DisplayLocation);
		if (Distance > Archetype->MoveSoundRadius)
		{
			// Tracked for the log line: "everything is out of range" and "there are no bugs" look
			// identical from the player's seat, and they have different fixes.
			if (DebugNearestOutOfRange < 0.f || Distance < DebugNearestOutOfRange)
			{
				DebugNearestOutOfRange = Distance;
			}
			continue;
		}

		Candidates.Add({Pair.Key, View.ArchetypeIndex, Distance});
	}

	DebugCandidateCount = Candidates.Num();

	Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; });

	// Candidates sorted by distance, so "the nearest un-consumed one" is a walk from the front.
	TMap<int32, int32> CandidateIndex;
	CandidateIndex.Reserve(Candidates.Num());
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		CandidateIndex.Add(Candidates[Index].StableId, Index);
	}

	TArray<bool> Consumed;
	Consumed.Init(false, Candidates.Num());

	// ---- 1. Keep what is still valid ------------------------------------------------------
	// A slot keeps its bug for as long as the bug is a candidate at all. Distance is deliberately
	// NOT reconsidered here: that is the hysteresis pass's job, and doing it in both places would
	// make the hysteresis do nothing.
	for (int32 Slot = 0; Slot < Emitters.Num(); ++Slot)
	{
		const int32 StableId = BoundStableIds[Slot];
		if (StableId == INDEX_NONE)
		{
			continue;
		}

		const int32* Found = CandidateIndex.Find(StableId);
		if (!Found)
		{
			// Died, or ran out of its own hearing radius.
			UnbindSlot(Slot);
			continue;
		}

		Consumed[*Found] = true;
		BoundDistances[Slot] = Candidates[*Found].Distance;
	}

	// ---- 2. Fill the free slots with the nearest bugs not already heard --------------------
	for (int32 Slot = 0; Slot < Emitters.Num(); ++Slot)
	{
		if (BoundStableIds[Slot] != INDEX_NONE)
		{
			continue;
		}

		int32 NearestFree = INDEX_NONE;
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			if (!Consumed[Index])
			{
				NearestFree = Index;
				break;
			}
		}

		if (NearestFree == INDEX_NONE)
		{
			break;
		}

		Consumed[NearestFree] = true;
		BindCandidateToSlot(Slot, Director, Candidates[NearestFree].StableId,
		                    Candidates[NearestFree].ArchetypeIndex);
		BoundDistances[Slot] = Candidates[NearestFree].Distance;
	}

	// ---- 3. Let a much closer bug take a slot from a distant one ---------------------------
	// Without this, whoever came into range first would hold the emitters until they died, and a
	// bug that walked right up to the player would stay silent behind six far ones. The loop
	// terminates because every swap strictly lowers the total bound distance.
	for (;;)
	{
		int32 NearestFree = INDEX_NONE;
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			if (!Consumed[Index])
			{
				NearestFree = Index;
				break;
			}
		}

		if (NearestFree == INDEX_NONE)
		{
			break;
		}

		int32 FarthestSlot = INDEX_NONE;
		float FarthestDistance = 0.f;
		for (int32 Slot = 0; Slot < Emitters.Num(); ++Slot)
		{
			if (BoundStableIds[Slot] != INDEX_NONE && BoundDistances[Slot] > FarthestDistance)
			{
				FarthestDistance = BoundDistances[Slot];
				FarthestSlot = Slot;
			}
		}

		if (FarthestSlot == INDEX_NONE)
		{
			break;
		}

		// Both sides are lengths and the factor is a ratio, so they compare directly.
		if (Candidates[NearestFree].Distance * ReassignHysteresis >= FarthestDistance)
		{
			break;
		}

		// Retire the loser and hand its slot to the challenger. The loser stays a candidate -
		// it has not gone anywhere - so it can be picked up again if the challenger leaves.
		const int32 LoserIndex = CandidateIndex.FindChecked(BoundStableIds[FarthestSlot]);
		Consumed[LoserIndex] = false;

		UnbindSlot(FarthestSlot);
		Consumed[NearestFree] = true;
		BindCandidateToSlot(FarthestSlot, Director, Candidates[NearestFree].StableId,
		                    Candidates[NearestFree].ArchetypeIndex);
		BoundDistances[FarthestSlot] = Candidates[NearestFree].Distance;
	}
}

void UKBSwarmAudioComponent::BindCandidateToSlot(int32 Slot, AKBEnemyDirector* Director,
                                                 int32 StableId, int32 ArchetypeIndex)
{
	const TArray<TObjectPtr<UKBEnemyArchetype>>& Archetypes = Director->GetArchetypes();
	const UKBEnemyArchetype* Archetype =
		Archetypes.IsValidIndex(ArchetypeIndex) ? Archetypes[ArchetypeIndex].Get() : nullptr;

	if (!Archetype)
	{
		UnbindSlot(Slot);
		return;
	}

	USoundBase* Sound = Archetype->MoveSound.LoadSynchronous();
	if (!Sound)
	{
		// A path that does not resolve. Treated as "silent archetype" rather than as an error:
		// LoadSynchronous has already logged why.
		UnbindSlot(Slot);
		return;
	}

	// Rolled once per binding, not per frame - see MoveSoundPitchMin.
	const float Pitch = FMath::FRandRange(
		FMath::Min(Archetype->MoveSoundPitchMin, Archetype->MoveSoundPitchMax),
		FMath::Max(Archetype->MoveSoundPitchMin, Archetype->MoveSoundPitchMax));

	BindSlot(Slot, StableId, Sound, FMath::Max(Archetype->MoveSoundRadius, 1.f),
	         Archetype->MoveSoundVolume, Pitch);

	BoundRadii[Slot] = FMath::Max(Archetype->MoveSoundRadius, 1.f);
	BoundVolumes[Slot] = Archetype->MoveSoundVolume;
	BoundPitches[Slot] = Pitch;
	// BoundStableIds is set by BindSlot, which is where "this slot now belongs to this bug" is
	// defined - not repeated here, so the two cannot disagree.
}

void UKBSwarmAudioComponent::BindSlot(int32 Slot, int32 StableId, USoundBase* Sound, float Radius,
                                      float Volume, float Pitch)
{
	UAudioComponent* Emitter = Emitters.IsValidIndex(Slot) ? Emitters[Slot].Get() : nullptr;
	if (!Emitter)
	{
		return;
	}

	if (BoundSounds[Slot] != Sound)
	{
		// A different sound means genuinely different audio, so the voice has to restart - there
		// is no way to cross-fade one source into another. Kept rare by the hysteresis: a slot
		// only changes sound when it changes ARCHETYPE, not when it changes bug.
		Emitter->Stop();
		Emitter->SetSound(Sound);
		Emitter->SetPitchMultiplier(Pitch);
		Emitter->Play();
	}
	else if (!Emitter->IsPlaying())
	{
		// Same sound, still bound, but the voice stopped - a one-shot that finished, or a sound
		// the engine reclaimed. Restarting it is what makes a non-looping asset degrade into a
		// repeated chirp instead of silence.
		Emitter->SetPitchMultiplier(Pitch);
		Emitter->Play();
	}
	else
	{
		Emitter->SetPitchMultiplier(Pitch);
	}

	BoundSounds[Slot] = Sound;

	// The line that makes the slot BOUND. Without it every other part of this component still
	// runs - a voice is started, a sound is set, the pool looks healthy - but UpdateEmitters
	// treats the slot as free, so it never gives it a position or a gain and the voice plays at
	// the zero volume the pool was created with. The whole feature is silent and nothing logs an
	// error. This is the "it is bound" statement; BindCandidateToSlot deliberately does not
	// repeat it.
	BoundStableIds[Slot] = StableId;
}

void UKBSwarmAudioComponent::UnbindSlot(int32 Slot)
{
	if (!Emitters.IsValidIndex(Slot))
	{
		return;
	}

	// Deliberately NOT stopped here. The gain is driven to zero by UpdateEmitters first, and a
	// voice cut mid-note is the click this pool exists to avoid.
	BoundStableIds[Slot] = INDEX_NONE;
	BoundSounds[Slot] = nullptr;
	BoundRadii[Slot] = 1.f;
	BoundDistances[Slot] = 0.f;
	BoundVolumes[Slot] = 0.f;
	TargetGains[Slot] = 0.f;
}

void UKBSwarmAudioComponent::UnbindAll()
{
	for (int32 Slot = 0; Slot < Emitters.Num(); ++Slot)
	{
		UnbindSlot(Slot);

		if (UAudioComponent* Emitter = Emitters[Slot].Get())
		{
			Emitter->SetVolumeMultiplier(0.f);
			Emitter->Stop();
		}

		CurrentGains[Slot] = 0.f;
	}
}

void UKBSwarmAudioComponent::UpdateEmitters(float DeltaTime, AKBEnemyDirector* Director,
                                            const FVector& Listener, bool bHasListener)
{
	const UKBEnemyVisualizerComponent* Visualizer = Director ? Director->GetVisualizer() : nullptr;
	const TMap<int32, FKBEnemyView>* Views = Visualizer ? &Visualizer->GetViews() : nullptr;

	const float FadeRate = FadeSeconds > KINDA_SMALL_NUMBER ? 1.f / FadeSeconds : 0.f;

	for (int32 Slot = 0; Slot < Emitters.Num(); ++Slot)
	{
		UAudioComponent* Emitter = Emitters[Slot].Get();
		if (!Emitter)
		{
			continue;
		}

		const int32 StableId = BoundStableIds[Slot];

		if (StableId != INDEX_NONE && !bHasListener)
		{
			// The listener went away between updates - a listen server whose last player left,
			// or a camera that has not been created yet. Silence rather than a voice left
			// playing at whatever the last distance happened to be.
			TargetGains[Slot] = 0.f;
		}
		else if (StableId != INDEX_NONE)
		{
			// The bug can vanish between rebinds - that is what dying is - and a voice left
			// following a bug that no longer exists would sit at its last position forever.
			const FKBEnemyView* View = Views ? Views->Find(StableId) : nullptr;

			if (View)
			{
				Emitter->SetWorldLocation(View->DisplayLocation);

				// Measured to the LISTENER, not to the emitter's own position - the emitter was
				// just moved onto the bug, so that distance is zero by construction and every
				// bug would play at full volume from anywhere in the arena. Measured on the
				// ground plane for the reason in KBListener::HearingDistance.
				const float Radius = FMath::Max(BoundRadii[Slot], 1.f);
				const float Distance = KBListener::HearingDistance(Listener, View->DisplayLocation);
				const float Falloff = 1.f - FMath::Clamp(Distance / Radius, 0.f, 1.f);
				TargetGains[Slot] = BoundVolumes[Slot] * Falloff;
			}
			else
			{
				UnbindSlot(Slot);
			}
		}

		// Fade towards the target rather than stepping to it: this is what makes a handover
		// inaudible and a bug leaving range a decay rather than a cut.
		CurrentGains[Slot] = FadeRate > 0.f
			? FMath::FInterpConstantTo(CurrentGains[Slot], TargetGains[Slot], DeltaTime, FadeRate)
			: TargetGains[Slot];

		Emitter->SetVolumeMultiplier(CurrentGains[Slot]);

		// Once a freed slot has faded out, stop the voice so it stops costing one. Checked here
		// rather than in UnbindSlot because only here is the fade known to have finished.
		if (BoundStableIds[Slot] == INDEX_NONE && CurrentGains[Slot] <= KINDA_SMALL_NUMBER
		    && Emitter->IsPlaying())
		{
			Emitter->Stop();
		}
	}
}
