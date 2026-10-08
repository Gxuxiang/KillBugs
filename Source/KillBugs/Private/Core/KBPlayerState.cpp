#include "Core/KBPlayerState.h"

#include "Combat/KBStatSheetComponent.h"
#include "Combat/KBWeaponInventoryComponent.h"
#include "Containers/Ticker.h"
#include "Core/KBCharacter.h"
#include "Core/KBPlayerController.h"
#include "Data/KBCardDefinition.h"
#include "EngineUtils.h"
#include "Loot/KBLootDirector.h"
#include "Data/KBWeaponDefinition.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "KBGameSettings.h"
#include "KillBugs.h"
#include "Net/UnrealNetwork.h"
#include "Persistence/KBProfileSubsystem.h"
#include "UI/KBBackpackModel.h"
#include "UObject/UObjectIterator.h"

AKBPlayerState::AKBPlayerState()
{
}

void AKBPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKBPlayerState, KBPlayerIndex);
	DOREPLIFETIME(AKBPlayerState, Gold);
	DOREPLIFETIME(AKBPlayerState, Materials);
	DOREPLIFETIME(AKBPlayerState, Medkits);
	DOREPLIFETIME(AKBPlayerState, XP);
	DOREPLIFETIME(AKBPlayerState, PlayerLevel);
	DOREPLIFETIME(AKBPlayerState, bIsDowned);
	DOREPLIFETIME(AKBPlayerState, PendingCardChoices);
	DOREPLIFETIME(AKBPlayerState, bHasPickedCard);
	DOREPLIFETIME(AKBPlayerState, TakenCards);
}

void AKBPlayerState::SetKBPlayerIndex(int32 InIndex)
{
	if (!HasAuthority())
	{
		return;
	}

	KBPlayerIndex = InIndex;
	OnRunStateChanged.Broadcast();
}

void AKBPlayerState::AddGold(int32 Amount)
{
	if (!HasAuthority() || Amount == 0)
	{
		return;
	}

	Gold = FMath::Max(0, Gold + Amount);
	OnRunStateChanged.Broadcast();
}

void AKBPlayerState::BeginPlay()
{
	Super::BeginPlay();

	// Server only, and the same rule the weapon loadout follows: a client must never author what
	// it carries, so the seed is applied on the authority from this machine's own profile.
	if (!HasAuthority())
	{
		return;
	}

	const UGameInstance* GameInstance = GetGameInstance();
	const UKBProfileSubsystem* Profile =
		GameInstance ? GameInstance->GetSubsystem<UKBProfileSubsystem>() : nullptr;

	if (!Profile)
	{
		// Deliberately not a silent grant. A backpack handed out here would be the same "the wipe
		// cost you nothing" bug the weapon loadout refuses to have.
		UE_LOG(LogKillBugs, Warning,
			TEXT("KB Backpack: no profile subsystem - starting a run with an empty backpack"));
		return;
	}

	SeedBackpack(Profile->GetBankedMedkits());
}

int32 AKBPlayerState::GetCarriedWeight() const
{
	return Materials * FMath::Max(0, KBSettings().MaterialUnitWeight)
		+ Medkits * FMath::Max(0, KBSettings().MedkitWeight);
}

int32 AKBPlayerState::GetBackpackCapacity() const
{
	return FMath::Max(0, KBSettings().BackpackCapacity);
}

bool AKBPlayerState::TryAddMaterials(int32 Amount, FString& OutReason)
{
	if (!HasAuthority() || Amount <= 0)
	{
		OutReason = TEXT("无效的数量");
		return false;
	}

	const int32 AddedWeight = Amount * FMath::Max(0, KBSettings().MaterialUnitWeight);
	const int32 Room = GetBackpackCapacity() - GetCarriedWeight();

	if (AddedWeight > Room)
	{
		// The numbers, not just the word "full": "why can I not pick this up" is the question
		// this line exists to answer, and a bare refusal does not.
		OutReason = FString::Printf(TEXT("背包放不下（材料 x%d 需 %d，只剩 %d）"), Amount, AddedWeight, Room);
		return false;
	}

	Materials += Amount;
	OnRunStateChanged.Broadcast();
	return true;
}

bool AKBPlayerState::TryAddMedkits(int32 Amount, FString& OutReason)
{
	if (!HasAuthority() || Amount <= 0)
	{
		OutReason = TEXT("无效的数量");
		return false;
	}

	const int32 AddedWeight = Amount * FMath::Max(0, KBSettings().MedkitWeight);
	const int32 Room = GetBackpackCapacity() - GetCarriedWeight();

	if (AddedWeight > Room)
	{
		OutReason = FString::Printf(TEXT("背包放不下（药包 x%d 需 %d，只剩 %d）"), Amount, AddedWeight, Room);
		return false;
	}

	Medkits += Amount;
	OnRunStateChanged.Broadcast();
	return true;
}

