#include "Persistence/KBProfileSubsystem.h"

#include "Core/KBPlayerState.h"
#include "Data/KBWeaponDefinition.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "KBGameSettings.h"
#include "KillBugs.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "Persistence/KBSaveGame.h"
#include "UObject/ObjectVersion.h"

namespace
{
	/** "GVAS" read as a little-endian uint32. See LooksLikeASaveGame. */
	constexpr int32 SaveGameFileTypeTag = 0x53415647;

	/**
	 * Whether the file on disk even claims to be a save game.
	 *
	 * This exists because the engine does NOT reject a file it does not recognise - it assumes it
	 * is a very old save and re-reads it from offset 0. From FSaveGameHeader::Read:
	 *
	 *     if (FileTypeTag != UE_SAVEGAME_FILE_TYPE_TAG)
	 *     {
	 *         // This is a very old saved game, back up the file pointer to the beginning and
	 *         // assume version 1. This is unlikely to work without additional licensee-specific
	 *         // modifications to this code
	 *         MemoryReader.Seek(0);
	 *         ...
	 *
	 * That fallback is what makes a foreign file dangerous: the old-format path then reads
	 * lengths out of whatever bytes are there. Measured, not assumed - a 22-byte text file in the
	 * slot crashed the game during startup with "FName's 1023 max length exceeded. Got 544501613
	 * characters", inside LoadGameFromSlot and before a return value existed for any Cast to
	 * guard against. The engine's own comment above calls it unlikely to work; this is the caller
	 * declining to find out.
	 *
	 * It catches a replaced, zeroed or foreign file, which is what a person or a bad copy
	 * produces. It cannot catch damage further into a file whose header is intact.
	 */
	bool LooksLikeASaveGame(const FString& SlotName, int32 UserIndex)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("SaveGames") /
			FString::Printf(TEXT("%s.sav"), *SlotName);

		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader)
		{
			return false;
		}

		int32 Tag = 0;
		*Reader << Tag;

		return Tag == SaveGameFileTypeTag;
	}
}

void UKBProfileSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LoadProfile();
}

void UKBProfileSubsystem::LoadProfile()
{
	const FString Slot = GetSlotName();
	const int32 User = GetUserIndex();

	BankedGold = 0;
	BankedMaterials = 0;
	Stash.Reset();

	if (!UGameplayStatics::DoesSaveGameExist(Slot, User))
	{
		// A brand new machine, and the ONLY moment a player is given anything for free.
		//
		// This is not a fallback for an empty stash - it is a first-run grant, and the difference
		// matters. The design makes a wipe cost the weapons you carried, with no floor under it,
		// so "if the stash is empty, hand out the starters" would quietly cancel the stake every
		// time it was about to bite. A player with no save has no weapon and cannot play at all;
		// a player who lost everything has lost it.
		SeedStarterWeapons();
		SaveProfile();

		UE_LOG(LogKillBugs, Display,
			TEXT("KBProfile: no profile at '%s' (user %d) - new profile, granted %d starter weapon(s)"),
			*Slot, User, Stash.Num());
		return;
	}

	// Checked before the engine is allowed near it - see LooksLikeASaveGame for why the Cast
	// below is not enough on its own.
	if (!LooksLikeASaveGame(Slot, User))
	{
		UE_LOG(LogKillBugs, Warning,
			TEXT("KBProfile: '%s' (user %d) is not a readable save - deleting it and starting at 0"),
			*Slot, User);

		UGameplayStatics::DeleteGameInSlot(Slot, User);
		return;
	}

	USaveGame* Loaded = UGameplayStatics::LoadGameFromSlot(Slot, User);
	const UKBSaveGame* Profile = Cast<UKBSaveGame>(Loaded);

	if (!Profile)
	{
		// Unreadable or written by a class that no longer exists. Delete rather than leave it:
		// the next save would otherwise have to overwrite a file the engine has already choked on.
		UE_LOG(LogKillBugs, Warning,
			TEXT("KBProfile: '%s' (user %d) could not be read - deleting it and starting at 0"),
			*Slot, User);

		UGameplayStatics::DeleteGameInSlot(Slot, User);
		return;
	}

	if (Profile->Gold < 0)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("KBProfile: '%s' held negative gold (%d) - clamping to 0"),
			*Slot, Profile->Gold);
	}

	// Clamped on the way in as well as out: the file is editable, and every later use of this
	// number (including seeding it into a PlayerState) would rather see a sane value.
	BankedGold = FMath::Clamp(Profile->Gold, 0, MaxBankedGold());
	BankedMaterials = FMath::Clamp(Profile->Materials, 0, MaxBankedMaterials());

	// Loaded as it is, including an EMPTY stash. A v2 file (gold and materials only) legitimately
	// has no weapons, and an emptied one is a player who lost them - neither is a reason to hand
	// out anything. Only the "no file at all" branch above grants the starters.
	Stash = Profile->Stash;
	Stash.RemoveAll([](const FKBSavedWeapon& Entry) { return Entry.Definition.IsNull(); });

	// The version is reported rather than enforced: a file written by an older build is a valid
	// file, and USaveGame's tagged format means its missing fields already read as their defaults.
	UE_LOG(LogKillBugs, Display,
		TEXT("KBProfile: loaded '%s' (user %d): %d gold | %d materials | %d weapon(s) (save v%d)"),
		*Slot, User, BankedGold, BankedMaterials, Stash.Num(), Profile->SaveVersion);
}

