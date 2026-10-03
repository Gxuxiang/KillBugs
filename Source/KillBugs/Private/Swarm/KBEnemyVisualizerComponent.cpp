#include "Swarm/KBEnemyVisualizerComponent.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Data/KBEnemyArchetype.h"
#include "KBConsoleVariables.h"
#include "KBStats.h"
#include "KillBugs.h"
#include "Materials/MaterialInterface.h"
#include "Swarm/KBEnemyDirector.h"

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

	if (UInstancedStaticMeshComponent* Instances = ArchetypeInstances[ArchetypeIndex])
	{
		// The transform is overwritten this same frame; it only needs to exist.
		Instances->AddInstance(GetHiddenTransform(), /*bWorldSpace*/ true);
	}

	return NewSlot;
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
	for (auto It = Views.CreateIterator(); It; ++It)
	{
		if (It.Value().LastSeenFrame != ReconcileFrame)
		{
			ReleaseInstanceSlot(It.Value().ArchetypeIndex, It.Value().InstanceIndex);
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

		Transforms[View.InstanceIndex] =
			FTransform(FRotator(0.f, View.DisplayYaw, 0.f), View.DisplayLocation);
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