bool AKBPlayerState::TryRemoveMaterials(int32 Amount, FString& OutReason)
{
	if (!HasAuthority() || Amount <= 0)
	{
		OutReason = TEXT("无效的数量");
		return false;
	}

	if (Materials < Amount)
	{
		OutReason = FString::Printf(TEXT("材料不足（有 %d，要丢 %d）"), Materials, Amount);
		return false;
	}

	Materials -= Amount;
	OnRunStateChanged.Broadcast();
	return true;
}

bool AKBPlayerState::TryRemoveMedkits(int32 Amount, FString& OutReason)
{
	if (!HasAuthority() || Amount <= 0)
	{
		OutReason = TEXT("无效的数量");
		return false;
	}

	if (Medkits < Amount)
	{
		OutReason = FString::Printf(TEXT("药包不足（有 %d，要丢 %d）"), Medkits, Amount);
		return false;
	}

	Medkits -= Amount;
	OnRunStateChanged.Broadcast();
	return true;
}

void AKBPlayerState::SeedBackpack(int32 InMedkits)
{
	if (!HasAuthority() || bBackpackSeeded)
	{
		return;
	}

	bBackpackSeeded = true;

	if (InMedkits <= 0)
	{
		return;
	}

	// Defensive clamp, and loud about it: a stash larger than the pack would otherwise be an
	// unexplained shortfall, and the player would have no way to tell it from a lost save.
	const int32 WeightEach = FMath::Max(0, KBSettings().MedkitWeight);
	const int32 FitByWeight = WeightEach > 0 ? GetBackpackCapacity() / WeightEach : InMedkits;
	const int32 Seeded = FMath::Min(InMedkits, FitByWeight);

	if (Seeded < InMedkits)
	{
		UE_LOG(LogKillBugs, Warning,
			TEXT("KB Backpack: %d medkit(s) in the stash but only %d fit the %d-weight pack - "
			     "the rest stay in the stash"),
			InMedkits, Seeded, GetBackpackCapacity());
	}

	Medkits = Seeded;
	OnRunStateChanged.Broadcast();

	UE_LOG(LogKillBugs, Display,
		TEXT("KB Backpack: seeded %d medkit(s) from the stash (weight %d/%d)"),
		Medkits, GetCarriedWeight(), GetBackpackCapacity());
}

bool AKBPlayerState::TryUseMedkit(FString& OutReason)
{
	if (!HasAuthority())
	{
		return false;
	}

	// A downed player is not somewhere they can act from - the same rule the pickup loop and the
	// channel gate already apply.
	if (IsDowned())
	{
		OutReason = TEXT("倒地时不能用");
		return false;
	}

	if (Medkits <= 0)
	{
		OutReason = TEXT("没有药包");
		return false;
	}

	const APawn* Pawn = GetPawn();
	UKBStatSheetComponent* Stats = Pawn ? Pawn->FindComponentByClass<UKBStatSheetComponent>() : nullptr;
	if (!Stats)
	{
		OutReason = TEXT("取不到生命值");
		return false;
	}

	// The old "never spend a medkit for nothing" rule, moved from pickup to use. Picking one up
	// is now free of judgement; pressing the key is the judgement, and a full-health press is
	// refused rather than silently wasted.
	if (Stats->GetHealthFraction() >= 1.f)
	{
		OutReason = TEXT("血量已满");
		return false;
	}

	--Medkits;

	const float Before = Stats->GetHealth();
	Stats->Heal(KBSettings().MedkitHealAmount);

	OnRunStateChanged.Broadcast();

	UE_LOG(LogKillBugs, Display,
		TEXT("KB Backpack: %s used a medkit (health %.0f -> %.0f, %d left, weight %d/%d)"),
		*GetNameSafe(Pawn), Before, Stats->GetHealth(), Medkits, GetCarriedWeight(), GetBackpackCapacity());

	return true;
}

void AKBPlayerState::SeedGold(int32 InGold)
{
	if (!HasAuthority() || bGoldSeeded)
	{
		return;
	}

	bGoldSeeded = true;

	// Set, not added: the profile total IS this player's starting gold. Clamped because the value
	// arrives from a file this process did not write.
	Gold = FMath::Clamp(InGold, 0, TNumericLimits<int32>::Max() / 2);

	OnRunStateChanged.Broadcast();

	UE_LOG(LogKillBugs, Display, TEXT("KBProfile: seeded player %d with %d gold"),
		KBPlayerIndex, Gold);
}

void AKBPlayerState::AddXP(int32 Amount)
{
	if (!HasAuthority() || Amount == 0)
	{
		return;
	}

	XP = FMath::Max(0, XP + Amount);

	// Recomputed from the total rather than incremented, so the level cannot drift away from the
	// XP it is supposed to represent however many times this is called.
	PlayerLevel = ComputeLevelForXP(XP);

	OnRunStateChanged.Broadcast();
}

