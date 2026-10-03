#include "Core/KBPlayerState.h"

#include "Combat/KBWeaponInventoryComponent.h"
#include "Data/KBCardDefinition.h"
#include "Data/KBWeaponDefinition.h"
#include "GameFramework/PlayerController.h"
#include "KillBugs.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UObjectIterator.h"

AKBPlayerState::AKBPlayerState()
{
}

void AKBPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKBPlayerState, KBPlayerIndex);
	DOREPLIFETIME(AKBPlayerState, Gold);
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

void AKBPlayerState::AddXP(int32 Amount)
{
	if (!HasAuthority() || Amount == 0)
	{
		return;
	}

	XP = FMath::Max(0, XP + Amount);
	OnRunStateChanged.Broadcast();
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
			TEXT("Player %d: level %d | xp %d | gold %d | downed %s"),
			PlayerState->GetKBPlayerIndex(), PlayerState->GetKBLevel(),
			PlayerState->GetXP(), PlayerState->GetGold(),
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
