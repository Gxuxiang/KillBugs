#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KBProjectileDirector.generated.h"

class UInstancedStaticMeshComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class USoundBase;
class AKBEnemyDirector;
class AKBPlayerState;

/** One bullet in flight. Never replicated as an actor - see the note on the director. */
struct FKBProjectile
{
	FVector Location = FVector::ZeroVector;

	/** Where it was last frame; the hit test sweeps between the two. */
	FVector PreviousLocation = FVector::ZeroVector;

	FVector Velocity = FVector::ZeroVector;

	float Damage = 0.f;
	float Radius = 0.f;

	/** Travel budget, so a shot that misses everything does not fly forever. */
	float RemainingRange = 0.f;

	/** Named Killer, not Owner: AActor already has an Owner and shadowing it is a build error. */
	TWeakObjectPtr<AKBPlayerState> Killer;

	/** Copied from the firing weapon so the impact can look right without a lookup. */
	TSoftObjectPtr<UNiagaraSystem> ImpactEffect;
	TSoftObjectPtr<USoundBase> ImpactSound;

	/**
	 * This bullet's own effect, when the director has one. Null means it is drawn as an
	 * instanced sphere instead - see UpdateVisuals.
	 *
	 * A raw pointer in a plain struct, which is normally a lifetime hazard. It is safe here for
	 * a specific reason: the component is created with this actor as its outer and registered,
	 * so the actor's OwnedComponents holds the only reference that matters and the GC sees it.
	 * This struct never outlives the actor.
	 */
	TObjectPtr<UNiagaraComponent> EffectComponent;

	/**
	 * Drawn but never resolved.
	 *
	 * Clients build their bullets from a multicast and must not run hit tests: damage is the
	 * server's alone, and a client that resolved its own hits would drift out of agreement
	 * with it.
	 */
	bool bCosmetic = false;
};

/**
 * Every bullet in flight, in one Actor, using the same shape as the enemy swarm: a flat array
 * simulated in a single Tick instead of one Actor per projectile.
 *
 * Bullets are not replicated per-shot. A shot is a cosmetic event, and the plan for Phase 5
 * is a compact replicated event stream rather than an actor or multicast per bullet - at four
 * players and six weapons each, per-shot messages are a bandwidth disaster.
 *
 * Damage resolves on arrival, not on fire: a target that moves out of the way is missed, which
 * is the whole point of having a travelling projectile rather than a hitscan line.
 */
UCLASS()
class KILLBUGS_API AKBProjectileDirector : public AActor
{
	GENERATED_BODY()

public:
	AKBProjectileDirector();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Server-only. Returns the launched projectile's index, or INDEX_NONE. */
	void FireProjectile(const FVector& Start, const FVector& Direction, float Speed,
	                    float Damage, float HitRadius, float MaxRange, AKBPlayerState* InKiller,
	                    UNiagaraSystem* InImpactEffect = nullptr, USoundBase* InImpactSound = nullptr,
	                    bool bCosmetic = false);

	/**
	 * Server-only: fires one trigger pull's worth of shots and tells clients to draw the same
	 * thing.
	 *
	 * Directions are computed by the caller and passed through unchanged, rather than letting
	 * each machine roll its own spread. If the client rolled its own, the cone it drew would
	 * not match the pellets the server actually fired, and shots would visibly hit things they
	 * visibly missed.
	 */
	void FireShot(const FVector& Start, const TArray<FVector>& Directions, float Speed,
	              float Damage, float HitRadius, float MaxRange, AKBPlayerState* InKiller,
	              UNiagaraSystem* InImpactEffect, USoundBase* InImpactSound);

	/**
	 * Cosmetic only. Clients build bullets from this; the server ignores it because it already
	 * has real ones.
	 *
	 * Directions are FVector_NetQuantizeNormal, NOT FVector_NetQuantize. The plain quantized
	 * vector rounds each component to a whole number, which is fine for a position in the
	 * thousands but destroys a unit vector: (0.98, -0.20, 0) and (0.99, 0.13, 0) both round to
	 * (1, 0, 0). Every pellet of a shotgun blast then arrives pointing the same way, they all
	 * spawn and fly as one, and the client draws a single bullet where the server drew six.
	 */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayShot(FVector_NetQuantize Start, const TArray<FVector_NetQuantizeNormal>& Directions,
	                       float Speed, float MaxRange, float HitRadius);

	UFUNCTION(BlueprintPure, Category = "KillBugs|Combat")
	int32 GetProjectileCount() const { return Projectiles.Num(); }

protected:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Projectiles")
	TObjectPtr<UStaticMesh> ProjectileMesh;

	// The bullet's actual look is UKBGameSettings::ProjectileEffect, not a property here, for the
	// same reason the tint is: this class has no Blueprint subclass, so an EditDefaultsOnly
	// property on it would not be editable anywhere - it would look configurable and be
	// unreachable. That field is also one shared system for every bullet rather than a per-weapon
	// one, because a setting is the same on the client as on the server, so a client draws what
	// the host draws with nothing replicated; a per-weapon effect would have to ride inside
	// MulticastPlayShot, which is Unreliable and already carries a direction per pellet.

	/**
	 * NOT configurable on purpose. Each bullet is drawn at exactly its own hit radius, so the
	 * visible bolt and the thing that actually damages are the same size.
	 *
	 * An earlier version drew a fixed 14-unit cube while testing a 60-to-140-unit radius: the
	 * bolts were about two pixels across at the camera's distance, and enemies died while the
	 * bolt was still visibly nowhere near them.
	 */

	// Bullet colour is UKBGameSettings::ProjectileTint, not a property here: this class has no
	// Blueprint subclass, so an EditDefaultsOnly property on it would not be editable anywhere.

	/** Bullets are small and fast; a cap bounds worst-case work if a weapon fires wildly. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Projectiles")
	int32 MaxProjectiles = 256;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Projectiles")
	float ProjectileLifetime = 4.f;

	/**
	 * Minimum gap between impact SOUNDS.
	 *
	 * A shotgun fires six pellets in one frame and they often land together. Six copies of the
	 * same impact stacked on one sample is not six times as loud, it is clipping and mush - it
	 * reads as noise rather than as hits. The visual effects still fire per pellet, because
	 * those need to show where each one landed.
	 *
	 * This class has no Blueprint subclass, so this is a code-level default rather than a
	 * designer-editable property.
	 */
	static constexpr float ImpactSoundMinInterval = 0.05f;

	/** World time of the last impact sound, for the interval above. */
	float LastImpactSoundTime = -1000.f;

private:
	TArray<FKBProjectile> Projectiles;

	UPROPERTY(Transient)
	TObjectPtr<UInstancedStaticMeshComponent> ProjectileInstances;

	TWeakObjectPtr<AKBEnemyDirector> CachedEnemyDirector;

	AKBEnemyDirector* ResolveEnemyDirector();
	void UpdateVisuals();

	/** Plays the firing weapon's impact effect and sound where a bullet landed. */
	void SpawnImpactFeedback(const FKBProjectile& Projectile, const FVector& HitLocation);

	TArray<FTransform> TransformScratch;

	// Perf log accumulators; driven by CVarKBSwarmPerfLog (see KBConsoleVariables.h).
	// Reports flights in progress, which is the one thing that proves bullets are actually
	// travelling rather than damage being applied instantly somewhere.
	double PerfSeconds = 0.0;
	int32 PerfFrames = 0;
	float PerfElapsed = 0.f;
	int32 PerfPeakInFlight = 0;
};