int32 AKBPlayerState::ComputeLevelForXP(int32 InXP) const
{
	const int32 PerLevel = FMath::Max(1, KBSettings().XpPerLevel);

	// Level N -> N+1 costs PerLevel * N, so early levels come quickly and later ones take
	// progressively longer. Walking the series is fine: the loop runs once per level earned, and
	// the number of levels reachable in a run is small.
	int32 Level = 1;
	int32 Remaining = InXP;

	while (Remaining >= PerLevel * Level)
	{
		Remaining -= PerLevel * Level;
		++Level;
	}

	return Level;
}

void AKBPlayerState::SetDowned(bool bInDowned)
{
	if (!HasAuthority() || bIsDowned == bInDowned)
	{
		return;
	}

	bIsDowned = bInDowned;
	OnRunStateChanged.Broadcast();
}

void AKBPlayerState::SetPendingCardChoices(const TArray<TObjectPtr<UKBCardDefinition>>& Choices)
{
	if (!HasAuthority())
	{
		return;
	}

	PendingCardChoices = Choices;

	// Empty choices means "nothing to offer", which is not the same as "already picked" -
	// either way there is nothing for this player to do, so treat it as done.
	bHasPickedCard = PendingCardChoices.Num() == 0;
	OnRunStateChanged.Broadcast();
}

void AKBPlayerState::MarkCardPicked()
{
	if (!HasAuthority())
	{
		return;
	}

	bHasPickedCard = true;
	OnRunStateChanged.Broadcast();
}

void AKBPlayerState::RecordTakenCard(UKBCardDefinition* Card)
{
	if (!HasAuthority() || !Card)
	{
		return;
	}

	TakenCards.Add(Card);
}

bool AKBPlayerState::HasTakenCard(const UKBCardDefinition* Card) const
{
	return Card && TakenCards.Contains(Card);
}

UKBWeaponInventoryComponent* AKBPlayerState::GetWeaponInventory() const
{
	const APlayerController* PlayerController = GetPlayerController();
	const APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
	return Pawn ? Pawn->FindComponentByClass<UKBWeaponInventoryComponent>() : nullptr;
}

// ---------------------------------------------------------------------------------------
// Debug: KB.Player.Damage
// ---------------------------------------------------------------------------------------

/**
 * Hurts the local player by a fixed amount, so a test can put them below full health.
 *
 * Exists for the medkit: a pickup that only fires when the player is hurt cannot be tested by
 * a command that arrives at full health. Goes through the stat sheet so it is the same funnel
 * every other source of damage uses - including the god-mode switch, which will correctly
 * refuse it.
 */
static void KBConsolePlayerDamage(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Player.Damage: expected <amount>"));
		return;
	}

	APlayerController* PlayerController = World->GetFirstPlayerController();
	APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
	UKBStatSheetComponent* Stats = Pawn ? Pawn->FindComponentByClass<UKBStatSheetComponent>() : nullptr;

	if (!Stats)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Player.Damage: no local stat sheet"));
		return;
	}

	const float Amount = FCString::Atof(*Args[0]);
	const float Before = Stats->GetHealth();
	Stats->ApplyDamage(Amount);

	UE_LOG(LogKillBugs, Display, TEXT("KB Loot: damaged the player for %.0f (health %.0f -> %.0f)"),
		Amount, Before, Stats->GetHealth());
}

static FAutoConsoleCommandWithWorldAndArgs KBConsolePlayerDamageCommand(
	TEXT("KB.Player.Damage"),
	TEXT("KB.Player.Damage <n> - hurt the local player, for testing anything that only happens "
	     "below full health. KB.Player.God 0 first, or it does nothing."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsolePlayerDamage));

// ---------------------------------------------------------------------------------------
// Debug: KB.Player.UseMedkit / KB.Player.GiveMedkit
//
// Routed through the pawn's debug hook and through the PlayerState's own mutator, so both
// exercise the real paths rather than a shortcut around them - the same reason
// KB.Weapon.FireManual goes through the character instead of calling the inventory.
// ---------------------------------------------------------------------------------------

static void KBConsolePlayerUseMedkit(const TArray<FString>& Args, UWorld* World)
{
	APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
	AKBCharacter* Character = Cast<AKBCharacter>(Pawn);

	if (!Character)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Player.UseMedkit: no local character"));
		return;
	}

	Character->DebugUseMedkit();
}

