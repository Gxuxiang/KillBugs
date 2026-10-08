#include "Swarm/KBFlowField.h"

#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/World.h"

namespace
{
	/**
	 * The eight neighbour offsets, orthogonals first.
	 *
	 * The order is part of the contract, not an implementation detail: the direction lookup takes
	 * the first neighbour with a STRICTLY smaller distance, so a fixed order is what makes the
	 * result deterministic for a given field. Ties would otherwise resolve differently per run,
	 * and a bug that jitters between two equally good directions reads as broken.
	 */
	constexpr int32 NeighbourOffsets[8][2] =
	{
		{  1,  0 }, { -1,  0 }, {  0,  1 }, {  0, -1 },
		{  1,  1 }, {  1, -1 }, { -1,  1 }, { -1, -1 }
	};

	/** How far the nearest-open-cell search looks before giving up, in cells. */
	constexpr int32 NearestOpenSearchRadius = 8;

	FORCEINLINE bool IsDiagonal(int32 DX, int32 DY)
	{
		return DX != 0 && DY != 0;
	}
}

void FKBFlowField::Reset(float InCellSize, const FVector& ArenaCentre, float ArenaHalfExtent)
{
	// Derived exactly the way FKBEnemyGrid::Reset derives its own rectangle, so the two can
	// never disagree about how big the arena is. That size is coupled to FLOOR_HALF_SIZE in
	// Tools/kb_setup_arena.py - see the tooltip on UKBGameSettings::ArenaHalfExtent.
	CellSize = FMath::Max(1.f, InCellSize);
	CellsX = FMath::Max(1, FMath::CeilToInt((ArenaHalfExtent * 2.f) / CellSize));
	CellsY = CellsX;
	Origin = FVector(ArenaCentre.X - ArenaHalfExtent, ArenaCentre.Y - ArenaHalfExtent, 0.f);

	const int32 Total = CellsX * CellsY;
	Blocked.Reset();
	Blocked.SetNumZeroed(Total);
	Distance.Reset();
	Distance.SetNumZeroed(Total);
	Queue.Reset();
	Queue.Reserve(Total);

	Sources.Reset();
	BlockedCount = 0;
	ReachableCount = 0;
	bObstaclesDirty = true;
	bSolved = false;
}

// ---------------------------------------------------------------------------------------
// Obstacles
// ---------------------------------------------------------------------------------------

int32 FKBFlowField::BakeObstacles(UWorld* World, float ProbeHeight)
{
	if (!World || !IsValid())
	{
		return 0;
	}

	// The probe is a whole cell wide and only 20 tall. A thin slice rather than a cube so the
	// floor (a plane at Z=0) is never touched while a 400-tall wall always is.
	const FCollisionShape Probe = FCollisionShape::MakeBox(FVector(CellSize * 0.5f, CellSize * 0.5f, 10.f));

	FCollisionQueryParams Params(TEXT("KBFlowFieldBake"), /*bTraceComplex=*/false);

	BlockedCount = 0;
	for (int32 Y = 0; Y < CellsY; ++Y)
	{
		for (int32 X = 0; X < CellsX; ++X)
		{
			FVector Centre = GetCellCentre(X, Y);
			Centre.Z = ProbeHeight;

			const bool bBlocked = World->OverlapBlockingTestByChannel(
				Centre, FQuat::Identity, ECC_WorldStatic, Probe, Params);

			Blocked[CellIndex(X, Y)] = bBlocked ? 1 : 0;
			BlockedCount += bBlocked ? 1 : 0;
		}
	}

	bObstaclesDirty = false;

	// The distance field describes the previous mask; refuse to serve it until it is rebuilt.
	bSolved = false;

	return BlockedCount;
}

void FKBFlowField::ClearObstacles()
{
	FMemory::Memzero(Blocked.GetData(), Blocked.Num() * sizeof(uint8));
	BlockedCount = 0;
	bObstaclesDirty = false;
	bSolved = false;
}

