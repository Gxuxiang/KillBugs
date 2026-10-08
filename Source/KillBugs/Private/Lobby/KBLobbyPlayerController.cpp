#include "Lobby/KBLobbyPlayerController.h"

#include "Audio/KBAudioSubsystem.h"
#include "Core/KBPlayerState.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/World.h"
#include "HAL/PlatformApplicationMisc.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "KillBugs.h"
#include "Lobby/KBLobbyGameMode.h"
#include "Lobby/KBLobbyGameState.h"
#include "Net/KBSessionSubsystem.h"
#include "Persistence/KBProfileSubsystem.h"
#include "UI/KBLobbyHud.h"
#include "UI/KBShopModel.h"

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

		// Hand the server this machine's banked total so the lobby's fresh PlayerState can show
		// it. Fire-and-forget: the run does not wait on it, and a failure costs a number on
		// screen rather than anything in the save file, because the bank never reads back from
		// authoritative state.
		if (const UGameInstance* GameInstance = GetGameInstance())
		{
			if (const UKBProfileSubsystem* Profile = GameInstance->GetSubsystem<UKBProfileSubsystem>())
			{
				ServerSeedGold(Profile->GetBankedGold());
			}
		}
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

		case EKBLobbyButton::Shop:
			PlayClickFeedback();
			LobbyHud->ToggleShop();
			return;

		case EKBLobbyButton::ManualJoin:
			PlayClickFeedback();
			LobbyHud->BeginAddressEntry();
			return;

		case EKBLobbyButton::CopyAddress:
			CopyLocalAddress(LobbyHud);
			return;

		default:
			return;
		}
	}

	// The shop takes the click before the server list does. The two never overlap on screen - the
	// panel shows one or the other - but ordering it explicitly is what stops a click from being
	// handled twice if that ever changes.
	if (LobbyHud->IsShopOpen())
	{
		HandleShopClick(LobbyHud);
		return;
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

void AKBLobbyPlayerController::HandleShopClick(AKBLobbyHud* LobbyHud)
{
	const int32 RowIndex = LobbyHud->HitTestShopRow(CursorPosition());
	const FKBShopRow* Row = LobbyHud->GetShopRow(RowIndex);
	if (!Row || !Row->bEnabled)
	{
		// A click on an inert row is still a click on the shop, so the panel's last message is
		// cleared rather than left over from a previous attempt.
		if (Row)
		{
			LobbyHud->SetShopMessage(FString());
		}
		return;
	}

	UKBProfileSubsystem* Profile = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UKBProfileSubsystem>() : nullptr;
	if (!Profile)
	{
		return;
	}

	// The controller maps a click to a subsystem call and nothing more. The rules - what a
	// purchase costs, whether the loadout has room, what a wipe takes - all live in the
	// subsystem, so there is exactly one writer of currency and stash.
	FString Reason;
	bool bDone = false;

	switch (Row->Action)
	{
	case EKBShopAction::SellAllMaterials:
		bDone = Profile->TrySellAllMaterials(Reason);
		break;

	case EKBShopAction::Buy:
		bDone = Row->Weapon && Profile->TryBuyWeapon(*Row->Weapon, Reason);
		break;

	case EKBShopAction::Upgrade:
		bDone = Profile->TryUpgradeWeapon(Row->StashIndex, Reason);
		break;

	case EKBShopAction::ToggleEquip:
	{
		const TArray<FKBSavedWeapon>& Stash = Profile->GetStash();
		const bool bCurrentlyEquipped =
			Stash.IsValidIndex(Row->StashIndex) && Stash[Row->StashIndex].bEquipped;

		bDone = Profile->SetWeaponEquipped(Row->StashIndex, !bCurrentlyEquipped, Reason);
		break;
	}

	default:
		return;
	}

	// Refusals keep their reason on screen; successes say what happened. Both are drawn on the
	// status line the lobby already has, so no new widget is invented for it.
	LobbyHud->SetShopMessage(Reason);
	PlayClickFeedback();
}

namespace
{
	/**
	 * The character a key would type, or false for keys that type nothing.
	 *
	 * An explicit table rather than FKey::GetDisplayName(): the display name is localised and
	 * layout-dependent, so on a French keyboard it would hand back something that is not a digit.
	 */
	bool KeyToAddressCharacter(const FKey& Key, TCHAR& OutCharacter)
	{
		const FKey DigitKeys[10] =
		{
			EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four,
			EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine
		};

		const FKey NumpadKeys[10] =
		{
			EKeys::NumPadZero, EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour,
			EKeys::NumPadFive, EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine
		};

		for (int32 Index = 0; Index < 10; ++Index)
		{
			if (Key == DigitKeys[Index] || Key == NumpadKeys[Index])
			{
				OutCharacter = static_cast<TCHAR>(TEXT('0') + Index);
				return true;
			}
		}

		if (Key == EKeys::Period || Key == EKeys::Decimal)
		{
			OutCharacter = TEXT('.');
			return true;
		}

		return false;
	}
}

bool AKBLobbyPlayerController::InputKey(const FInputKeyEventArgs& EventArgs)
{
	AKBLobbyHud* LobbyHud = GetHUD<AKBLobbyHud>();

	// Everything takes the normal path unless the address field is open - this override exists
	// for one field and must not become a second input system.
	if (!LobbyHud || !LobbyHud->IsTypingAddress())
	{
		return Super::InputKey(EventArgs);
	}

	// Presses and repeats only. Handling the release as well would append every character twice.
	if (EventArgs.Event != IE_Pressed && EventArgs.Event != IE_Repeat)
	{
		return true;
	}

	const FKey Key = EventArgs.Key;

	if (Key == EKeys::Escape)
	{
		LobbyHud->CancelAddressEntry();
		return true;
	}

	if (Key == EKeys::Enter)
	{
		CommitManualJoin(LobbyHud);
		return true;
	}

	if (Key == EKeys::BackSpace)
	{
		LobbyHud->BackspaceAddress();
		return true;
	}

	// Both Ctrl keys: which one a player reaches for is not worth guessing at.
	if (Key == EKeys::V && (IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl)))
	{
		LobbyHud->PasteAddressFromClipboard();
		return true;
	}

	// Not named Character: APlayerController already has a member by that name, and shadowing it
	// is a warning-as-error in this project.
	TCHAR TypedCharacter = 0;
	if (KeyToAddressCharacter(Key, TypedCharacter))
	{
		LobbyHud->AppendAddressChar(TypedCharacter);
	}

	// Everything else is swallowed while the field is open, so a stray key cannot reach the
	// lobby behind it.
	return true;
}

