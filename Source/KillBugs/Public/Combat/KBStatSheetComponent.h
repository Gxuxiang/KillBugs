#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Data/KBCardDefinition.h"
#include "KBStatSheetComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FKBOnHealthDepleted);

/**
 * The player's running totals.
 *
 * Every card that touches numbers folds its mods in here rather than mutating weapons or the
 * character directly, so there is exactly one place that answers "what is this player's
 * current damage multiplier". Weapons ask this component instead of caching values, which
 * means a card picked mid-run takes effect on the very next shot.
 */
UCLASS(ClassGroup = (KillBugs), meta = (BlueprintSpawnableComponent))
class KILLBUGS_API UKBStatSheetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UKBStatSheetComponent();

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Server-only. Folds one card's mods into the running totals. */
	void ApplyMods(const FKBStatMods& Mods);

	// ---- Health --------------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "KillBugs|Health")
	float GetMaxHealth() const;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Health")
	float GetHealth() const { return CurrentHealth; }

	/** 0..1, for the HUD bar. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Health")
	float GetHealthFraction() const;

	/**
	 * Shakes the hurt player's camera, if a shake is configured.
	 *
	 * A no-op unless UKBGameSettings::PlayerDamageCameraShake is set, which it is not by default.
	 * Kept apart from ApplyDamage so the health maths reads as health maths.
	 */
	void PlayDamageCameraShake() const;

	/**
	 * Server-only. Returns the damage actually applied.
	 *
	 * Broadcasts OnHealthDepleted when this brings the player to zero. The stat sheet does not
	 * decide what dying means - the pawn does.
	 */
	float ApplyDamage(float Damage);

	/** Server-only. Amount <= 0 heals to full, which is what a wave-start respawn wants. */
	void Heal(float Amount);

	/** Raised when health reaches zero. */
	UPROPERTY(BlueprintAssignable, Category = "KillBugs|Health")
	FKBOnHealthDepleted OnHealthDepleted;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Stats")
	float GetMoveSpeed() const;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Stats")
	float GetDamageMultiplier() const { return TotalMods.DamageMult; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Stats")
	float GetCooldownMultiplier() const { return TotalMods.CooldownMult; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Stats")
	float GetPickupRadius() const;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Stats")
	float GetXpMultiplier() const { return TotalMods.XpGainMult; }

	// ---- Base values ---------------------------------------------------------------------
	// Health, move speed and pickup radius come from UKBGameSettings -> Player, so they can be
	// tuned without touching code.

protected:
	/** Folded total of every card taken. Multipliers start at 1, additives at 0. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Stats")
	FKBStatMods TotalMods;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Health")
	float CurrentHealth = 100.f;

	/** Server-only bookkeeping, so raising max health can grant the difference. */
	float LastKnownMaxHealth = 0.f;

	/** Pushes derived values that live on other components, e.g. MaxWalkSpeed. */
	void PushDerivedValues();
};
