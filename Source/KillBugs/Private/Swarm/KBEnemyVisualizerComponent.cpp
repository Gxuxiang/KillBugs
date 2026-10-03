#include "Swarm/KBEnemyVisualizerComponent.h"

#include "AnimToTextureDataAsset.h"
#include "AnimToTextureInstancePlaybackHelpers.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Data/KBEnemyArchetype.h"
#include "KBConsoleVariables.h"
#include "KBStats.h"
#include "KillBugs.h"
#include "Materials/MaterialInterface.h"
#include "Swarm/KBEnemyDirector.h"
#include "Swarm/KBGoreComponent.h"

DEFINE_STAT(STAT_KB_SwarmVisuals);

UKBEnemyVisualizerComponent::UKBEnemyVisualizerComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// After the director's TG_PrePhysics simulation, so a frame draws the state it just
	// produced rather than the previous frame's.
	PrimaryComponentTick.TickGroup = TG_DuringPhysics;
}

FTransform UKBEnemyVisualizerComponent::GetHiddenTransform()
{
	// Zero scale is what actually hides it; the offset just keeps it out of the way of
	// anything that might still consider its bounds.
	return FTransform(FRotator::ZeroRotator, FVector(0.f, 0.f, -100000.f), FVector::ZeroVector);
}