static void KBConsolePlayerGiveMedkit(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 1)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Player.GiveMedkit: expected <n>"));
		return;
	}

	const int32 Amount = FCString::Atoi(*Args[0]);

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		AKBPlayerState* PlayerState = It->Get() ? It->Get()->GetPlayerState<AKBPlayerState>() : nullptr;
		if (!PlayerState)
		{
			continue;
		}

		FString Reason;
		if (PlayerState->TryAddMedkits(Amount, Reason))
		{
			UE_LOG(LogKillBugs, Display, TEXT("KB Backpack: +%d medkit(s) (now %d, weight %d/%d)"),
				Amount, PlayerState->GetMedkits(),
				PlayerState->GetCarriedWeight(), PlayerState->GetBackpackCapacity());
		}
		else
		{
			UE_LOG(LogKillBugs, Display, TEXT("KB Backpack: +%d medkit(s) refused - %s"),
				Amount, *Reason);
		}
		return;
	}
}

static FAutoConsoleCommandWithWorldAndArgs KBConsolePlayerUseMedkitCommand(
	TEXT("KB.Player.UseMedkit"),
	TEXT("KB.Player.UseMedkit - use one medkit from the backpack, through the same path the key "
	     "takes. Refused (with the reason) while downed, empty, or at full health."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsolePlayerUseMedkit));

static FAutoConsoleCommandWithWorldAndArgs KBConsolePlayerGiveMedkitCommand(
	TEXT("KB.Player.GiveMedkit"),
	TEXT("KB.Player.GiveMedkit <n> - put n medkits in the local player's backpack, capacity "
	     "permitting. The medkit analogue of KB.Loot.Give."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsolePlayerGiveMedkit));

// ---------------------------------------------------------------------------------------
// Debug: KB.Backpack.* - the in-run panel, its hotbar, and dropping.
//
// Every one of these goes through the same function the key or the menu uses. A command that
// called the PlayerState directly would prove the state machine works and say nothing at all
// about whether the key, the slot lookup or the drop placement do.
// ---------------------------------------------------------------------------------------

namespace KBBackpackDebug
{
	AKBPlayerController* ResolveController(UWorld* World)
	{
		return World ? Cast<AKBPlayerController>(World->GetFirstPlayerController()) : nullptr;
	}

	AKBCharacter* ResolveCharacter(UWorld* World)
	{
		AKBPlayerController* Controller = ResolveController(World);
		return Controller ? Cast<AKBCharacter>(Controller->GetPawn()) : nullptr;
	}

	/** Shared by the commands that name an item. Reports rather than guessing at a typo. */
	bool ParseItem(const FString& Text, EKBItemType& OutType)
	{
		if (Text.Equals(TEXT("material"), ESearchCase::IgnoreCase))
		{
			OutType = EKBItemType::Material;
			return true;
		}

		if (Text.Equals(TEXT("medkit"), ESearchCase::IgnoreCase))
		{
			OutType = EKBItemType::Medkit;
			return true;
		}

		UE_LOG(LogKillBugs, Warning, TEXT("expected <material|medkit>, got '%s'"), *Text);
		return false;
	}

	void UseSlot(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.UseSlot: expected <1-6>"));
			return;
		}

