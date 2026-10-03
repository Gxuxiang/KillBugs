#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "KBWeaponInventoryComponent.generated.h"

class UKBWeaponDefinition;
class UKBStatSheetComponent;
class AKBEnemyDirector;
class AKBPlayerState;

/** One weapon this player owns, plus its server-only firing state. */
USTRUCT(BlueprintType)
struct FKBOwnedWeapon
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	TObjectPtr<UKBWeaponDefinition> Definition;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon", meta = (ClampMin = "1"))
	int32 Level = 1;

	/** Server-only. What the firing logic actually gates on. */
	UPROPERTY(Transient)
	float CooldownRemaining = 0.f;

	/**
	 * Server world time of the last shot, replicated so clients can draw a cooldown bar.
	 *
	 * The obvious thing to replicate is CooldownRemaining, but it changes every frame, which
	 * means re-sending the whole weapons array every frame. A fire time changes only when the
	 * weapon fires - a couple of times a second - and the client derives a smooth bar from it
	 * with the cooldown length it can already work out locally from the definition and the
	 * replicated stat sheet.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Weapon")
	float LastFireServerTime = -1000.f;
};

/**
 * The player's weapons and their firing.
 *
 * Auto weapons are fired entirely by the server: acquisition and the cooldown loop both run
 * here, so they behave identically at any latency. Manual weapons take an aim point from the
 * client and resolve it server-side - see TryFireManualWeapon.
 */
UCLASS(ClassGroup = (KillBugs), meta = (BlueprintSpawnableComponent))
class KILLBUGS_API UKBWeaponInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UKBWeaponInventoryComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Server-only. Adds the weapon, or levels it up if already owned. */
	bool GrantWeapon(UKBWeaponDefinition* Definition);

	bool HasWeapon(const UKBWeaponDefinition* Definition, int32& OutLevel) const;

	/** First manual weapon slot, or INDEX_NONE. The pawn's trigger drives this one. */
	int32 FindFirstManualWeaponSlot() const;

	/**
	 * Server-only. Fires the manual weapon in SlotIndex toward AimPoint.
	 *
	 * The caller supplies a world point, never a hit list: the server re-derives the
	 * direction and does its own line test. Returns true if a shot was actually spent.
	 */
	bool TryFireManualWeapon(int32 SlotIndex, const FVector& AimPoint);

	const TArray<FKBOwnedWeapon>& GetWeapons() const { return Weapons; }

	/**
	 * 0 = ready to fire, 1 = just fired. Drives the HUD's cooldown bars.
	 *
	 * Matters most for a manual weapon: without it the player has no way to tell whether the
	 * trigger is being ignored because of the cooldown or because the shot missed.
	 *
	 * NOTE: CooldownRemaining is server-only and not replicated, so on a client this reads
	 * zero for everyone. Phase 5 needs a replicated cooldown before the bar is meaningful in
	 * multiplayer.
	 */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Weapons")
	float GetCooldownFraction(int32 WeaponIndex) const;

	/** Weapons every player starts with. Loaded by path so Phase 3 needs no editor setup. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Weapons")
	TArray<TObjectPtr<UKBWeaponDefinition>> StartingWeapons;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Weapons")
	int32 MaxWeaponSlots = 6;

protected:
	/** Replicated so the HUD can show slots and levels; cooldowns stay server-side. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Weapons")
	TArray<FKBOwnedWeapon> Weapons;

	AKBEnemyDirector* ResolveDirector() const;

	/** The world's projectile simulator, or null before it has been spawned. */
	class AKBProjectileDirector* ResolveProjectileDirector() const;

	AKBPlayerState* ResolveKillerState() const;

	/** The owner's stat sheet, which scales damage and cooldown. May be null. */
	const UKBStatSheetComponent* ResolveStatSheet() const;

	/** The SERVER clock, which both sides use so cooldown maths agrees across the wire. */
	float GetServerTimeNow() const;

	void TickAutoWeapons();
	bool FireAtAcquiredTarget(const UKBWeaponDefinition& Definition, int32 WeaponLevel,
	                          int32 TargetIndex, const FVector& TargetLocation);

	/**
	 * Launches one shot's worth of projectiles, applying the weapon's cone spread.
	 *
	 * Shared by the auto and manual paths so a weapon behaves identically however it is fired.
	 */
	void FireProjectileSpread(const UKBWeaponDefinition& Definition, const FVector& Start,
	                          const FVector& Direction, float Damage, AKBPlayerState* Killer);

	/**
	 * Plays the weapon's muzzle effect and fire sound.
	 *
	 * Called on the machine that fired. Other clients do not hear it yet - see the note on
	 * UKBWeaponDefinition::FireSound.
	 */
	void PlayFireFeedback(const UKBWeaponDefinition& Definition, const FVector& Start,
	                      const FVector& Direction);

	/**
	 * Cached rather than searched per frame. The director is spawned once at run start and
	 * never replaced; the weak pointer covers a level change or a teardown.
	 */
	mutable TWeakObjectPtr<AKBEnemyDirector> CachedDirector;
	mutable TWeakObjectPtr<class AKBProjectileDirector> CachedProjectileDirector;

	// ---- Firing feedback ---------------------------------------------------------------
	// Placeholder visuals drawn with the debug renderer: without them a hitscan weapon is
	// indistinguishable from bugs vanishing on their own. Phase 6 replaces this with Niagara
	// and, in Phase 5, with a replicated shot-event stream so other players see the shots.

	struct FKBShotTracer
	{
		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		float Radius = 0.f;
		FLinearColor Color = FLinearColor::White;
		float ExpireTime = 0.f;
	};

	/** Server-only: records what a shot should look like. */
	void RecordTracer(const FVector& Start, const FVector& End, const FLinearColor& Color);
	void RecordBurst(const FVector& Centre, float Radius, const FLinearColor& Color);

	/** Runs on every machine; draws and expires the queued feedback. */
	void DrawShotFeedback(float DeltaTime);

	TArray<FKBShotTracer> ShotTracers;

	/** Decrements every weapon's cooldown, auto or manual. */
	void TickWeaponCooldowns(float DeltaTime);
};
