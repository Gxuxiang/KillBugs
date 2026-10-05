#include "Swarm/KBGoreComponent.h"

#include "Audio/KBListenerLocation.h"
#include "Components/AudioComponent.h"
#include "Components/DecalComponent.h"
#include "Core/KBGameState.h"
#include "Data/KBEnemyArchetype.h"
#include "Engine/World.h"
#include "KBGameSettings.h"
#include "KillBugs.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"
#include "Swarm/KBEnemyDirector.h"

namespace
{
	/** Scalar parameter the decal material must expose so the pool can fade it out. */
	const FName DecalOpacityParameter(TEXT("Opacity"));

	/** Colour parameter on the decal material. */
	const FName DecalColourParameter(TEXT("Base Color"));

	/**
	 * How far above the floor a decal sits.
	 *
	 * Decals project downward from their origin, and one placed exactly on the floor z-fights
	 * with it. A couple of units up is enough to stop the flicker and far too little to see.
	 */
	constexpr float DecalHeight = 8.f;

	/** Pitch that points the decal's projection straight down. */
	const FRotator DecalRotation(-90.f, 0.f, 0.f);

	/**
	 * How far the decal projects along its own axis.
	 *
	 * This is DecalSize.X, the depth - it has to reach from where the component sits down
	 * through the floor. The engine's default for it is 128, which is generous for a decal
	 * lying on the ground and comfortably more than the 8 units of clearance used here.
	 */
	constexpr float DecalProjectionDepth = 128.f;
}

UKBGoreComponent::UKBGoreComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// After the visualizer, which is what feeds us: the visualizer reconciles in
	// TG_DuringPhysics, and a splat spawned this frame should belong to the same frame the bug
	// disappeared in, not the next one.
	PrimaryComponentTick.TickGroup = TG_DuringPhysics;
}

void UKBGoreComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                     FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	SplatsThisFrame = 0;
	DeathSoundsThisFrame = 0;

	UpdateDecals();
}

int32 UKBGoreComponent::GetActiveDecalCount() const
{
	return ActiveDecals;
}

void UKBGoreComponent::EnsureDecalPool(const AKBEnemyDirector* Director)
{
	if (DecalPool.Num() > 0)
	{
		return;
	}

	if (MaxDecals <= 0)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Gore: MaxDecals is 0, so no slime will ever be left"));
		return;
	}

	// No archetype is needed to CREATE a decal, only to colour one - the material and colour
	// are assigned when a slot is claimed. The pool is therefore built once, up front, and
	// never grows: creating and destroying decal components per death would be the per-actor
	// churn this project avoids everywhere else.
	DecalPool.Reserve(MaxDecals);
	DecalSpawnTimes.Reserve(MaxDecals);

	UMaterialInterface* SlimeMaterial = KBSettings().SlimeDecalMaterial.LoadSynchronous();
	if (!SlimeMaterial)
	{
		// Loud, because the failure is otherwise invisible: without a decal material the
		// components still exist and still get placed, and nothing at all is drawn.
		UE_LOG(LogKillBugs, Warning,
			TEXT("Gore: no SlimeDecalMaterial in project settings - no slime will be visible. ")
			TEXT("See Project Settings > Game > KillBugs > 战斗|死亡."));
	}

	for (int32 Index = 0; Index < MaxDecals; ++Index)
	{
		UDecalComponent* Decal = NewObject<UDecalComponent>(GetOwner());
		Decal->SetupAttachment(this);

		// Movable: a pooled decal is re-placed for every bug that reuses its slot.
		Decal->SetMobility(EComponentMobility::Movable);

		// Placeholder until a bug claims the slot; the real size comes from the archetype's
		// BodyRadius. Ordered (depth, width, height) like every other DecalSize here.
		Decal->DecalSize = FVector(DecalProjectionDepth, 64.f, 64.f);

		// Nothing to turn off: UDecalComponent derives from USceneComponent, not
		// UPrimitiveComponent, so it has no collision, shadow or navigation to disable in the
		// first place. (Calling SetCollisionEnabled here does not compile.)

		Decal->RegisterComponent();
		Decal->SetVisibility(false);

		// The material AND its dynamic instance are made once, here, and then reused for the
		// life of the slot. Every death only writes a colour and an opacity into the instance
		// it already has - creating one per death would allocate at the swarm's death rate.
		if (SlimeMaterial)
		{
			Decal->SetDecalMaterial(SlimeMaterial);
		}

		UMaterialInstanceDynamic* Dynamic = Decal->CreateDynamicMaterialInstance();
		if (Dynamic)
		{
			Dynamic->SetScalarParameterValue(DecalOpacityParameter, 0.f);
		}

		DecalPool.Add(Decal);
		DecalMIDs.Add(Dynamic);
		DecalSpawnTimes.Add(-1.f);
	}

	UE_LOG(LogKillBugs, Display, TEXT("Gore: %d decal slots pooled, %.1fs lifetime"),
		DecalPool.Num(), DecalLifetimeSeconds);
}

