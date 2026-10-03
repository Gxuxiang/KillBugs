#include "Core/KBCharacter.h"

#include "Camera/CameraComponent.h"
#include "Combat/KBStatSheetComponent.h"
#include "Combat/KBWeaponInventoryComponent.h"
#include "Components/CapsuleComponent.h"
#include "Core/KBPlayerController.h"
#include "Core/KBPlayerState.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "KBConsoleVariables.h"
#include "KBGameSettings.h"
#include "KillBugs.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

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
	}

	// ---- Combat ------------------------------------------------------------------------
	WeaponInventory = CreateDefaultSubobject<UKBWeaponInventoryComponent>(TEXT("WeaponInventory"));
	StatSheet = CreateDefaultSubobject<UKBStatSheetComponent>(TEXT("StatSheet"));

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
}

void AKBCharacter::ApplyRevivedState()
{
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->SetMovementMode(MOVE_Walking);
	}

	SetActorTickEnabled(true);
}

void AKBCharacter::Revive()
{
	if (StatSheet)
	{
		StatSheet->Heal(0.f); // to full
	}

	if (bDowned)
	{
		bDowned = false;
		ApplyRevivedState();
	}
}

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

	// A silent failure here is exactly the "left click does nothing" symptom, so say so.
	UE_LOG(LogKillBugs, Display,
		TEXT("Input bound: move=%s fire=%s (controller=%s)"),
		MoveAction ? TEXT("yes") : TEXT("NO"),
		FireAction ? TEXT("yes") : TEXT("NO"),
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
