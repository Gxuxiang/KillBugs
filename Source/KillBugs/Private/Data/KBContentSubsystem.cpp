#include "Data/KBContentSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Combat/KBWeaponInventoryComponent.h"
#include "Core/KBPlayerState.h"
#include "Data/KBCardDefinition.h"
#include "Data/KBWeaponDefinition.h"
#include "KillBugs.h"

namespace
{
	const TCHAR* const CardSearchPath = TEXT("/Game/KillBugs/Cards");
const TCHAR* const WeaponSearchPath = TEXT("/Game/KillBugs/Weapons");

	/** Rarity is expressed as a divisor on draw weight, so Common needs no special case. */
	float GetRarityWeightMultiplier(EKBCardRarity Rarity)
	{
		switch (Rarity)
		{
		case EKBCardRarity::Rare:      return 0.45f;
		case EKBCardRarity::Epic:      return 0.18f;
		case EKBCardRarity::Legendary: return 0.06f;
		case EKBCardRarity::Common:
		default:                       return 1.f;
		}
	}

	/** How much less likely a card is once this player has already taken it. */
	constexpr float AlreadyTakenWeightScale = 0.3f;
}

void UKBContentSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Deliberately no LoadCards() here - see GetAllCards().
}

const TArray<TObjectPtr<UKBCardDefinition>>& UKBContentSubsystem::GetAllCards() const
{
	// Re-scan while empty. The asset registry may simply not have been ready the first time.
	if (AllCards.Num() == 0)
	{
		LoadCards();
	}

	return AllCards;
}

const TArray<TObjectPtr<UKBWeaponDefinition>>& UKBContentSubsystem::GetAllWeapons() const
{
	// Same lazy-rescan contract as cards, and the same reason: the registry may not have been
	// ready the first time, and scanning in Initialize would find nothing and look like "this
	// project has no weapons".
	if (AllWeapons.Num() == 0)
	{
		LoadWeapons();
	}

	return AllWeapons;
}

