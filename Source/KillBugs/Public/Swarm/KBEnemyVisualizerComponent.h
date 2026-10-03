#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "KBEnemyVisualizerComponent.generated.h"

class UInstancedStaticMeshComponent;
class AKBEnemyDirector;

/** Render state for one bug, matched across frames by its network-stable id. */
struct FKBEnemyView
{
	int32 StableId = 0;
	uint8 ArchetypeIndex = 0;
	uint8 HealthPct = 255;

	/** Where the last network update said this bug is. */
	FVector TargetLocation = FVector::ZeroVector;
	float TargetYaw = 0.f;

	/** Where it is being drawn right now. */
	FVector DisplayLocation = FVector::ZeroVector;
	float DisplayYaw = 0.f;

	/** Purge marker; see TickComponent. */
	int32 LastSeenFrame = -1;

	/**
	 * This bug's fixed slot in its archetype's instanced mesh, held for the bug's whole life.
	 *
	 * Fixed, not recomputed per frame: if slots were reassigned, an instance would represent
	 * a different bug from one frame to the next, and the renderer - interpolating between
	 * the two positions - would draw a smear across the arena. That is exactly the "bugs
	 * flicker and teleport" artefact.
	 */
	int32 InstanceIndex = INDEX_NONE;
};

/**
 * Draws the swarm on every machine, including a listen server's host, as a handful of
 * instanced mesh draws.
 *
 * It reads AKBEnemyDirector::ReplicatedEnemies directly rather than hooking
 * FFastArraySerializer's delta callbacks. That array is the authoritative render source on
 * the server (the director maintains it) and the replicated mirror on a client, so one code
 * path covers both, and it sidesteps callbacks that are non-virtual, dispatched through the
 * concrete serializer type, and hand out indices valid only for the duration of the call.
 *
 * O(n) reconcile at ~500 entries is negligible next to a frame.
 */
UCLASS(ClassGroup = (KillBugs), meta = (BlueprintSpawnableComponent))
class KILLBUGS_API UKBEnemyVisualizerComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UKBEnemyVisualizerComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * Time for a drawn bug to cover the distance to its last replicated position.
	 *
	 * Keep this at roughly AKBEnemyDirector::NetSyncInterval * NetStaggerStride. Too short and
	 * bugs visibly stutter between updates; too long and they swim behind reality.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Swarm")
	float InterpolationTime = 0.13f;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Swarm")
	int32 GetRenderedBugCount() const { return Views.Num(); }

	/**
	 * Read-only access for the HUD, which floats health bars over damaged bugs.
	 *
	 * The HUD must use these views rather than the replicated positions: a view holds the
	 * INTERPOLATED location actually being drawn, so the bar tracks the bug instead of
	 * lagging a network update behind it.
	 */
	const TMap<int32, FKBEnemyView>& GetViews() const { return Views; }


protected:
	void EnsureInstanceComponents(const AKBEnemyDirector* Director);
	void PushToInstances();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> ArchetypeInstances;

	/**
	 * Per archetype, indexed by instance slot. Live bugs occupy their own slot; slots of dead
	 * bugs hold a hidden transform and go on a free list to be handed to the next spawn.
	 *
	 * The pool only ever grows to the peak concurrent count, so it stays bounded, and no
	 * instance is ever removed - which is what keeps every slot's identity stable.
	 */
	TArray<TArray<FTransform>> InstanceTransforms;
	TArray<TArray<int32>> FreeInstanceIndices;

	/** Transform that makes an instance invisible: zero scale, and far below the arena. */
	static FTransform GetHiddenTransform();

	/** Claims a slot in ArchetypeIndex's pool, growing the instanced mesh if needed. */
	int32 AcquireInstanceSlot(int32 ArchetypeIndex);

	/** Returns a slot to the free list and hides its instance. */
	void ReleaseInstanceSlot(int32 ArchetypeIndex, int32 InstanceIndex);

	/** Live bug render state, keyed by AKBEnemyNetItem::StableId. */
	TMap<int32, FKBEnemyView> Views;

	int32 ReconcileFrame = 0;

	// Perf log accumulators; driven by CVarKBSwarmPerfLog (see KBConsoleVariables.h).
	// On a client these measure the cost of turning the replicated swarm into instances,
	// which is the other half of the swarm's frame budget.
	double PerfSeconds = 0.0;
	int32 PerfFrames = 0;
	float PerfElapsed = 0.f;
};