void UKBEnemyVisualizerComponent::EnsureInstanceComponents(const AKBEnemyDirector* Director)
{
	const TArray<TObjectPtr<UKBEnemyArchetype>>& Archetypes = Director->GetArchetypes();
	if (ArchetypeInstances.Num() == Archetypes.Num())
	{
		return;
	}

	ArchetypeInstances.SetNum(Archetypes.Num());
	InstanceTransforms.SetNum(Archetypes.Num());
	FreeInstanceIndices.SetNum(Archetypes.Num());
	ArchetypeAnimData.SetNum(Archetypes.Num());
	WrittenTimeOffsets.SetNum(Archetypes.Num());

	for (int32 Index = 0; Index < Archetypes.Num(); ++Index)
	{
		if (ArchetypeInstances[Index])
		{
			continue;
		}

		const UKBEnemyArchetype* Archetype = Archetypes[Index].Get();
		if (!Archetype)
		{
			continue;
		}

		// UInstancedStaticMeshComponent, NOT the hierarchical variant.
		//
		// Every instance here moves every frame. HISM maintains a cluster tree for culling
		// and pays to update it on that path, while the culling it buys is nearly worthless
		// under a fixed camera that already frames the whole arena. One draw call per
		// archetype either way.
		UInstancedStaticMeshComponent* Instances = NewObject<UInstancedStaticMeshComponent>(GetOwner());
		Instances->SetupAttachment(this);

		Instances->SetMobility(EComponentMobility::Movable);
		Instances->SetStaticMesh(Archetype->Mesh.LoadSynchronous());
		if (UMaterialInterface* Material = Archetype->Material.LoadSynchronous())
		{
			Instances->SetMaterial(0, Material);
		}
		Instances->SetWorldScale3D(Archetype->MeshScale);

		// ---- Baked vertex animation -------------------------------------------------------
		//
		// The animation moves vertices in the vertex shader, so nothing about the profile of the
		// drawn bug is visible to culling: the bounds have to be inflated by hand or the swarm
		// disappears near the screen edge. SetBoundsScale is the component-level version, which
		// is the only one reachable from Python or C++ alike - the plugin's own
		// SetBoundsExtensions is a plain static function with no UFUNCTION macro.
		ArchetypeAnimData[Index] = Archetype->AnimData.LoadSynchronous();

		if (ArchetypeAnimData[Index])
		{
			// Four floats per instance, exactly the size of FAnimToTextureAutoPlayData
			// (TimeOffset, PlayRate, StartFrame, EndFrame). This is the channel the material
			// reads to work out which frame THIS instance is on.
			Instances->NumCustomDataFloats = 4;
			Instances->SetBoundsScale(BakedAnimationBoundsScale);
		}

		// This archetype owns its own component, so one vector-parameter write tints all of
		// its bugs through a single dynamic material instance. It is what makes placeholder
		// archetypes distinguishable - two grey cubes read as one bug type.
		if (!Archetype->TintParameterName.IsNone())
		{
			const FLinearColor& Tint = Archetype->Tint;
			Instances->SetVectorParameterValueOnMaterials(
				Archetype->TintParameterName, FVector(Tint.R, Tint.G, Tint.B));
		}

		// A shiny surface blows out to white under a strong sun, hiding the tint completely.
		// Both writes are no-ops on a material that lacks the parameter.
		Instances->SetScalarParameterValueOnMaterials(TEXT("Roughness"), Archetype->Roughness);
		Instances->SetScalarParameterValueOnMaterials(TEXT("Metallic"), Archetype->Metallic);

		// The swarm never collides and never casts a shadow; a flat blob carries that job
		// instead, at zero shadow-map cost.
		Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Instances->SetGenerateOverlapEvents(false);
		Instances->SetCastShadow(false);
		Instances->SetCanEverAffectNavigation(false);

		// A deferred decal projects onto everything inside its box, so a bug walking over a
		// slime puddle was being painted with it - a flat splat stuck to its back. The puddles
		// are for the floor, and the swarm never needs to receive decals: nothing is ever
		// decalled onto a bug on purpose, and at 50-130 pixels the effect only ever reads as a
		// texture glitch.
		Instances->SetReceivesDecals(false);

		Instances->RegisterComponent();
		ArchetypeInstances[Index] = Instances;

		// Once per archetype at startup. Reports the value the material instance actually
		// holds, not just what we asked for - a silent no-op parameter write and a working
		// one look identical from the call site.
		//
		// NOTE: readback proves what was ASSIGNED, never what RENDERS. A material without
		// bUsedWithInstancedStaticMeshes is silently replaced by the engine default, and no
		// amount of readback will show that.
		FLinearColor ReadBack = FLinearColor::Black;
		float RoughnessReadBack = -1.f;
		if (const UMaterialInstanceDynamic* DynamicMaterial =
			Cast<UMaterialInstanceDynamic>(Instances->GetMaterial(0)))
		{
			DynamicMaterial->GetVectorParameterValue(
				FHashedMaterialParameterInfo(Archetype->TintParameterName), ReadBack);
			DynamicMaterial->GetScalarParameterValue(
				FHashedMaterialParameterInfo(TEXT("Roughness")), RoughnessReadBack);
		}

		UE_LOG(LogKillBugs, Display,
			TEXT("Archetype %d '%s': mesh=%s | material=%s | ")
			TEXT("tint want(%.2f,%.2f,%.2f) readback(%.2f,%.2f,%.2f) via '%s' | ")
			TEXT("roughness want %.2f readback %.2f"),
			Index,
			*Archetype->DisplayName.ToString(),
			Instances->GetStaticMesh() ? *Instances->GetStaticMesh()->GetName() : TEXT("NONE"),
			Instances->GetMaterial(0) ? *Instances->GetMaterial(0)->GetName() : TEXT("NONE"),
			Archetype->Tint.R, Archetype->Tint.G, Archetype->Tint.B,
			ReadBack.R, ReadBack.G, ReadBack.B,
			*Archetype->TintParameterName.ToString(),
			Archetype->Roughness, RoughnessReadBack);
	}
}

int32 UKBEnemyVisualizerComponent::AcquireInstanceSlot(int32 ArchetypeIndex)
{
	if (!InstanceTransforms.IsValidIndex(ArchetypeIndex))
	{
		return INDEX_NONE;
	}

	TArray<int32>& FreeSlots = FreeInstanceIndices[ArchetypeIndex];
	if (FreeSlots.Num() > 0)
	{
		// Reuse a slot left behind by a dead bug: its instance already exists and is hidden.
		return FreeSlots.Pop();
	}

	const int32 NewSlot = InstanceTransforms[ArchetypeIndex].Add(GetHiddenTransform());

	// Kept the same length as the slot array so the debug mirror has a home for every slot.
	if (WrittenTimeOffsets.IsValidIndex(ArchetypeIndex))
	{
		WrittenTimeOffsets[ArchetypeIndex].Add(0.f);
	}

	if (UInstancedStaticMeshComponent* Instances = ArchetypeInstances[ArchetypeIndex])
	{
		// The transform is overwritten this same frame; it only needs to exist.
		Instances->AddInstance(GetHiddenTransform(), /*bWorldSpace*/ true);

		// The instance now exists, so it can be given a phase. This is the only moment a slot's
		// per-instance data is written, because it is the only moment the instance comes into
		// being - see WriteAnimationPhase.
		WriteAnimationPhase(ArchetypeIndex, NewSlot);
	}

	return NewSlot;
}

