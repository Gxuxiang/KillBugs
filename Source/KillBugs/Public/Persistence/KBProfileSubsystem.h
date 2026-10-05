#pragma once

#include "CoreMinimal.h"
#include "Persistence/KBStashTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "KBProfileSubsystem.generated.h"

class UKBWeaponDefinition;

/**
 * The local player's persistent profile - what survives after the world it was earned in is gone.
 *
 * A GameInstanceSubsystem for the same reason UKBSessionSubsystem is one: the travels in this game
 * are non-seamless, so every PlayerState is destroyed on the way to the lobby and Gold/XP/level go
 * with it. The GameInstance outlives both worlds, so the total lives here.
 *
 * ONE PROFILE PER MACHINE, AND NO KEY. Splitscreen is off and a client is one player, so the
 * machine IS the identity. There is nothing else to key on: KBPlayerIndex is run-local and grows
 * on reconnect, and the only other candidate is an online-subsystem display name that
 * UKBSessionSubsystem documents as likely empty. A key would add a "mismatched key reads as zero"
 * failure mode with nothing to consume it.
 *
 * WHAT IS PERSISTED IS A TOTAL, NOT THE ARENA'S NUMBER. AKBPlayerState::Gold keeps its existing
 * meaning - this run's earnings, starting at zero, written only by AddGold on the authority. The
 * bank is separate, and a finished run is folded into it. That separation is what lets a client
 * own its own profile at all: nothing here ever has to be written into server-authoritative
 * state, so there is no client-writes-authority problem to solve.
 *
 * The bank only ever goes UP, and only by an observed run total. A dropped RPC, a missed
 * handshake or a stale message can leave the lobby displaying a stale number, but none of them can
 * overwrite what is already banked.
 */
UCLASS()
class KILLBUGS_API UKBProfileSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Profile")
	int32 GetBankedGold() const { return BankedGold; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Profile")
	int32 GetBankedMaterials() const { return BankedMaterials; }

	/** The owned weapons, with their levels and which of them are chosen for the next run. */
	const TArray<FKBSavedWeapon>& GetStash() const { return Stash; }

	/** Index into GetStash() for this weapon, or INDEX_NONE. Matched by path, not by pointer. */
	int32 FindStashIndex(const UKBWeaponDefinition& Definition) const;

	int32 GetStashLevel(const UKBWeaponDefinition& Definition) const;

	/**
	 * The two weapons a brand new profile starts with, as asset paths.
	 *
	 * Handed out exactly ONCE - when there is no save file on disk at all - because a player with
	 * no weapon cannot play. It is deliberately NOT a fallback for "the stash is empty": losing
	 * your last weapon to a wipe is the stake the design asks for, and re-granting here would
	 * quietly cancel it.
	 */
	static const TArray<FSoftObjectPath>& GetStarterWeaponPaths();

	// ---- Shop -------------------------------------------------------------------------------
	//
	// These four are the ONLY writers of the bank and the stash, so that the rules - what a
	// purchase costs, what a wipe takes - live in one place and the UI only asks. Each returns
	// false with a reason a player can read, and saves only when it changed something.

	bool TryBuyWeapon(const UKBWeaponDefinition& Definition, FString& OutReason);
	bool TryUpgradeWeapon(int32 StashIndex, FString& OutReason);
	bool TrySellAllMaterials(FString& OutReason);
	bool SetWeaponEquipped(int32 StashIndex, bool bEquipped, FString& OutReason);

	/**
	 * What came home from an extraction: the weapons the player was holding at the end.
	 *
	 * Those levels stick (a card that upgraded a carried weapon during the run is kept), any
	 * weapon that was not in the stash before is added by having been carried out, and the whole
	 * set becomes the loadout. Weapons left at home are untouched.
	 */
	void ApplyExtractedWeapons(const TArray<FKBSavedWeapon>& CarriedOut);

	/**
	 * A wipe took everything that was brought in. Called on the local machine.
	 *
	 * The snapshot is taken by BeginRun rather than inferred now, because by this point the
	 * arena's inventory holds card-granted weapons too and there would be no way to tell which of
	 * them the player actually owned and carried.
	 */
	void LoseCarriedWeapons();

	/** Debug: puts currency straight into the bank. `KB.Profile.Grant`. */
	void AddBankedForDebug(int32 Gold, int32 Materials);

	/** Re-reads the save from disk. `KB.Profile.Reload`, for testing persistence in one process. */
	void ReloadProfile();

	/** Called when this machine enters a fresh arena, so the once-per-run fold can happen again. */
	void BeginRun();

	/**
	 * Folds one finished run's earnings into the bank and writes it to disk.
	 *
	 * Latched per run. A run ends by exactly one path - EndRun sets the phase to RunOver, and the
	 * phase only changes once - so a second call for the same run could only double the earnings
	 * rather than correct anything. BeginRun clears the latch for the next one.
	 */
	void BankRunGold(int32 RunGold);

	/**
	 * Folds the materials carried out of a successful extraction into the stash.
	 *
	 * Separate from BankRunGold, and with its OWN latch, for two reasons. The two are banked at
	 * different moments - gold on any ending, materials only on an extraction - so they cannot
	 * share a "this run is done" flag. And sharing one would fail silently in the worst way: the
	 * gold call sets the flag first, and the materials bank would then be skipped without a word.
	 *
	 * A wipe does not reach this at all; the caller reports the loss instead.
	 */
	void BankRunMaterials(int32 RunMaterials);

	/** Deletes the save and starts from zero. Bound to `KB.Profile.Reset`. */
	void ResetProfile();

	/** The slot this machine's profile lives in. One per machine - see the class comment. */
	static FString GetSlotName() { return TEXT("KillBugsProfile"); }
	static int32 GetUserIndex() { return 0; }

	/**
	 * A ceiling on the bank, so a tampered or corrupt file cannot make AddGold's arithmetic
	 * overflow when the value is seeded into a PlayerState later.
	 */
	static int32 MaxBankedGold() { return TNumericLimits<int32>::Max() / 2; }

	/** The same ceiling, for the same reason. */
	static int32 MaxBankedMaterials() { return TNumericLimits<int32>::Max() / 2; }

private:
	/** Reads the profile off disk, or starts at zero. Never fails - a bad profile is a fresh one. */
	void LoadProfile();

	/** Puts the two starter weapons in the stash. Only ever called for a profile with no past. */
	void SeedStarterWeapons();

	/** Writes the bank to disk. Returns whether the write landed. */
	bool SaveProfile() const;

	/** The persisted total. */
	int32 BankedGold = 0;

	/** Materials brought home by successful extractions. */
	int32 BankedMaterials = 0;

	/** Owned weapons. Shrinks on a wipe, grows on a purchase or a successful extraction. */
	TArray<FKBSavedWeapon> Stash;

	/** What the current run set out with. See LoseCarriedWeapons. */
	TArray<FSoftObjectPath> CarriedThisRun;

	/** Whether this run's earnings have already been banked. Cleared by BeginRun. */
	bool bBankedThisRun = false;

	/**
	 * Its own latch, not shared with the gold one - see BankRunMaterials. Cleared by BeginRun.
	 */
	bool bMaterialsBankedThisRun = false;
};
