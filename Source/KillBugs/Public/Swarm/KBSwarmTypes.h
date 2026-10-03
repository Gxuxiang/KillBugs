#pragma once

#include "CoreMinimal.h"
#include "Engine/NetSerialization.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "KBSwarmTypes.generated.h"

/**
 * Server-side simulation state for one bug. A plain struct on purpose: it is never reflected,
 * never replicated directly and never an Actor. 500 of these cost well under 100 KB.
 */
struct FKBEnemySim
{
	FVector Location = FVector::ZeroVector;
	FVector Velocity = FVector::ZeroVector;

	float Health = 0.f;
	float MaxHealth = 0.f;

	/** Staggered: steering and target re-acquisition only run when the time is reached. */
	float NextThinkTime = 0.f;

	float HitFlashEndTime = 0.f;

	uint8 ArchetypeIndex = 0;
	uint8 StateFlags = 0;

	/** Survives array reordering; the client uses it to match instances across updates. */
	int32 StableId = 0;

	/**
	 * Weak on purpose. A player who quits mid-wave must not leave 500 enemies holding a
	 * dangling pointer - every read re-validates, and a stale entry falls back to a
	 * re-acquire rather than freezing the bug in place.
	 */
	TWeakObjectPtr<AActor> Target;

	float GetHealthPct() const
	{
		return MaxHealth > 0.f ? FMath::Clamp(Health / MaxHealth, 0.f, 1.f) : 0.f;
	}
};

/**
 * The replicated slice of one bug. Roughly 12 bytes on the wire, versus tens of bytes for a
 * full Actor channel plus FRepMovement.
 */
USTRUCT()
struct FKBEnemyNetItem : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY()
	int32 StableId = 0;

	UPROPERTY()
	FVector_NetQuantize Location = FVector::ZeroVector;

	/** Yaw packed to 16 bits; plenty for a bug. */
	UPROPERTY()
	uint16 Yaw = 0;

	UPROPERTY()
	uint8 ArchetypeIndex = 0;

	UPROPERTY()
	uint8 StateFlags = 0;

	UPROPERTY()
	uint8 HealthPct = 255;

	/** Remembers where we last told clients this bug was, to skip sub-threshold jitter. */
	FVector LastReplicatedLocation = FVector(FLT_MAX);

	static uint16 PackYaw(float YawDegrees)
	{
		// Map to 0..65535 over a full turn.
		const float Normalized = FRotator::NormalizeAxis(YawDegrees) / 360.f + 0.5f;
		return static_cast<uint16>(FMath::Clamp(Normalized, 0.f, 0.99999f) * 65535.f);
	}

	static float UnpackYaw(uint16 Packed)
	{
		return (static_cast<float>(Packed) / 65535.f - 0.5f) * 360.f;
	}
};

/**
 * THE replicated swarm. One property on one always-relevant Actor, instead of one Actor
 * channel per bug - which is what makes join-in-progress nearly free and keeps
 * ServerReplicateActors from walking 500 actors every update.
 */
USTRUCT()
struct FKBEnemyArray : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FKBEnemyNetItem> Items;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParms)
	{
		return FFastArraySerializer::FastArrayDeltaSerialize<FKBEnemyNetItem, FKBEnemyArray>(Items, DeltaParms, *this);
	}

	// Deliberately NO PreReplicatedRemove / PostReplicatedAdd / PostReplicatedChange overrides.
	//
	// This array is the render source on BOTH sides: on the server AKBEnemyDirector keeps it
	// up to date, and on a client replication writes the same field. So the visualizer simply
	// reads Items every frame and matches entries by StableId, and never needs to know which
	// indices changed.
	//
	// That matters because the engine's delta hooks are non-virtual, are invoked through the
	// concrete SerializerType (so a signature typo silently means "never called"), hand out
	// indices that are valid only for the duration of the call, and forbid mutating Items
	// inside PreReplicatedRemove (there is a size assertion right after it returns). Reading
	// the array costs O(n) at ~500 entries, which is free next to a frame.
};

template <>
struct TStructOpsTypeTraits<FKBEnemyArray> : public TStructOpsTypeTraitsBase2<FKBEnemyArray>
{
	enum
	{
		WithNetDeltaSerializer = true
	};
};

/**
 * Uniform grid over the arena, rebuilt from scratch every frame with a counting sort.
 *
 * No per-cell TArrays and no allocation after warm-up: two O(n) passes over flat index
 * buffers. Player counts are tiny (<= 4) so target acquisition just walks the player array;
 * the grid exists for separation and, later, weapon area queries.
 */
class FKBEnemyGrid
{
public:
	void Reset(float InCellSize, const FVector& ArenaCentre, float ArenaHalfExtent);
	void Rebuild(TArrayView<const FKBEnemySim> Enemies);

	/** Appends indices of enemies whose centre is within Radius of Centre, excluding IgnoreIndex. */
	void QuerySphere(const FVector& Centre, float Radius, int32 IgnoreIndex,
	                 TArrayView<const FKBEnemySim> Enemies, TArray<int32>& Out) const;

	int32 GetCellSize() const { return FMath::Max(1, static_cast<int32>(CellSize)); }

private:
	bool WorldToCell(const FVector& Location, int32& OutX, int32& OutY) const;
	int32 CellIndex(int32 X, int32 Y) const { return Y * CellsX + X; }

	float CellSize = 200.f;
	FVector GridOrigin = FVector::ZeroVector;
	int32 CellsX = 0;
	int32 CellsY = 0;

	TArray<int32> CellCounts;  // [CellsX*CellsY]
	TArray<int32> CellStart;   // [CellsX*CellsY+1] prefix sum
	TArray<int32> Cursor;      // [CellsX*CellsY] scratch during placement
	TArray<int32> SortedIndex; // [Num] enemy indices grouped by cell
};
