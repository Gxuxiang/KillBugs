#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "KBEnemyArchetype.generated.h"

class UStaticMesh;
class UMaterialInterface;
class UNiagaraSystem;

/**
 * How an archetype is represented. This single knob is the switch between the two enemy
 * paths, and it is the most consequential field on this asset.
 *
 *  Instanced - simulated in AKBEnemyDirector's struct array and drawn as HISM instances.
 *              Cheap enough for hundreds at once. No collision, no Actor, no net channel.
 *  Actor     - a real pooled AKBEnemyActor with engine movement replication. Budget this
 *              to a few dozen at most.
 */
UENUM(BlueprintType)
enum class EKBRenderTier : uint8
{
	Instanced UMETA(DisplayName = "Instanced (swarm)"),
	Actor     UMETA(DisplayName = "Actor (hero/elite)")
};

/** How an archetype picks its movement target. */
UENUM(BlueprintType)
enum class EKBSteeringBehavior : uint8
{
	SeekNearestPlayer  UMETA(DisplayName = "Seek nearest player"),
	SeekLowestHealth   UMETA(DisplayName = "Seek lowest-health player"),
	Charge             UMETA(DisplayName = "Charge (seek at speed, slow turns)"),
	Orbit              UMETA(DisplayName = "Orbit at preferred distance")
};

/**
 * A bug type. Everything about an enemy that a designer should be able to tune without
 * touching code lives here.
 */
UCLASS(BlueprintType)
class KILLBUGS_API UKBEnemyArchetype : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// ---- Presentation -------------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	FText DisplayName;

	/** Placeholder primitives (SM_Cube / SM_Cylinder) until Phase 6 art lands. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftObjectPtr<UMaterialInterface> Material;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	FVector MeshScale = FVector(1.f);

	/**
	 * Tints every bug of this archetype.
	 *
	 * Applied as a vector parameter on this archetype's own instanced mesh component, so it
	 * costs one dynamic material instance per archetype rather than per bug. Without it there
	 * is nothing to tell two similarly-shaped archetypes apart on screen.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	FLinearColor Tint = FLinearColor::White;

	/** Vector parameter on Material that Tint drives. Empty disables tinting. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	FName TintParameterName = TEXT("Base Color");

	/**
	 * Surface response, pushed as scalar parameters of the same name.
	 *
	 * These matter more than they look: a low roughness under a bright sun produces a broad
	 * white specular highlight that swamps the base colour entirely, so every archetype reads
	 * as the same white blob no matter what Tint says. Rough placeholder surfaces keep the
	 * tint legible.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Roughness = 0.9f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Metallic = 0.0f;

	// ---- Stats --------------------------------------------------------------------------
	// "PerWave" fields are the linear growth applied per wave index, so difficulty scales
	// without authoring a wave entry per archetype per wave.

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats")
	float BaseHealth = 10.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats")
	float HealthPerWave = 3.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats")
	float MoveSpeed = 260.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats")
	float SpeedPerWave = 8.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats")
	float ContactDamage = 5.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats")
	float DamagePerWave = 1.f;

	/** Used for separation, weapon hit tests and the blob shadow radius. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "1.0"))
	float BodyRadius = 40.f;

	// ---- Steering -----------------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Steering")
	EKBSteeringBehavior Steering = EKBSteeringBehavior::SeekNearestPlayer;

	/** Only meaningful for Orbit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Steering")
	float PreferredDistance = 400.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Steering")
	float Acceleration = 900.f;

	/** Degrees per second. Low values give a heavy, flankable charge. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Steering")
	float TurnRateDegrees = 360.f;

	// ---- Rewards ------------------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rewards")
	int32 XpValue = 1;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Rewards")
	int32 GoldValue = 0;

	// ---- VFX ----------------------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VFX")
	TSoftObjectPtr<UNiagaraSystem> DeathEffect;

	// ---- Classification -----------------------------------------------------------------

	/** e.g. Enemy.Flying, Enemy.Armored, Enemy.Explosive. Drives card prerequisites. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Classification")
	FGameplayTagContainer ArchetypeTags;

	// ---- Representation -----------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Representation")
	EKBRenderTier RenderTier = EKBRenderTier::Instanced;

	/** Cached index into AKBEnemyDirector::Archetypes, assigned at run start. Not authored. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Representation")
	int32 RuntimeIndex = INDEX_NONE;

	/** Per-wave health multiplier helper. */
	float GetHealthForWave(int32 WaveIndex) const { return BaseHealth + HealthPerWave * WaveIndex; }

	float GetSpeedForWave(int32 WaveIndex) const { return MoveSpeed + SpeedPerWave * WaveIndex; }

	float GetContactDamageForWave(int32 WaveIndex) const { return ContactDamage + DamagePerWave * WaveIndex; }
};
