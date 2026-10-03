#include "Combat/KBStatSheetComponent.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "KBGameSettings.h"
#include "Net/UnrealNetwork.h"

UKBStatSheetComponent::UKBStatSheetComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UKBStatSheetComponent::BeginPlay()
{
	Super::BeginPlay();

	// Establish the baseline before any card folds in, so the first max-health card's delta is
	// measured against the real starting value rather than against zero.
	LastKnownMaxHealth = GetMaxHealth();
	CurrentHealth = LastKnownMaxHealth;

	// Push once up front: the movement component's authored MaxWalkSpeed and this sheet's
	// BaseMoveSpeed are two sources of truth, and the sheet's must win or the character
	// silently walks at the wrong speed until the first card is picked.
	PushDerivedValues();
}

void UKBStatSheetComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UKBStatSheetComponent, TotalMods);
	DOREPLIFETIME(UKBStatSheetComponent, CurrentHealth);
}

float UKBStatSheetComponent::GetMaxHealth() const
{
	// Additive on purpose: health is the one stat where a stacking multiplier gets silly fast.
	return FMath::Max(1.f, UKBGameSettings::Get().BaseMaxHealth + TotalMods.MaxHealthAdd);
}

float UKBStatSheetComponent::GetHealthFraction() const
{
	const float Max = GetMaxHealth();
	return Max > 0.f ? FMath::Clamp(CurrentHealth / Max, 0.f, 1.f) : 0.f;
}

float UKBStatSheetComponent::ApplyDamage(float Damage)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || Damage <= 0.f)
	{
		return 0.f;
	}

	const float Applied = FMath::Min(Damage, CurrentHealth);
	CurrentHealth -= Applied;

	if (CurrentHealth <= 0.f)
	{
		CurrentHealth = 0.f;
		OnHealthDepleted.Broadcast();
	}

	return Applied;
}

void UKBStatSheetComponent::Heal(float Amount)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	const float Max = GetMaxHealth();
	CurrentHealth = Amount <= 0.f ? Max : FMath::Min(Max, CurrentHealth + Amount);
}

void UKBStatSheetComponent::ApplyMods(const FKBStatMods& Mods)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	// Multipliers compose, additives accumulate. Doing it this way means card order never
	// matters, so a player cannot be penalised for picking the same two cards in the
	// "wrong" sequence.
	TotalMods.MaxHealthAdd += Mods.MaxHealthAdd;
	TotalMods.MoveSpeedMult *= Mods.MoveSpeedMult;
	TotalMods.DamageMult *= Mods.DamageMult;
	TotalMods.CooldownMult *= Mods.CooldownMult;
	TotalMods.PickupRadiusMult *= Mods.PickupRadiusMult;
	TotalMods.XpGainMult *= Mods.XpGainMult;

	PushDerivedValues();
}

float UKBStatSheetComponent::GetMoveSpeed() const
{
	return UKBGameSettings::Get().BaseMoveSpeed * TotalMods.MoveSpeedMult;
}

float UKBStatSheetComponent::GetPickupRadius() const
{
	return UKBGameSettings::Get().BasePickupRadius * TotalMods.PickupRadiusMult;
}

void UKBStatSheetComponent::PushDerivedValues()
{
	// Raising max health also grants the difference to current health. Otherwise a player who
	// picks a "+30 max health" card while damaged sees a bigger empty bar and nothing else.
	const float NewMaxHealth = GetMaxHealth();
	const float MaxHealthIncrease = NewMaxHealth - LastKnownMaxHealth;
	if (MaxHealthIncrease > 0.f)
	{
		CurrentHealth += MaxHealthIncrease;
	}
	CurrentHealth = FMath::Clamp(CurrentHealth, 0.f, NewMaxHealth);
	LastKnownMaxHealth = NewMaxHealth;

	// Move speed lives on the movement component, so it has to be pushed rather than read.
	if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			Movement->MaxWalkSpeed = GetMoveSpeed();
		}
	}
}
