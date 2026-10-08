#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Persistence/KBStashTypes.h"
#include "KBSaveGame.generated.h"

/**
 * The local player's profile, as it exists on disk.
 *
 * Deliberately narrow, and grown one field at a time: gold, then materials, then the stash, then
 * carried medkits. Each was added when the game had something to put in it, because a container
 * written ahead of its contents is a guess twice over - once about the shape, once about what
 * goes in it. (This comment used to say gold was the only thing that outlived a run, which stopped
 * being true two fields ago.)
 *
 * The format carries the reason it can be grown this cheaply: USaveGame serializes by tag, so a
 * field that did not exist when a file was written loads at its default. SaveVersion records which
 * build wrote the file, and is reported rather than enforced - there is nothing to enforce.
 */
UCLASS()
class KILLBUGS_API UKBSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/** The current on-disk layout. Bump when a field is added or its meaning changes. */
	static constexpr int32 CurrentVersion = 4;

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
	 * Medkits in the stash - the first thing the stash holds that is neither currency nor a weapon.
	 *
	 * A scalar for the same reason Materials is one, and the same promise applies: a file written
	 * before this field existed loads with it at 0, which is the correct value for a build that
	 * had no carried consumables. A v3 file therefore needs no migration.
	 *
	 * Unlike materials, this travels BOTH ways: the whole stash comes into the run, and what is
	 * still in the backpack at the end replaces it. A wipe takes the ones that were carried in,
	 * exactly as it takes the weapons.
	 */
	UPROPERTY()
	int32 Medkits = 0;

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