bool UKBProfileSubsystem::SaveProfile() const
{
	UKBSaveGame* Profile = Cast<UKBSaveGame>(
		UGameplayStatics::CreateSaveGameObject(UKBSaveGame::StaticClass()));
	if (!Profile)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("KBProfile: could not create a save object"));
		return false;
	}

	Profile->Gold = BankedGold;
	Profile->Materials = BankedMaterials;
	Profile->Stash = Stash;
	Profile->SaveVersion = UKBSaveGame::CurrentVersion;

	const bool bSaved = UGameplayStatics::SaveGameToSlot(Profile, GetSlotName(), GetUserIndex());
	if (!bSaved)
	{
		// The bank in memory is still correct, so nothing is lost yet - the next finished run
		// will try again.
		UE_LOG(LogKillBugs, Warning, TEXT("KBProfile: failed to write '%s' (user %d)"),
			*GetSlotName(), GetUserIndex());
	}

	return bSaved;
}

void UKBProfileSubsystem::BeginRun()
{
	bBankedThisRun = false;
	bMaterialsBankedThisRun = false;

	// Snapshot what is being carried in, now, while it is still knowable.
	//
	// By the time the run ends the arena's inventory also holds whatever cards handed out, and
	// there would be no way to separate "owned and brought" from "found in there" - which is
	// exactly the difference a wipe turns on.
	CarriedThisRun.Reset();
	for (const FKBSavedWeapon& Entry : Stash)
	{
		if (Entry.bEquipped)
		{
			CarriedThisRun.Add(Entry.Definition);
		}
	}
}

// ---- Shop ------------------------------------------------------------------------------------

