#include "Lobby/KBLobbyPlayerController.h"

#include "Audio/KBAudioSubsystem.h"
#include "Core/KBPlayerState.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "KillBugs.h"
#include "Lobby/KBLobbyGameMode.h"
#include "Lobby/KBLobbyGameState.h"
#include "Net/KBSessionSubsystem.h"
#include "UI/KBLobbyHud.h"

AKBLobbyPlayerController::AKBLobbyPlayerController()
{
}

void AKBLobbyPlayerController::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Before BeginPlay on purpose, matching AKBPlayerController: SetupInputComponent can run
	// during possession, and an action that does not exist yet binds to nothing in silence.
	BuildRuntimeInput();
}

void AKBLobbyPlayerController::BuildRuntimeInput()
{
	ClickAction = NewObject<UInputAction>(this, TEXT("KB_LobbyClickAction"));
	if (!ClickAction)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Lobby: could not create the click input action"));
		return;
	}

	// Boolean: a click is a trigger, not an axis.
	ClickAction->ValueType = EInputActionValueType::Boolean;

	RuntimeMappingContext = NewObject<UInputMappingContext>(this, TEXT("KB_LobbyMappingContext"));
	if (!RuntimeMappingContext)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Lobby: could not create the runtime mapping context"));
		return;
	}

	RuntimeMappingContext->MapKey(ClickAction, EKeys::LeftMouseButton);
}

void AKBLobbyPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalController())
	{
		// Same contract as the arena: a free, always-visible pointer to click with. There is no
		// pawn to aim, so the lock behaviour matters less, but the cursor must not be captured
		// or the buttons cannot be pressed.
		FInputModeGameAndUI InputMode;
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		SetInputMode(InputMode);

		bShowMouseCursor = true;
	}

	if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		if (RuntimeMappingContext)
		{
			InputSubsystem->AddMappingContext(RuntimeMappingContext, 0);
		}
	}
}

void AKBLobbyPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent);
	if (!EnhancedInput || !ClickAction)
	{
		return;
	}

	// Started, not Triggered: one press is one click. Triggered would fire every frame the
	// button is held, which would re-run the action under the player's finger.
	EnhancedInput->BindAction(ClickAction, ETriggerEvent::Started, this,
		&AKBLobbyPlayerController::HandleLobbyClick);

	// Said out loud because the failure is otherwise invisible: with no click action bound the
	// lobby draws perfectly and simply never responds to anything, and a headless run cannot
	// see the difference. This line is what makes the binding checkable without eyes on screen.
	UE_LOG(LogKillBugs, Display, TEXT("Lobby: click input bound"));
}

void AKBLobbyPlayerController::PlayClickFeedback()
{
	// Local and immediate rather than waiting for the server: the action is going to happen,
	// and a click with no sound reads as a dropped input.
	if (UKBAudioSubsystem* Audio = GetWorld() ? GetWorld()->GetSubsystem<UKBAudioSubsystem>() : nullptr)
	{
		Audio->PlayCardPick();
	}
}

void AKBLobbyPlayerController::HandleLobbyClick()
{
	AKBLobbyHud* LobbyHud = GetHUD<AKBLobbyHud>();
	if (!LobbyHud)
	{
		return;
	}

	float MouseX = 0.f;
	float MouseY = 0.f;
	if (!GetMousePosition(MouseX, MouseY))
	{
		return;
	}

	const FVector2D Cursor(MouseX, MouseY);

	const int32 ButtonIndex = LobbyHud->HitTestButton(Cursor);
	if (ButtonIndex != INDEX_NONE)
	{
		UKBSessionSubsystem* Sessions = GetGameInstance()
			? GetGameInstance()->GetSubsystem<UKBSessionSubsystem>() : nullptr;

		switch (static_cast<EKBLobbyButton>(ButtonIndex))
		{
		case EKBLobbyButton::Ready:
		{
			// The button's label says which way it will go, so the current state is what
			// decides the argument - the click is a toggle, not a command.
			const AKBLobbyGameState* LobbyState =
				GetWorld() ? GetWorld()->GetGameState<AKBLobbyGameState>() : nullptr;
			const AKBPlayerState* LocalState = GetPlayerState<AKBPlayerState>();
			const bool bCurrentlyReady = LobbyState && LocalState &&
				LobbyState->IsPlayerReady(LocalState->GetKBPlayerIndex());

			PlayClickFeedback();
			ServerRequestSetReady(!bCurrentlyReady);
			return;
		}

		case EKBLobbyButton::Host:
			if (Sessions)
			{
				PlayClickFeedback();
				Sessions->HostSession();
			}
			return;

		case EKBLobbyButton::Refresh:
			if (Sessions)
			{
				PlayClickFeedback();
				Sessions->FindSessions();
			}
			return;

		case EKBLobbyButton::Start:
			// Not gated on IsHosting() here: the button is drawn greyed for a non-host, and the
			// GameMode re-checks authority anyway. Client-side gating would only be a second,
			// weaker copy of a rule that has to be enforced on the server regardless.
			PlayClickFeedback();
			ServerRequestStart();
			return;

		case EKBLobbyButton::Solo:
			PlayClickFeedback();
			ServerRequestSolo();
			return;

		case EKBLobbyButton::Leave:
			if (Sessions)
			{
				PlayClickFeedback();
				Sessions->LeaveSession();
			}
			return;

		default:
			return;
		}
	}

	const int32 RowIndex = LobbyHud->HitTestServerRow(Cursor);
	if (RowIndex == INDEX_NONE)
	{
		return;
	}

	UKBSessionSubsystem* Sessions = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UKBSessionSubsystem>() : nullptr;
	if (!Sessions || !Sessions->GetSearchResults().IsValidIndex(RowIndex))
	{
		return;
	}

	// The row's own index is not the result index - entries we could not resolve an address for
	// were dropped while building the list, so the array has holes. Joining must use the value
	// the entry carries, not its position on screen.
	PlayClickFeedback();
	Sessions->JoinSession(Sessions->GetSearchResults()[RowIndex].ResultIndex);
}

bool AKBLobbyPlayerController::ServerRequestStart_Validate()
{
	return true;
}

void AKBLobbyPlayerController::ServerRequestStart_Implementation()
{
	if (AKBLobbyGameMode* LobbyGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AKBLobbyGameMode>() : nullptr)
	{
		LobbyGameMode->RequestStartGame(GetPlayerState<AKBPlayerState>());
	}
}

bool AKBLobbyPlayerController::ServerRequestSetReady_Validate(bool bReady)
{
	// No arguments to bound. Which player is being marked is NOT taken from the client - the
	// GameMode readies whoever owns the calling controller - so there is nothing here a
	// tampered client could point at somebody else.
	return true;
}

void AKBLobbyPlayerController::ServerRequestSetReady_Implementation(bool bReady)
{
	if (AKBLobbyGameMode* LobbyGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AKBLobbyGameMode>() : nullptr)
	{
		// GetPlayerState() of THIS controller, never a parameter: a client may only ready
		// itself, and taking the target from the RPC would let one player ready or un-ready
		// everybody else.
		LobbyGameMode->RequestSetReady(GetPlayerState<AKBPlayerState>(), bReady);
	}
}

bool AKBLobbyPlayerController::ServerRequestSolo_Validate()
{
	return true;
}

void AKBLobbyPlayerController::ServerRequestSolo_Implementation()
{
	if (AKBLobbyGameMode* LobbyGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AKBLobbyGameMode>() : nullptr)
	{
		LobbyGameMode->StartSoloRun();
	}
}