int32 UKBGoreComponent::AcquireDecalSlot()
{
	if (DecalPool.Num() == 0)
	{
		return INDEX_NONE;
	}

	// Prefer a genuinely free slot; once the pool is saturated, take the ring's tail, which is
	// the oldest puddle. Recycling the oldest is the whole point of the cap: the arena keeps
	// its most recent kills and forgets the rest, rather than refusing to paint.
	int32 Slot = INDEX_NONE;

	for (int32 Offset = 0; Offset < DecalPool.Num(); ++Offset)
	{
		const int32 Candidate = (NextDecalSlot + Offset) % DecalPool.Num();
		if (DecalSpawnTimes[Candidate] < 0.f)
		{
			Slot = Candidate;
			break;
		}
	}

	if (Slot == INDEX_NONE)
	{
		Slot = NextDecalSlot;
	}

	NextDecalSlot = (Slot + 1) % DecalPool.Num();
	DecalSpawnTimes[Slot] = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	return Slot;
}

void UKBGoreComponent::OnBugDied(const FVector& Location, int32 ArchetypeIndex)
{
	// A bug can stop being replicated without dying: tearing the map down removes all of them at
	// once, and AKBEnemyDirector::CullAllRemaining - the wave-clear path - removes every survivor
	// at once. The visualizer calls us for every purge it does and cannot tell the difference, so
	// the filter belongs here.
	//
	// The run-over half of this is not hypothetical. Ending a run used to cull the survivors, and
	// over a field of 186 bugs that read as 186 simultaneous deaths: 186 splats and 186 puddles
	// into the frame the summary appeared. That path now freezes the swarm instead (see
	// AKBEnemyDirector::Tick), but the cull call is still there for a wave clear to use, and this
	// is what keeps it from reading as a mass death when it is used.
	if (UWorld* World = GetWorld())
	{
		if (World->bIsTearingDown)
		{
			return;
		}

		if (const AKBGameState* RunState = World->GetGameState<AKBGameState>())
		{
			if (RunState->GetWavePhase() == EKBWavePhase::RunOver)
			{
				return;
			}
		}
	}

	const AKBEnemyDirector* Director = Cast<AKBEnemyDirector>(GetOwner());
	if (!Director || !Director->GetArchetypes().IsValidIndex(ArchetypeIndex))
	{
		return;
	}

	const UKBEnemyArchetype* Archetype = Director->GetArchetypes()[ArchetypeIndex].Get();
	if (!Archetype)
	{
		return;
	}

	EnsureDecalPool(Director);

	// ---- The puddle ----
	const int32 Slot = AcquireDecalSlot();
	if (Slot != INDEX_NONE)
	{
		UDecalComponent* Decal = DecalPool[Slot];

		const float Radius = FMath::Max(Archetype->BodyRadius, 1.f);

		// Roughly twice the body across, with a little give so a row of deaths is not a row of
		// identical circles.
		const float Spread = FMath::FRandRange(1.6f, 2.4f);

		// DecalSize is (DEPTH along the projection axis, width, height) - X is not a width.
		// The engine's own default is (128, 256, 256), which is the giveaway.
		//
		// An earlier version put the puddle's width in X and a constant in Z, which made the
		// projection a shallow box - about 84 units deep, barely reaching the floor from the
		// 8 units the component sits at - with a 84x128 cross-section. Combined with the random
		// yaw below, that drew a field of rectangles pointing in every direction: the "slime is
		// a bunch of squares" that sent us looking at the material, which was blameless.
		//
		// Y and Z are equal on purpose: a square cross-section means the random yaw cannot
		// change the shape, only the orientation of the texture inside it.
		Decal->DecalSize = FVector(DecalProjectionDepth, Radius * Spread, Radius * Spread);

		if (UMaterialInstanceDynamic* Dynamic = DecalMIDs[Slot])
		{
			// Opacity is restored here rather than in AcquireDecalSlot: a recycled slot is still
			// at whatever opacity its previous puddle faded down to, and this puddle is new.
			Dynamic->SetScalarParameterValue(DecalOpacityParameter, 1.f);
			Dynamic->SetVectorParameterValue(DecalColourParameter,
				FVector(Archetype->SlimeColor.R, Archetype->SlimeColor.G, Archetype->SlimeColor.B));
		}

		// Random yaw so repeated deaths in one spot do not stack into one darker circle.
		Decal->SetWorldLocation(FVector(Location.X, Location.Y, DecalHeight));
		Decal->SetWorldRotation(FRotator(DecalRotation.Pitch, FMath::FRandRange(0.f, 360.f), 0.f));
		Decal->SetVisibility(true);

		++ActiveDecals;
	}

	// ---- The death sound ----
	// Before the burst, and outside its budget: the sound and the particles are gated by
	// different limits, so a frame that has spent its splats must not also go silent.
	PlayDeathSound(*Archetype, Location);

	// ---- The burst ----
	// Budgeted separately from the puddles: a puddle is a single quad and reads as the record
	// that something died; the burst is the expensive half, and a wave ending can ask for
	// hundreds of them in one frame.
	if (SplatsThisFrame >= MaxSplatsPerFrame)
	{
		return;
	}

	UNiagaraSystem* Effect = Archetype->DeathEffect.LoadSynchronous();
	if (!Effect)
	{
		// Not an error. The decal alone is a complete answer to "something died here", and an
		// archetype that has no burst yet should still leave slime.
		return;
	}

	++SplatsThisFrame;

	UNiagaraComponent* Burst = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
		this, Effect, FVector(Location.X, Location.Y, DecalHeight), FRotator::ZeroRotator,
		FVector::OneVector, /*bAutoDestroy*/ true, /*bAutoActivate*/ true);

	if (!Burst)
	{
		return;
	}

	// The contract the Niagara graph has to honour. A system that ignores these still plays;
	// it just will not be tinted or sized per archetype.
	Burst->SetColorParameter(TEXT("SplatColor"), Archetype->SlimeColor);
	Burst->SetFloatParameter(TEXT("SplatScale"), FMath::Max(Archetype->BodyRadius, 1.f) / 40.f);
}

