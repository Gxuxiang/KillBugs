#include "CoreMinimal.h"

#include "Data/KBContentSubsystem.h"
#include "Data/KBWeaponDefinition.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "KBGameSettings.h"
#include "KillBugs.h"
#include "Persistence/KBProfileSubsystem.h"

/**
 * Console surface for the shop, so its RULES can be tested without the panel.
 *
 * -nullrhi never runs DrawHUD, so a headless run cannot press a button - the profile's Try*
 * methods are the surface under test, and these commands are how a test reaches them.
 *
 * Indices: `KB.Shop.List` prints the catalogue in the order the shop draws it (sorted by name),
 * and Buy takes an index into THAT list. Craft and Equip take a STASH index, printed next to
 * each owned weapon. The two are different lists and the command help says so.
 */

namespace KBShopCommands
{
	UKBProfileSubsystem* ResolveProfile(UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UKBProfileSubsystem>() : nullptr;
	}

	UKBContentSubsystem* ResolveContent(UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UKBContentSubsystem>() : nullptr;
	}

	const TCHAR* EquippedMark(bool bEquipped)
	{
		return bEquipped ? TEXT("[带]") : TEXT("   ");
	}

	void List(const TArray<FString>& Args, UWorld* World)
	{
		UKBProfileSubsystem* Profile = ResolveProfile(World);
		if (!Profile)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Shop.List: no profile subsystem"));
			return;
		}

		UE_LOG(LogKillBugs, Display, TEXT("KB Shop === %d gold | %d materials | max %d slots ==="),
			Profile->GetBankedGold(), Profile->GetBankedMaterials(), KBSettings().MaxWeaponSlots);

		UE_LOG(LogKillBugs, Display, TEXT("KB Shop --- stash (%d) ---"), Profile->GetStash().Num());
		for (int32 Index = 0; Index < Profile->GetStash().Num(); ++Index)
		{
			const FKBSavedWeapon& Entry = Profile->GetStash()[Index];
			const UKBWeaponDefinition* Definition = Cast<UKBWeaponDefinition>(Entry.Definition.TryLoad());

			UE_LOG(LogKillBugs, Display, TEXT("KB Shop   [%d] %s %s  Lv %d"),
				Index,
				EquippedMark(Entry.bEquipped),
				Definition ? *Definition->DisplayName.ToString() : *Entry.Definition.ToString(),
				Entry.Level);
		}

		UKBContentSubsystem* Content = ResolveContent(World);
		const TArray<TObjectPtr<UKBWeaponDefinition>>& Catalogue =
			Content ? Content->GetAllWeapons() : TArray<TObjectPtr<UKBWeaponDefinition>>();

