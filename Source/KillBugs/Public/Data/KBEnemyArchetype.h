#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "KBEnemyArchetype.generated.h"

class UStaticMesh;
class UMaterialInterface;
class UNiagaraSystem;
class UAnimToTextureDataAsset;
class USoundBase;

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

	/**
	 * The baked vertex animation for Mesh, from Tools/kb_setup_bug_vat.py.
	 *
	 * Needed at DRAW time, not just at bake time: the frame count and sample rate in here are
	 * what tell the swarm each bug's phase through the animation, which is the only way 600
	 * instances avoid marching in lockstep. Leave it unset and the bugs simply stand still in
	 * their rest pose - which is exactly what an instanced static mesh does with no help.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftObjectPtr<UAnimToTextureDataAsset> AnimData;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	TSoftObjectPtr<UMaterialInterface> Material;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	FVector MeshScale = FVector(1.f);

	/**
	 * Degrees added to a bug's facing when its mesh is drawn.
	 *
	 * The swarm turns each instance to face the direction it is travelling, which assumes the
	 * mesh was authored looking down +X. A model authored looking down -X then runs tail-first -
	 * and nothing about the code can tell, because a bug has no obvious front in a bounds check.
	 * 180 corrects exactly that, and 0 leaves a mesh authored the usual way alone.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
	float MeshYawOffset = 0.f;

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

	/**
	 * Burst played where the bug died, on every machine.
	 *
	 * The system should expose these User parameters; UKBGoreComponent writes them:
	 *   User.SplatColor     (colour) - this archetype's SlimeColor
	 *   User.SplatScale     (float)  - from BodyRadius, so a Brute throws more than a Runner
	 *   User.SplatDirection (vector) - the bug's last heading, so a splat can throw forward
	 *
	 * A system that ignores them still works - it just will not be tinted or sized per
	 * archetype. Leaving this unset is also fine: the decal below is the part that reads as
	 * "something died here", and it does not need Niagara at all.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VFX")
	TSoftObjectPtr<UNiagaraSystem> DeathEffect;

	/**
	 * What this bug's insides look like.
	 *
	 * The decal MATERIAL is shared by every archetype and lives in UKBGameSettings; only the
	 * colour is per archetype. That mirrors how the bugs themselves work - one M_KBEnemy tinted
	 * per archetype - and it is what lets the decal pool create one dynamic material per slot
	 * and keep it, instead of rebuilding a material instance on every reuse.
	 *
	 * Deliberately separate from Tint, which colours the BODY: a bug's shell colour and the
	 * colour of what is inside it are two different art decisions, and tying them together
	 * would make every archetype's gore the same hue as its shell by accident rather than on
	 * purpose. Distinguishing archetypes by the colour of the puddle is the point.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "VFX")
	FLinearColor SlimeColor = FLinearColor(0.36f, 0.72f, 0.18f, 1.f);

	// ---- Audio --------------------------------------------------------------------------
	//
	// Two sounds per bug, and the split follows the one that already exists for VFX: one that
	// runs for as long as the bug is doing something (moving), and one that fires at the single
	// instant it stops (dying). Both are optional - an archetype with neither is silent, which
	// is what every archetype is until art lands, and neither breaks anything.
	//
	// Unlike DeathEffect, which is spawned once per bug per death, MoveSound is played by a
	// small POOL: hundreds of bugs cannot each hold a looping voice, so
	// UKBSwarmAudioComponent binds a handful of emitters to whichever bugs are nearest the
	// listener. MoveSoundRadius is therefore a hearing distance, not a per-bug property.

	/**
	 * Looped while this bug is one of the nearest to the listener.
	 *
	 * Should be a LOOPING source. UKBSwarmAudioComponent re-plays a stopped emitter, but a
	 * one-shot will simply restart each time it does, which is audible as a stutter rather
	 * than a bed.
	 *
	 * Leave unset and this archetype is silent while moving - it still takes an emitter slot,
	 * so an archetype with no sound does not crowd out one that has.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audio")
	TSoftObjectPtr<USoundBase> MoveSound;

	/** Gain for MoveSound at zero distance, before the falloff to MoveSoundRadius. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audio", meta = (ClampMin = "0.0"))
	float MoveSoundVolume = 0.6f;

	/**
	 * How far (cm) this bug can be from the listener before its movement sound goes silent.
	 *
	 * The falloff is linear from 1.0 at the bug's position to 0 here, applied by the component
	 * rather than by a USoundAttenuation: the project has no attenuation assets, and a shared
	 * curve would have to be authored before a single bug could be heard.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audio", meta = (ClampMin = "1.0"))
	float MoveSoundRadius = 1800.f;

	/**
	 * Pitch is rolled once per emitter BINDING, not per frame.
	 *
	 * A swarm playing one sound in lockstep reads as a single loud insect; the spread is what
	 * makes it read as a crowd. Rolling per frame instead would warble.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audio", meta = (ClampMin = "0.01"))
	float MoveSoundPitchMin = 0.85f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audio", meta = (ClampMin = "0.01"))
	float MoveSoundPitchMax = 1.15f;

	/**
	 * One-shot played where this bug died, by UKBGoreComponent::OnBugDied.
	 *
	 * Budgeted separately from the movement emitters and from the Niagara burst: a wave ending
	 * removes hundreds of bugs in a single frame, and hundreds of simultaneous voices is a
	 * worse failure than hundreds of simultaneous particles.
	 *
	 * Leave unset and this archetype's deaths are silent.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audio")
	TSoftObjectPtr<USoundBase> DeathSound;

	/** Gain for DeathSound at zero distance. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audio", meta = (ClampMin = "0.0"))
	float DeathSoundVolume = 1.f;

	/** Pitch spread, so a wave of deaths is a chorus rather than one sound played N times. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audio", meta = (ClampMin = "0.01"))
	float DeathSoundPitchMin = 0.9f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Audio", meta = (ClampMin = "0.01"))
	float DeathSoundPitchMax = 1.1f;

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
