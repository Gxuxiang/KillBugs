#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "KBCharacter.generated.h"

class UCameraComponent;
class USceneComponent;
class UInputAction;
class UKBWeaponInventoryComponent;
class UKBStatSheetComponent;
struct FInputActionValue;

/**
 * Player pawn: fixed-angle follow camera, camera-relative WASD, and cursor-to-world aiming.
 *
 * The camera NEVER rotates. It is parented to a boom whose transform uses absolute rotation,
 * so the pawn spinning to face the aim point does not drag the view around with it. This is
 * the single most important property of the whole feel of the game, and it is why movement
 * input is derived from the camera rather than from the control rotation.
 */
UCLASS()
class KILLBUGS_API AKBCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AKBCharacter();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Revive() used to be declared here: server-side, back on your feet at full health. It had
	// exactly one caller - the auto-revive that fired for every downed player at the start of
	// every wave - and once rescue became something a teammate does, that caller went away and
	// so did the function. HandleRescued below is what replaced it.

	/**
	 * Test hook: fire the manual weapon once, through the same routing the trigger uses.
	 *
	 * A headless run has no cursor and no mouse, so without this the client-to-server fire
	 * path - the whole thing being verified - cannot be exercised at all. Distance is how far
	 * ahead of the pawn the synthetic aim point is placed.
	 */
	UFUNCTION(BlueprintCallable, Category = "KillBugs|Debug")
	void DebugFireManualWeapon(float Distance = 800.f);

	/**
	 * Aim direction, owned by the CLIENT and replicated through the server.
	 *
	 * A client cannot write a replicated property straight to the server - replication only
	 * runs server to client - so the yaw travels up through ServerSetAimYaw and comes back
	 * down as this property. That round trip is also what lets the server sanity-check a
	 * manual shot against where the player was actually pointing.
	 */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Aim")
	float GetAimYaw() const { return AimYaw; }

	/** Client to server. Unreliable: a dropped aim update is corrected by the next one. */
	UFUNCTION(Server, Unreliable)
	void ServerSetAimYaw(float NewAimYaw);

	/**
	 * Client to server: fire the manual weapon in SlotIndex toward AimPoint.
	 *
	 * The client sends a POINT, never a hit result. The server re-derives the direction, runs
	 * its own line test and applies the damage, so a modified client can only aim badly - it
	 * cannot claim a kill.
	 */
	UFUNCTION(Server, Unreliable, WithValidation)
	void ServerFireManualWeapon(int32 SlotIndex, FVector_NetQuantize AimPoint);

	/** Floor point under the cursor. Only meaningful when HasAimPoint() is true. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Aim")
	FVector GetAimPoint() const { return CachedAimPoint; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Aim")
	bool HasAimPoint() const { return bHasAimPoint; }

	/** Unit horizontal direction from the pawn toward the aim point; zero if unavailable. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Aim")
	FVector GetAimDirection() const;

protected:
	void Move(const FInputActionValue& Value);

	/** Trigger pressed / released. Held-fire is resolved every Tick while the flag is set. */
	void StartFire(const FInputActionValue& Value);
	void StopFire(const FInputActionValue& Value);

	/** Fires the first manual weapon at the current aim point, if the trigger is held. */
	void TickManualWeapon();

	/** True while this player has cards on screen and has not chosen one. */
	bool IsCardDraftOpen() const;

	/** Bound to the stat sheet; runs when health reaches zero. */
	UFUNCTION()
	void HandleHealthDepleted();

	UFUNCTION()
	void OnRep_AimYaw();

	/**
	 * Replicated so the CLIENT also stops its own pawn.
	 *
	 * Movement is client-predicted, so stopping it only on the server leaves the owning client
	 * walking its corpse around - the server would keep correcting it back, and the player
	 * would feel a fight they cannot win. Both sides apply the same state.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_bDowned, BlueprintReadOnly, Category = "KillBugs|Health")
	bool bDowned = false;

	UFUNCTION()
	void OnRep_bDowned();

	/** Stops movement, firing and the aim tick. Safe to call on either side. */
	void ApplyDownedState();

	/** Reverses ApplyDownedState. */
	void ApplyRevivedState();

	/**
	 * The rescue zone this character projects while downed.
	 *
	 * Lives on the character rather than being spawned as its own actor because the zone IS this
	 * character's position - a separate actor would be a second thing to keep in step with a
	 * body that never moves, and one more actor per downed player.
	 *
	 * Its radius, duration and Reset policy are configured from KBSettings in the constructor,
	 * where the settings are already being read for movement speed and health.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Health")
	TObjectPtr<class UKBChannelComponent> RescueChannel;

	/** Server-side. Called when the rescue channel fills. */
	UFUNCTION()
	void HandleRescued();

	/** Rotates the pawn to the replicated aim; used for everyone except the local player. */
	void ApplyReplicatedAim();

	UPROPERTY(ReplicatedUsing = OnRep_AimYaw, BlueprintReadOnly, Category = "KillBugs|Aim")
	float AimYaw = 0.f;

	/** Client-side throttle so holding the trigger does not send an RPC every frame. */
	float LastAimSendTime = -1000.f;
	float LastAimSentYaw = 0.f;

	/** Client-side throttle on the fire request; the server's cooldown is the real limit. */
	float LastFireRequestTime = -1000.f;

	/** Throttled upload of the local player's aim; see ServerSetAimYaw. */
	void SendAimToServer(float NewYaw);

	/** Pulls tilt, distance and FOV from UKBGameSettings; see the camera comment above. */
	void ApplyCameraSettings();

	/**
	 * Deprojects the cursor and intersects it with the horizontal floor plane at ArenaFloorZ.
	 * Returns false when the ray is parallel to the floor, points away from it, or the cursor
	 * is not over the viewport.
	 */
	bool ComputeAimPointFromCursor(FVector& OutAimPoint) const;

	// --- Combat -------------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Combat")
	TObjectPtr<UKBWeaponInventoryComponent> WeaponInventory;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Combat")
	TObjectPtr<UKBStatSheetComponent> StatSheet;

	// --- Camera -------------------------------------------------------------------------
	// Not a SpringArm: nothing here needs collision pull-in, and a SpringArm would add a
	// per-frame sweep for no benefit under a fixed overhead view.

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Camera")
	TObjectPtr<USceneComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Camera")
	TObjectPtr<UCameraComponent> TopDownCamera;

	// Tilt, distance and field of view come from UKBGameSettings -> Camera, applied in
	// BeginPlay rather than the constructor: the settings object is a CDO that may not exist
	// yet while this class's own CDO is being constructed.

	// --- Aim ----------------------------------------------------------------------------

	/** Height of the arena floor plane the cursor is projected onto. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Aim")
	float ArenaFloorZ = 0.f;

	/** Cursor-to-pawn distance clamp, so a near-horizon cursor cannot aim across the map. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Aim", meta = (ClampMin = "100.0"))
	float MaxAimDistance = 6000.f;

	/** Phase 1 scaffolding: proves the cursor deprojection is correct. Remove in Phase 3. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Debug")
	bool bDrawDebugAimPoint = true;

	// --- Input --------------------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(Transient)
	FVector CachedAimPoint = FVector::ZeroVector;

	UPROPERTY(Transient)
	bool bHasAimPoint = false;

	/** True while the fire button is held. Manual weapons fire every Tick while set. */
	UPROPERTY(Transient)
	bool bFireHeld = false;
};