int32 UKBProfileSubsystem::FindStashIndex(const UKBWeaponDefinition& Definition) const
{
	const FSoftObjectPath Path(&Definition);

	for (int32 Index = 0; Index < Stash.Num(); ++Index)
	{
		if (Stash[Index].Definition == Path)
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

int32 UKBProfileSubsystem::GetStashLevel(const UKBWeaponDefinition& Definition) const
{
	const int32 Index = FindStashIndex(Definition);
	return Stash.IsValidIndex(Index) ? Stash[Index].Level : 0;
}

const TArray<FSoftObjectPath>& UKBProfileSubsystem::GetStarterWeaponPaths()
{
	// One place, so a fresh profile and any future "what does a new player get" question cannot
	// drift apart. The paths match the three assets under Content/KillBugs/Weapons.
	static const TArray<FSoftObjectPath> Starters = {
		FSoftObjectPath(TEXT("/Game/KillBugs/Weapons/DA_Weapon_AutoRifle.DA_Weapon_AutoRifle")),
		FSoftObjectPath(TEXT("/Game/KillBugs/Weapons/DA_Weapon_Shotgun.DA_Weapon_Shotgun"))
	};

	return Starters;
}

void UKBProfileSubsystem::SeedStarterWeapons()
{
	Stash.Reset();

	for (const FSoftObjectPath& Path : GetStarterWeaponPaths())
	{
		FKBSavedWeapon& Entry = Stash.AddDefaulted_GetRef();
		Entry.Definition = Path;
		Entry.Level = 1;
		Entry.bEquipped = true;
	}
}

bool UKBProfileSubsystem::TryBuyWeapon(const UKBWeaponDefinition& Definition, FString& OutReason)
{
	if (!Definition.IsForSale())
	{
		OutReason = TEXT("这把武器不出售");
		return false;
	}

	if (FindStashIndex(Definition) != INDEX_NONE)
	{
		OutReason = TEXT("已经拥有了");
		return false;
	}

	if (BankedGold < Definition.BuyPriceGold)
	{
		OutReason = FString::Printf(TEXT("金币不足（缺 %d）"), Definition.BuyPriceGold - BankedGold);
		return false;
	}

	BankedGold -= Definition.BuyPriceGold;

	FKBSavedWeapon& Entry = Stash.AddDefaulted_GetRef();
	Entry.Definition = FSoftObjectPath(&Definition);
	Entry.Level = 1;

	// Bought weapons arrive unequipped: choosing what to carry is the player's decision, and the
	// slot ceiling means a full loadout could not take it anyway.
	Entry.bEquipped = false;

	SaveProfile();

	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: bought %s for %d gold (%d left)"),
		*Definition.DisplayName.ToString(), Definition.BuyPriceGold, BankedGold);

	OutReason = FString::Printf(TEXT("已购买 %s"), *Definition.DisplayName.ToString());
	return true;
}

bool UKBProfileSubsystem::TryUpgradeWeapon(int32 StashIndex, FString& OutReason)
{
	if (!Stash.IsValidIndex(StashIndex))
	{
		OutReason = TEXT("没有这把武器");
		return false;
	}

	FKBSavedWeapon& Entry = Stash[StashIndex];
	const UKBWeaponDefinition* Definition = Cast<UKBWeaponDefinition>(Entry.Definition.TryLoad());
	if (!Definition)
	{
		OutReason = TEXT("武器资产读不出来");
		return false;
	}

	if (Entry.Level >= Definition->MaxLevel)
	{
		OutReason = TEXT("已经满级");
		return false;
	}

	const int32 Cost = Definition->GetUpgradeCost(Entry.Level);
	if (Cost <= 0)
	{
		OutReason = TEXT("这把武器不可合成");
		return false;
	}

	if (BankedMaterials < Cost)
	{
		OutReason = FString::Printf(TEXT("材料不足（缺 %d）"), Cost - BankedMaterials);
		return false;
	}

	BankedMaterials -= Cost;
	++Entry.Level;

	SaveProfile();

	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: upgraded %s to level %d for %d materials"),
		*Definition->DisplayName.ToString(), Entry.Level, Cost);

	OutReason = FString::Printf(TEXT("%s 升到 %d 级"), *Definition->DisplayName.ToString(), Entry.Level);
	return true;
}

bool UKBProfileSubsystem::TrySellAllMaterials(FString& OutReason)
{
	if (BankedMaterials <= 0)
	{
		OutReason = TEXT("没有材料可卖");
		return false;
	}

	const int32 Sold = BankedMaterials;
	const int32 Earned = Sold * FMath::Max(1, KBSettings().GoldPerMaterial);

	BankedMaterials = 0;
	BankedGold = FMath::Clamp(BankedGold + Earned, 0, MaxBankedGold());

	SaveProfile();

	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: sold %d material(s) for %d gold"), Sold, Earned);

	OutReason = FString::Printf(TEXT("卖出 %d 材料，得到 %d 金币"), Sold, Earned);
	return true;
}

bool UKBProfileSubsystem::SetWeaponEquipped(int32 StashIndex, bool bEquipped, FString& OutReason)
{
	if (!Stash.IsValidIndex(StashIndex))
	{
		OutReason = TEXT("没有这把武器");
		return false;
	}

	if (Stash[StashIndex].bEquipped == bEquipped)
	{
		OutReason = bEquipped ? TEXT("已经在配装里了") : TEXT("本来就没带");
		return false;
	}

	if (bEquipped)
	{
		// The ceiling is a settings value, read here rather than off a component instance: the
		// shop exists in the lobby, where there is no pawn and therefore no inventory to ask.
		int32 EquippedCount = 0;
		for (const FKBSavedWeapon& Entry : Stash)
		{
			EquippedCount += Entry.bEquipped ? 1 : 0;
		}

		if (EquippedCount >= FMath::Max(1, KBSettings().MaxWeaponSlots))
		{
			OutReason = FString::Printf(TEXT("最多只能带 %d 把"), KBSettings().MaxWeaponSlots);
			return false;
		}
	}

	Stash[StashIndex].bEquipped = bEquipped;
	SaveProfile();

	OutReason = bEquipped ? TEXT("已加入配装") : TEXT("已移出配装");
	return true;
}

// ---- Run reconciliation ----------------------------------------------------------------------

