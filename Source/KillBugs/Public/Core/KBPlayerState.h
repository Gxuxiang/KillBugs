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

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Player")
	int32 XP = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Player")
	int32 PlayerLevel = 1;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Player")
	bool bIsDowned = false;
};