void UKBContentSubsystem::LoadWeapons() const
{
	AllWeapons.Reset();

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	FARFilter Filter;
	Filter.ClassPaths.Add(UKBWeaponDefinition::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(WeaponSearchPath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Found;
	AssetRegistryModule.Get().GetAssets(Filter, Found);

	for (const FAssetData& AssetData : Found)
	{
		if (UKBWeaponDefinition* Weapon = Cast<UKBWeaponDefinition>(AssetData.GetAsset()))
		{
			AllWeapons.Add(Weapon);
		}
	}

	// Sorted, because the shop draws them in this order and its console commands index into it -
	// an order that shifts between runs would make "buy catalogue index 2" mean different things.
	// The predicate takes the DEREFERENCED objects, not the pointers: TArray::Sort wraps it for
	// arrays of pointers and hands out references to what they point at.
	AllWeapons.Sort([](const UKBWeaponDefinition& A, const UKBWeaponDefinition& B)
	{
		return A.DisplayName.ToString() < B.DisplayName.ToString();
	});

	UE_LOG(LogKillBugs, Display, TEXT("Content: found %d weapon(s) under %s"),
		AllWeapons.Num(), WeaponSearchPath);
}

void UKBContentSubsystem::LoadCards() const
{
	AllCards.Reset();

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	FARFilter Filter;
	Filter.ClassPaths.Add(UKBCardDefinition::StaticClass()->GetClassPathName());
	Filter.PackagePaths.Add(FName(CardSearchPath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Found;
	AssetRegistryModule.Get().GetAssets(Filter, Found);

	for (const FAssetData& AssetData : Found)
	{
		if (UKBCardDefinition* Card = Cast<UKBCardDefinition>(AssetData.GetAsset()))
		{
			AllCards.Add(Card);
		}
	}

	UE_LOG(LogKillBugs, Display, TEXT("Content: found %d card(s) under %s"),
		AllCards.Num(), CardSearchPath);
}

bool UKBContentSubsystem::IsCardEligible(const AKBPlayerState& PlayerState,
                                         const UKBCardDefinition& Card, int32 WaveIndex) const
{
	if (PlayerState.GetKBLevel() < Card.MinPlayerLevel || WaveIndex < Card.MinWave)
	{
		return false;
	}

	// Repeat rules. Stackable cards skip this check entirely.
	if (Card.RepeatRule != EKBCardRepeatRule::Stackable && PlayerState.HasTakenCard(&Card))
	{
		return false;
	}

	const UKBWeaponInventoryComponent* Inventory = PlayerState.GetWeaponInventory();

	switch (Card.Effect)
	{
	case EKBCardEffect::AddWeapon:
	{
		if (!Card.Weapon || !Inventory)
		{
			return false;
		}

		int32 ExistingLevel = 0;
		if (Inventory->HasWeapon(Card.Weapon, ExistingLevel))
		{
			return false; // already owned; its upgrade card is the one to offer
		}

		return Inventory->GetWeapons().Num() < UKBWeaponInventoryComponent::GetMaxWeaponSlots();
	}

	case EKBCardEffect::UpgradeWeapon:
	{
		if (!Card.RequiresWeapon || !Inventory)
		{
			return false;
		}

		int32 Level = 0;
		if (!Inventory->HasWeapon(Card.RequiresWeapon, Level))
		{
			return false;
		}

		// A maxed weapon must never offer its own upgrade. This is the single biggest source
		// of dead cards in this genre, and it is worth an explicit guard.
		return Level < Card.RequiresWeapon->MaxLevel;
	}

	case EKBCardEffect::StatBoost:
	case EKBCardEffect::Heal:
	default:
		return true;
	}
}

void UKBContentSubsystem::RollCardChoices(const AKBPlayerState& PlayerState, int32 WaveIndex,
                                          int32 CardCount,
                                          TArray<TObjectPtr<UKBCardDefinition>>& OutChoices) const
{
	OutChoices.Reset();

	struct FWeightedCard
	{
		UKBCardDefinition* Card = nullptr;
		float Weight = 0.f;
	};

	const TArray<TObjectPtr<UKBCardDefinition>>& Cards = GetAllCards();

	TArray<FWeightedCard> Candidates;
	Candidates.Reserve(Cards.Num());

	for (UKBCardDefinition* Card : Cards)
	{
		if (!Card || !IsCardEligible(PlayerState, *Card, WaveIndex))
		{
			continue;
		}

		float Weight = Card->Weight * GetRarityWeightMultiplier(Card->Rarity);
		if (PlayerState.HasTakenCard(Card))
		{
			Weight *= AlreadyTakenWeightScale;
		}

		if (Weight > 0.f)
		{
			Candidates.Add({Card, Weight});
		}
	}

	if (Candidates.Num() == 0)
	{
		// Nothing this player can take. Reported honestly rather than padded with cards they
		// cannot use; the caller decides what to do about it.
		UE_LOG(LogKillBugs, Warning,
			TEXT("No eligible cards for player %d at wave %d (%d cards known)"),
			PlayerState.GetKBPlayerIndex(), WaveIndex, AllCards.Num());
		return;
	}

	// Deterministic in (wave, player) so the same run rolls the same cards.
	FRandomStream Stream(WaveIndex * 7919 + PlayerState.GetKBPlayerIndex() * 104729 + 12345);

	// Roulette sampling WITHOUT replacement, so one draft never offers the same card twice.
	const int32 Wanted = FMath::Min(CardCount, Candidates.Num());
	for (int32 Pick = 0; Pick < Wanted; ++Pick)
	{
		float TotalWeight = 0.f;
		for (const FWeightedCard& Candidate : Candidates)
		{
			TotalWeight += Candidate.Weight;
		}

		if (TotalWeight <= 0.f)
		{
			break;
		}

		float Roll = Stream.FRandRange(0.f, TotalWeight);
		int32 Chosen = Candidates.Num() - 1;
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			Roll -= Candidates[Index].Weight;
			if (Roll <= 0.f)
			{
				Chosen = Index;
				break;
			}
		}

		OutChoices.Add(Candidates[Chosen].Card);
		Candidates.RemoveAtSwap(Chosen);
	}
}
