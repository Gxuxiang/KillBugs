#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "KBStashTypes.generated.h"

/**
 * One weapon the player owns, as it is stored on disk.
 *
 * A path rather than a pointer: UKBWeaponDefinition is a DataAsset, and a save file has to be
 * able to name one without holding it loaded - and to survive the asset being renamed or moved
 * without the file becoming unreadable.
 *
 * The loadout is a flag on each owned weapon rather than a separate ordered list, because two
 * lists can disagree (a loadout entry for a weapon that was never owned) and because the two
 * have different sizes: the stash may legitimately hold more weapons than a run can carry.
 */
USTRUCT(BlueprintType)
struct FKBSavedWeapon
{
	GENERATED_BODY()

	UPROPERTY()
	FSoftObjectPath Definition;

	UPROPERTY()
	int32 Level = 1;

	/** Chosen for the next run. At most MaxWeaponSlots entries may be set. */
	UPROPERTY()
	bool bEquipped = false;
};
