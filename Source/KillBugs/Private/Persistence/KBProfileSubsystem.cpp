#include "Persistence/KBProfileSubsystem.h"

#include "Core/KBPlayerState.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
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

	if (!UGameplayStatics::DoesSaveGameExist(Slot, User))
	{
		// The ordinary first-launch path, not a problem: say so at Display so that a run which
		// looks like it lost gold can be told apart from a run that started with none.
		UE_LOG(LogKillBugs, Display, TEXT("KBProfile: no profile at '%s' (user %d) - starting at 0"),
			*Slot, User);
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

	// The version is reported rather than enforced: a file written by an older build is a valid
	// file, and USaveGame's tagged format means its missing fields already read as their defaults.
	UE_LOG(LogKillBugs, Display,
		TEXT("KBProfile: loaded '%s' (user %d): %d gold | %d materials (save v%d)"),
		*Slot, User, BankedGold, BankedMaterials, Profile->SaveVersion);
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

	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: '%s' (user %d) deleted - starting at 0"),
		*GetSlotName(), GetUserIndex());
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
			TEXT("KBProfile: banked %d gold | %d materials | slot '%s' (user %d) | file %s"),
			Profile->GetBankedGold(), Profile->GetBankedMaterials(), *Slot, User,
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
}

static FAutoConsoleCommandWithWorldAndArgs KBConsoleProfileGold(
	TEXT("KB.Profile.Gold"),
	TEXT("KB.Profile.Gold - print the local profile's banked total and where it is stored."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBProfileCommands::LogGold));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleProfileReset(
	TEXT("KB.Profile.Reset"),
	TEXT("KB.Profile.Reset - delete the local profile and start from zero."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBProfileCommands::ResetGold));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleProfileAddGold(
	TEXT("KB.Profile.AddGold"),
	TEXT("KB.Profile.AddGold <n> - add n gold to the local player's run total (server only). A "
	     "testing shortcut: KB.Swarm.Kill cannot be used for this because it credits a null killer."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBProfileCommands::AddGold));
