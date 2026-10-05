#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "KBProfileSubsystem.generated.h"

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

private:
	/** Reads the profile off disk, or starts at zero. Never fails - a bad profile is a fresh one. */
	void LoadProfile();

	/** Writes the bank to disk. Returns whether the write landed. */
	bool SaveProfile() const;

	/** The persisted total. */
	int32 BankedGold = 0;

	/** Whether this run's earnings have already been banked. Cleared by BeginRun. */
	bool bBankedThisRun = false;
};