void UKBEnemyVisualizerComponent::LogAnimationData() const
{
	for (int32 Index = 0; Index < ArchetypeInstances.Num(); ++Index)
	{
		const UInstancedStaticMeshComponent* Instances = ArchetypeInstances[Index];
		if (!Instances)
		{
			continue;
		}

		UE_LOG(LogKillBugs, Display,
			TEXT("Swarm anim | archetype %d | mesh=%s | customFloats=%d | instances=%d | ")
			TEXT("animData=%s | boundsScale=%.2f"),
			Index,
			Instances->GetStaticMesh() ? *Instances->GetStaticMesh()->GetName() : TEXT("NONE"),
			Instances->NumCustomDataFloats,
			Instances->GetInstanceCount(),
			(ArchetypeAnimData.IsValidIndex(Index) && ArchetypeAnimData[Index]) ? TEXT("yes") : TEXT("NO"),
			Instances->BoundsScale);

		// The first few slots: the phase each one was born with. If these are all zero, nothing
		// was ever written; if they are all IDENTICAL, the de-sync is not happening; if they
		// differ, the data reached the component and the material is what is ignoring it.
		if (!WrittenTimeOffsets.IsValidIndex(Index))
		{
			continue;
		}

		const int32 SlotsToShow = FMath::Min(3, WrittenTimeOffsets[Index].Num());
		for (int32 Slot = 0; Slot < SlotsToShow; ++Slot)
		{
			UE_LOG(LogKillBugs, Display,
				TEXT("    slot %d TimeOffset = %.4f"), Slot, WrittenTimeOffsets[Index][Slot]);
		}
	}
}

void UKBEnemyVisualizerComponent::WriteAnimationPhase(int32 ArchetypeIndex, int32 InstanceIndex)
{
	if (!ArchetypeAnimData.IsValidIndex(ArchetypeIndex))
	{
		return;
	}

	UAnimToTextureDataAsset* AnimData = ArchetypeAnimData[ArchetypeIndex];
	UInstancedStaticMeshComponent* Instances = ArchetypeInstances.IsValidIndex(ArchetypeIndex)
		? ArchetypeInstances[ArchetypeIndex].Get() : nullptr;

	if (!AnimData || !Instances)
	{
		return;
	}

	FAnimToTextureAutoPlayData AutoPlay;
	if (!UAnimToTextureInstancePlaybackLibrary::GetAutoPlayDataFromDataAsset(
		AnimData, /*AnimationIndex*/ 0, AutoPlay))
	{
		return;
	}

	// TimeOffset is in SECONDS, not a normalised phase: the helper computes
	// Frame = (Time + TimeOffset) * PlayRate * SampleRate and then wraps it. Spreading it over
	// one whole cycle therefore starts each bug somewhere different in the animation.
	//
	// De-syncing is the entire reason this class uses per-instance data at all. Six hundred
	// identical meshes playing the same frame at the same instant do not read as a swarm, they
	// read as one machine - and the effect is strongest on a bug whose legs are its silhouette.
	const float SampleRate = FMath::Max(AnimData->SampleRate, 1.f);
	const float CycleSeconds = FMath::Max(
		(AutoPlay.EndFrame - AutoPlay.StartFrame + 1.f) / SampleRate, 0.01f);

	AutoPlay.TimeOffset = FMath::FRandRange(0.f, CycleSeconds);

	// Deliberately NOT re-rolled when a slot is recycled from a dead bug: the instance keeps the
	// phase it was born with. Re-rolling would be per-spawn work for a difference that cannot be
	// seen - two bugs never occupy the same slot at the same time.
	UAnimToTextureInstancePlaybackLibrary::UpdateInstanceAutoPlayData(
		Instances, InstanceIndex, AutoPlay);

	// Mirrored for the debug log; see WrittenTimeOffsets.
	if (WrittenTimeOffsets.IsValidIndex(ArchetypeIndex) &&
		WrittenTimeOffsets[ArchetypeIndex].IsValidIndex(InstanceIndex))
	{
		WrittenTimeOffsets[ArchetypeIndex][InstanceIndex] = AutoPlay.TimeOffset;
	}
}

