#include "Core/KBCharacter.h"

#include "Camera/CameraComponent.h"
#include "Combat/KBStatSheetComponent.h"
#include "Combat/KBWeaponInventoryComponent.h"
#include "Components/CapsuleComponent.h"
#include "Core/KBPlayerController.h"
#include "Core/KBPlayerState.h"
#include "EnhancedInputComponent.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "Interaction/KBChannelComponent.h"
#include "KBConsoleVariables.h"
#include "KBGameSettings.h"
#include "KillBugs.h"
#include "Loot/KBLootDirector.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "UI/KBBackpackModel.h"

AKBCharacter::AKBCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	// The pawn is rotated explicitly in Tick to face the aim point; letting the controller
	// drive yaw as well would fight it.
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->bOrientRotationToMovement = false;
		Movement->MaxWalkSpeed = 700.f;
		Movement->BrakingDecelerationWalking = 2400.f;
	}

	// ---- Mesh --------------------------------------------------------------------------
	// Placeholder mannequin. These are the engine template's assets; swap for real art in
	// Phase 6. Hard-coded paths are acceptable while the character has no art of its own.
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MannequinMesh(
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
	static ConstructorHelpers::FClassFinder<UAnimInstance> MannequinAnimBP(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"));

	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		if (MannequinMesh.Succeeded())
		{
			MeshComp->SetSkeletalMesh(MannequinMesh.Object);
		}
		if (MannequinAnimBP.Succeeded())
		{
			MeshComp->SetAnimInstanceClass(MannequinAnimBP.Class);
		}
		MeshComp->SetRelativeLocation(FVector(0.f, 0.f, -90.f));
		MeshComp->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));

		// A deferred decal projects onto EVERYTHING inside its box, not just the surface it was
		// aimed at. The slime puddles are meant for the floor, and with this on the player wore
		// a flat green splat across the chest for twenty seconds after standing near a kill -
		// the decal projected onto the character like a decal onto a wall, because that is
		// exactly what it is.
		//
		// The camera never gets close enough for decals on the player to be worth having, and
		// this is the only thing that stops them.
		MeshComp->SetReceivesDecals(false);
	}

	// ---- Combat ------------------------------------------------------------------------
	WeaponInventory = CreateDefaultSubobject<UKBWeaponInventoryComponent>(TEXT("WeaponInventory"));
	StatSheet = CreateDefaultSubobject<UKBStatSheetComponent>(TEXT("StatSheet"));

	// The rescue zone. Inactive until this character is downed.
	//
	// Radius and duration are placeholders here and overwritten from KBSettings in BeginPlay,
	// for the same reason the camera values above are: touching another class's CDO while this
	// one is still being constructed is not safe.
	RescueChannel = CreateDefaultSubobject<UKBChannelComponent>(TEXT("RescueChannel"));
	RescueChannel->SetupAttachment(RootComponent);

	// The owner's own pawn does not count, so a downed player cannot revive themselves by lying
	// still inside their own circle - which they always are.
	RescueChannel->Gate = EKBChannelGate::AnyOtherPlayer;

	// Reset, not Pause: walking away from somebody you were picking up should cost the progress.
	// Deliberately the opposite of the extraction zone, which pauses.
	RescueChannel->BreakPolicy = EKBChannelBreak::Reset;

	// ---- Camera ------------------------------------------------------------------------
	CameraBoom = CreateDefaultSubobject<USceneComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	// Absolute rotation is what makes the camera fixed: the boom keeps its own world rotation
	// no matter how the pawn turns. Its LOCATION still follows the pawn.
	CameraBoom->SetUsingAbsoluteRotation(true);
	// Placeholder values; ApplyCameraSettings in BeginPlay overwrites them from
	// UKBGameSettings. Reading the settings here would mean touching another class's CDO while
	// this one is still being constructed, which is not safe.
	CameraBoom->SetRelativeRotation(FRotator(-65.f, 0.f, 0.f));

	TopDownCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("TopDownCamera"));
	TopDownCamera->SetupAttachment(CameraBoom);
	// Sit back along the boom axis, which at CameraPitch puts the camera above and behind
	// the pawn, looking down at it.
	TopDownCamera->SetRelativeLocation(FVector(-1800.f, 0.f, 0.f));
	TopDownCamera->bUsePawnControlRotation = false;

	// ---- Input -------------------------------------------------------------------------
	static ConstructorHelpers::FObjectFinder<UInputAction> MoveInputAction(
		TEXT("/Game/Input/Actions/IA_Move.IA_Move"));

	if (MoveInputAction.Succeeded())
	{
		MoveAction = MoveInputAction.Object;
	}
}