		AKBCharacter* Character = ResolveCharacter(World);
		if (!Character)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.UseSlot: no local character"));
			return;
		}

		Character->DebugUseHotbarSlot(FCString::Atoi(*Args[0]) - 1);
	}

	void Drop(const TArray<FString>& Args, UWorld* World)
	{
		EKBItemType Type;
		if (Args.Num() < 1 || !ParseItem(Args[0], Type))
		{
			return;
		}

		AKBCharacter* Character = ResolveCharacter(World);
		if (!Character)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.Drop: no local character"));
			return;
		}

		Character->DebugDropItem(Type);
	}

	void Assign(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.Assign: expected <1-6> <material|medkit>"));
			return;
		}

		EKBItemType Type;
		if (!ParseItem(Args[1], Type))
		{
			return;
		}

		AKBPlayerController* Controller = ResolveController(World);
		if (!Controller)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.Assign: no local controller"));
			return;
		}

		Controller->SetHotbarSlot(FCString::Atoi(*Args[0]) - 1, Type);
	}

	/**
	 * Empties the run's backpack.
	 *
	 * Both counters, because "clear the backpack" means the thing that is full, and which of the
	 * two that is depends on the run. It does NOT touch the stash - and that distinction is the
	 * whole reason there is a second command: anything banked comes back in on the next run, so
	 * clearing the backpack alone would look like it had done nothing.
	 */
	void Clear(const TArray<FString>& Args, UWorld* World)
	{
		AKBPlayerState* State = ResolveController(World)
			? ResolveController(World)->GetPlayerState<AKBPlayerState>() : nullptr;

		if (!State)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.Clear: no local player state"));
			return;
		}

		const int32 MaterialsBefore = State->GetMaterials();
		const int32 MedkitsBefore = State->GetMedkits();

		FString Reason;
		if (MaterialsBefore > 0)
		{
			State->TryRemoveMaterials(MaterialsBefore, Reason);
		}
		if (MedkitsBefore > 0)
		{
			State->TryRemoveMedkits(MedkitsBefore, Reason);
		}

		UE_LOG(LogKillBugs, Display,
			TEXT("KB Backpack: cleared this run's pack (materials %d -> %d, medkits %d -> %d, weight %d/%d)"),
			MaterialsBefore, State->GetMaterials(), MedkitsBefore, State->GetMedkits(),
			State->GetCarriedWeight(), State->GetBackpackCapacity());
	}

	void ClearStashMedkits(const TArray<FString>& Args, UWorld* World)
	{
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		UKBProfileSubsystem* Profile =
			GameInstance ? GameInstance->GetSubsystem<UKBProfileSubsystem>() : nullptr;

		if (!Profile)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.ClearStashMedkits: no profile subsystem"));
			return;
		}

		Profile->ClearBankedMedkitsForDebug();
	}

	/** Picks a slot and puts it down on another - the gesture the mouse makes, minus the mouse. */
	void Swap(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.Swap: expected <1-6> <1-6>"));
			return;
		}

		AKBPlayerController* Controller = ResolveController(World);
		if (!Controller)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.Swap: no local controller"));
			return;
		}

		Controller->PickHotbarSlot(FCString::Atoi(*Args[0]) - 1);
		Controller->ApplyPickToHotbarSlot(FCString::Atoi(*Args[1]) - 1);
	}

	/** Picks a slot and puts it down on the backpack, i.e. takes it off the bar. */
	void Unequip(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.Unequip: expected <1-6>"));
			return;
		}

		AKBPlayerController* Controller = ResolveController(World);
		if (!Controller)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.Unequip: no local controller"));
			return;
		}

		Controller->PickHotbarSlot(FCString::Atoi(*Args[0]) - 1);
		Controller->ApplyPickToBackpack();
	}

	void List(const TArray<FString>& Args, UWorld* World)
	{
		AKBPlayerController* Controller = ResolveController(World);
		const AKBPlayerState* PlayerState =
			Controller ? Controller->GetPlayerState<AKBPlayerState>() : nullptr;

		if (!PlayerState)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.List: no local player state"));
			return;
		}

		UE_LOG(LogKillBugs, Display, TEXT("KB Backpack === panel %s | weight %d/%d ==="),
			Controller->IsBackpackOpen() ? TEXT("open") : TEXT("closed"),
			PlayerState->GetCarriedWeight(), PlayerState->GetBackpackCapacity());

		TArray<FKBBackpackRow> Rows;
		BuildBackpackRows(*PlayerState, Rows);
		UE_LOG(LogKillBugs, Display, TEXT("KB Backpack --- rows (%d) ---"), Rows.Num());
		for (const FKBBackpackRow& Row : Rows)
		{
			UE_LOG(LogKillBugs, Display, TEXT("KB Backpack   %s  %s  %s"),
				*Row.Label, *Row.RightLabel,
				Row.bCanEquip ? TEXT("[equip|drop]") : TEXT("[drop only]"));
		}

		for (int32 SlotIndex = 0; SlotIndex < AKBPlayerController::HotbarSlots; ++SlotIndex)
		{
			EKBItemType Type;
			const bool bFilled = Controller->GetHotbarSlot(SlotIndex, Type);
			UE_LOG(LogKillBugs, Display, TEXT("KB Backpack   slot %d: %s"),
				SlotIndex + 1, bFilled ? *KBItemDisplayName(Type) : TEXT("-"));
		}
	}
}

static FAutoConsoleCommandWithWorldAndArgs KBConsoleBackpackUseSlot(
	TEXT("KB.Backpack.UseSlot"),
	TEXT("KB.Backpack.UseSlot <1-6> - use whatever is in that hotbar slot, through the same path "
	     "the number key takes."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBBackpackDebug::UseSlot));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleBackpackDrop(
	TEXT("KB.Backpack.Drop"),
	TEXT("KB.Backpack.Drop <material|medkit> - drop one, through the same RPC the panel's menu uses."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBBackpackDebug::Drop));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleBackpackAssign(
	TEXT("KB.Backpack.Assign"),
	TEXT("KB.Backpack.Assign <1-6> <material|medkit> - put an item in a hotbar slot. Materials are "
	     "refused: they have no use."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBBackpackDebug::Assign));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleBackpackClear(
	TEXT("KB.Backpack.Clear"),
	TEXT("KB.Backpack.Clear - empty this run's backpack (materials and medkits). The STASH is not "
	     "touched, so whatever is banked comes straight back in on the next run - see "
	     "KB.Backpack.ClearStashMedkits if that is the thing you actually want gone."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBBackpackDebug::Clear));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleBackpackClearStashMedkits(
	TEXT("KB.Backpack.ClearStashMedkits"),
	TEXT("KB.Backpack.ClearStashMedkits - empty the stash of medkits and write the save. Banked "
	     "medkits are the one thing a run always carries in and has no way to spend down, so this "
	     "is the only way back. Gold, materials and weapons are untouched."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBBackpackDebug::ClearStashMedkits));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleBackpackSwap(
	TEXT("KB.Backpack.Swap"),
	TEXT("KB.Backpack.Swap <1-6> <1-6> - swap two hotbar slots, through the same pick-then-place the "
	     "mouse uses."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBBackpackDebug::Swap));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleBackpackUnequip(
	TEXT("KB.Backpack.Unequip"),
	TEXT("KB.Backpack.Unequip <1-6> - take whatever is in that slot off the bar, through the same "
	     "pick-then-place the mouse uses."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBBackpackDebug::Unequip));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleBackpackList(
	TEXT("KB.Backpack.List"),
	TEXT("KB.Backpack.List - the panel's rows, the hotbar, and the weight. Headless evidence for "
	     "what the panel draws, since DrawHUD does not run under -nullrhi."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBBackpackDebug::List));

