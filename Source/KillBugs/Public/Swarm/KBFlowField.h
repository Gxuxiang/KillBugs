#pragma once

#include "CoreMinimal.h"

/**
 * A grid-aligned "which way is the nearest player" field for the swarm.
 *
 * The swarm steers with custom seek + separation and never touches NavMesh - that is the whole
 * reason 600 bugs fit in a tick. It also means nothing in the simulation knows a wall exists:
 * the only thing keeping bugs inside the arena is a coordinate clamp. This is the missing piece,
 * and it is deliberately NOT the engine's navigation: it is a uniform grid over the arena, a
 * breadth-first search out from the players, and a direction read at the bug's cell.
 *
 * Two lifetimes, and they are different on purpose:
 *   - the BLOCKED mask comes from the world (Baked) and only changes when geometry does
 *   - the DISTANCE field comes from wherever the players are and is rebuilt on a fixed cadence
 * Baking traces the world's collision on a grid, so a procedurally built maze needs no
 * bookkeeping at all - build the geometry, then call AKBEnemyDirector::InvalidateFlowObstacles().
 * Forgetting that call is silent: everything runs, and the field quietly describes the old arena.
 *
 * Server-only. Clients never simulate the swarm, so none of this replicates.
 */
struct FKBFlowField
{
public:
	/**
	 * Sizes the grid and allocates everything. Call once; nothing below allocates afterwards.
	 *
	 * The rectangle is derived from the arena half-extent exactly the way FKBEnemyGrid derives
	 * its own, so the two can never disagree about how big the arena is - and that value is
	 * coupled to Tools/kb_setup_arena.py's FLOOR_HALF_SIZE (see UKBGameSettings::ArenaHalfExtent).
	 */
	void Reset(float InCellSize, const FVector& ArenaCentre, float ArenaHalfExtent);

	bool IsValid() const { return CellsX > 0 && CellsY > 0; }

	// ---- Obstacles ------------------------------------------------------------------------

	/**
	 * Samples the world's collision into the blocked mask. Returns the number of blocked cells.
	 *
	 * One box overlap per cell, at ProbeHeight, on ECC_WorldStatic. A whole-cell box rather than
	 * a point sample: the arena's walls are a known thickness and their inner faces land exactly
	 * on a cell centre, so a point probe would decide a whole border ring on floating-point luck.
	 * Covering the cell also means a wall that only clips a cell's corner still blocks it, which
	 * is the clearance the swarm wants and saves a separate dilation pass.
	 *
	 * ProbeHeight must be above the floor and below the top of a wall (100 sits in the middle of
	 * both). Note the coupling this creates: ANY obstacle that does not block ECC_WorldStatic is
	 * invisible to the swarm. There is no project collision channel to extend yet.
	 */
	int32 BakeObstacles(UWorld* World, float ProbeHeight);

	/** Forgets every obstacle without touching the distance field. Test hook. */
	void ClearObstacles();

	/** Marks the mask stale. The next refresh bakes before it solves. */
	void InvalidateObstacles() { bObstaclesDirty = true; }
	bool NeedsObstacleBake() const { return bObstaclesDirty; }
	bool HasObstacles() const { return BlockedCount > 0; }

	// ---- Solve ----------------------------------------------------------------------------

	/**
	 * Rebuilds the distance field, breadth-first, from every source at once.
	 *
	 * Multi-source is not a convenience: it reproduces the swarm's actual targeting rule
	 * ("chase whichever player is nearest") in one solve instead of one solve per player, and
	 * the distances come out relative to the nearest source by construction.
	 */
	void SetSources(const TArray<FVector>& SourceLocations);
	void Rebuild();
	bool IsSolved() const { return bSolved; }

	// ---- Queries --------------------------------------------------------------------------

	bool WorldToCell(const FVector& Location, int32& OutX, int32& OutY) const;
	bool IsCellBlocked(int32 X, int32 Y) const;
	bool IsBlockedAt(const FVector& Location) const;
	FVector GetCellCentre(int32 X, int32 Y) const;

	/** One of eight cell directions, or false when the cell is blocked, unreachable, or a source. */
	bool GetDirectionAtCell(int32 X, int32 Y, FVector& OutDirection) const;

	/**
	 * The cell direction at Location, resolving through the nearest open cell when the sample
	 * lands inside geometry (a bug shoved into a wall, or a player standing in one).
	 */
	bool GetDirection(const FVector& Location, FVector& OutDirection) const;

	/**
	 * True when no blocked cell is crossed between the two points.
	 *
	 * THE START CELL IS SKIPPED, and that is load-bearing rather than an optimisation:
	 *  - a bug pressed against the arena's border wall still sees its target, so it keeps the
	 *    plain seek it has always used and nothing about the open-field game changes;
	 *  - a bug that ends up INSIDE geometry has blocked cells after its start cell, so it takes
	 *    the field branch and walks itself out - no special case at spawn time.
	 * An out-of-grid endpoint counts as clear: a physics edge case must never freeze a bug.
	 */
	bool LineOfSightIsClear(const FVector& Start, const FVector& End) const;

	// ---- Diagnostics and test hooks -------------------------------------------------------

	float GetCellSize() const { return CellSize; }
	int32 GetCellsX() const { return CellsX; }
	int32 GetCellsY() const { return CellsY; }
	int32 GetBlockedCellCount() const { return BlockedCount; }
	int32 GetReachableCellCount() const { return ReachableCount; }
	int32 GetDistanceAt(int32 X, int32 Y) const;
	const TArray<FVector>& GetSources() const { return Sources; }

	/** Blocks cells directly, bypassing the world. The only way to build a reproducible corridor. */
	void InjectBlockedCells(const TArray<FIntPoint>& Cells);

	void ForEachBlockedCell(TFunctionRef<void(int32 X, int32 Y)> Fn) const;

	/** Calls Fn(CellCentre, Direction) on a lattice, for the debug draw. */
	void ForEachDirectionSample(int32 Stride, TFunctionRef<void(const FVector&, const FVector&)> Fn) const;

private:
	int32 CellIndex(int32 X, int32 Y) const { return Y * CellsX + X; }
	bool InBounds(int32 X, int32 Y) const { return X >= 0 && Y >= 0 && X < CellsX && Y < CellsY; }

	bool FindNearestOpenCell(int32 X, int32 Y, int32 MaxRadius, int32& OutX, int32& OutY) const;

	float CellSize = 0.f;
	FVector Origin = FVector::ZeroVector;
	int32 CellsX = 0;
	int32 CellsY = 0;

	TArray<uint8> Blocked;   // [CellsX*CellsY], 1 = the swarm may not enter this cell
	TArray<int32> Distance;  // [CellsX*CellsY], steps to the nearest source, -1 = blocked/unreachable
	TArray<int32> Queue;     // BFS frontier, reused so Rebuild never allocates

	TArray<FVector> Sources;
	int32 BlockedCount = 0;
	int32 ReachableCount = 0;
	bool bObstaclesDirty = true;
	bool bSolved = false;
};