void AKBCharacter::BeginPlay()
{
	Super::BeginPlay();

	ApplyCameraSettings();

	if (StatSheet)
	{
		StatSheet->OnHealthDepleted.AddDynamic(this, &AKBCharacter::HandleHealthDepleted);
	}

	// The rescue zone's shape comes from the settings, here rather than in the constructor,
	// alongside the camera values above and for the same reason.
	if (RescueChannel)
	{
		RescueChannel->Radius = KBSettings().RescueRadius;
		RescueChannel->DurationSeconds = KBSettings().RescueSeconds;

		// Bound once, in BeginPlay, rather than every time this character goes down: AddDynamic
		// does not de-duplicate, so binding in ApplyDownedState would stack a new handler on
		// every death and fire the revive N times on the Nth knockdown.
		RescueChannel->OnChannelComplete.AddDynamic(this, &AKBCharacter::HandleRescued);
	}
}

void AKBCharacter::ApplyCameraSettings()
{
	const UKBGameSettings& GameSettings = UKBGameSettings::Get();

	if (CameraBoom)
	{
		CameraBoom->SetRelativeRotation(FRotator(GameSettings.CameraPitch, 0.f, 0.f));
	}

	if (TopDownCamera)
	{
		TopDownCamera->SetRelativeLocation(FVector(-GameSettings.CameraDistance, 0.f, 0.f));
		TopDownCamera->SetFieldOfView(GameSettings.CameraFieldOfView);
	}
}

void AKBCharacter::HandleHealthDepleted()
{
	// The stat sheet only broadcasts on authority, so this is server-side by construction.
	if (AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>())
	{
		KBPlayerState->SetDowned(true);
	}

	// Guarded so the replicated OnRep does not apply it a second time.
	if (!bDowned)
	{
		bDowned = true;
		ApplyDownedState();
	}

	UE_LOG(LogKillBugs, Display, TEXT("Character downed"));
}

void AKBCharacter::OnRep_bDowned()
{
	if (bDowned)
	{
		ApplyDownedState();
	}
	else
	{
		ApplyRevivedState();
	}
}

void AKBCharacter::ApplyDownedState()
{
	bFireHeld = false;

	// Stop where it stands. Leaving movement enabled would let input drive a corpse around,
	// and the swarm would keep chewing a player who is already out.
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}

	// Also stops the aim update and the manual-weapon tick.
	SetActorTickEnabled(false);

	// The rescue zone opens where the body fell, and stays open until somebody fills it.
	//
	// Note this runs on clients too (via the replicated OnRep), but SetChannelActive is
	// server-only by construction, so only the authority ever starts the timer - everyone else
	// just sees the ring.
	if (RescueChannel)
	{
		RescueChannel->SetChannelActive(true);
	}
}

void AKBCharacter::ApplyRevivedState()
{
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->SetMovementMode(MOVE_Walking);
	}

	SetActorTickEnabled(true);

	// Close the zone. Harmless if it already completed and closed itself - the completion path
	// sets bActive false before broadcasting.
	if (RescueChannel)
	{
		RescueChannel->SetChannelActive(false);
	}
}

void AKBCharacter::ApplyRunOverState()
{
	// The same freeze as going down, by the same means and for the same reason: movement is
	// client-predicted, so this has to be applied on both sides or the owning client keeps walking.
	//
	// What it does NOT do is open a rescue ring. The run is over; there is nobody left to be
	// rescued from, and a ring on every body during the summary would be a lie.
	bFireHeld = false;

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->DisableMovement();
	}

	SetActorTickEnabled(false);
}

void AKBCharacter::OnRep_bRunOver()
{
	if (bRunOver)
	{
		ApplyRunOverState();
	}
}

