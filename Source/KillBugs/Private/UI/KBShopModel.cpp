#include "UI/KBShopModel.h"

#include "Data/KBContentSubsystem.h"
#include "Data/KBWeaponDefinition.h"
#include "KBGameSettings.h"
#include "Persistence/KBProfileSubsystem.h"

namespace
{
	// Matches the lobby HUD's palette. Duplicated rather than shared because the two files are
	// merged by a unity build and a shared anonymous-namespace helper is exactly the collision
	// KBLobbyHud.cpp already documents.
	const FLinearColor ShopGood(0.36f, 0.74f, 0.46f, 1.f);
	const FLinearColor ShopBad(0.90f, 0.36f, 0.32f, 1.f);
	const FLinearColor ShopDim(0.62f, 0.66f, 0.74f, 1.f);
	const FLinearColor ShopFaint(0.38f, 0.41f, 0.48f, 1.f);
	const FLinearColor ShopInk(0.92f, 0.94f, 0.98f, 1.f);
}

void BuildShopRows(const UKBProfileSubsystem& Profile, const UKBContentSubsystem& Content,
                   TArray<FKBShopRow>& OutRows)
{
	OutRows.Reset();

	const int32 Gold = Profile.GetBankedGold();
	const int32 Materials = Profile.GetBankedMaterials();

	// ---- Selling: the one row that is not about a weapon --------------------------------------
	//
	// Hidden rather than disabled when there is nothing to sell: an inert "sell 0" line is noise,
	// and "no materials" is better said by the line's absence than by a row that does nothing.
	if (Materials > 0 && KBSettings().GoldPerMaterial > 0)
	{
		const int32 Yield = Materials * KBSettings().GoldPerMaterial;

		FKBShopRow& Row = OutRows.AddDefaulted_GetRef();
		Row.Action = EKBShopAction::SellAllMaterials;
		Row.Label = FString::Printf(TEXT("出售全部材料（%d）"), Materials);
		Row.RightLabel = FString::Printf(TEXT("+%d 金"), Yield);
		Row.RightColour = ShopGood;
		Row.bEnabled = true;
	}

	// ---- Owned weapons: what you bring, and what you can improve -------------------------------
	for (int32 Index = 0; Index < Profile.GetStash().Num(); ++Index)
	{
		const FKBSavedWeapon& Entry = Profile.GetStash()[Index];
		const UKBWeaponDefinition* Weapon = Cast<UKBWeaponDefinition>(Entry.Definition.TryLoad());
		if (!Weapon)
		{
			continue;
		}

		{
			FKBShopRow& Row = OutRows.AddDefaulted_GetRef();
			Row.Action = EKBShopAction::ToggleEquip;
			Row.StashIndex = Index;
			Row.Label = FString::Printf(TEXT("%s  Lv %d"), *Weapon->DisplayName.ToString(), Entry.Level);
			Row.RightLabel = Entry.bEquipped ? TEXT("已配装（点此卸下）") : TEXT("点此带上");
			Row.RightColour = Entry.bEquipped ? ShopGood : ShopDim;

			// Always enabled: removing something from the loadout can never fail, and adding one
			// only fails at the slot ceiling - which the refusal message explains when it happens.
			Row.bEnabled = true;
		}

		if (Entry.Level < Weapon->MaxLevel)
		{
			const int32 Cost = Weapon->GetUpgradeCost(Entry.Level);

			if (Cost <= 0)
			{
				FKBShopRow& Row = OutRows.AddDefaulted_GetRef();
				Row.Action = EKBShopAction::Upgrade;
				Row.StashIndex = Index;
				Row.Label = FString::Printf(TEXT("    %s 升级"), *Weapon->DisplayName.ToString());
				Row.RightLabel = TEXT("不可合成");
				Row.RightColour = ShopFaint;
				Row.bEnabled = false;
			}
			else
			{
				const int32 Short = Cost - Materials;

				FKBShopRow& Row = OutRows.AddDefaulted_GetRef();
				Row.Action = EKBShopAction::Upgrade;
				Row.StashIndex = Index;
				Row.Cost = Cost;
				Row.Label = FString::Printf(TEXT("    %s 升级到 %d 级"),
					*Weapon->DisplayName.ToString(), Entry.Level + 1);
				Row.RightLabel = Short > 0
					? FString::Printf(TEXT("合成 %d 材料（缺 %d）"), Cost, Short)
					: FString::Printf(TEXT("合成 %d 材料"), Cost);
				Row.RightColour = Short > 0 ? ShopBad : ShopGood;
				Row.bEnabled = Short <= 0;
			}
		}
		else
		{
			FKBShopRow& Row = OutRows.AddDefaulted_GetRef();
			Row.Action = EKBShopAction::None;
			Row.Label = FString::Printf(TEXT("    %s"), *Weapon->DisplayName.ToString());
			Row.RightLabel = TEXT("已满级");
			Row.RightColour = ShopFaint;
			Row.bEnabled = false;
		}
	}

	// ---- The catalogue: what gold can still buy ------------------------------------------------
	for (const TObjectPtr<UKBWeaponDefinition>& Entry : Content.GetAllWeapons())
	{
		const UKBWeaponDefinition* Weapon = Entry.Get();
		if (!Weapon || Profile.FindStashIndex(*Weapon) != INDEX_NONE)
		{
			continue;
		}

		// Unpriced weapons are omitted entirely rather than shown as free. A row that says
		// nothing is different from a row that says a weapon costs nothing.
		if (!Weapon->IsForSale())
		{
			continue;
		}

		const int32 Short = Weapon->BuyPriceGold - Gold;

		FKBShopRow& Row = OutRows.AddDefaulted_GetRef();
		Row.Action = EKBShopAction::Buy;
		Row.Weapon = const_cast<UKBWeaponDefinition*>(Weapon);
		Row.Cost = Weapon->BuyPriceGold;
		Row.Label = FString::Printf(TEXT("购买 %s"), *Weapon->DisplayName.ToString());
		Row.RightLabel = Short > 0
			? FString::Printf(TEXT("%d 金（缺 %d）"), Weapon->BuyPriceGold, Short)
			: FString::Printf(TEXT("%d 金"), Weapon->BuyPriceGold);
		Row.RightColour = Short > 0 ? ShopBad : ShopGood;
		Row.bEnabled = Short <= 0;
	}

	// ---- Nothing at all ------------------------------------------------------------------------
	if (OutRows.Num() == 0)
	{
		FKBShopRow& Row = OutRows.AddDefaulted_GetRef();
		Row.Label = TEXT("仓库是空的，也没有可购买的武器");
		Row.RightColour = ShopInk;
		Row.bEnabled = false;
	}
}