void UKBEnemyVisualizerComponent::ReleaseInstanceSlot(int32 ArchetypeIndex, int32 InstanceIndex)
{
	if (!InstanceTransforms.IsValidIndex(ArchetypeIndex) ||
		!InstanceTransforms[ArchetypeIndex].IsValidIndex(InstanceIndex))
	{
		return;
	}

	// Hidden, never removed. Removing an instance would shift the slots above it and every
	// bug above it would appear to jump.
	InstanceTransforms[ArchetypeIndex][InstanceIndex] = GetHiddenTransform();
	FreeInstanceIndices[ArchetypeIndex].Add(InstanceIndex);
}

void UKBEnemyVisualizerComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const AKBEnemyDirector* Director = Cast<AKBEnemyDirector>(GetOwner());
	if (!Director)
	{
		return;
	}

	SCOPE_CYCLE_COUNTER(STAT_KB_SwarmVisuals);

	const double StartSeconds = FPlatformTime::Seconds();

	EnsureInstanceComponents(Director);

	// ---- Reconcile against the network array ----
	++ReconcileFrame;

	for (const FKBEnemyNetItem& Item : Director->GetReplicatedEnemies().Items)
	{
		FKBEnemyView& View = Views.FindOrAdd(Item.StableId);

		if (View.LastSeenFrame == -1)
		{
			// First sighting: claim a slot and appear where the bug actually is, rather than
			// sliding across the arena from wherever a previous bug left that slot.
			View.StableId = Item.StableId;
			View.ArchetypeIndex = Item.ArchetypeIndex;
			View.InstanceIndex = AcquireInstanceSlot(View.ArchetypeIndex);
			View.DisplayLocation = Item.Location;
			View.DisplayYaw = FKBEnemyNetItem::UnpackYaw(Item.Yaw);
		}

		View.ArchetypeIndex = Item.ArchetypeIndex;
		View.HealthPct = Item.HealthPct;
		View.TargetLocation = Item.Location;
		View.TargetYaw = FKBEnemyNetItem::UnpackYaw(Item.Yaw);
		View.LastSeenFrame = ReconcileFrame;
	}

	// Anything not seen this frame is gone: killed, or removed by a wave cull.
	//
	// This purge is also the ONLY place a death is ever noticed, and it is why nothing about a
	// death needs replicating: over the network a death is just an item vanishing from the
	// array, and the only record of where that bug was and what it was is the view being erased
	// right here. The gore is told before the view goes, which is the last moment that
	// information exists.
	//
	// The gore component filters out the purges that are not deaths - a wave cull takes every
	// survivor at once, and tearing the map down takes all of them.
	UKBGoreComponent* GoreComponent = Director ? Director->GetGoreComponent() : nullptr;

	for (auto It = Views.CreateIterator(); It; ++It)
	{
		if (It.Value().LastSeenFrame != ReconcileFrame)
		{
			const FKBEnemyView& Gone = It.Value();

			if (GoreComponent)
			{
				// DisplayLocation, not TargetLocation: the player's mental model is where they
				// SAW the bug, and DisplayLocation is exactly where it was drawn.
				GoreComponent->OnBugDied(Gone.DisplayLocation, Gone.ArchetypeIndex);
			}

			ReleaseInstanceSlot(Gone.ArchetypeIndex, Gone.InstanceIndex);
			It.RemoveCurrent();
		}
	}

	// ---- Interpolate ----
	// Positions only arrive at ~7.5 Hz (15 Hz net updates with a stagger of 2), so without
	// this the swarm would visibly step between updates.
	const float Alpha = InterpolationTime > KINDA_SMALL_NUMBER
		? FMath::Clamp(DeltaTime / InterpolationTime, 0.f, 1.f)
		: 1.f;

	for (TPair<int32, FKBEnemyView>& Pair : Views)
	{
		FKBEnemyView& View = Pair.Value;

		View.DisplayLocation = FMath::Lerp(View.DisplayLocation, View.TargetLocation, Alpha);

		// Yaw wrap handled explicitly: interpolating raw degrees spins a bug the long way
		// round every time it crosses the +/-180 seam.
		const float YawDelta = FRotator::NormalizeAxis(View.TargetYaw - View.DisplayYaw);
		View.DisplayYaw = FRotator::NormalizeAxis(View.DisplayYaw + YawDelta * Alpha);
	}

	PushToInstances();

	const float PerfLogInterval = CVarKBSwarmPerfLog.GetValueOnGameThread();
	if (PerfLogInterval > 0.f)
	{
		PerfSeconds += FPlatformTime::Seconds() - StartSeconds;
		++PerfFrames;
		PerfElapsed += DeltaTime;

		if (PerfElapsed >= PerfLogInterval)
		{
			int32 ActiveBatches = 0;
			int32 PooledSlots = 0;
			for (int32 Index = 0; Index < ArchetypeInstances.Num(); ++Index)
			{
				ActiveBatches += ArchetypeInstances[Index] ? 1 : 0;
				PooledSlots += InstanceTransforms.IsValidIndex(Index) ? InstanceTransforms[Index].Num() : 0;
			}

			const double Frames = FMath::Max(PerfFrames, 1);
			UE_LOG(LogKillBugs, Display,
				TEXT("Swarm visuals | %d bugs | %d draw batches | %d pooled slots | %.3f ms | %.0f fps"),
				Views.Num(), ActiveBatches, PooledSlots,
				PerfSeconds / Frames * 1000.0, Frames / PerfElapsed);

			// Piggy-backed on the existing perf interval: the swarm has to be alive with bugs in
			// it for the answer to mean anything, and that is exactly when this block runs.
			LogAnimationData();

			PerfSeconds = 0.0;
			PerfFrames = 0;
			PerfElapsed = 0.f;
		}
	}
}

