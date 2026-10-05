#include "Core/KBPlayerController.h"

#include "Audio/KBAudioSubsystem.h"
#include "Core/KBGameMode.h"
#include "Core/KBGameState.h"
#include "Core/KBPlayerState.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameInstance.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "KillBugs.h"
#include "Net/UnrealNetwork.h"
#include "Persistence/KBProfileSubsystem.h"
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

	// Only the local controller banks anything: on a listen server every player's controller sees
	// the phase change, and it is this machine's own earnings that go to this machine's profile.
	if (IsLocalController())
	{
		if (const UGameInstance* GameInstance = GetGameInstance())
		{
			if (UKBProfileSubsystem* Profile = GameInstance->GetSubsystem<UKBProfileSubsystem>())
			{
				// A fresh arena means a fresh run, so the once-per-run latch has to clear here -
				// the subsystem outlives the world the last run ended in.
				Profile->BeginRun();
			}
		}
	}
}

void AKBPlayerController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bBoundToRunState)
	{
		TryBindToRunState();
	}
}

void AKBPlayerController::TryBindToRunState()
{
	if (!IsLocalController())
	{
		// Nothing to bind: a remote controller must not bank the local player's gold.
		bBoundToRunState = true;
		return;
	}

	AKBGameState* RunState = GetWorld() ? GetWorld()->GetGameState<AKBGameState>() : nullptr;
	if (!RunState)
	{
		return; // Not yet. Tick will ask again.
	}

	RunState->OnWavePhaseChanged.AddDynamic(this, &AKBPlayerController::HandleWavePhaseChanged);
	RunState->OnRunResultChanged.AddDynamic(this, &AKBPlayerController::HandleRunResultChanged);
	bBoundToRunState = true;

	// Logged because the failure mode this guards against is silence: a controller that never
	// bound would simply never bank anything, and nothing else would say so.
	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: this machine will bank its gold when the run ends"));
}

void AKBPlayerController::HandleWavePhaseChanged(EKBWavePhase NewPhase)
{
	if (NewPhase != EKBWavePhase::RunOver || !IsLocalController())
	{
		return;
	}

	const UGameInstance* GameInstance = GetGameInstance();
	UKBProfileSubsystem* Profile =
		GameInstance ? GameInstance->GetSubsystem<UKBProfileSubsystem>() : nullptr;

	const AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>();
	if (!Profile || !KBPlayerState)
	{
		return;
	}

	// The run's earnings, not a total: AKBPlayerState::Gold means "what this run earned" and
	// nothing here changes that. The bank adds it on top. BankRunGold latches, so both endings
	// reaching RunOver through the same funnel is all this needs.
	Profile->BankRunGold(KBPlayerState->GetGold());
}

void AKBPlayerController::HandleRunResultChanged(EKBRunResult NewResult)
{
	if (!IsLocalController() || NewResult == EKBRunResult::InProgress)
	{
		return;
	}

	const UGameInstance* GameInstance = GetGameInstance();
	UKBProfileSubsystem* Profile =
		GameInstance ? GameInstance->GetSubsystem<UKBProfileSubsystem>() : nullptr;

	const AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>();
	if (!Profile || !KBPlayerState)
	{
		return;
	}

	const int32 Carried = KBPlayerState->GetMaterials();

	if (NewResult == EKBRunResult::Extracted)
	{
		Profile->BankRunMaterials(Carried);
		return;
	}

	// The wipe. Nothing to do - the PlayerState and its materials are destroyed by the travel
	// home - but silence here would be the wrong kind of quiet: the player lost something the
	// design says they were supposed to lose, and the log is where that becomes visible.
	UE_LOG(LogKillBugs, Display,
		TEXT("KBProfile: wiped out - %d carried material(s) lost, not banked"), Carried);
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
