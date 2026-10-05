#include "Combat/KBStatSheetComponent.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "KBConsoleVariables.h"
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

	// KB.Player.God - testing switch, off by default. Returning zero rather than absorbing the
	// hit is deliberate: everything downstream reads the return value as "how much actually
	// landed", so a shielded hit must look like a miss rather than like damage that was survived.
	if (CVarKBPlayerGod.GetValueOnGameThread() != 0)
	{
		return 0.f;
	}

	const float Applied = FMath::Min(Damage, CurrentHealth);
	CurrentHealth -= Applied;

	// Only for damage that actually landed. A hit on an already-empty health bar applies zero,
	// and shaking for it would make the shake outlast the player's life.
	if (Applied > 0.f)
	{
		PlayDamageCameraShake();
	}

	if (CurrentHealth <= 0.f)
	{
		CurrentHealth = 0.f;
		OnHealthDepleted.Broadcast();
	}

	return Applied;
}

void UKBStatSheetComponent::PlayDamageCameraShake() const
{
	const UKBGameSettings& Settings = UKBGameSettings::Get();

	// Unset by default: this is the hook for a shake asset, not a shake. See the tooltip on
	// UKBGameSettings::PlayerDamageCameraShake.
	if (!Settings.PlayerDamageCameraShake || Settings.PlayerDamageShakeScale <= 0.f)
	{
		return;
	}

	// The controller is what owns a camera, so no controller means nothing to shake: an enemy
	// pawn, or the brief window before a player's pawn is possessed.
	const APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* PlayerController =
		Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;

	if (!PlayerController)
	{
		return;
	}

	// This runs on the SERVER - ApplyDamage refuses to do anything else - and
	// ClientStartCameraShake is a Client RPC, so calling it here is precisely what routes the
	// shake to the machine that owns the camera. Nothing has to be replicated by hand, and on a
	// listen server the host's own controller simply executes it locally.
	//
	// Unreliable by design: a dropped hit shake is a cosmetic miss, not a desync.
	PlayerController->ClientStartCameraShake(Settings.PlayerDamageCameraShake,
	                                         Settings.PlayerDamageShakeScale);
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
