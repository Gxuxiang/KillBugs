#include "Core/KBPlayerController.h"

#include "Audio/KBAudioSubsystem.h"
#include "Combat/KBWeaponInventoryComponent.h"
#include "Core/KBCharacter.h"
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
#include "UI/KBBackpackModel.h"
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

	// Medkits read at the same moment, for the same reason: this is the last point at which the
	// backpack exists. They go both ways, like the weapons and unlike the materials.
	const int32 CarriedMedkits = KBPlayerState->GetMedkits();

	// What the player is holding right now, which is the loadout plus anything cards handed out
	// during the run. Read here because it is the last moment it exists: the travel home destroys
	// the pawn, and with it the inventory.
	TArray<FKBSavedWeapon> CarriedWeapons;
	if (const APawn* LocalPawn = GetPawn())
	{
		if (const UKBWeaponInventoryComponent* Inventory =
			LocalPawn->FindComponentByClass<UKBWeaponInventoryComponent>())
		{
			for (const FKBOwnedWeapon& Weapon : Inventory->GetWeapons())
			{
				if (!Weapon.Definition)
				{
					continue;
				}

				FKBSavedWeapon& Entry = CarriedWeapons.AddDefaulted_GetRef();
				Entry.Definition = FSoftObjectPath(Weapon.Definition);
				Entry.Level = Weapon.Level;
				Entry.bEquipped = true;
			}
		}
	}

	if (NewResult == EKBRunResult::Extracted)
	{
		Profile->BankRunMaterials(Carried);

		// Weapons and medkits are the things that go both ways: carried out, they are yours -
		// including the ones a card handed you in there, and including the levels they gained.
		// The medkits are an absolute replacement, not an addition: the whole stash came in with
		// you, so what is still in the backpack is the new stock.
		Profile->ApplyExtractedWeapons(CarriedWeapons);
		Profile->ApplyExtractedMedkits(CarriedMedkits);
		return;
	}

	// The wipe. Materials are simply gone with the PlayerState, but the weapons and the medkits
	// have to be taken OUT of the stash, because that is where they live between runs - and they
	// are the thing the design says a wipe costs you.
	Profile->LoseCarriedWeapons();
	Profile->LoseCarriedMedkits();

	UE_LOG(LogKillBugs, Display,
		TEXT("KBProfile: wiped out - %d carried material(s), %d carried medkit(s) and %d carried weapon(s) lost, not banked"),
		Carried, CarriedMedkits, CarriedWeapons.Num());
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

	// Use-item, built in the same context. Two separate actions rather than one with modifiers:
	// they are different verbs and are read by different components.
	UseItemAction = NewObject<UInputAction>(this, TEXT("KB_UseItemAction"));
	if (!UseItemAction)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Could not create the use-item input action"));
		return;
	}
	UseItemAction->ValueType = EInputActionValueType::Boolean;

	RuntimeMappingContext->MapKey(UseItemAction, EKeys::Q);

	// Backpack panel toggle and its row menu. Two more verbs in the same context.
	BackpackAction = NewObject<UInputAction>(this, TEXT("KB_BackpackAction"));
	if (BackpackAction)
	{
		BackpackAction->ValueType = EInputActionValueType::Boolean;
		RuntimeMappingContext->MapKey(BackpackAction, EKeys::B);
	}
	else
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Could not create the backpack input action"));
	}

	BackpackMenuAction = NewObject<UInputAction>(this, TEXT("KB_BackpackMenuAction"));
	if (BackpackMenuAction)
	{
		BackpackMenuAction->ValueType = EInputActionValueType::Boolean;
		RuntimeMappingContext->MapKey(BackpackMenuAction, EKeys::RightMouseButton);
	}
	else
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Could not create the backpack menu input action"));
	}

	// One action per number key. Built in a loop, but they stay separate objects - see the note
	// on HotbarActions for why a single axis action cannot work here.
	const FKey HotbarKeys[HotbarSlots] =
	{
		EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six
	};

	HotbarActions.Reset();
	for (int32 SlotIndex = 0; SlotIndex < HotbarSlots; ++SlotIndex)
	{
		UInputAction* Action = NewObject<UInputAction>(
			this, *FString::Printf(TEXT("KB_Hotbar%dAction"), SlotIndex + 1));

		if (!Action)
		{
			// Leave a null in the array so the slot numbering still lines up with the keys.
			HotbarActions.Add(nullptr);
			UE_LOG(LogKillBugs, Warning, TEXT("Could not create hotbar action %d"), SlotIndex + 1);
			continue;
		}

		Action->ValueType = EInputActionValueType::Boolean;
		RuntimeMappingContext->MapKey(Action, HotbarKeys[SlotIndex]);
		HotbarActions.Add(Action);
	}
}

bool AKBPlayerController::GetHotbarSlot(int32 SlotIndex, EKBItemType& OutType) const
{
	if (!IsValidHotbarSlot(SlotIndex) || !Hotbar[SlotIndex].IsSet())
	{
		return false;
	}

	OutType = Hotbar[SlotIndex].GetValue();
	return true;
}

