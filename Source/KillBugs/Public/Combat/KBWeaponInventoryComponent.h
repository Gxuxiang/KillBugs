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

	// StartingWeapons used to live here - a hardcoded rifle-plus-shotgun loadout. It is gone
	// rather than kept as a fallback: the stash is the only source of what a player carries, and
	// a hardcoded floor under it would quietly cancel the cost of a wipe.

	/**
	 * How many weapons can be carried at once.
	 *
	 * Was an EditDefaultsOnly member, which on a code-created component is not editable anywhere -
	 * and the shop now needs the same number to bound the loadout, so it moved to UKBGameSettings
	 * where every other design value lives. One source, read through here.
	 */
	static int32 GetMaxWeaponSlots();

	/**
	 * Server-only. Replaces the whole weapon list with the stash entries marked equipped.
	 *
	 * Deliberately not built on GrantWeapon: that can only add a weapon or bump a level by one,
	 * and a loadout needs both things it cannot do - set an absolute level (a weapon carried at
	 * level 4) and remove what is there (so a re-apply does not stack). Returns how many were
	 * carried in, which is zero for a player who owns nothing.
	 */
	int32 ApplyLoadout(const TArray<struct FKBSavedWeapon>& Stash);

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
	                          int32 TargetIndex, const FVector& TargetLocation, int32 SlotIndex);

	/**
	 * Plays the weapon's muzzle effect and fire sound on EVERY machine.
	 *
	 * Unreliable on purpose, and for the same reason the shot itself is: a dropped muzzle flash
	 * is a cosmetic miss, and paying for reliability on every bullet would cost far more than the
	 * occasional missing flash is worth.
	 *
	 * The SLOT INDEX crosses the wire, not the weapon asset. A UObject that does not replicate
	 * cannot be an RPC parameter - the engine warns and sends null - and UKBWeaponDefinition is a
	 * DataAsset, so it never replicates. Every client already has the shooter's replicated
	 * Weapons array, so an index is enough for each machine to resolve the same definition.
	 *
	 * Called from the moment the shot is committed, before the delivery switch, so all three
	 * delivery types get feedback. It used to live inside the projectile path, which is why a
	 * radial weapon (the Shockwave) has never had a muzzle flash or a fire sound.
	 */
	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPlayFireFeedback(int32 SlotIndex, FVector_NetQuantize MuzzleLocation,
	                               FVector_NetQuantizeNormal Direction);

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
	 * Pushes the shooter backwards along their own aim, if the weapon asks for it.
	 *
	 * Takes the fire direction rather than reading the pawn's facing: the auto path aims at an
	 * acquired bug and the manual path at a cursor point, and neither is necessarily where the
	 * character happens to be pointing.
	 *
	 * Server-side only, because that is where both fire paths run - which also makes it correct
	 * in multiplayer for free: the character movement component replicates the velocity, so the
	 * owning client sees its own pawn shoved back without a message being written for it.
	 */
	void ApplyRecoil(const UKBWeaponDefinition& Definition, const FVector& FireDirection);

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
