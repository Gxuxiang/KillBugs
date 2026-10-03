#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "KBContentSubsystem.generated.h"

class UKBCardDefinition;
class UKBWeaponDefinition;

/**
 * Loads and owns the game's data-driven content, and rolls card drafts.
 *
 * Cards are discovered by scanning the asset registry rather than being listed by hand: the
 * whole point of the card layer is that adding content is authoring an asset, and a
 * hand-maintained list would quietly become a second place to forget to update.
 */
UCLASS()
class KILLBUGS_API UKBContentSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/**
	 * Loads on first use, not in Initialize.
	 *
	 * A GameInstance subsystem initialises before the asset registry has finished its
	 * initial scan, so scanning there reliably finds nothing - and the failure is silent,
	 * looking exactly like "the project has no cards". Re-scanning until cards appear also
	 * makes this self-healing if the registry is slow.
	 */
	const TArray<TObjectPtr<UKBCardDefinition>>& GetAllCards() const;

	/**
	 * Server-side. Fills OutChoices with up to CardCount distinct cards this player may take.
	 *
	 * Per-player, never a shared pool: four players own different weapons, and a shared pool
	 * would hand out cards that are dead for half of them.
	 *
	 * May return fewer than CardCount when the player has simply run out of things to take -
	 * that is reported honestly rather than padded with unusable cards.
	 */
	void RollCardChoices(const class AKBPlayerState& PlayerState, int32 WaveIndex, int32 CardCount,
	                     TArray<TObjectPtr<UKBCardDefinition>>& OutChoices) const;

	/** True if this player could take this card right now. */
	bool IsCardEligible(const AKBPlayerState& PlayerState, const UKBCardDefinition& Card, int32 WaveIndex) const;

private:
	/** Mutable so the lazily-populated cache works from const queries. */
	UPROPERTY(Transient)
	mutable TArray<TObjectPtr<UKBCardDefinition>> AllCards;

	/** Scans the asset registry for cards. Safe to call repeatedly; fills the mutable cache. */
	void LoadCards() const;
};