// ---------------------------------------------------------------------------------------
// KB.Backpack.UiSelfTest
// ---------------------------------------------------------------------------------------

namespace KBBackpackUiTest
{
	/**
	 * Walks the panel's data and the drop path, one step per log line, on a ticker.
	 *
	 * The ticker is not decoration: the central claim - "a dropped item is NOT taken back by the
	 * pickup tick on the next frame" - is a statement about what happens over time, and
	 * `-ExecCmds` runs once in a single frame. That is the same problem the extraction self test
	 * has, and this is the same answer.
	 *
	 * It deliberately does NOT try to prove anything about pixels: DrawHUD never runs under
	 * -nullrhi. What it proves is the model the panel draws from and the actions its menu
	 * triggers, both of which go through the same functions the keys and the menu call.
	 */
	struct FSelfTest
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<AKBCharacter> Character;
		TWeakObjectPtr<AKBPlayerController> Controller;
		TWeakObjectPtr<AKBPlayerState> PlayerState;

		float StartTime = 0.f;
		int32 Step = 0;

		int32 MaterialsAtStart = 0;
		int32 MedkitsAtStart = 0;
		int32 DroppedMaterials = 0;
		int32 DroppedId = INDEX_NONE;
		FVector DroppedLocation = FVector::ZeroVector;

