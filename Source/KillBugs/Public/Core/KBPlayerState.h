#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "KBPlayerState.generated.h"

class UKBCardDefinition;

/** Raised whenever any per-player run stat changes, so HUD widgets can rebind lazily. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FKBOnPlayerRunStateChanged);

/**
 * Replicated per-player run state: everything the HUD needs about THIS player.
 *
 * Mutators are server-only (see the Set* helpers - they no-op on clients). Owned weapons,
 * passives and the stat sheet arrive in Phase 3/4; this starts with the currency and the
 * downed flag that the wave loop needs.
 */
UCLASS()
class KILLBUGS_API AKBPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	AKBPlayerState();

	virtual void BeginPlay() override;

	/** Index into the player array, stable for the run. Used to seed per-player card rolls. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Player")
	int32 GetKBPlayerIndex() const { return KBPlayerIndex; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Player")
	int32 GetGold() const { return Gold; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Player")
	int32 GetXP() const { return XP; }

	// Named GetKBLevel, not GetLevel: AActor already declares a GetLevel() for the actor's
	// ULevel, and UHT rejects a UFUNCTION override that would collide with it.
	UFUNCTION(BlueprintPure, Category = "KillBugs|Player")
	int32 GetKBLevel() const { return PlayerLevel; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Player")
	bool IsDowned() const { return bIsDowned; }

	/** Server-only. Assigns the stable run index at login. */
	void SetKBPlayerIndex(int32 InIndex);

	/** Server-only. */
	void AddGold(int32 Amount);

	UFUNCTION(BlueprintPure, Category = "KillBugs|Player")
	int32 GetMaterials() const { return Materials; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Player")
	int32 GetMedkits() const { return Medkits; }

	/** Everything carried, in backpack weight units. Derived - never stored. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Player")
	int32 GetCarriedWeight() const;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Player")
	int32 GetBackpackCapacity() const;

	/** True when nothing more will fit. The HUD turns this red. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Player")
	bool IsBackpackFull() const { return GetCarriedWeight() >= GetBackpackCapacity(); }

	/**
	 * Server-only. Adds only if the backpack has room for the whole amount.
	 *
	 * The whole amount or nothing: a partial pickup would mean splitting a ground stack, and
	 * stacks that split are a container mechanic this cut does not have. OutReason is
	 * player-facing Chinese, because every refusal reaches the screen one way or another.
	 */
	bool TryAddMaterials(int32 Amount, FString& OutReason);
	bool TryAddMedkits(int32 Amount, FString& OutReason);

	/**
	 * Server-only. Takes items OUT of the backpack, for dropping them on the ground.
	 *
	 * Broadcasts like the TryAdd pair does, and it has to: the HUD's weight readout and the
	 * capacity gate both derive from these two counters, so a silent decrement would leave the
	 * weight bar showing a number that no longer matches what is carried.
	 */
	bool TryRemoveMaterials(int32 Amount, FString& OutReason);
	bool TryRemoveMedkits(int32 Amount, FString& OutReason);

	/**
	 * Puts the banked medkits into the backpack. Server-only, and only ONCE per run.
	 *
	 * The medkit analogue of SeedGold, and it exists for the same shape of reason: the loadout
	 * comes from this machine's own profile, so it has to be applied by the authority and must
	 * never be re-applied (a second seed would duplicate the stash into the run).
	 */
	void SeedBackpack(int32 InMedkits);

	/**
	 * Server-only, and the ONLY writer of Medkits. Consumes one and heals.
	 *
	 * Refused, with a reason and no consumption, while downed, with none carried, or at full
	 * health - the last of which is the old "do not waste a medkit" rule, moved from pickup
	 * time (where the game decided for you) to use time (where you decide).
	 */
	bool TryUseMedkit(FString& OutReason);

	/** `KB.Backpack.UiSelfTest`. Lives here with the rest of the backpack's console surface. */
	static void ConsoleBackpackUiSelfTest(const TArray<FString>& Args, UWorld* World);

	/**
	 * Gives this player the profile total they arrived with. Server-only, and it only works ONCE.
	 *
	 * A separate entry point from AddGold rather than a use of it, for two reasons. The number
	 * comes from the client's own save file and is therefore not something the server can check -
	 * so it belongs to a call that is obviously about trust rather than one that looks like a
	 * reward. And it must not be repeatable: a second seed arriving late, or on purpose, would
	 * reshape a total that has since been earned.
	 *
	 * The trust part is worth stating plainly: a modified client can claim any number here, and
	 * the server has no source to contradict it. That is inherent to letting each machine own its
	 * own profile, and it is equivalent to editing the .sav file directly. This is a LAN co-op
	 * game; the alternative is a server-owned profile, which was considered and rejected.
	 */
	void SeedGold(int32 InGold);

	/** Server-only. */
	void AddXP(int32 Amount);

	/**
	 * What level a given total XP corresponds to.
	 *
	 * Exists because PlayerLevel used to be a field nothing ever wrote: it sat at 1 for the whole
	 * run while the HUD printed "等级 1" and every card's MinPlayerLevel gate compared against a
	 * constant. Deriving it from XP is what makes both of those real.
	 *
	 * The curve is KBSettings().XpPerLevel - see the tooltip there.
	 */
	int32 ComputeLevelForXP(int32 InXP) const;

	/** Server-only. */
	void SetDowned(bool bInDowned);

	// ---- Card draft ----------------------------------------------------------------------

	/**
	 * The three cards this player is choosing between. Empty outside a draft, which is how
	 * the HUD knows to hide the widget rather than needing a separate phase query.
	 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Cards")
	TArray<TObjectPtr<UKBCardDefinition>> PendingCardChoices;

	/** True when there is nothing to pick - starts true so no widget shows before wave 1. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Cards")
	bool bHasPickedCard = true;

	/** Everything taken so far, for the repeat rules. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Cards")
	TArray<TObjectPtr<UKBCardDefinition>> TakenCards;

	/** Server-only. */
	void SetPendingCardChoices(const TArray<TObjectPtr<UKBCardDefinition>>& Choices);

	/** Server-only. */
	void MarkCardPicked();

	/** Server-only. */
	void RecordTakenCard(UKBCardDefinition* Card);

	bool HasTakenCard(const UKBCardDefinition* Card) const;

	/** True when there is nothing to pick, so no draft should be shown. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Cards")
	bool HasPickedCard() const { return bHasPickedCard; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Cards")
	bool HasPendingCardDraft() const { return !bHasPickedCard && PendingCardChoices.Num() > 0; }

	/** This player's pawn weapon inventory, or null if unpossessed. */
	class UKBWeaponInventoryComponent* GetWeaponInventory() const;

	UPROPERTY(BlueprintAssignable, Category = "KillBugs|Player")
	FKBOnPlayerRunStateChanged OnRunStateChanged;

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Player")
	int32 KBPlayerIndex = INDEX_NONE;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Player")
	int32 Gold = 0;

	/**
	 * Materials picked up this run, carried until the run ends.
	 *
	 * Beside Gold because they behave like Gold: per player, replicated, server-authoritative,
	 * and destroyed by the non-seamless travel home. Unlike Gold they are NOT banked on a wipe -
	 * that difference is the whole point of searching, and it is decided at the run's result, not
	 * here.
	 *
	 * On the PlayerState rather than a component on the pawn: materials have no state of their
	 * own, and they have to survive a knockdown and revive inside a run. A pawn component would
	 * reintroduce exactly the lifetime question this object already answers.
	 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Player")
	int32 Materials = 0;

	/**
	 * Medkits carried this run, in the backpack.
	 *
	 * A counter rather than a container, for the same reason Materials is one: there is exactly
	 * one kind of medkit, and a container with one element is a taxonomy with one element. What
	 * makes it more than a second Materials is where the number comes from and where it goes -
	 * it is seeded from the stash at the start of a run, and what is left at the end goes back
	 * to the stash. Materials only ever travel one way.
	 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Player")
	int32 Medkits = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Player")
	int32 XP = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Player")
	int32 PlayerLevel = 1;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Player")
	bool bIsDowned = false;

	/** Not replicated: it is only ever consulted on the authority, which is where seeding happens. */
	bool bGoldSeeded = false;

	/** Same reasoning as bGoldSeeded. See SeedBackpack. */
	bool bBackpackSeeded = false;
};