void UKBProfileSubsystem::ApplyExtractedWeapons(const TArray<FKBSavedWeapon>& CarriedOut)
{
	// What the player left with IS what they own. Two consequences, both intended:
	//   - a weapon a card handed out during the run is now theirs, because they carried it home;
	//   - a weapon that was upgraded in the run keeps the higher level.
	// Weapons left at home are untouched, and stop being equipped - the run's list is the loadout.
	for (FKBSavedWeapon& Entry : Stash)
	{
		Entry.bEquipped = false;
	}

	for (const FKBSavedWeapon& Carried : CarriedOut)
	{
		if (Carried.Definition.IsNull())
		{
			continue;
		}

		const int32 Existing = Stash.IndexOfByPredicate(
			[&Carried](const FKBSavedWeapon& Entry) { return Entry.Definition == Carried.Definition; });

		if (Stash.IsValidIndex(Existing))
		{
			Stash[Existing].Level = FMath::Max(Stash[Existing].Level, Carried.Level);
			Stash[Existing].bEquipped = true;
			continue;
		}

		FKBSavedWeapon& Added = Stash.AddDefaulted_GetRef();
		Added.Definition = Carried.Definition;
		Added.Level = FMath::Max(1, Carried.Level);
		Added.bEquipped = true;

		UE_LOG(LogKillBugs, Display, TEXT("KBProfile: brought a new weapon home: %s"),
			*Carried.Definition.ToString());
	}

	SaveProfile();

	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: extraction kept %d carried weapon(s), stash now %d"),
		CarriedOut.Num(), Stash.Num());
}

void UKBProfileSubsystem::LoseCarriedWeapons()
{
	const int32 Before = Stash.Num();

	Stash.RemoveAll([this](const FKBSavedWeapon& Entry)
	{
		return CarriedThisRun.Contains(Entry.Definition);
	});

	SaveProfile();

	// Logged with both numbers: "you lost 3" and "your stash is now empty" are different
	// sentences, and the second one is the one that matters to a player who cannot start again.
	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: wipe took %d carried weapon(s), stash %d -> %d"),
		Before - Stash.Num(), Before, Stash.Num());

	CarriedThisRun.Reset();
}

void UKBProfileSubsystem::AddBankedForDebug(int32 Gold, int32 Materials)
{
	BankedGold = FMath::Clamp(BankedGold + Gold, 0, MaxBankedGold());
	BankedMaterials = FMath::Clamp(BankedMaterials + Materials, 0, MaxBankedMaterials());

	SaveProfile();

	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: debug grant -> %d gold | %d materials"),
		BankedGold, BankedMaterials);
}

void UKBProfileSubsystem::ReloadProfile()
{
	LoadProfile();
}

void UKBProfileSubsystem::BankRunMaterials(int32 RunMaterials)
{
	if (bMaterialsBankedThisRun)
	{
		return;
	}

	bMaterialsBankedThisRun = true;

	const int32 Before = BankedMaterials;
	BankedMaterials = FMath::Clamp(BankedMaterials + FMath::Max(RunMaterials, 0), 0, MaxBankedMaterials());

	const bool bSaved = SaveProfile();

	UE_LOG(LogKillBugs, Display,
		TEXT("KBProfile: banked materials %d -> %d (%s)"),
		Before, BankedMaterials, bSaved ? TEXT("written") : TEXT("WRITE FAILED"));
}

void UKBProfileSubsystem::BankRunGold(int32 RunGold)
{
	if (bBankedThisRun)
	{
		return;
	}

	// Latched before the write, not after: if the write fails, retrying it on a later frame would
	// add the same earnings a second time, and doubling the bank is far worse than losing the
	// write (which the next finished run repairs anyway).
	bBankedThisRun = true;

	const int32 Before = BankedGold;
	BankedGold = FMath::Clamp(BankedGold + FMath::Max(RunGold, 0), 0, MaxBankedGold());

	const bool bSaved = SaveProfile();

	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: banked %d -> %d (%s)"),
		Before, BankedGold, bSaved ? TEXT("written") : TEXT("WRITE FAILED"));
}

void UKBProfileSubsystem::ResetProfile()
{
	UGameplayStatics::DeleteGameInSlot(GetSlotName(), GetUserIndex());

	BankedGold = 0;
	BankedMaterials = 0;
	bBankedThisRun = false;
	bMaterialsBankedThisRun = false;
	CarriedThisRun.Reset();

	// Reset means "as if installed fresh", so the starters come back with it - and are written
	// immediately, so a reload in the same process sees the same thing a restart would.
	SeedStarterWeapons();
	SaveProfile();

	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: '%s' (user %d) deleted - starting fresh with %d weapon(s)"),
		*GetSlotName(), GetUserIndex(), Stash.Num());
}

// ---- Console commands ------------------------------------------------------------------------

