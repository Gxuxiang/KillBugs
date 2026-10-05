#pragma once

#include "CoreMinimal.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "KBLootTypes.generated.h"

/** What is lying on the ground. */
UENUM(BlueprintType)
enum class EKBItemType : uint8
{
	/** The reason to search. Carried for the run and banked only by a successful extraction. */
	Material UMETA(DisplayName = "Material"),

	/** Relief, not the goal: picked up on contact and spent immediately. */
	Medkit   UMETA(DisplayName = "Medkit")
};

/**
 * One thing lying on the ground.
 *
 * No `Count` companion on the server, no health, no velocity - a drop has no state of its own
 * beyond where it is. That is why the replicated array below is the single source of truth on
 * both sides, unlike the swarm which needs a server-only simulation array beside it.
 */
USTRUCT()
struct FKBItemNetItem : public FFastArraySerializerItem
{
	GENERATED_BODY()

	/** Stable across replication; the visual pass matches on it rather than on index. */
	UPROPERTY()
	int32 StableId = 0;

	UPROPERTY()
	FVector_NetQuantize Location = FVector::ZeroVector;

	/** EKBItemType. A uint8 rather than the enum so the wire layout is explicit. */
	UPROPERTY()
	uint8 Type = 0;

	UPROPERTY()
	int32 Count = 1;
};

/**
 * THE replicated drop list. One property on one always-relevant Actor.
 *
 * The same shape as FKBEnemyArray, and for the same reason: the project has one model for world
 * state that several machines must agree on, and a few dozen drops are the simplest possible
 * instance of it - a position, a type, a count. A replicated Actor per drop would work at this
 * scale, but it would be a second model for the same problem, with its own relevancy, channel
 * and spawn-during-tick questions, and nothing to show for it.
 */
USTRUCT()
struct FKBItemArray : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FKBItemNetItem> Items;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FKBItemNetItem, FKBItemArray>(
			Items, DeltaParms, *this);
	}

	// No PreReplicatedRemove / PostReplicatedAdd overrides, for the reasons FKBEnemyArray spells
	// out: the hooks are non-virtual, are called through the concrete serializer type (so a
	// signature typo means "never called"), and forbid mutating Items inside the remove hook.
	// Both sides read Items every frame instead.
};

template <>
struct TStructOpsTypeTraits<FKBItemArray> : public TStructOpsTypeTraitsBase2<FKBItemArray>
{
	enum { WithNetDeltaSerializer = true };
};