void FKBFlowField::InjectBlockedCells(const TArray<FIntPoint>& Cells)
{
	for (const FIntPoint& Cell : Cells)
	{
		if (!InBounds(Cell.X, Cell.Y) || Blocked[CellIndex(Cell.X, Cell.Y)])
		{
			continue;
		}

		Blocked[CellIndex(Cell.X, Cell.Y)] = 1;
		++BlockedCount;
	}

	bSolved = false;
}

void FKBFlowField::ForEachBlockedCell(TFunctionRef<void(int32, int32)> Fn) const
{
	for (int32 Y = 0; Y < CellsY; ++Y)
	{
		for (int32 X = 0; X < CellsX; ++X)
		{
			if (Blocked[CellIndex(X, Y)])
			{
				Fn(X, Y);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------
// Solve
// ---------------------------------------------------------------------------------------

void FKBFlowField::SetSources(const TArray<FVector>& SourceLocations)
{
	Sources = SourceLocations;
}

void FKBFlowField::Rebuild()
{
	const int32 Total = CellsX * CellsY;
	if (Total <= 0)
	{
		return;
	}

	Distance.Init(-1, Total);
	ReachableCount = 0;
	bSolved = false;

	Queue.Reset();

	// ---- Seed the frontier with every source ----
	// A source inside geometry resolves outward first: a player pushed into a wall by the swarm
	// should still pull bugs toward the hole they can actually reach, not invalidate the field.
	for (const FVector& Source : Sources)
	{
		int32 X = 0;
		int32 Y = 0;
		if (!WorldToCell(Source, X, Y))
		{
			continue;
		}

		if (Blocked[CellIndex(X, Y)])
		{
			int32 OpenX = 0;
			int32 OpenY = 0;
			if (!FindNearestOpenCell(X, Y, NearestOpenSearchRadius, OpenX, OpenY))
			{
				continue;
			}
			X = OpenX;
			Y = OpenY;
		}

		const int32 Index = CellIndex(X, Y);
		if (Distance[Index] == 0)
		{
			continue; // already seeded by an earlier source in the same cell
		}

		Distance[Index] = 0;
		Queue.Add(Index);
		++ReachableCount;
	}

	// ---- Flood ----
	// Uniform cost, so a plain queue is enough and the result is Chebyshev distance. The corner
	// rule is what keeps the field honest around geometry: a diagonal step is only taken when
	// both orthogonal neighbours are open, so a bug can never be routed through a gap that is
	// actually two walls meeting at a corner.
	//
	// That rule is ALSO what makes the direction lookup safe: every diagonal that holds a
	// distance was reached legally, so a strictly smaller diagonal neighbour is always a move
	// the swarm may actually make. Hence no second corner check there.
	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const int32 Index = Queue[Head];
		const int32 CellX = Index % CellsX;
		const int32 CellY = Index / CellsX;
		const int32 Next = Distance[Index] + 1;

		for (const auto& Offset : NeighbourOffsets)
		{
			const int32 NX = CellX + Offset[0];
			const int32 NY = CellY + Offset[1];
			if (!InBounds(NX, NY))
			{
				continue;
			}

			if (IsDiagonal(Offset[0], Offset[1])
				&& (Blocked[CellIndex(CellX + Offset[0], CellY)] || Blocked[CellIndex(CellX, CellY + Offset[1])]))
			{
				continue;
			}

			const int32 NIndex = CellIndex(NX, NY);
			if (Blocked[NIndex] || Distance[NIndex] >= 0)
			{
				continue;
			}

			Distance[NIndex] = Next;
			Queue.Add(NIndex);
			++ReachableCount;
		}
	}

	bSolved = true;
}

// ---------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------

bool FKBFlowField::WorldToCell(const FVector& Location, int32& OutX, int32& OutY) const
{
	const int32 CellX = FMath::FloorToInt((Location.X - Origin.X) / CellSize);
	const int32 CellY = FMath::FloorToInt((Location.Y - Origin.Y) / CellSize);

	if (!InBounds(CellX, CellY))
	{
		return false;
	}

	OutX = CellX;
	OutY = CellY;
	return true;
}

bool FKBFlowField::IsCellBlocked(int32 X, int32 Y) const
{
	return InBounds(X, Y) && Blocked[CellIndex(X, Y)] != 0;
}

bool FKBFlowField::IsBlockedAt(const FVector& Location) const
{
	int32 X = 0;
	int32 Y = 0;
	return WorldToCell(Location, X, Y) && IsCellBlocked(X, Y);
}

int32 FKBFlowField::GetDistanceAt(int32 X, int32 Y) const
{
	return InBounds(X, Y) ? Distance[CellIndex(X, Y)] : -1;
}

FVector FKBFlowField::GetCellCentre(int32 X, int32 Y) const
{
	return Origin + FVector((X + 0.5f) * CellSize, (Y + 0.5f) * CellSize, 0.f);
}

bool FKBFlowField::GetDirectionAtCell(int32 X, int32 Y, FVector& OutDirection) const
{
	if (!bSolved || !InBounds(X, Y) || Blocked[CellIndex(X, Y)])
	{
		return false;
	}

	const int32 Here = Distance[CellIndex(X, Y)];
	if (Here < 0)
	{
		return false;
	}

	// Strictly smaller only: equal distance means "sideways", which is progress toward nothing.
	// The first match in a fixed order wins, so the answer is reproducible frame to frame.
	int32 BestDistance = Here;
	int32 BestX = INDEX_NONE;
	int32 BestY = INDEX_NONE;

	for (const auto& Offset : NeighbourOffsets)
	{
		const int32 NX = X + Offset[0];
		const int32 NY = Y + Offset[1];
		if (!InBounds(NX, NY))
		{
			continue;
		}

		const int32 NDistance = Distance[CellIndex(NX, NY)];
		if (NDistance >= 0 && NDistance < BestDistance)
		{
			BestDistance = NDistance;
			BestX = NX;
			BestY = NY;
			// No break: a later offset may be strictly better still.
		}
	}

	if (BestX == INDEX_NONE)
	{
		// Standing on a source, so there is nowhere closer to point at.
		return false;
	}

	OutDirection = (GetCellCentre(BestX, BestY) - GetCellCentre(X, Y)).GetSafeNormal();
	return true;
}

bool FKBFlowField::GetDirection(const FVector& Location, FVector& OutDirection) const
{
	if (!bSolved)
	{
		return false;
	}

	int32 X = 0;
	int32 Y = 0;
	if (!WorldToCell(Location, X, Y))
	{
		return false; // outside the arena: not our problem, the seek path handles it
	}

	if (!Blocked[CellIndex(X, Y)] && GetDirectionAtCell(X, Y, OutDirection))
	{
		return true;
	}

	// Inside geometry. Walk out to the nearest open cell and follow the field from there, which
	// is what lets a bug that spawned in a wall leave on its own.
	int32 OpenX = 0;
	int32 OpenY = 0;
	if (!FindNearestOpenCell(X, Y, NearestOpenSearchRadius, OpenX, OpenY))
	{
		return false;
	}

	return GetDirectionAtCell(OpenX, OpenY, OutDirection);
}

bool FKBFlowField::FindNearestOpenCell(int32 X, int32 Y, int32 MaxRadius, int32& OutX, int32& OutY) const
{
	if (InBounds(X, Y) && !Blocked[CellIndex(X, Y)])
	{
		OutX = X;
		OutY = Y;
		return true;
	}

	// Square rings out to MaxRadius. The first ring that yields anything wins, and within a ring
	// the scan order is fixed, so the answer does not depend on iteration order.
	for (int32 Radius = 1; Radius <= MaxRadius; ++Radius)
	{
		for (int32 OffsetY = -Radius; OffsetY <= Radius; ++OffsetY)
		{
			for (int32 OffsetX = -Radius; OffsetX <= Radius; ++OffsetX)
			{
				// Ring only: the interior was already rejected by an earlier radius.
				if (FMath::Abs(OffsetX) != Radius && FMath::Abs(OffsetY) != Radius)
				{
					continue;
				}

				const int32 NX = X + OffsetX;
				const int32 NY = Y + OffsetY;
				if (InBounds(NX, NY) && !Blocked[CellIndex(NX, NY)])
				{
					OutX = NX;
					OutY = NY;
					return true;
				}
			}
		}
	}

	return false;
}

bool FKBFlowField::LineOfSightIsClear(const FVector& Start, const FVector& End) const
{
	int32 X0 = 0;
	int32 Y0 = 0;
	int32 X1 = 0;
	int32 Y1 = 0;
	if (!WorldToCell(Start, X0, Y0) || !WorldToCell(End, X1, Y1))
	{
		return true;
	}

	if (X0 == X1 && Y0 == Y1)
	{
		return true; // one cell, and the start cell is skipped by contract
	}

	// Amanatides-Woo: march from boundary to boundary, one cell per step, instead of sampling
	// the segment. At 100 cm cells a 3000 cm sight line costs ~30 steps rather than the 60+ a
	// half-cell sampling loop would need, and it cannot step over a cell the way sampling can.
	const float DX = End.X - Start.X;
	const float DY = End.Y - Start.Y;

	const int32 StepX = DX > 0.f ? 1 : (DX < 0.f ? -1 : 0);
	const int32 StepY = DY > 0.f ? 1 : (DY < 0.f ? -1 : 0);

	const float Inf = TNumericLimits<float>::Max();

	auto FirstBoundaryT = [this](float StartComponent, float OriginComponent, int32 Cell, int32 Step,
	                             float Delta) -> float
	{
		if (Step == 0)
		{
			return TNumericLimits<float>::Max();
		}

		const float Boundary = OriginComponent + (Step > 0 ? (Cell + 1) : Cell) * CellSize;
		return (Boundary - StartComponent) / Delta;
	};

	float TMaxX = FirstBoundaryT(Start.X, Origin.X, X0, StepX, DX);
	float TMaxY = FirstBoundaryT(Start.Y, Origin.Y, Y0, StepY, DY);
	const float TDeltaX = StepX != 0 ? FMath::Abs(CellSize / DX) : Inf;
	const float TDeltaY = StepY != 0 ? FMath::Abs(CellSize / DY) : Inf;

	int32 X = X0;
	int32 Y = Y0;

	// Bounded by construction, but a guard keeps a degenerate delta (both components zero, which
	// the equal-cell early-out above does not catch when the points differ only in Z) from ever
	// spinning here.
	const int32 MaxSteps = (CellsX + CellsY) * 4;
	for (int32 Step = 0; Step < MaxSteps; ++Step)
	{
		if (TMaxX < TMaxY)
		{
			X += StepX;
			TMaxX += TDeltaX;
		}
		else
		{
			Y += StepY;
			TMaxY += TDeltaY;
		}

		if (!InBounds(X, Y))
		{
			return true; // left the grid: permissive, never freeze a bug over an edge case
		}

		// Checked before the arrival test on purpose: a target standing inside geometry must read
		// as blocked, so the bug takes the field branch instead of pressing into the wall.
		if (Blocked[CellIndex(X, Y)])
		{
			return false;
		}

		if (X == X1 && Y == Y1)
		{
			return true;
		}
	}

	return true;
}

void FKBFlowField::ForEachDirectionSample(int32 Stride,
                                          TFunctionRef<void(const FVector&, const FVector&)> Fn) const
{
	if (!bSolved || Stride < 1)
	{
		return;
	}

	for (int32 Y = 0; Y < CellsY; Y += Stride)
	{
		for (int32 X = 0; X < CellsX; X += Stride)
		{
			FVector Direction;
			if (GetDirectionAtCell(X, Y, Direction))
			{
				Fn(GetCellCentre(X, Y), Direction);
			}
		}
	}
}