void AKBCharacter::SetRunOverServer(bool bInRunOver)
{
	if (!HasAuthority() || bRunOver == bInRunOver)
	{
		return;
	}

	bRunOver = bInRunOver;

	// Applied here as well as in the OnRep, because the authority never receives its own rep.
	ApplyRunOverState();
}

void AKBCharacter::HandleRescued()
{
	// The channel only ever completes on the server, so this is server-side by construction -
	// the same argument HandleHealthDepleted makes.
	if (!bDowned)
	{
		return;
	}

	// Half health, not full: being picked up should leave you fragile enough that the next few
	// seconds still matter. KBSettings().ReviveHealthFraction is the knob.
	if (StatSheet)
	{
		StatSheet->Heal(StatSheet->GetMaxHealth() * KBSettings().ReviveHealthFraction);
	}

	if (AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>())
	{
		KBPlayerState->SetDowned(false);
	}

	bDowned = false;
	ApplyRevivedState();

	UE_LOG(LogKillBugs, Display, TEXT("Character revived by a teammate"));
}

// NOTE: AKBCharacter::Revive() used to live here, healing to full and clearing the downed flag.
// Its only caller was AKBGameMode::ReviveDownedPlayers, which revived everybody at the start of
// every wave. That auto-revive is gone - rescue is now something a teammate does - so the
// function had no callers left and was deleted rather than kept "in case". HandleRescued below
// is what replaced it, and it deliberately heals to a fraction instead of to full.

void AKBCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	FVector AimPoint;
	if (ComputeAimPointFromCursor(AimPoint))
	{
		CachedAimPoint = AimPoint;
		bHasAimPoint = true;

		// Face the aim point in yaw only. The camera will not follow, because the boom uses
		// absolute rotation.
		FVector ToAim = AimPoint - GetActorLocation();
		ToAim.Z = 0.f;
		if (!ToAim.IsNearlyZero())
		{
			const float NewYaw = ToAim.Rotation().Yaw;

			if (IsLocallyControlled())
			{
				// Turn immediately so the local player never waits on a round trip, and push
				// the same yaw up so everyone else sees the same facing.
				SetActorRotation(FRotator(0.f, NewYaw, 0.f));
				SendAimToServer(NewYaw);
			}
		}

#if ENABLE_DRAW_DEBUG
		if (bDrawDebugAimPoint)
		{
			DrawDebugSphere(GetWorld(), CachedAimPoint, 40.f, 12, FColor::Yellow, false, -1.f, 0, 2.f);
			DrawDebugLine(GetWorld(), GetActorLocation(), CachedAimPoint, FColor::Yellow, false, -1.f, 0, 1.f);
		}
#endif
	}
	else
	{
		bHasAimPoint = false;
	}

	TickManualWeapon();
}

void AKBCharacter::TickManualWeapon()
{
	// Also guards the case where the trigger was already held when the draft opened.
	if (!bFireHeld || !bHasAimPoint || !WeaponInventory || IsCardDraftOpen())
	{
		return;
	}

	const int32 Slot = WeaponInventory->FindFirstManualWeaponSlot();
	if (Slot == INDEX_NONE)
	{
		return;
	}

	if (IsLocallyControlled() && !HasAuthority())
	{
		// A client cannot fire directly: its own weapon component has no authority, so the
		// call would only move a cooldown the server never sees and no shot would exist.
		//
		// Throttled because the trigger is held, not clicked - the server's own cooldown is
		// what actually limits the rate, this only stops an RPC per frame.
		const UWorld* World = GetWorld();
		const float Now = World ? World->GetTimeSeconds() : 0.f;
		if (Now - LastFireRequestTime < 0.05f)
		{
			return;
		}
		LastFireRequestTime = Now;

		ServerFireManualWeapon(Slot, CachedAimPoint);
		return;
	}

	// Authority path: a listen host or standalone fires straight away.
	WeaponInventory->TryFireManualWeapon(Slot, CachedAimPoint);
}

void AKBCharacter::DebugFireManualWeapon(float Distance)
{
	// There is no cursor in a headless run, so stand in for one: aim straight ahead.
	CachedAimPoint = GetActorLocation() + GetActorForwardVector() * Distance;
	bHasAimPoint = true;

	const bool bWasFiring = bFireHeld;
	bFireHeld = true;
	TickManualWeapon();
	bFireHeld = bWasFiring;

	UE_LOG(LogKillBugs, Display,
		TEXT("DebugFireManual: local=%s authority=%s aim=(%.0f,%.0f)"),
		IsLocallyControlled() ? TEXT("yes") : TEXT("no"),
		HasAuthority() ? TEXT("yes") : TEXT("no"),
		CachedAimPoint.X, CachedAimPoint.Y);
}

void AKBCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKBCharacter, AimYaw);
	DOREPLIFETIME(AKBCharacter, bDowned);
	DOREPLIFETIME(AKBCharacter, bRunOver);
}

void AKBCharacter::SendAimToServer(float NewYaw)
{
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;

	// Throttled on both axes: a stationary cursor sends nothing at all, and a moving one is
	// capped at 20 Hz. Aim is only needed for facing and for validating shots, so a dropped
	// update costs nothing - the next one supersedes it.
	const bool bTurnedEnough = FMath::Abs(FRotator::NormalizeAxis(NewYaw - LastAimSentYaw)) > 1.5f;
	const bool bWaitedEnough = (Now - LastAimSendTime) > 0.05f;

	if (!bTurnedEnough || !bWaitedEnough)
	{
		return;
	}

	LastAimSendTime = Now;
	LastAimSentYaw = NewYaw;
	ServerSetAimYaw(NewYaw);
}

void AKBCharacter::ServerSetAimYaw_Implementation(float NewAimYaw)
{
	const float ClampedYaw = FRotator::NormalizeAxis(NewAimYaw);
	AimYaw = ClampedYaw;

	// The shooter's own client already turned locally; everyone else learns about it here.
	if (!IsLocallyControlled())
	{
		SetActorRotation(FRotator(0.f, ClampedYaw, 0.f));
	}
}

void AKBCharacter::OnRep_AimYaw()
{
	ApplyReplicatedAim();
}

void AKBCharacter::ApplyReplicatedAim()
{
	// The local player is already facing their own cursor, and re-applying a slightly stale
	// replicated value would fight it.
	if (!IsLocallyControlled())
	{
		SetActorRotation(FRotator(0.f, AimYaw, 0.f));
	}
}

bool AKBCharacter::ServerFireManualWeapon_Validate(int32 SlotIndex, FVector_NetQuantize AimPoint)
{
	// Cheap structural checks only. Everything that decides whether the shot is legitimate -
	// slot ownership, weapon behaviour, cooldown, and what the ray actually hits - is done in
	// TryFireManualWeapon against authoritative state.
	return SlotIndex >= 0 && SlotIndex < 16 && !AimPoint.ContainsNaN();
}

void AKBCharacter::ServerFireManualWeapon_Implementation(int32 SlotIndex, FVector_NetQuantize AimPoint)
{
	if (!WeaponInventory)
	{
		return;
	}

	WeaponInventory->TryFireManualWeapon(SlotIndex, AimPoint);
}

void AKBCharacter::StartFire(const FInputActionValue& Value)
{
	// While a card draft is open the trigger picks a card instead of firing. Getting this
	// wrong would let a player shoot during a phase where the swarm is already gone.
	if (AKBPlayerController* KBController = Cast<AKBPlayerController>(GetController()))
	{
		if (KBController->TryPickCardUnderCursor())
		{
			bFireHeld = false;
			return;
		}

		// Then the backpack, for the same reason: choosing an entry from a row's menu is a left
		// click, and without this every "drop" would also put a bullet downrange. Only a click
		// that lands on the panel is consumed, so clicking empty space still fires.
		if (KBController->TryHandleBackpackClick())
		{
			bFireHeld = false;
			return;
		}
	}

	bFireHeld = true;
}

bool AKBCharacter::IsCardDraftOpen() const
{
	// Named KBPlayerState, not PlayerState: APawn already has a PlayerState member and
	// shadowing it is a build error under this project's warning settings.
	const AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>();
	return KBPlayerState && KBPlayerState->HasPendingCardDraft();
}

void AKBCharacter::StopFire(const FInputActionValue& Value)
{
	bFireHeld = false;
}

void AKBCharacter::UseMedkit(const FInputActionValue& Value)
{
	RequestUseMedkit();
}