void AKBPlayerController::ClearHotbarSlot(int32 SlotIndex)
{
	if (IsValidHotbarSlot(SlotIndex))
	{
		Hotbar[SlotIndex].Reset();
	}
}

void AKBPlayerController::SetHotbarSlot(int32 SlotIndex, EKBItemType Type)
{
	if (!IsValidHotbarSlot(SlotIndex))
	{
		return;
	}

	// The bar holds things you can USE. Materials are currency, so a slot holding them would be
	// a key that does nothing - and the menu already declines to offer it. Refusing here as well
	// means the two can never disagree, including from a console command.
	if (!KBIsUsableItem(Type))
	{
		UE_LOG(LogKillBugs, Warning, TEXT("KB Backpack: %s cannot be put on the hotbar - it has no use"),
			*KBItemDisplayName(Type));
		return;
	}

	// One item type, one slot: assigning something that is already on the bar MOVES it rather
	// than appearing twice. Two slots for the same item would be two keys that do exactly the
	// same thing, and the bar would claim to be fuller than it is.
	for (int32 Other = 0; Other < HotbarSlots; ++Other)
	{
		if (Other != SlotIndex && Hotbar[Other].IsSet() && Hotbar[Other].GetValue() == Type)
		{
			Hotbar[Other].Reset();
		}
	}

	Hotbar[SlotIndex] = Type;

	UE_LOG(LogKillBugs, Display, TEXT("KB Backpack: %s equipped to slot %d"),
		*KBItemDisplayName(Type), SlotIndex + 1);
}

void AKBPlayerController::SwapHotbarSlots(int32 A, int32 B)
{
	if (!IsValidHotbarSlot(A) || !IsValidHotbarSlot(B) || A == B)
	{
		return;
	}

	// A straight exchange, not two SetHotbarSlot calls: that path is what enforces "one item,
	// one slot" by clearing duplicates, and running it twice here would empty both slots.
	const TOptional<EKBItemType> Swapped = Hotbar[A];
	Hotbar[A] = Hotbar[B];
	Hotbar[B] = Swapped;

	UE_LOG(LogKillBugs, Display, TEXT("KB Backpack: slots %d and %d swapped"), A + 1, B + 1);
}

void AKBPlayerController::PickHotbarSlot(int32 SlotIndex)
{
	if (!IsValidHotbarSlot(SlotIndex))
	{
		return;
	}

	PickedKind = EKBBackpackPick::HotbarSlot;
	PickedIndex = SlotIndex;
}

void AKBPlayerController::PickBackpackRow(int32 RowIndex)
{
	const AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>();
	if (!KBPlayerState)
	{
		return;
	}

	TArray<FKBBackpackRow> Rows;
	BuildBackpackRows(*KBPlayerState, Rows);

	if (!Rows.IsValidIndex(RowIndex))
	{
		return;
	}

	// Materials cannot be equipped, so picking one up would be a gesture with no valid
	// destination. Say why rather than silently doing nothing.
	if (!Rows[RowIndex].bCanEquip)
	{
		UE_LOG(LogKillBugs, Display, TEXT("KB Backpack: %s cannot be equipped - it has no use"),
			*Rows[RowIndex].Label);
		return;
	}

	PickedKind = EKBBackpackPick::BackpackRow;
	PickedIndex = RowIndex;
}

void AKBPlayerController::ClearBackpackPick()
{
	PickedKind = EKBBackpackPick::None;
	PickedIndex = INDEX_NONE;
}

bool AKBPlayerController::ApplyPickToHotbarSlot(int32 SlotIndex)
{
	if (!IsValidHotbarSlot(SlotIndex) || PickedKind == EKBBackpackPick::None)
	{
		return false;
	}

	const EKBBackpackPick Kind = PickedKind;
	const int32 From = PickedIndex;

	if (Kind == EKBBackpackPick::HotbarSlot)
	{
		SwapHotbarSlots(From, SlotIndex);
	}
	else if (Kind == EKBBackpackPick::BackpackRow)
	{
		const AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>();
		TArray<FKBBackpackRow> Rows;
		if (!KBPlayerState)
		{
			return false;
		}

		BuildBackpackRows(*KBPlayerState, Rows);
		if (!Rows.IsValidIndex(From) || !Rows[From].bCanEquip)
		{
			return false;
		}

		SetHotbarSlot(SlotIndex, Rows[From].Type);
	}
	else
	{
		return false;
	}

	ClearBackpackPick();
	return true;
}

