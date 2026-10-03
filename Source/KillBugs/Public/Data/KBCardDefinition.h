#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "KBCardDefinition.generated.h"

class UKBWeaponDefinition;

UENUM(BlueprintType)
enum class EKBCardRarity : uint8
{
	Common    UMETA(DisplayName = "Common"),
	Rare      UMETA(DisplayName = "Rare"),
	Epic      UMETA(DisplayName = "Epic"),
	Legendary UMETA(DisplayName = "Legendary")
};

/** What picking the card actually does. */
UENUM(BlueprintType)
enum class EKBCardEffect : uint8
{
	/** Give a weapon the player does not have yet. */
	AddWeapon     UMETA(DisplayName = "Add weapon"),

	/** Level up a weapon the player already owns. */
	UpgradeWeapon UMETA(DisplayName = "Upgrade weapon"),

	/** Apply permanent stat changes. */
	StatBoost     UMETA(DisplayName = "Stat boost"),

	/** Heal to full, or by a fixed amount. */
	Heal          UMETA(DisplayName = "Heal")
};

/** How often the same card may be taken. */
UENUM(BlueprintType)
enum class EKBCardRepeatRule : uint8
{
	OncePerRun    UMETA(DisplayName = "Once per run"),
	OncePerPlayer UMETA(DisplayName = "Once per player"),
	Stackable     UMETA(DisplayName = "Stackable")
};

/**
 * Permanent stat changes a card applies. Multipliers are multiplicative and start at 1.0;
 * the stat sheet folds every card's contribution together.
 *
 * EditAnywhere, NOT EditDefaultsOnly. These are fields of a struct that lives inside another
 * asset, and EditDefaultsOnly forbids writing them on a struct instance - which silently
 * leaves every stat card at its neutral defaults, so picking one does nothing at all.
 */
USTRUCT(BlueprintType)
struct FKBStatMods
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stats")
	float MaxHealthAdd = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stats")
	float MoveSpeedMult = 1.f;

	/** Scales every weapon's damage. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stats")
	float DamageMult = 1.f;

	/** Scales every weapon's cooldown; below 1 is faster. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stats")
	float CooldownMult = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stats")
	float PickupRadiusMult = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stats")
	float XpGainMult = 1.f;
};

/**
 * One draftable upgrade.
 *
 * Every card is an asset, so adding content never means touching code - which is the whole
 * point of the data layer, given how many cards a game in this genre accumulates.
 */
UCLASS(BlueprintType)
class KILLBUGS_API UKBCardDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	FText Title;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity", meta = (MultiLine = "true"))
	FText Description;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	EKBCardRarity Rarity = EKBCardRarity::Common;

	/** Base draw weight before rarity, level and anti-repeat bias. Higher is more common. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity", meta = (ClampMin = "0.01"))
	float Weight = 1.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	EKBCardEffect Effect = EKBCardEffect::StatBoost;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
	EKBCardRepeatRule RepeatRule = EKBCardRepeatRule::Stackable;

	// ---- Effect payload ------------------------------------------------------------------

	/** AddWeapon and UpgradeWeapon. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Effect")
	TObjectPtr<UKBWeaponDefinition> Weapon;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Effect")
	FKBStatMods StatMods;

	/** Heal only. 0 means "heal to full". */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Effect")
	float HealAmount = 0.f;

	// ---- Eligibility ---------------------------------------------------------------------

	/** Hidden until the player reaches this level. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Eligibility", meta = (ClampMin = "1"))
	int32 MinPlayerLevel = 1;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Eligibility")
	int32 MinWave = 0;

	/** UpgradeWeapon only: the weapon that must already be owned. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Eligibility")
	TObjectPtr<UKBWeaponDefinition> RequiresWeapon;
};
