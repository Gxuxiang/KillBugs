#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Persistence/KBStashTypes.h"
#include "KBSaveGame.generated.h"

/**
 * The local player's profile, as it exists on disk.
 *
 * Deliberately narrow. Gold is the only thing the game has that is meant to outlive a run, so it
 * is the only thing stored; the stash arrives with the loot system, when there is something to
 * put in it and therefore something to be right about. Writing a container for loot that does not
 * exist yet would be guessing twice - once about the shape, once about the contents.
 *
 * SaveVersion exists so that field can be added later without the loader having to guess what it
 * is reading.
 */
UCLASS()
class KILLBUGS_API UKBSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/** The current on-disk layout. Bump when a field is added or its meaning changes. */
	static constexpr int32 CurrentVersion = 3;

	/** The persisted total, across every run this machine has ever finished. */
	UPROPERTY()
	int32 Gold = 0;

	/**
	 * Materials brought home by successful extractions. A wipe loses them.
	 *
	 * A scalar rather than a container, because there is exactly one kind of material so far and
	 * a container with one element is a taxonomy with one element. The format is what makes it
	 * extensible: USaveGame serializes by tag, so adding tiers later is an additive field, and a
	 * file written here loads with that field empty.
	 *
	 * A v1 file - gold only - loads with this field at its default of 0. No migration.
	 */
	UPROPERTY()
	int32 Materials = 0;

	/**
	 * The weapons this machine owns, and which of them are chosen for the next run.
	 *
	 * A wipe REMOVES the ones that were carried in - the design's "takes everything you brought,
	 * gear included" - so this list can shrink, and it can end up empty with nothing to fall back
	 * on. There is deliberately no "you always have a starter" rule anywhere past the very first
	 * profile: any such rule would quietly undo the stake.
	 *
	 * A v2 file (gold and materials) loads with this empty, which is correct for it - that build
	 * had no stash at all.
	 */
	UPROPERTY()
	TArray<FKBSavedWeapon> Stash;

	UPROPERTY()
	int32 SaveVersion = CurrentVersion;
};