		bool Advance(float DeltaSeconds);
	};

	bool FSelfTest::Advance(float DeltaSeconds)
	{
		UWorld* LiveWorld = World.Get();
		AKBCharacter* Pawn = Character.Get();
		AKBPlayerController* KBController = Controller.Get();
		AKBPlayerState* State = PlayerState.Get();

		if (!LiveWorld || !Pawn || !KBController || !State)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.UiSelfTest: the world or the player went away"));
			return false;
		}

		const float Since = LiveWorld->GetTimeSeconds() - StartTime;
		UKBStatSheetComponent* Stats = Pawn->FindComponentByClass<UKBStatSheetComponent>();

		if (!Stats)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.UiSelfTest: no stat sheet"));
			return false;
		}

		switch (Step)
		{
		case 0:
			StartTime = LiveWorld->GetTimeSeconds();

			// Stock the precondition rather than assuming it. A run starts with whatever the
			// stash held, which is routinely zero materials - and the first version of this test
			// dropped a material it did not have, which reads as a broken drop rather than as a
			// test that never set itself up.
			{
				FString Reason;
				if (!State->TryAddMaterials(5, Reason))
				{
					UE_LOG(LogKillBugs, Warning,
						TEXT("Backpack.UiSelfTest: could not stock materials for the drop step (%s)"), *Reason);
				}
			}

			MaterialsAtStart = State->GetMaterials();
			MedkitsAtStart = State->GetMedkits();

			KBController->SetHotbarSlot(2, EKBItemType::Medkit);

			UE_LOG(LogKillBugs, Display,
				TEXT("Backpack.UiSelfTest 1/7: a medkit must equip to slot 3 - carrying %d medkit(s), %d material(s)"),
				MedkitsAtStart, MaterialsAtStart);
			Step = 1;
			break;

		case 1:
			if (Since < 0.4f)
			{
				break;
			}
			{
				// Equipping the same item to another slot must MOVE it: two keys doing the same
				// thing would make the bar look fuller than it is.
				KBController->SetHotbarSlot(4, EKBItemType::Medkit);

				EKBItemType Type;
				const bool bSlot3Empty = !KBController->GetHotbarSlot(2, Type);
				const bool bSlot5Filled = KBController->GetHotbarSlot(4, Type)
					&& Type == EKBItemType::Medkit;

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.UiSelfTest 2/7: equipping it to slot 5 must CLEAR slot 3 - slot3_empty=%d, ")
					TEXT("slot5=medkit=%d -> %s"),
					static_cast<int32>(bSlot3Empty), static_cast<int32>(bSlot5Filled),
					(bSlot3Empty && bSlot5Filled) ? TEXT("PASS") : TEXT("FAIL"));

				// Materials have no use, so they must not be placeable at all.
				KBController->SetHotbarSlot(0, EKBItemType::Material);
				const bool bMaterialRefused = !KBController->GetHotbarSlot(0, Type);
				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.UiSelfTest 3/7: a material must be REFUSED by the hotbar - slot1_empty=%d -> %s"),
					static_cast<int32>(bMaterialRefused),
					bMaterialRefused ? TEXT("PASS") : TEXT("FAIL"));

				// Below full health, so the use has something to heal - and so a refusal (which is
				// what full health produces) cannot be mistaken for a pass.
				Stats->ApplyDamage(80.f);
			}
			Step = 2;
			break;

		case 2:
			if (Since < 0.8f)
			{
				break;
			}
			{
				const int32 HealthBefore = FMath::RoundToInt(Stats->GetHealth());
				const int32 MedkitsBefore = State->GetMedkits();

				Pawn->DebugUseHotbarSlot(4);

				const int32 Expected = FMath::Min(
					HealthBefore + FMath::RoundToInt(KBSettings().MedkitHealAmount),
					FMath::RoundToInt(Stats->GetMaxHealth()));

				const bool bPass = State->GetMedkits() == MedkitsBefore - 1
					&& FMath::RoundToInt(Stats->GetHealth()) == Expected
					&& HealthBefore < Expected;

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.UiSelfTest 4/7: using slot 5 must spend one and heal %.0f - health %d -> %d ")
					TEXT("(want %d), medkits %d -> %d (want %d) -> %s"),
					KBSettings().MedkitHealAmount, HealthBefore, FMath::RoundToInt(Stats->GetHealth()),
					Expected, MedkitsBefore, State->GetMedkits(), MedkitsBefore - 1,
					bPass ? TEXT("PASS") : TEXT("FAIL"));
			}
			Step = 3;
			break;

		case 3:
			if (Since < 1.2f)
			{
				break;
			}
			{
				// An empty slot is silent by design, but it must also be inert.
				const int32 Before = State->GetMedkits();
				Pawn->DebugUseHotbarSlot(0);
				const bool bInert = State->GetMedkits() == Before;

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.UiSelfTest 5/7: an empty slot must do nothing at all - medkits %d -> %d -> %s"),
					Before, State->GetMedkits(), bInert ? TEXT("PASS") : TEXT("FAIL"));
			}
			Step = 4;
			break;

		case 4:
			if (Since < 1.6f)
			{
				break;
			}
			{
				DroppedMaterials = State->GetMaterials();
				const int32 WeightBefore = State->GetCarriedWeight();

				Pawn->DebugDropItem(EKBItemType::Material);

				// Remembered by id, not by index: the array is the replicated list and an index
				// means nothing once something else is removed.
				DroppedId = INDEX_NONE;
				for (TActorIterator<AKBLootDirector> It(LiveWorld); It; ++It)
				{
					if (It->GetDrops().Items.Num() > 0)
					{
						const FKBItemNetItem& Last = It->GetDrops().Items.Last();
						DroppedId = Last.StableId;
						DroppedLocation = Last.Location;
					}
					break;
				}

				const bool bPass = State->GetMaterials() == DroppedMaterials - 1
					&& State->GetCarriedWeight() == WeightBefore - FMath::Max(1, KBSettings().MaterialUnitWeight)
					&& DroppedId != INDEX_NONE;

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.UiSelfTest 6/7: dropping one must take it out of the pack AND put it on the ground - ")
					TEXT("materials %d -> %d (want %d), weight %d -> %d, drop id %d -> %s"),
					DroppedMaterials, State->GetMaterials(), DroppedMaterials - 1,
					WeightBefore, State->GetCarriedWeight(), DroppedId,
					bPass ? TEXT("PASS") : TEXT("FAIL"));
			}
			Step = 5;
			break;

		case 5:
			// A moment of standing still, with the pickup tick running every frame. This is the
			// step the whole test exists for: a drop placed at the player's feet would be taken
			// back here, and the player would see "dropping does not work".
			if (Since < 2.6f)
			{
				break;
			}
			{
				bool bStillThere = false;
				for (TActorIterator<AKBLootDirector> It(LiveWorld); It; ++It)
				{
					for (const FKBItemNetItem& Drop : It->GetDrops().Items)
					{
						bStillThere = bStillThere || Drop.StableId == DroppedId;
					}
					break;
				}

				const float Distance = FVector::Dist2D(Pawn->GetActorLocation(), DroppedLocation);
				const float PickupRadius = Stats->GetPickupRadius();

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.UiSelfTest 7/7: after a second of standing still the drop must STILL be there - ")
					TEXT("on_ground=%d, %.0f units away, pickup radius %.0f, materials still %d (want %d) -> %s"),
					static_cast<int32>(bStillThere), Distance, PickupRadius,
					State->GetMaterials(), DroppedMaterials - 1,
					(bStillThere && State->GetMaterials() == DroppedMaterials - 1)
						? TEXT("PASS") : TEXT("FAIL"));

				// And it must be recoverable: walk onto it, and the same pickup tick that
				// refused to take it from a distance takes it now.
				Pawn->SetActorLocation(DroppedLocation, false, nullptr, ETeleportType::TeleportPhysics);
			}
			Step = 6;
			break;

		case 6:
			if (Since < 3.4f)
			{
				break;
			}
			{
				const bool bPass = State->GetMaterials() == DroppedMaterials;
				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.UiSelfTest 8/8: walking onto it must pick it back up - materials %d (want %d) -> %s"),
					State->GetMaterials(), DroppedMaterials, bPass ? TEXT("PASS") : TEXT("FAIL"));
			}

			UE_LOG(LogKillBugs, Display, TEXT("Backpack.UiSelfTest: done"));
			return false;

		default:
			return false;
		}

		return true;
	}
} // namespace KBBackpackUiTest

