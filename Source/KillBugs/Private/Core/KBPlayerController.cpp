#include "Core/KBPlayerController.h"

#include "Audio/KBAudioSubsystem.h"
#include "Core/KBGameMode.h"
#include "Core/KBGameState.h"
#include "Core/KBPlayerState.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "KillBugs.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "UI/KBHud.h"

AKBPlayerController::AKBPlayerController()
{
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultContext(
		TEXT("/Game/Input/IMC_Default.IMC_Default"));

	if (DefaultContext.Succeeded())
	{
		DefaultMappingContext = DefaultContext.Object;
	}
}

void AKBPlayerController::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Earlier than BeginPlay on purpose. A pawn's SetupPlayerInputComponent can run during
	// possession, and if the fire action did not exist yet the binding would be silently
	// skipped - leaving a trigger that does nothing.
	BuildRuntimeInput();
}

void AKBPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalController())
	{
		// The camera never rotates, so the cursor stays a free, visible pointer for the pawn
		// to deproject onto the arena floor.
		//
		// HideCursorDuringCapture(false) is the important part: manual weapons are
		// hold-to-fire, and with the default capture behaviour the crosshair would vanish
		// for as long as the trigger is held, which makes aiming impossible.
		FInputModeGameAndUI InputMode;
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		SetInputMode(InputMode);

		bShowMouseCursor = true;
	}

	// AddMappingContext is safe on a listen server's host and on a dedicated client alike;
	// GetLocalPlayer() is null for a non-local controller, which the null check covers.
	if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		if (DefaultMappingContext)
		{
			InputSubsystem->AddMappingContext(DefaultMappingContext, 0);
		}

		if (RuntimeMappingContext)
		{
			// One priority above the template context so a future conflicting binding on the
			// same key resolves to ours.
			InputSubsystem->AddMappingContext(RuntimeMappingContext, 1);
		}
	}
}

void AKBPlayerController::BuildRuntimeInput()
{
	// Boolean action: fire is a trigger, not an axis.
	FireAction = NewObject<UInputAction>(this, TEXT("KB_FireAction"));
	if (!FireAction)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Could not create the fire input action"));
		return;
	}
	FireAction->ValueType = EInputActionValueType::Boolean;

	RuntimeMappingContext = NewObject<UInputMappingContext>(this, TEXT("KB_RuntimeMappingContext"));
	if (!RuntimeMappingContext)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Could not create the runtime mapping context"));
		return;
	}

	RuntimeMappingContext->MapKey(FireAction, EKeys::LeftMouseButton);
}

void AKBPlayerController::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKBPlayerController, AimYaw);
}

bool AKBPlayerController::TryPickCardUnderCursor()
{
	const AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>();
	if (!KBPlayerState || !KBPlayerState->HasPendingCardDraft())
	{
		return false;
	}

	// Ignore clicks during the grace period, so a click already in flight when the draft
	// appeared cannot choose for the player.
	const AKBGameState* RunState = GetWorld() ? GetWorld()->GetGameState<AKBGameState>() : nullptr;
	if (RunState && RunState->IsCardDraftInputLocked())
	{
		return false;
	}

	const AKBHud* Hud = GetHUD<AKBHud>();
	if (!Hud)
	{
		return false;
	}

	float MouseX = 0.f;
	float MouseY = 0.f;
	if (!GetMousePosition(MouseX, MouseY))
	{
		return false;
	}

	const int32 Index = Hud->HitTestCard(FVector2D(MouseX, MouseY));
	if (Index == INDEX_NONE)
	{
		return false;
	}

	// Played locally and immediately rather than waiting for the server to confirm: the pick
	// is going to land, and a click with no feedback reads as a dropped input.
	if (UKBAudioSubsystem* Audio = GetWorld() ? GetWorld()->GetSubsystem<UKBAudioSubsystem>() : nullptr)
	{
		Audio->PlayCardPick();
	}

	ServerPickCard(Index);
	return true;
}

bool AKBPlayerController::ServerPickCard_Validate(int32 ChoiceIndex)
{
	// Bounds only; the GameMode re-checks eligibility against the real card pool.
	return ChoiceIndex >= 0 && ChoiceIndex < 16;
}

void AKBPlayerController::ServerPickCard_Implementation(int32 ChoiceIndex)
{
	AKBGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AKBGameMode>() : nullptr;
	if (!GameMode)
	{
		return;
	}

	GameMode->ApplyCardChoice(GetPlayerState<AKBPlayerState>(), ChoiceIndex);
}

void AKBPlayerController::SetAimYaw(float InAimYaw)
{
	if (!HasAuthority())
	{
		return;
	}

	// Phase 5 clamps the rate of change here (e.g. 720 deg/s) so a client cannot snap-spin
	// to a target, and uses the result for both pawn facing and auto-weapon cone checks.
	AimYaw = FRotator::NormalizeAxis(InAimYaw);
}