bool AKBPlayerController::ApplyPickToBackpack()
{
	if (PickedKind != EKBBackpackPick::HotbarSlot)
	{
		// A row dropped onto the backpack is already where it belongs; nothing to move.
		ClearBackpackPick();
		return false;
	}

	const int32 From = PickedIndex;
	EKBItemType Type;
	if (GetHotbarSlot(From, Type))
	{
		UE_LOG(LogKillBugs, Display, TEXT("KB Backpack: %s taken off slot %d"),
			*KBItemDisplayName(Type), From + 1);
	}

	ClearHotbarSlot(From);
	ClearBackpackPick();
	return true;
}

void AKBPlayerController::OpenBackpackMenuUnderCursor()
{
	OpenBackpackMenuRow = INDEX_NONE;

	if (!bBackpackOpen)
	{
		return;
	}

	const AKBHud* Hud = GetHUD<AKBHud>();
	float MouseX = 0.f;
	float MouseY = 0.f;
	if (!Hud || !GetMousePosition(MouseX, MouseY))
	{
		return;
	}

	const FVector2D Cursor(MouseX, MouseY);
	const int32 RowIndex = Hud->HitTestBackpackRow(Cursor);

	// Only a hit opens a menu. A right-click on empty space closes whatever was open, which is
	// how a menu is dismissed without choosing anything from it.
	if (RowIndex == INDEX_NONE)
	{
		return;
	}

	OpenBackpackMenuRow = RowIndex;
	BackpackMenuPosition = Cursor;
}

bool AKBPlayerController::TryHandleBackpackClick()
{
	if (!bBackpackOpen)
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

	// A menu is open: the click either picks an entry or dismisses the menu. Either way it is
	// the panel's click, and the trigger must not also arm - that would put a bullet downrange
	// every time the player picked "drop".
	if (OpenBackpackMenuRow != INDEX_NONE)
	{
		const EKBBackpackAction Action = Hud->HitTestBackpackMenu(FVector2D(MouseX, MouseY));
		const int32 Row = OpenBackpackMenuRow;
		OpenBackpackMenuRow = INDEX_NONE;

		if (Action == EKBBackpackAction::None)
		{
			return true; // dismissed
		}

		const AKBPlayerState* KBPlayerState = GetPlayerState<AKBPlayerState>();
		if (!KBPlayerState)
		{
			return true;
		}

		// Rebuilt here rather than trusted from the HUD: the rows are one frame old, and the
		// thing being acted on is "whatever is at that index now".
		TArray<FKBBackpackRow> Rows;
		BuildBackpackRows(*KBPlayerState, Rows);
		if (!Rows.IsValidIndex(Row))
		{
			return true;
		}

		const FKBBackpackRow& Chosen = Rows[Row];

		if (Action == EKBBackpackAction::Equip)
		{
			if (!Chosen.bCanEquip)
			{
				return true;
			}

			// First free slot. Assigning to whichever number key the player wants is a drag
			// interaction this cut does not have; "put it somewhere" is what was asked for.
			int32 Target = INDEX_NONE;
			for (int32 SlotIndex = 0; SlotIndex < HotbarSlots; ++SlotIndex)
			{
				if (!Hotbar[SlotIndex].IsSet())
				{
					Target = SlotIndex;
					break;
				}
			}

			if (Target == INDEX_NONE)
			{
				// The bar is full: replace the last slot rather than refusing, so "equip" always
				// does something. Six slots and one consumable type make this unreachable today,
				// but the alternative is a menu entry that silently does nothing later.
				Target = HotbarSlots - 1;
			}

			SetHotbarSlot(Target, Chosen.Type);
		}
		else if (Action == EKBBackpackAction::Drop)
		{
			// Server-side, and through the pawn: the drop has to move the item out of a state
			// only the authority owns, and put an actor-visible drop in the world.
			// (Named KBPawn, not Character: APlayerController already has a Character member.)
			if (AKBCharacter* KBPawn = Cast<AKBCharacter>(GetPawn()))
			{
				KBPawn->ServerDropItem(Chosen.Type);
			}
		}

		return true;
	}

	// No menu open: the panel's click-to-move gesture.
	const FVector2D Cursor(MouseX, MouseY);
	const int32 SlotUnder = Hud->HitTestHotbarSlot(Cursor);
	const int32 RowUnder = Hud->HitTestBackpackRow(Cursor);

	if (SlotUnder == INDEX_NONE && RowUnder == INDEX_NONE)
	{
		// Empty space. Put down whatever was picked up, and let the click through so it still
		// fires - the panel is an overlay on a fight, not a modal that disarms you.
		ClearBackpackPick();
		return false;
	}

	if (PickedKind != EKBBackpackPick::None)
	{
		// Something is already held: this click is where it goes.
		if (SlotUnder != INDEX_NONE)
		{
			ApplyPickToHotbarSlot(SlotUnder);
		}
		else
		{
			ApplyPickToBackpack();
		}

		return true;
	}

	// Nothing held yet: pick up what is under the cursor.
	if (SlotUnder != INDEX_NONE)
	{
		PickHotbarSlot(SlotUnder);
	}
	else
	{
		PickBackpackRow(RowUnder);
	}

	return true;
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
