#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Loot/KBLootTypes.h"
#include "KBLootDirector.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;
class UKBEnemyArchetype;

/**
 * Everything lying on the ground, in one Actor.
 *
 * The same shape as AKBEnemyDirector and AKBProjectileDirector: a flat array in a single
 * always-relevant Actor instead of an Actor per drop. Drops are the simplest case of that shape
 * - they never move, never tick, have no collision and no lifetime - so the array is not a
 * compromise here, it is the whole fit. There is deliberately NO server-only simulation array
 * beside it: a drop has no hidden state, so the replicated list is the single source of truth on
 * both machines.
 *
 * Pickup is a 2D distance test, not a physics overlap. The project has no overlap components
 * anywhere and one proximity idiom (`FVector::Dist2D`), and the radius is already curved by the
 * player's own stat sheet - UKBStatSheetComponent::GetPickupRadius has existed unused since
 * before this feature, and the 远见 card has been multiplying a number nothing read.
 */
UCLASS()
class KILLBUGS_API AKBLootDirector : public AActor
{
	GENERATED_BODY()

public:
	AKBLootDirector();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** The replicated list. Read-only for everyone but this class. */
	const FKBItemArray& GetDrops() const { return Drops; }

	/**
	 * Decides whether this death leaves something behind, and puts it there.
	 *
	 * Called by AKBEnemyDirector::ApplyDamageToEnemy, which is the only place that still knows
	 * the location and the archetype. Server only.
	 *
	 * The roll does NOT care who - if anyone - is credited with the kill. A drop is a property
	 * of the bug dying rather than a reward for killing it: weapons always pass a real killer,
	 * but contact damage and whatever environmental death comes later cannot be relied on to, and
	 * tying loot to credit would make those deaths silently drop nothing.
	 */
	void RollDropForDeath(const FVector& DeathLocation, const UKBEnemyArchetype& Archetype);

	/** Server only. Puts one drop in the world. Used by the roll and by KB.Loot.Spawn. */
	void SpawnDrop(const FVector& Location, EKBItemType Type, int32 Count);

	/** `KB.Backpack.SelfTest`. Lives here because this is what owns the ground drops it drives. */
	static void ConsoleBackpackSelfTest(const TArray<FString>& Args, UWorld* World);

	/** Server only. Removes every drop. Used by KB.Loot.List's sibling test paths. */
	void ClearDrops();

protected:
	virtual void BeginPlay() override;

	/** The replicated list. Fast-array delta serialized; see FKBItemArray. */
	UPROPERTY(Replicated)
	FKBItemArray Drops;

private:
	/**
	 * Server only. Walks every player and takes whatever they are standing on.
	 *
	 * Every frame rather than on a cadence: the cadence pattern in this project is for RATES
	 * (contact damage ticks every 0.6s because biting is a rate), whereas a pickup is an edge -
	 * half a second of lag would read as the game ignoring the player.
	 */
	void TickPickups();

	/** Rewrites the instanced meshes from the replicated list. Runs on every machine. */
	void UpdateVisuals();

	/** Resolves a drop mesh from settings, falling back to a visible placeholder. */
	UStaticMesh* ResolveMesh(EKBItemType Type);

	/** Gives the instances a material that can take this type's colour, and applies it. */
	void ApplyMaterial(UInstancedStaticMeshComponent* Instances, EKBItemType Type);

	UInstancedStaticMeshComponent* GetInstances(EKBItemType Type);

	/** One per item type, created on first use. */
	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> MaterialInstances;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> MedkitInstances;

	int32 NextStableId = 1;

	/** Set once the placeholder warning has been said, so it does not fill the log. */
	bool bWarnedAboutPlaceholderMesh = false;

	/** Same, for "the drop material takes no colour" - which would make the two types identical. */
	bool bWarnedAboutMissingTintParameter = false;

	/** Set once the drop cap has been hit, so the warning is said once per run, not per death. */
	bool bWarnedAboutDropCap = false;

	/**
	 * Throttles the "backpack is full, left it on the ground" line.
	 *
	 * A refusal happens per drop per frame, and a full backpack next to a pile of loot would
	 * write a line for every one of them every frame. One line a second says the same thing.
	 */
	float LastRefusalLogTime = -1000.f;
};