void AKBPlayerState::ConsoleBackpackUiSelfTest(const TArray<FString>& Args, UWorld* World)
{
	AKBPlayerController* Controller = KBBackpackDebug::ResolveController(World);
	AKBCharacter* Character = KBBackpackDebug::ResolveCharacter(World);
	AKBPlayerState* PlayerState = Controller ? Controller->GetPlayerState<AKBPlayerState>() : nullptr;

	if (!World || !Controller || !Character || !PlayerState)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Backpack.UiSelfTest: no local player"));
		return;
	}

	TSharedPtr<KBBackpackUiTest::FSelfTest> Test = MakeShared<KBBackpackUiTest::FSelfTest>();
	Test->World = World;
	Test->Character = Character;
	Test->Controller = Controller;
	Test->PlayerState = PlayerState;

	UE_LOG(LogKillBugs, Display, TEXT("Backpack.UiSelfTest: started"));

	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Test](float DeltaSeconds) -> bool
		{
			return Test->Advance(DeltaSeconds);
		}));
}

// See the note on the sibling in KBLootDirector.cpp: the packaging target builds with unity ON,
// which puts both files in one translation unit, so file-scope statics need module-unique names.
static FAutoConsoleCommandWithWorldAndArgs KBConsoleBackpackUiTest(
	TEXT("KB.Backpack.UiSelfTest"),
	TEXT("KB.Backpack.UiSelfTest - one command walks the panel's chain: equip to a slot, move it to another ")
	TEXT("(the old slot must clear), materials refused by the bar, use-from-slot heals once, an empty slot is ")
	TEXT("inert, dropping takes it out of the pack and puts it on the ground far enough away that it is NOT ")
	TEXT("picked straight back up - and walking onto it is. Eight steps, each logging what it proves."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AKBPlayerState::ConsoleBackpackUiSelfTest));

// ---------------------------------------------------------------------------------------
// Debug: KB.Player.Stats
// ---------------------------------------------------------------------------------------

static void KBConsolePlayerStats(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}

	int32 Reported = 0;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PlayerController = It->Get();
		const AKBPlayerState* PlayerState = PlayerController ? PlayerController->GetPlayerState<AKBPlayerState>() : nullptr;
		if (!PlayerState)
		{
			continue;
		}

		++Reported;
		UE_LOG(LogKillBugs, Display,
			TEXT("Player %d: level %d | xp %d | gold %d | materials %d | medkits %d | weight %d/%d | downed %s"),
			PlayerState->GetKBPlayerIndex(), PlayerState->GetKBLevel(),
			PlayerState->GetXP(), PlayerState->GetGold(), PlayerState->GetMaterials(),
			PlayerState->GetMedkits(), PlayerState->GetCarriedWeight(), PlayerState->GetBackpackCapacity(),
			PlayerState->IsDowned() ? TEXT("yes") : TEXT("no"));

		if (const APawn* Pawn = PlayerController->GetPawn())
		{
			if (const UKBWeaponInventoryComponent* Inventory =
				Pawn->FindComponentByClass<UKBWeaponInventoryComponent>())
			{
				for (const FKBOwnedWeapon& Weapon : Inventory->GetWeapons())
				{
					UE_LOG(LogKillBugs, Display, TEXT("    weapon: %s (level %d)"),
						Weapon.Definition ? *Weapon.Definition->DisplayName.ToString() : TEXT("NONE"),
						Weapon.Level);
				}
			}
		}
	}

	if (Reported == 0)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("KB.Player.Stats: no players in this world"));
	}
}

static FAutoConsoleCommandWithWorldAndArgs KBConsolePlayerStatsCommand(
	TEXT("KB.Player.Stats"),
	TEXT("Log each player's level, XP, gold and owned weapons."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsolePlayerStats));