void UKBEnemyVisualizerComponent::PushToInstances()
{
	// Every slot starts hidden. Slots belonging to bugs that died this frame are never
	// written, so they stay hidden - which is how a death is drawn without touching the
	// instance list at all.
	for (TArray<FTransform>& Transforms : InstanceTransforms)
	{
		for (FTransform& Transform : Transforms)
		{
			Transform = GetHiddenTransform();
		}
	}

	for (const TPair<int32, FKBEnemyView>& Pair : Views)
	{
		const FKBEnemyView& View = Pair.Value;
		if (!InstanceTransforms.IsValidIndex(View.ArchetypeIndex))
		{
			continue;
		}

		TArray<FTransform>& Transforms = InstanceTransforms[View.ArchetypeIndex];
		if (!Transforms.IsValidIndex(View.InstanceIndex))
		{
			continue;
		}

		// The mesh's own forward is not necessarily the actor's +X. The swarm yaws an instance to
		// face the way the bug is travelling, which is correct for a mesh authored looking down
		// +X and visibly wrong for one authored looking down -X: the bug runs tail-first. The
		// offset lives on the archetype so a replacement model can correct it without touching
		// this loop.
		float MeshYaw = View.DisplayYaw;
		if (const AKBEnemyDirector* Owner = Cast<AKBEnemyDirector>(GetOwner()))
		{
			if (Owner->GetArchetypes().IsValidIndex(View.ArchetypeIndex))
			{
				if (const UKBEnemyArchetype* Archetype =
					Owner->GetArchetypes()[View.ArchetypeIndex].Get())
				{
					MeshYaw += Archetype->MeshYawOffset;
				}
			}
		}

		Transforms[View.InstanceIndex] =
			FTransform(FRotator(0.f, FRotator::NormalizeAxis(MeshYaw), 0.f), View.DisplayLocation);
	}

	// One batch write per archetype. Safe to send the whole pool including hidden slots,
	// because each slot's identity never changes - no instance is ever reused by another bug
	// while it is alive, so nothing smears.
	for (int32 Index = 0; Index < ArchetypeInstances.Num(); ++Index)
	{
		UInstancedStaticMeshComponent* Instances = ArchetypeInstances[Index];
		const TArray<FTransform>& Transforms = InstanceTransforms[Index];
		if (Instances && Transforms.Num() > 0)
		{
			Instances->BatchUpdateInstancesTransforms(
				0, Transforms, /*bWorldSpace*/ true, /*bMarkRenderStateDirty*/ true, /*bTeleport*/ false);
		}
	}
}