void UKBGoreComponent::EnsureDeathSoundPool()
{
	if (DeathSoundPool.Num() > 0 || MaxDeathSoundVoices <= 0)
	{
		return;
	}

	DeathSoundPool.Reserve(MaxDeathSoundVoices);

	for (int32 Index = 0; Index < MaxDeathSoundVoices; ++Index)
	{
		UAudioComponent* Voice = NewObject<UAudioComponent>(GetOwner());
		Voice->SetupAttachment(this);
		Voice->SetMobility(EComponentMobility::Movable);

		// The pool outlives every individual death, so nothing may destroy itself when its
		// sound ends - that would take the slot with it.
		Voice->bAutoDestroy = false;
		Voice->bAllowSpatialization = true;

		// Positional, with the engine's distance falloff switched off and replaced by the one
		// computed in PlayDeathSound. Same reasoning as UKBSwarmAudioComponent: the project has
		// no USoundAttenuation assets to point at, and the archetype - not the sound asset - is
		// what knows how far a death should carry. Spatialisation is a separate flag, so this
		// costs none of the left/right sense of where the bug died.
		Voice->bOverrideAttenuation = true;
		Voice->AttenuationOverrides.bAttenuate = false;
		Voice->AttenuationOverrides.bSpatialize = true;

		Voice->RegisterComponent();

		DeathSoundPool.Add(Voice);
	}
}

int32 UKBGoreComponent::AcquireDeathSoundSlot()
{
	for (int32 Index = 0; Index < DeathSoundPool.Num(); ++Index)
	{
		if (UAudioComponent* Voice = DeathSoundPool[Index].Get())
		{
			if (!Voice->IsPlaying())
			{
				return Index;
			}
		}
	}

	// Every voice is busy. The death is dropped rather than stealing one - see
	// MaxDeathSoundVoices.
	return INDEX_NONE;
}