void AKBCharacter::RequestUseMedkit()
{
	// Unconditional, and that is deliberate: a Server RPC invoked on the authority runs locally,
	// so the listen host and a connecting client take the same line. ServerPickCard relies on
	// exactly this, and branching here would be the thing that broke on the host.
	//
	// Q and the hotbar converge here: both are "use this item", so there is one server entry
	// point and one place a refusal is worded.
	ServerUseItem(EKBItemType::Medkit);
}

bool AKBCharacter::ServerUseItem_Validate(EKBItemType Type)
{
	return true;
}

void AKBCharacter::ServerUseItem_Implementation(EKBItemType Type)
{
	AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>();
	if (!KBPlayerState)
	{
		return;
	}

	FString Reason;
	bool bUsed = false;

	switch (Type)
	{
	case EKBItemType::Medkit:
		bUsed = KBPlayerState->TryUseMedkit(Reason);
		break;

	default:
		// Materials are currency: there is no verb that spends one from your hand. The hotbar
		// refuses to hold them (see KBIsUsableItem), so this is the belt to that braces.
		Reason = TEXT("这个东西不能用");
		break;
	}

	if (!bUsed)
	{
		// Logged, not swallowed. "I pressed the key and nothing happened" is the symptom this
		// whole feature produces when a refusal is silent - and the reasons are ordinary ones
		// (full health, none left, downed), so they are Display, not Warning.
		UE_LOG(LogKillBugs, Display, TEXT("KB Backpack: use refused - %s"), *Reason);
	}
}

bool AKBCharacter::ServerDropItem_Validate(EKBItemType Type)
{
	return true;
}

void AKBCharacter::ServerDropItem_Implementation(EKBItemType Type)
{
	AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>();
	if (!KBPlayerState)
	{
		return;
	}

	FString Reason;
	const bool bRemoved = Type == EKBItemType::Medkit
		? KBPlayerState->TryRemoveMedkits(1, Reason)
		: KBPlayerState->TryRemoveMaterials(1, Reason);

	if (!bRemoved)
	{
		UE_LOG(LogKillBugs, Display, TEXT("KB Backpack: drop refused - %s"), *Reason);
		return;
	}

	// Found by iteration rather than injected: this happens when a player asks for it, a few
	// times a run, and an iterator over the handful of actors in the arena is not worth a
	// lifetime-aware member. (AKBEnemyDirector gets it injected because it drops on every death.)
	UWorld* World = GetWorld();
	AKBLootDirector* Loot = nullptr;
	for (TActorIterator<AKBLootDirector> It(World); It; ++It)
	{
		Loot = *It;
		break;
	}

	if (!Loot)
	{
		// The item is already out of the backpack at this point, so say so - otherwise the
		// player has lost an item to a silent failure.
		UE_LOG(LogKillBugs, Warning,
			TEXT("KB Backpack: no loot director - dropped %s is gone rather than on the ground"),
			*KBItemDisplayName(Type));
		return;
	}

	// IN FRONT, not at the feet. The pickup test is a per-frame 2D distance check, so a drop
	// under the player is taken back on the very next frame and reads as "dropping does not
	// work". The distance is a setting because it has to clear the card-boosted pickup radius.
	FVector Direction = GetAimDirection();
	if (Direction.IsNearlyZero())
	{
		Direction = GetActorForwardVector();
	}
	Direction.Z = 0.f;
	Direction = Direction.GetSafeNormal();

	const float Distance = FMath::Max(KBSettings().DroppedItemDistance, 0.f);
	const FVector Location = GetActorLocation() + Direction * Distance;

	Loot->SpawnDrop(Location, Type, 1);

	UE_LOG(LogKillBugs, Display, TEXT("KB Backpack: dropped %s x1 %.0f units ahead (weight %d/%d)"),
		*KBItemDisplayName(Type), Distance,
		KBPlayerState->GetCarriedWeight(), KBPlayerState->GetBackpackCapacity());
}

void AKBCharacter::UseHotbarSlot(int32 SlotIndex)
{
	const AKBPlayerController* KBController = Cast<AKBPlayerController>(GetController());

	EKBItemType Type;
	if (!KBController || !KBController->GetHotbarSlot(SlotIndex, Type))
	{
		// An empty slot is silent on purpose. The bar draws it as empty, so there is nothing to
		// explain, and a log line per stray keypress would be noise in a run.
		return;
	}

	ServerUseItem(Type);
}

