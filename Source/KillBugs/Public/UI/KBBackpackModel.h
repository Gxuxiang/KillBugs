#pragma once

#include "CoreMinimal.h"
#include "Loot/KBLootTypes.h"

class AKBPlayerState;

/** What a row's right-click menu can offer. */
enum class EKBBackpackAction : uint8
{
	None,
	Equip, // put this item on the hotbar, so a number key can use it
	Drop   // put one of them on the ground
};

/**
 * One line of the in-run backpack panel.
 *
 * Built once per frame and used for BOTH drawing and hit-testing, the same contract the lobby's
 * shop rows follow: building the list twice is how a panel ends up acting on the row above the
 * one you clicked, and the failure is invisible until someone notices the wrong thing happened.
 *
 * Being a plain function over the PlayerState rather than something the HUD computes inline is
 * what makes it testable: `DrawHUD` never runs under -nullrhi, so a headless run can only ever
 * assert on this.
 */
struct FKBBackpackRow
{
	EKBItemType Type = EKBItemType::Material;

	/** How many are carried. Zero-count rows are not produced at all. */
	int32 Count = 0;

	/** Left column: the item's name. */
	FString Label;

	/** Right column: how many, and what it weighs in total. */
	FString RightLabel;

	/**
	 * Whether the menu offers "equip" for this row.
	 *
	 * Materials are currency - there is no verb that spends one from your hand - so they can only
	 * be dropped. Keeping this per row rather than switching on the type at the menu means the
	 * menu and the model cannot disagree about what is offered.
	 */
	bool bCanEquip = false;
};

/** Whether this item can be used from the hotbar at all. */
bool KBIsUsableItem(EKBItemType Type);

/** The Chinese name, in one place, so the panel, the hotbar and the logs agree. */
FString KBItemDisplayName(EKBItemType Type);

/**
 * One row per carried item type with a non-zero count, in a fixed order.
 *
 * Fixed order rather than "whatever order things were picked up": a list that reorders itself as
 * you play is a list you cannot click without reading it again every time.
 */
void BuildBackpackRows(const AKBPlayerState& PlayerState, TArray<FKBBackpackRow>& OutRows);