		UE_LOG(LogKillBugs, Display, TEXT("KB Shop --- catalogue (%d) ---"), Catalogue.Num());
		for (int32 Index = 0; Index < Catalogue.Num(); ++Index)
		{
			const UKBWeaponDefinition* Weapon = Catalogue[Index].Get();
			if (!Weapon)
			{
				continue;
			}

			// Shown for every weapon, owned or not: the shop hides unpriced ones, and a test
			// needs to see that "not for sale" is a decision rather than a missing row.
			UE_LOG(LogKillBugs, Display, TEXT("KB Shop   <%d> %s  buy %s  upgrade %s"),
				Index, *Weapon->DisplayName.ToString(),
				Weapon->IsForSale() ? *FString::FromInt(Weapon->BuyPriceGold) : TEXT("不出售"),
				Weapon->BaseUpgradeCost > 0 ? *FString::FromInt(Weapon->BaseUpgradeCost) : TEXT("不可合成"));
		}
	}

	void Buy(const TArray<FString>& Args, UWorld* World)
	{
		UKBProfileSubsystem* Profile = ResolveProfile(World);
		UKBContentSubsystem* Content = ResolveContent(World);
		if (!Profile || !Content || Args.Num() < 1)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Shop.Buy: expected <catalogue index>"));
			return;
		}

		const int32 Index = FCString::Atoi(*Args[0]);
		if (!Content->GetAllWeapons().IsValidIndex(Index) || !Content->GetAllWeapons()[Index])
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Shop.Buy: no weapon at catalogue index %d"), Index);
			return;
		}

		FString Reason;
		const bool bDone = Profile->TryBuyWeapon(*Content->GetAllWeapons()[Index], Reason);
		UE_LOG(LogKillBugs, Display, TEXT("KB Shop: buy -> %s (%s)"),
			bDone ? TEXT("OK") : TEXT("refused"), *Reason);
	}

	void Craft(const TArray<FString>& Args, UWorld* World)
	{
		UKBProfileSubsystem* Profile = ResolveProfile(World);
		if (!Profile || Args.Num() < 1)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Shop.Craft: expected <stash index>"));
			return;
		}

		FString Reason;
		const bool bDone = Profile->TryUpgradeWeapon(FCString::Atoi(*Args[0]), Reason);
		UE_LOG(LogKillBugs, Display, TEXT("KB Shop: craft -> %s (%s)"),
			bDone ? TEXT("OK") : TEXT("refused"), *Reason);
	}

	void SellAll(const TArray<FString>& Args, UWorld* World)
	{
		UKBProfileSubsystem* Profile = ResolveProfile(World);
		if (!Profile)
		{
			return;
		}

		FString Reason;
		const bool bDone = Profile->TrySellAllMaterials(Reason);
		UE_LOG(LogKillBugs, Display, TEXT("KB Shop: sell all -> %s (%s)"),
			bDone ? TEXT("OK") : TEXT("refused"), *Reason);
	}

	void Equip(const TArray<FString>& Args, UWorld* World)
	{
		UKBProfileSubsystem* Profile = ResolveProfile(World);
		if (!Profile || Args.Num() < 1)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Shop.Equip: expected <stash index> [0|1]"));
			return;
		}

		const bool bEquip = Args.Num() < 2 || FCString::Atoi(*Args[1]) != 0;

		FString Reason;
		const bool bDone = Profile->SetWeaponEquipped(FCString::Atoi(*Args[0]), bEquip, Reason);
		UE_LOG(LogKillBugs, Display, TEXT("KB Shop: equip -> %s (%s)"),
			bDone ? TEXT("OK") : TEXT("refused"), *Reason);
	}

	/** Clears the loadout and equips exactly the given stash indices - the loadout in one shot. */
	void Loadout(const TArray<FString>& Args, UWorld* World)
	{
		UKBProfileSubsystem* Profile = ResolveProfile(World);
		if (!Profile)
		{
			return;
		}

		for (int32 Index = 0; Index < Profile->GetStash().Num(); ++Index)
		{
			FString Reason;
			Profile->SetWeaponEquipped(Index, false, Reason);
		}

		for (const FString& Arg : Args)
		{
			FString Reason;
			const bool bDone = Profile->SetWeaponEquipped(FCString::Atoi(*Arg), true, Reason);
			if (!bDone)
			{
				UE_LOG(LogKillBugs, Warning, TEXT("Shop.Loadout: [%s] refused (%s)"), *Arg, *Reason);
			}
		}

		UE_LOG(LogKillBugs, Display, TEXT("KB Shop: loadout set to %d weapon(s)"), Args.Num());
	}
}

static FAutoConsoleCommandWithWorldAndArgs KBConsoleShopList(
	TEXT("KB.Shop.List"),
	TEXT("KB.Shop.List - gold, materials, the stash (with indices), and the catalogue."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBShopCommands::List));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleShopBuy(
	TEXT("KB.Shop.Buy"),
	TEXT("KB.Shop.Buy <catalogue index> - buy with gold. The index is from KB.Shop.List's "
	     "CATALOGUE list, which is a different list from the stash."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBShopCommands::Buy));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleShopCraft(
	TEXT("KB.Shop.Craft"),
	TEXT("KB.Shop.Craft <stash index> - spend materials to raise an owned weapon one level."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBShopCommands::Craft));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleShopSellAll(
	TEXT("KB.Shop.SellAll"),
	TEXT("KB.Shop.SellAll - sell every material for gold."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBShopCommands::SellAll));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleShopEquip(
	TEXT("KB.Shop.Equip"),
	TEXT("KB.Shop.Equip <stash index> [0|1] - add to or remove from the loadout."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBShopCommands::Equip));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleShopLoadout(
	TEXT("KB.Shop.Loadout"),
	TEXT("KB.Shop.Loadout <stash index...> - clear the loadout, then equip exactly these."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBShopCommands::Loadout));
