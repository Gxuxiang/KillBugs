#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "KBEnemyVisualizerComponent.generated.h"

class UInstancedStaticMeshComponent;
class UAnimToTextureDataAsset;
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

	/**
	 * How far the instanced mesh's bounds are inflated beyond the rest pose.
	 *
	 * A baked vertex animation moves the vertices in the VERTEX SHADER, so the mesh's own bounds
	 * know nothing about it. With legs kicked out and a body bobbing, the drawn bug sticks out
	 * past the bounds it is culled against, and the swarm vanishes near the edge of the screen -
	 * bugs popping out of existence at exactly the moment the player looks at them.
	 *
	 * 2.0 is generous for a bug whose animation moves it by a fraction of its own length; the
	 * cost of being generous is a slightly larger bounding volume, which for a component that is
	 * never culled anyway is nothing.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Swarm", meta = (ClampMin = "1.0"))
	float BakedAnimationBoundsScale = 2.f;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Swarm")
	int32 GetRenderedBugCount() const { return Views.Num(); }

	/**
	 * Reports what per-instance animation data the instanced meshes are actually carrying.
	 *
	 * Written because the animation would not play and there was no way to tell WHICH half was
	 * broken - the data never being written, or the material never reading it. A headless run
	 * cannot render, so "look at it" is not available; this turns the question into a log line.
	 */
	void LogAnimationData() const;

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
	 * Baked animation per archetype, resolved once when the components are built.
	 *
	 * Kept because the per-instance phase is written when a slot is CLAIMED (a bug spawning),
	 * which happens in AcquireInstanceSlot - a long way from the archetype lookup that built the
	 * components, and at a point where going back to the director for it would be a lookup per
	 * spawn.
	 *
	 * A null entry means that archetype has no baked animation, and its bugs are simply drawn
	 * as static meshes with no per-instance data at all.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAnimToTextureDataAsset>> ArchetypeAnimData;

	/**
	 * The animation phase written to each slot, mirrored for the debug log.
	 *
	 * UInstancedStaticMeshComponent has no readable accessor for per-instance custom data, so
	 * there is no way to ask the component what it holds. Recording what was written is enough
	 * to tell the two failure modes apart: no data written at all, versus data written but the
	 * material ignoring it.
	 *
	 * Parallel to InstanceTransforms. Debug-only state that costs a float per pooled slot.
	 */
	TArray<TArray<float>> WrittenTimeOffsets;

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

	/**
	 * Gives a freshly created instance its own phase in the baked animation.
	 *
	 * Called once per slot, when the slot is born. See the implementation for why a recycled
	 * slot does not get a new phase.
	 */
	void WriteAnimationPhase(int32 ArchetypeIndex, int32 InstanceIndex);

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
