#include "UI/KBBackpackModel.h"

#include "Core/KBPlayerState.h"
#include "KBGameSettings.h"

bool KBIsUsableItem(EKBItemType Type)
{
	// Exhaustive on purpose: a new item type should force this question to be answered rather
	// than defaulting into "the hotbar accepts it and nothing happens when you press the key".
	switch (Type)
	{
	case EKBItemType::Medkit:
		return true;

	case EKBItemType::Material:
	default:
		return false;
	}
}

FString KBItemDisplayName(EKBItemType Type)
{
	switch (Type)
	{
	case EKBItemType::Medkit:
		return TEXT("药包");

	case EKBItemType::Material:
	default:
		return TEXT("材料");
	}
}

void BuildBackpackRows(const AKBPlayerState& PlayerState, TArray<FKBBackpackRow>& OutRows)
{
	OutRows.Reset();

	// A fixed order, so a row does not move under the cursor because something else was picked
	// up. Materials first: it is the bulk item and the one the player is usually looking for.
	const EKBItemType Ordered[] = { EKBItemType::Material, EKBItemType::Medkit };

	for (const EKBItemType Type : Ordered)
	{
		const int32 Count = Type == EKBItemType::Medkit
			? PlayerState.GetMedkits()
			: PlayerState.GetMaterials();

		if (Count <= 0)
		{
			continue;
		}

		const int32 UnitWeight = Type == EKBItemType::Medkit
			? FMath::Max(0, KBSettings().MedkitWeight)
			: FMath::Max(0, KBSettings().MaterialUnitWeight);

		FKBBackpackRow& Row = OutRows.AddDefaulted_GetRef();
		Row.Type = Type;
		Row.Count = Count;
		Row.Label = KBItemDisplayName(Type);
		Row.RightLabel = FString::Printf(TEXT("x%d     重 %d"), Count, Count * UnitWeight);
		Row.bCanEquip = KBIsUsableItem(Type);
	}
}