void AKBCharacter::UseHotbarSlot1(const FInputActionValue& Value) { UseHotbarSlot(0); }
void AKBCharacter::UseHotbarSlot2(const FInputActionValue& Value) { UseHotbarSlot(1); }
void AKBCharacter::UseHotbarSlot3(const FInputActionValue& Value) { UseHotbarSlot(2); }
void AKBCharacter::UseHotbarSlot4(const FInputActionValue& Value) { UseHotbarSlot(3); }
void AKBCharacter::UseHotbarSlot5(const FInputActionValue& Value) { UseHotbarSlot(4); }
void AKBCharacter::UseHotbarSlot6(const FInputActionValue& Value) { UseHotbarSlot(5); }

void AKBCharacter::ToggleBackpack(const FInputActionValue& Value)
{
	if (AKBPlayerController* KBController = Cast<AKBPlayerController>(GetController()))
	{
		KBController->ToggleBackpack();
	}
}

void AKBCharacter::OpenBackpackMenu(const FInputActionValue& Value)
{
	if (AKBPlayerController* KBController = Cast<AKBPlayerController>(GetController()))
	{
		KBController->OpenBackpackMenuUnderCursor();
	}
}

void AKBCharacter::DebugUseHotbarSlot(int32 SlotIndex)
{
	UE_LOG(LogKillBugs, Display, TEXT("DebugUseHotbarSlot: slot %d, local=%s authority=%s"),
		SlotIndex + 1,
		IsLocallyControlled() ? TEXT("yes") : TEXT("no"),
		HasAuthority() ? TEXT("yes") : TEXT("no"));

	UseHotbarSlot(SlotIndex);
}

void AKBCharacter::DebugDropItem(EKBItemType Type)
{
	UE_LOG(LogKillBugs, Display, TEXT("DebugDropItem: %s, local=%s authority=%s"),
		*KBItemDisplayName(Type),
		IsLocallyControlled() ? TEXT("yes") : TEXT("no"),
		HasAuthority() ? TEXT("yes") : TEXT("no"));

	ServerDropItem(Type);
}

void AKBCharacter::DebugUseMedkit()
{
	// Logged before the attempt, so the reason that follows reads as its result.
	UE_LOG(LogKillBugs, Display, TEXT("DebugUseMedkit: local=%s authority=%s"),
		IsLocallyControlled() ? TEXT("yes") : TEXT("no"),
		HasAuthority() ? TEXT("yes") : TEXT("no"));

	RequestUseMedkit();
}

void AKBCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EnhancedInput)
	{
		return;
	}

	if (MoveAction)
	{
		EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AKBCharacter::Move);
	}

	// Fire comes from the controller, which builds the action and its binding at runtime.
	const AKBPlayerController* KBController = Cast<AKBPlayerController>(GetController());
	UInputAction* FireAction = KBController ? KBController->GetFireAction() : nullptr;

	if (FireAction)
	{
		EnhancedInput->BindAction(FireAction, ETriggerEvent::Started, this, &AKBCharacter::StartFire);
		EnhancedInput->BindAction(FireAction, ETriggerEvent::Completed, this, &AKBCharacter::StopFire);
	}

	// Same runtime-built pattern as fire, for the same reason: no asset to author, and one place
	// the key is decided.
	UInputAction* UseItemAction = KBController ? KBController->GetUseItemAction() : nullptr;

	if (UseItemAction)
	{
		EnhancedInput->BindAction(UseItemAction, ETriggerEvent::Started, this, &AKBCharacter::UseMedkit);
	}

	// The backpack panel, its row menu, and the six hotbar keys.
	UInputAction* BackpackAction = KBController ? KBController->GetBackpackAction() : nullptr;
	if (BackpackAction)
	{
		EnhancedInput->BindAction(BackpackAction, ETriggerEvent::Started, this, &AKBCharacter::ToggleBackpack);
	}

	UInputAction* BackpackMenuAction = KBController ? KBController->GetBackpackMenuAction() : nullptr;
	if (BackpackMenuAction)
	{
		EnhancedInput->BindAction(BackpackMenuAction, ETriggerEvent::Started, this, &AKBCharacter::OpenBackpackMenu);
	}

	// One binding per slot, each to its own thin handler. See the note on the handlers for why
	// these are not one shared function.
	using FHotbarHandler = void (AKBCharacter::*)(const FInputActionValue&);
	const FHotbarHandler HotbarHandlers[AKBPlayerController::HotbarSlots] =
	{
		&AKBCharacter::UseHotbarSlot1, &AKBCharacter::UseHotbarSlot2, &AKBCharacter::UseHotbarSlot3,
		&AKBCharacter::UseHotbarSlot4, &AKBCharacter::UseHotbarSlot5, &AKBCharacter::UseHotbarSlot6
	};

	int32 BoundHotbarKeys = 0;
	for (int32 SlotIndex = 0; SlotIndex < AKBPlayerController::HotbarSlots; ++SlotIndex)
	{
		if (UInputAction* HotbarAction = KBController ? KBController->GetHotbarAction(SlotIndex) : nullptr)
		{
			EnhancedInput->BindAction(HotbarAction, ETriggerEvent::Started, this, HotbarHandlers[SlotIndex]);
			++BoundHotbarKeys;
		}
	}

	// A silent failure here is exactly the "left click does nothing" symptom, so say so. The
	// hotbar count is on the line because a key that failed to bind is otherwise only noticed
	// when the player presses it in a fight.
	UE_LOG(LogKillBugs, Display,
		TEXT("Input bound: move=%s fire=%s use=%s backpack=%s menukey=%s hotbar=%d/%d (controller=%s)"),
		MoveAction ? TEXT("yes") : TEXT("NO"),
		FireAction ? TEXT("yes") : TEXT("NO"),
		UseItemAction ? TEXT("yes") : TEXT("NO"),
		BackpackAction ? TEXT("yes") : TEXT("NO"),
		BackpackMenuAction ? TEXT("yes") : TEXT("NO"),
		BoundHotbarKeys, AKBPlayerController::HotbarSlots,
		KBController ? TEXT("KBPlayerController") : TEXT("MISSING"));
}

void AKBCharacter::Move(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (Axis.IsNearlyZero())
	{
		return;
	}

	// Camera-relative rather than world-axis, so movement stays correct if the arena is ever
	// authored with a different yaw. With the fixed camera this is a stable mapping.
	const FRotator YawOnly(0.f, CameraBoom ? CameraBoom->GetComponentRotation().Yaw : 0.f, 0.f);
	const FRotationMatrix YawMatrix(YawOnly);

	AddMovementInput(YawMatrix.GetUnitAxis(EAxis::X), Axis.Y);
	AddMovementInput(YawMatrix.GetUnitAxis(EAxis::Y), Axis.X);
}

FVector AKBCharacter::GetAimDirection() const
{
	if (!bHasAimPoint)
	{
		return FVector::ZeroVector;
	}

	FVector Direction = CachedAimPoint - GetActorLocation();
	Direction.Z = 0.f;
	return Direction.GetSafeNormal();
}

bool AKBCharacter::ComputeAimPointFromCursor(FVector& OutAimPoint) const
{
	const APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return false;
	}

	FVector RayOrigin;
	FVector RayDirection;
	if (!PC->DeprojectMousePositionToWorld(RayOrigin, RayDirection))
	{
		return false;
	}

	// Intersect with the horizontal floor plane. A ray that is parallel to the plane never
	// meets it, and one pointing away from it meets it behind the camera - both are rejected
	// rather than producing a garbage point.
	if (FMath::IsNearlyZero(RayDirection.Z))
	{
		return false;
	}

	const float RayT = (ArenaFloorZ - RayOrigin.Z) / RayDirection.Z;
	if (RayT <= 0.f)
	{
		return false;
	}

	FVector HitPoint = RayOrigin + RayDirection * RayT;

	// Clamp the distance from the pawn so a near-horizon cursor cannot aim absurdly far.
	const FVector FromPawn = HitPoint - GetActorLocation();
	const float MaxAimDistanceSq = FMath::Square(MaxAimDistance);
	if (FromPawn.SizeSquared() > MaxAimDistanceSq)
	{
		HitPoint = GetActorLocation() + FromPawn.GetSafeNormal() * MaxAimDistance;
	}

	OutAimPoint = HitPoint;
	return true;
}
