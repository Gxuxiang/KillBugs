#include "Swarm/KBSwarmTypes.h"

void FKBEnemyGrid::Reset(float InCellSize, const FVector& ArenaCentre, float ArenaHalfExtent)
{
	CellSize = FMath::Max(1.f, InCellSize);
	CellsX = FMath::Max(1, FMath::CeilToInt((ArenaHalfExtent * 2.f) / CellSize));
	CellsY = CellsX;

	// FloorToInt on (Location - GridOrigin) / CellSize lands cell 0 at the arena's low corner.
	GridOrigin = FVector(ArenaCentre.X - ArenaHalfExtent, ArenaCentre.Y - ArenaHalfExtent, 0.f);

	const int32 Total = CellsX * CellsY;
	CellCounts.SetNumZeroed(Total);
	CellStart.SetNumZeroed(Total + 1);
	Cursor.SetNumZeroed(Total);

	SortedIndex.Reset();
}

void FKBEnemyGrid::Rebuild(TArrayView<const FKBEnemySim> Enemies)
{
	const int32 Num = Enemies.Num();
	const int32 Total = CellsX * CellsY;
	if (Total <= 0)
	{
		return;
	}

	SortedIndex.SetNumZeroed(Num);
	if (Num == 0)
	{
		FMemory::Memzero(CellCounts.GetData(), Total * sizeof(int32));
		return;
	}

	// Pass 1 - count per cell. No clearing of nested arrays, no allocation.
	FMemory::Memzero(CellCounts.GetData(), Total * sizeof(int32));
	for (int32 Index = 0; Index < Num; ++Index)
	{
		int32 CellX = 0;
		int32 CellY = 0;
		if (WorldToCell(Enemies[Index].Location, CellX, CellY))
		{
			++CellCounts[CellIndex(CellX, CellY)];
		}
	}

	// Prefix sum into CellStart, seeding the write cursor as we go.
	int32 Running = 0;
	for (int32 Cell = 0; Cell < Total; ++Cell)
	{
		CellStart[Cell] = Running;
		Cursor[Cell] = Running;
		Running += CellCounts[Cell];
	}
	CellStart[Total] = Running;

	// Pass 2 - scatter. Enemies outside the grid are simply absent from SortedIndex, and
	// every consumer treats "not found" as "no neighbours".
	for (int32 Index = 0; Index < Num; ++Index)
	{
		int32 CellX = 0;
		int32 CellY = 0;
		if (WorldToCell(Enemies[Index].Location, CellX, CellY))
		{
			SortedIndex[Cursor[CellIndex(CellX, CellY)]++] = Index;
		}
	}
}

void FKBEnemyGrid::QuerySphere(const FVector& Centre, float Radius, int32 IgnoreIndex,
                               TArrayView<const FKBEnemySim> Enemies, TArray<int32>& Out) const
{
	const int32 Total = CellsX * CellsY;
	if (Total <= 0 || SortedIndex.Num() == 0)
	{
		return;
	}

	const float InvCellSize = 1.f / CellSize;
	const int32 MinX = FMath::Max(0, FMath::FloorToInt((Centre.X - Radius - GridOrigin.X) * InvCellSize));
	const int32 MaxX = FMath::Min(CellsX - 1, FMath::FloorToInt((Centre.X + Radius - GridOrigin.X) * InvCellSize));
	const int32 MinY = FMath::Max(0, FMath::FloorToInt((Centre.Y - Radius - GridOrigin.Y) * InvCellSize));
	const int32 MaxY = FMath::Min(CellsY - 1, FMath::FloorToInt((Centre.Y + Radius - GridOrigin.Y) * InvCellSize));

	const float RadiusSq = Radius * Radius;

	for (int32 CellY = MinY; CellY <= MaxY; ++CellY)
	{
		for (int32 CellX = MinX; CellX <= MaxX; ++CellX)
		{
			const int32 Cell = CellIndex(CellX, CellY);
			for (int32 S = CellStart[Cell]; S < CellStart[Cell + 1]; ++S)
			{
				const int32 Candidate = SortedIndex[S];
				if (Candidate == IgnoreIndex)
				{
					continue;
				}

				// The grid narrows the search; this is what makes the result an actual sphere
				// rather than "whatever shared a cell".
				const FVector Delta = Enemies[Candidate].Location - Centre;
				if (Delta.SizeSquared2D() <= RadiusSq)
				{
					Out.Add(Candidate);
				}
			}
		}
	}
}

bool FKBEnemyGrid::WorldToCell(const FVector& Location, int32& OutX, int32& OutY) const
{
	const int32 CellX = FMath::FloorToInt((Location.X - GridOrigin.X) / CellSize);
	const int32 CellY = FMath::FloorToInt((Location.Y - GridOrigin.Y) / CellSize);

	if (CellX < 0 || CellY < 0 || CellX >= CellsX || CellY >= CellsY)
	{
		return false;
	}

	OutX = CellX;
	OutY = CellY;
	return true;
}