void UKBGoreComponent::PlayDeathSound(const UKBEnemyArchetype& Archetype, const FVector& Location)
{
	USoundBase* Sound = Archetype.DeathSound.LoadSynchronous();
	if (!Sound)
	{
		// Not an error, for the same reason a missing DeathEffect is not: an archetype whose
		// sound has not landed yet should still leave slime and still play its burst.
		return;
	}

	// Both cheap gates run before the pool is even built, so a wave-end cull costs a few
	// distance checks rather than four hundred Stop/SetSound/Play round trips.
	if (DeathSoundsThisFrame >= MaxDeathSoundsPerFrame)
	{
		return;
	}

	// Gated on a LOCAL LISTENER, not on authority. The visualizer calls us on every machine, and
	// a dedicated server has no listener at all - playing there would be a voice mixed into
	// nothing. Note this is deliberately NOT the pattern in
	// AKBProjectileDirector::SpawnImpactFeedback, which gates on authority and so leaves clients
	// unable to hear their own weapon impacts.
	FVector Listener = FVector::ZeroVector;
	if (!KBListener::FindLocation(GetWorld(), Listener))
	{
		return;
	}

	const float Distance = KBListener::HearingDistance(Listener, Location);
	if (Distance > MaxDeathSoundDistance)
	{
		// Most deaths in a full wave happen across the arena, and this is the line that makes
		// them free.
		return;
	}

	EnsureDeathSoundPool();

	const int32 Slot = AcquireDeathSoundSlot();
	if (Slot == INDEX_NONE)
	{
		return;
	}

	++DeathSoundsThisFrame;

	UAudioComponent* Voice = DeathSoundPool[Slot].Get();
	if (!Voice)
	{
		return;
	}

	// Same falloff shape as the movement sounds, so a bug that dies while it is being heard does
	// not jump in level at the moment it changes from one to the other.
	const float Falloff = 1.f - FMath::Clamp(Distance / FMath::Max(MaxDeathSoundDistance, 1.f), 0.f, 1.f);

	const float Pitch = FMath::FRandRange(
		FMath::Min(Archetype.DeathSoundPitchMin, Archetype.DeathSoundPitchMax),
		FMath::Max(Archetype.DeathSoundPitchMin, Archetype.DeathSoundPitchMax));

	Voice->Stop();
	Voice->SetSound(Sound);
	Voice->SetWorldLocation(Location);
	Voice->SetVolumeMultiplier(Archetype.DeathSoundVolume * Falloff);
	Voice->SetPitchMultiplier(Pitch);
	Voice->Play();
}

void UKBGoreComponent::UpdateDecals()
{
	if (DecalPool.Num() == 0)
	{
		return;
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();
	const float FadeStart = FMath::Max(DecalLifetimeSeconds - DecalFadeSeconds, 0.f);

	for (int32 Index = 0; Index < DecalPool.Num(); ++Index)
	{
		if (DecalSpawnTimes[Index] < 0.f)
		{
			continue;
		}

		const float Age = Now - DecalSpawnTimes[Index];

		if (Age >= DecalLifetimeSeconds)
		{
			// Retire: hide and hand the slot back. The component is kept, not destroyed.
			if (UDecalComponent* Decal = DecalPool[Index])
			{
				Decal->SetVisibility(false);
			}

			DecalSpawnTimes[Index] = -1.f;
			ActiveDecals = FMath::Max(ActiveDecals - 1, 0);
			continue;
		}

		if (DecalFadeSeconds <= KINDA_SMALL_NUMBER || Age < FadeStart)
		{
			continue;
		}

		// Fade by material opacity rather than by scaling the box down: scaling a projection
		// shrinks the puddle towards its centre, which reads as the stain retracting rather
		// than drying out.
		const float Alpha = 1.f - (Age - FadeStart) / DecalFadeSeconds;

		if (UMaterialInstanceDynamic* Dynamic = DecalMIDs[Index])
		{
			Dynamic->SetScalarParameterValue(DecalOpacityParameter, FMath::Clamp(Alpha, 0.f, 1.f));
		}
	}
}
