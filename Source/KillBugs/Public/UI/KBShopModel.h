#pragma once

#include "CoreMinimal.h"
#include "KBShopModel.generated.h"

class UKBContentSubsystem;
class UKBProfileSubsystem;
class UKBWeaponDefinition;

/** What clicking a shop row does. */
UENUM()
enum class EKBShopAction : uint8
{
	None,
	/** Turn every material into gold. */
	SellAllMaterials,
	/** Gold in, own the weapon at level 1. */
	Buy,
	/** Materials in, +1 level on a weapon already owned. */
	Upgrade,
	/** Add or remove an owned weapon from the loadout. */
	ToggleEquip
};

/**
 * One clickable line in the shop.
 *
 * Built once per frame by BuildShopRows and used for BOTH drawing and hit-testing, so the row
 * that gets drawn and the row that gets clicked can never disagree - which is the failure mode
 * of building the list twice and is invisible until someone clicks the wrong thing.
 *
 * One action per row. An owned weapon therefore produces up to two rows (equip, and upgrade when
 * it can be upgraded) rather than one row with two buttons - the Canvas vocabulary here is a
 * whole-row rect, and adding per-row sub-buttons would mean a second rect list and a second
 * hover slot for nothing the player needs.
 */
struct FKBShopRow
{
	EKBShopAction Action = EKBShopAction::None;

	/** Buy only. */
	UKBWeaponDefinition* Weapon = nullptr;

	/** Stash index for Upgrade and ToggleEquip. */
	int32 StashIndex = INDEX_NONE;

	/** Gold for Buy, materials for Upgrade. Zero for the other actions. */
	int32 Cost = 0;

	/** Left column: what this row is about. */
	FString Label;

	/** Right column: what clicking does, and what it costs. */
	FString RightLabel;

	FLinearColor RightColour = FLinearColor::White;

	/** False draws the row inert and refuses the click. */
	bool bEnabled = false;
};

/**
 * Fills OutRows with the current shop, in the order it is drawn.
 *
 * Reads the profile for what is owned and affordable, and the content subsystem for the
 * catalogue. Neither is modified - the profile is the only writer of currency and stash, and a
 * row is a question, not an answer.
 */
void BuildShopRows(const UKBProfileSubsystem& Profile, const UKBContentSubsystem& Content,
                   TArray<FKBShopRow>& OutRows);