namespace KBProfileCommands
{
	UKBProfileSubsystem* Resolve(UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UKBProfileSubsystem>() : nullptr;
	}

	void LogGold(const TArray<FString>& Args, UWorld* World)
	{
		UKBProfileSubsystem* Profile = Resolve(World);
		if (!Profile)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Profile.Gold: no profile subsystem"));
			return;
		}

		const FString Slot = UKBProfileSubsystem::GetSlotName();
		const int32 User = UKBProfileSubsystem::GetUserIndex();

		UE_LOG(LogKillBugs, Display,
			TEXT("KBProfile: banked %d gold | %d materials | %d weapon(s) | slot '%s' (user %d) | file %s"),
			Profile->GetBankedGold(), Profile->GetBankedMaterials(), Profile->GetStash().Num(),
			*Slot, User,
			UGameplayStatics::DoesSaveGameExist(Slot, User) ? TEXT("exists") : TEXT("missing"));
	}

	void ResetGold(const TArray<FString>& Args, UWorld* World)
	{
		if (UKBProfileSubsystem* Profile = Resolve(World))
		{
			Profile->ResetProfile();
		}
	}

	/**
	 * Adds gold to the LOCAL player's run total, so a persistence test does not have to earn it.
	 *
	 * Server-side by construction: AddGold refuses without authority, so this only does anything
	 * on a standalone or listen-server process. That is enough - the thing under test is the
	 * save, and the earn path (a kill crediting a real killer) is exercised by playing.
	 *
	 * It exists because KB.Swarm.Kill cannot be used for this: it passes a null killer and
	 * ApplyDamageToEnemy only pays out when the killer is valid.
	 */
	void AddGold(const TArray<FString>& Args, UWorld* World)
	{
		if (!World || Args.Num() < 1)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Profile.AddGold: expected <amount>"));
			return;
		}

		const int32 Amount = FCString::Atoi(*Args[0]);

		APlayerController* PlayerController = World->GetFirstPlayerController();
		AKBPlayerState* PlayerState =
			PlayerController ? PlayerController->GetPlayerState<AKBPlayerState>() : nullptr;

		if (!PlayerState)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Profile.AddGold: no local player state"));
			return;
		}

		PlayerState->AddGold(Amount);

		UE_LOG(LogKillBugs, Display, TEXT("KBProfile: +%d gold to the run total (now %d)"),
			Amount, PlayerState->GetGold());
	}

	/**
	 * Puts currency straight into the BANK.
	 *
	 * Needed because AddGold and KB.Loot.Give both write the run total, which is zero in the
	 * lobby - so without this the shop cannot be funded from a headless run at all.
	 */
	void Grant(const TArray<FString>& Args, UWorld* World)
	{
		UKBProfileSubsystem* Profile = Resolve(World);
		if (!Profile || Args.Num() < 2)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Profile.Grant: expected <gold> <materials>"));
			return;
		}

		Profile->AddBankedForDebug(FCString::Atoi(*Args[0]), FCString::Atoi(*Args[1]));
	}

	/** Re-reads the file, so persistence can be proven without restarting the process. */
	void Reload(const TArray<FString>& Args, UWorld* World)
	{
		if (UKBProfileSubsystem* Profile = Resolve(World))
		{
			Profile->ReloadProfile();
		}
	}
}

static FAutoConsoleCommandWithWorldAndArgs KBConsoleProfileGold(
	TEXT("KB.Profile.Gold"),
	TEXT("KB.Profile.Gold - print the local profile's banked total and where it is stored."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBProfileCommands::LogGold));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleProfileReset(
	TEXT("KB.Profile.Reset"),
	TEXT("KB.Profile.Reset - delete the local profile and start from zero."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBProfileCommands::ResetGold));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleProfileGrant(
	TEXT("KB.Profile.Grant"),
	TEXT("KB.Profile.Grant <gold> <materials> - add straight to the BANK. KB.Profile.AddGold and "
	     "KB.Loot.Give write the run total, which is 0 in the lobby, so this is what funds a shop "
	     "test."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBProfileCommands::Grant));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleProfileReload(
	TEXT("KB.Profile.Reload"),
	TEXT("KB.Profile.Reload - re-read the save from disk, so persistence can be proven without "
	     "restarting the process."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBProfileCommands::Reload));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleProfileAddGold(
	TEXT("KB.Profile.AddGold"),
	TEXT("KB.Profile.AddGold <n> - add n gold to the local player's run total (server only). A "
	     "testing shortcut: KB.Swarm.Kill cannot be used for this because it credits a null killer."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBProfileCommands::AddGold));