void AKBLobbyPlayerController::CommitManualJoin(AKBLobbyHud* LobbyHud)
{
	const FString Typed = LobbyHud->TakeAddressBuffer();
	if (Typed.IsEmpty())
	{
		LobbyHud->SetShopMessage(TEXT("没有输入地址"));
		return;
	}

	UKBSessionSubsystem* Sessions =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UKBSessionSubsystem>() : nullptr;
	if (!Sessions)
	{
		return;
	}

	const FString Address = UKBSessionSubsystem::NormalizeJoinAddress(Typed);

	// The outcome goes on the status line the lobby already has, rather than into a second
	// place to look - the same bargain HandleShopClick made.
	LobbyHud->SetShopMessage(FString::Printf(TEXT("直连 %s…"), *Address));

	PlayClickFeedback();
	Sessions->JoinAddress(Address);
}

void AKBLobbyPlayerController::CopyLocalAddress(AKBLobbyHud* LobbyHud)
{
	const FString Own = LobbyHud->GetPreferredLocalAddress();
	if (Own.IsEmpty())
	{
		LobbyHud->SetShopMessage(TEXT("没有可复制的地址（没联网？）"));
		return;
	}

	// Copied WITH the port, not just the IP: the other player pastes it straight into the field
	// above, and the port is the part they cannot be expected to know. NormalizeJoinAddress is
	// what decides it, so the clipboard and the typed path agree by construction.
	const FString Address = UKBSessionSubsystem::NormalizeJoinAddress(Own);
	FPlatformApplicationMisc::ClipboardCopy(*Address);

	LobbyHud->SetShopMessage(FString::Printf(TEXT("已复制 %s"), *Address));
	PlayClickFeedback();
}

FVector2D AKBLobbyPlayerController::CursorPosition() const
{
	float MouseX = 0.f;
	float MouseY = 0.f;
	return GetMousePosition(MouseX, MouseY) ? FVector2D(MouseX, MouseY) : FVector2D::ZeroVector;
}

void AKBLobbyPlayerController::ServerSeedGold_Implementation(int32 InGold)
{
	// The GameMode is looked up rather than assumed: this is the second lock on "lobby only",
	// after the fact that this controller class does not exist in the arena. If a version of the
	// arena ever reuses it, the seed still cannot land mid-run.
	AKBLobbyGameMode* LobbyGameMode =
		GetWorld() ? GetWorld()->GetAuthGameMode<AKBLobbyGameMode>() : nullptr;

	if (!LobbyGameMode)
	{
		UE_LOG(LogKillBugs, Warning,
			TEXT("KBProfile: a gold seed arrived outside the lobby - ignored (player %s)"),
			*GetNameSafe(this));
		return;
	}

	// The PlayerState is the CALLING controller's, never one named by the client.
	LobbyGameMode->SeedPlayerGold(GetPlayerState<AKBPlayerState>(), InGold);
}

bool AKBLobbyPlayerController::ServerSeedGold_Validate(int32 InGold)
{
	// Bounded so a tampered client cannot hand the server a negative number or something that
	// would overflow later arithmetic. It cannot check the value is EARNED - the server has no
	// source for that, by design; see the note on AKBPlayerState::SeedGold.
	return InGold >= 0 && InGold <= UKBProfileSubsystem::MaxBankedGold();
}
