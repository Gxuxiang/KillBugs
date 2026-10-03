#include "Combat/KBWeaponInventoryComponent.h"

#include "Combat/KBProjectileDirector.h"
#include "Combat/KBStatSheetComponent.h"
#include "Core/KBCharacter.h"
#include "Core/KBPlayerState.h"
#include "Data/KBWeaponDefinition.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "KillBugs.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "NiagaraFunctionLibrary.h"
#include "Swarm/KBEnemyDirector.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/**
	 * Starting loadout, loaded by path so Phase 3 runs with no editor wiring. Replace with a
	 * designer-editable list (or a run definition) once weapons are granted by cards.
	 */
	/**
	 * Deliberately just these two: one auto and one manual, which is the hybrid the game is
	 * built around. Everything else is meant to be earned through card drafts, so the very
	 * first draft has something meaningful to offer.
	 */
	const TCHAR* const StartingWeaponPaths[] =
	{
		TEXT("/Game/KillBugs/Weapons/DA_Weapon_AutoRifle.DA_Weapon_AutoRifle"),
		TEXT("/Game/KillBugs/Weapons/DA_Weapon_Shotgun.DA_Weapon_Shotgun")
	};
}

UKBWeaponInventoryComponent::UKBWeaponInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);

	for (const TCHAR* Path : StartingWeaponPaths)
	{
		// Static so the load happens once for the CDO rather than per instance.
		static TMap<FString, TObjectPtr<UKBWeaponDefinition>> Loaded;
		TObjectPtr<UKBWeaponDefinition>& Cached = Loaded.FindOrAdd(FString(Path));

		if (!Cached)
		{
			Cached = Cast<UKBWeaponDefinition>(
				StaticLoadObject(UKBWeaponDefinition::StaticClass(), nullptr, Path));
		}

		if (Cached)
		{
			StartingWeapons.Add(Cached);
		}
		else
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Starting weapon asset missing: %s"), Path);
		}
	}
}

void UKBWeaponInventoryComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UKBWeaponInventoryComponent, Weapons);
}

void UKBWeaponInventoryComponent::BeginPlay()
{
	Super::BeginPlay();

	// Server only: a client must never author the loadout.
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	for (UKBWeaponDefinition* Weapon : StartingWeapons)
	{
		GrantWeapon(Weapon);
	}

	UE_LOG(LogKillBugs, Display, TEXT("Granted %d starting weapon(s)"), Weapons.Num());
	for (const FKBOwnedWeapon& Weapon : Weapons)
	{
		UE_LOG(LogKillBugs, Display, TEXT("    %s  %s / %s  dmg %.1f @ %.2fs"),
			Weapon.Definition ? *Weapon.Definition->DisplayName.ToString() : TEXT("NONE"),
			Weapon.Definition && Weapon.Definition->Behavior == EKBWeaponBehavior::Auto ? TEXT("auto") : TEXT("manual"),
			Weapon.Definition && Weapon.Definition->Delivery == EKBWeaponDelivery::Radial ? TEXT("radial") : TEXT("hitscan"),
			Weapon.Definition ? Weapon.Definition->GetDamage(Weapon.Level) : 0.f,
			Weapon.Definition ? Weapon.Definition->GetCooldown(Weapon.Level) : 0.f);
	}
}

void UKBWeaponInventoryComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                                FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Drawn on every machine, including a client: this is local cosmetic feedback, and it
	// must not be gated behind authority or a client would see nothing at all.
	DrawShotFeedback(DeltaTime);

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	TickWeaponCooldowns(DeltaTime);
	TickAutoWeapons();
}

void UKBWeaponInventoryComponent::TickWeaponCooldowns(float DeltaTime)
{
	// EVERY weapon ticks down, manual ones included. Only decrementing auto weapons leaves a
	// manual weapon stuck at whatever cooldown its last shot set, so it fires once and then
	// never again.
	for (FKBOwnedWeapon& Weapon : Weapons)
	{
		Weapon.CooldownRemaining = FMath::Max(0.f, Weapon.CooldownRemaining - DeltaTime);
	}
}

bool UKBWeaponInventoryComponent::GrantWeapon(UKBWeaponDefinition* Definition)
{
	if (!Definition || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return false;
	}

	int32 ExistingLevel = 0;
	if (HasWeapon(Definition, ExistingLevel))
	{
		if (ExistingLevel >= Definition->MaxLevel)
		{
			return false;
		}

		for (FKBOwnedWeapon& Weapon : Weapons)
		{
			if (Weapon.Definition == Definition)
			{
				++Weapon.Level;
				return true;
			}
		}
	}

	if (Weapons.Num() >= MaxWeaponSlots)
	{
		return false;
	}

	FKBOwnedWeapon& NewWeapon = Weapons.AddDefaulted_GetRef();
	NewWeapon.Definition = Definition;
	NewWeapon.Level = 1;
	NewWeapon.CooldownRemaining = 0.f;
	return true;
}

bool UKBWeaponInventoryComponent::HasWeapon(const UKBWeaponDefinition* Definition, int32& OutLevel) const
{
	for (const FKBOwnedWeapon& Weapon : Weapons)
	{
		if (Weapon.Definition == Definition)
		{
			OutLevel = Weapon.Level;
			return true;
		}
	}

	OutLevel = 0;
	return false;
}

int32 UKBWeaponInventoryComponent::FindFirstManualWeaponSlot() const
{
	for (int32 Index = 0; Index < Weapons.Num(); ++Index)
	{
		if (Weapons[Index].Definition && Weapons[Index].Definition->Behavior == EKBWeaponBehavior::Manual)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

AKBEnemyDirector* UKBWeaponInventoryComponent::ResolveDirector() const
{
	if (CachedDirector.IsValid())
	{
		return CachedDirector.Get();
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	for (TActorIterator<AKBEnemyDirector> It(World); It; ++It)
	{
		CachedDirector = *It;
		return *It;
	}

	return nullptr;
}

const UKBStatSheetComponent* UKBWeaponInventoryComponent::ResolveStatSheet() const
{
	if (const AActor* Owner = GetOwner())
	{
		return Owner->FindComponentByClass<UKBStatSheetComponent>();
	}
	return nullptr;
}

AKBPlayerState* UKBWeaponInventoryComponent::ResolveKillerState() const
{
	if (const APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		return Pawn->GetPlayerState<AKBPlayerState>();
	}
	return nullptr;
}

void UKBWeaponInventoryComponent::TickAutoWeapons()
{
	AKBEnemyDirector* Director = ResolveDirector();
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Director || !Pawn)
	{
		return;
	}

	const FVector Origin = Pawn->GetActorLocation();

	for (FKBOwnedWeapon& Weapon : Weapons)
	{
		const UKBWeaponDefinition* Definition = Weapon.Definition;
		if (!Definition || Definition->Behavior != EKBWeaponBehavior::Auto)
		{
			continue;
		}

		if (Weapon.CooldownRemaining > 0.f)
		{
			continue;
		}

		FVector TargetLocation = FVector::ZeroVector;
		const int32 TargetIndex = Director->FindNearestEnemy(Origin, Definition->Range, TargetLocation);
		if (TargetIndex == INDEX_NONE)
		{
			continue; // hold fire; cooldown stays at zero
		}

		const UKBStatSheetComponent* Stats = ResolveStatSheet();
		Weapon.CooldownRemaining = Definition->GetCooldown(Weapon.Level) * (Stats ? Stats->GetCooldownMultiplier() : 1.f);
		Weapon.LastFireServerTime = GetServerTimeNow();
		FireAtAcquiredTarget(*Definition, Weapon.Level, TargetIndex, TargetLocation);
	}
}

bool UKBWeaponInventoryComponent::FireAtAcquiredTarget(const UKBWeaponDefinition& Definition,
                                                       int32 WeaponLevel, int32 TargetIndex,
                                                       const FVector& TargetLocation)
{
	AKBEnemyDirector* Director = ResolveDirector();
	if (!Director)
	{
		return false;
	}

	AKBPlayerState* Killer = ResolveKillerState();

	// Read the multiplier at fire time rather than caching it, so a card picked mid-run
	// changes the very next shot.
	const UKBStatSheetComponent* Stats = ResolveStatSheet();
	const float Damage = Definition.GetDamage(WeaponLevel) * (Stats ? Stats->GetDamageMultiplier() : 1.f);

	const APawn* Pawn = Cast<APawn>(GetOwner());
	const FVector MuzzleLocation = Pawn ? Pawn->GetActorLocation() : TargetLocation;

	switch (Definition.Delivery)
	{
	case EKBWeaponDelivery::Projectile:
	{
		if (!ResolveProjectileDirector())
		{
			return false;
		}

		// No tracer recorded: the bullets ARE the visual. Drawing both would double up.
		//
		// Normalised before it goes any further. This is a muzzle-to-target DELTA at this
		// point, hundreds of units long, and the spread rotates it without rescaling. The
		// manual path normalises here for the same reason.
		FVector Direction = TargetLocation - MuzzleLocation;
		Direction.Z = 0.f;
		if (!Direction.Normalize())
		{
			return false; // target directly on the muzzle; nothing sensible to shoot at
		}

		FireProjectileSpread(Definition, MuzzleLocation, Direction, Damage, Killer);
		return true;
	}

	case EKBWeaponDelivery::Radial:
		RecordBurst(TargetLocation, Definition.RadialRadius, Definition.TracerColor);
		Director->ApplyRadialDamage(TargetLocation, Definition.RadialRadius, Damage, Killer);
		return true;

	case EKBWeaponDelivery::Hitscan:
	default:
		RecordTracer(MuzzleLocation, TargetLocation, Definition.TracerColor);
		return Director->ApplyDamageToEnemy(TargetIndex, Damage, Killer);
	}
}

void UKBWeaponInventoryComponent::FireProjectileSpread(const UKBWeaponDefinition& Definition,
                                                       const FVector& Start, const FVector& Direction,
                                                       float Damage, AKBPlayerState* Killer)
{
	AKBProjectileDirector* Projectiles = ResolveProjectileDirector();
	if (!Projectiles)
	{
		return;
	}

	PlayFireFeedback(Definition, Start, Direction);

	// Resolved once per shot rather than per pellet: a shotgun fires six of them.
	UNiagaraSystem* ImpactEffect = Definition.ImpactEffect.LoadSynchronous();
	USoundBase* ImpactSound = Definition.ImpactSound.LoadSynchronous();

	const int32 Count = FMath::Max(1, Definition.ProjectilesPerShot);
	const float HalfSpreadDegrees = FMath::Max(0.f, Definition.SpreadDegrees) * 0.5f;

	// The cone is rolled ONCE, here, and the resulting directions are used for the server's
	// real bullets AND sent to clients unchanged. Letting each machine roll its own would give
	// the client a cone that does not match where the server's pellets actually land, so shots
	// would visibly hit things they visibly missed.
	TArray<FVector> Directions;
	Directions.Reserve(Count);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		float AngleDegrees = 0.f;

		if (HalfSpreadDegrees > 0.f)
		{
			if (Count > 1)
			{
				// Evenly spaced across the cone, plus a little jitter so consecutive shots do
				// not retrace exactly the same pattern - a perfectly regular fan reads as a
				// single wide shape rather than as pellets.
				const float Alpha = static_cast<float>(Index) / static_cast<float>(Count - 1);
				AngleDegrees = FMath::Lerp(-HalfSpreadDegrees, HalfSpreadDegrees, Alpha);
				AngleDegrees += FMath::FRandRange(-HalfSpreadDegrees, HalfSpreadDegrees) * 0.18f;
			}
			else
			{
				AngleDegrees = FMath::FRandRange(-HalfSpreadDegrees, HalfSpreadDegrees);
			}
		}

		Directions.Add(Direction.RotateAngleAxis(AngleDegrees, FVector::UpVector));
	}

	// Flight distance is the weapon's stated range, with no hidden multiplier. The distance a
	// bullet visibly travels and the distance it can still damage at have to be the same
	// number, or a shot that looks like it reaches simply stops existing.
	Projectiles->FireShot(Start, Directions, Definition.ProjectileSpeed, Damage,
	                      Definition.HitRadius, Definition.Range, Killer,
	                      ImpactEffect, ImpactSound);
}

void UKBWeaponInventoryComponent::PlayFireFeedback(const UKBWeaponDefinition& Definition,
                                                   const FVector& Start, const FVector& Direction)
{
	if (!GetOwner())
	{
		return;
	}

	// Placed back along the barrel rather than at the pawn's origin, so the flash is not
	// buried inside the character mesh and the sound is not heard from the feet.
	const FVector Heading = Direction.GetSafeNormal();
	const FVector Muzzle = Start + Heading * 70.f + FVector(0.f, 0.f, 60.f);

	if (UNiagaraSystem* Effect = Definition.MuzzleEffect.LoadSynchronous())
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			GetOwner(), Effect, Muzzle, Heading.Rotation(), FVector::OneVector,
			/*bAutoDestroy*/ true, /*bAutoActivate*/ true);
	}

	if (USoundBase* Sound = Definition.FireSound.LoadSynchronous())
	{
		UGameplayStatics::PlaySoundAtLocation(GetOwner(), Sound, Muzzle,
			/*VolumeMultiplier*/ 1.f, /*PitchMultiplier*/ Definition.FireSoundPitch);
	}
}

float UKBWeaponInventoryComponent::GetServerTimeNow() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.f;
	}

	// The SERVER clock on purpose: LastFireServerTime is stamped from it, and a client's own
	// world time runs on a different origin, which would make every bar permanently wrong.
	const AGameStateBase* RunState = World->GetGameState();
	return RunState ? RunState->GetServerWorldTimeSeconds() : World->GetTimeSeconds();
}

float UKBWeaponInventoryComponent::GetCooldownFraction(int32 WeaponIndex) const
{
	if (!Weapons.IsValidIndex(WeaponIndex))
	{
		return 0.f;
	}

	const FKBOwnedWeapon& Weapon = Weapons[WeaponIndex];
	if (!Weapon.Definition)
	{
		return 0.f;
	}

	const UKBStatSheetComponent* Stats = ResolveStatSheet();
	const float FullCooldown =
		Weapon.Definition->GetCooldown(Weapon.Level) * (Stats ? Stats->GetCooldownMultiplier() : 1.f);

	if (FullCooldown <= 0.f)
	{
		return 0.f;
	}

	// Derived from the replicated fire time rather than from CooldownRemaining, so a client
	// gets the same smooth bar the server would draw with no per-frame replication.
	const float Elapsed = GetServerTimeNow() - Weapon.LastFireServerTime;
	return FMath::Clamp(1.f - (Elapsed / FullCooldown), 0.f, 1.f);
}

AKBProjectileDirector* UKBWeaponInventoryComponent::ResolveProjectileDirector() const
{
	if (CachedProjectileDirector.IsValid())
	{
		return CachedProjectileDirector.Get();
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	for (TActorIterator<AKBProjectileDirector> It(World); It; ++It)
	{
		CachedProjectileDirector = *It;
		return *It;
	}

	return nullptr;
}

void UKBWeaponInventoryComponent::RecordTracer(const FVector& Start, const FVector& End, const FLinearColor& Color)
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FKBShotTracer& Tracer = ShotTracers.AddDefaulted_GetRef();
	Tracer.Start = Start;
	Tracer.End = End;
	Tracer.Color = Color;
	Tracer.ExpireTime = World->GetTimeSeconds() + 0.12f;
}

void UKBWeaponInventoryComponent::RecordBurst(const FVector& Centre, float Radius, const FLinearColor& Color)
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FKBShotTracer& Tracer = ShotTracers.AddDefaulted_GetRef();
	Tracer.Start = Centre;
	Tracer.End = Centre;
	Tracer.Radius = Radius;
	Tracer.Color = Color;
	Tracer.ExpireTime = World->GetTimeSeconds() + 0.2f;
}

void UKBWeaponInventoryComponent::DrawShotFeedback(float DeltaTime)
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float Now = World->GetTimeSeconds();

	for (int32 Index = ShotTracers.Num() - 1; Index >= 0; --Index)
	{
		const FKBShotTracer& Tracer = ShotTracers[Index];
		if (Now >= Tracer.ExpireTime)
		{
			ShotTracers.RemoveAtSwap(Index);
			continue;
		}

#if ENABLE_DRAW_DEBUG
		const FColor Color = Tracer.Color.ToFColor(true);

		if (Tracer.Radius > 0.f)
		{
			DrawDebugCircle(World, Tracer.Start, Tracer.Radius, 40, Color,
				false, 0.15f, 0, 5.f, FVector(1.f, 0.f, 0.f), FVector(0.f, 1.f, 0.f), false);
		}
		else
		{
			DrawDebugLine(World, Tracer.Start, Tracer.End, Color, false, 0.15f, 0, 3.f);
			DrawDebugSphere(World, Tracer.End, 45.f, 8, Color, false, 0.15f, 0, 2.f);
		}
#endif
	}
}

bool UKBWeaponInventoryComponent::TryFireManualWeapon(int32 SlotIndex, const FVector& AimPoint)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !Weapons.IsValidIndex(SlotIndex))
	{
		return false;
	}

	FKBOwnedWeapon& Weapon = Weapons[SlotIndex];
	const UKBWeaponDefinition* Definition = Weapon.Definition;
	if (!Definition || Definition->Behavior != EKBWeaponBehavior::Manual)
	{
		return false;
	}

	if (Weapon.CooldownRemaining > 0.f)
	{
		return false;
	}

	const APawn* Pawn = Cast<APawn>(GetOwner());
	AKBEnemyDirector* Director = ResolveDirector();
	if (!Pawn || !Director)
	{
		return false;
	}

	const FVector Start = Pawn->GetActorLocation();
	FVector Direction = AimPoint - Start;
	Direction.Z = 0.f;
	if (!Direction.Normalize())
	{
		return false; // aim point directly on the pawn; nothing sensible to shoot at
	}

	// The client sent a point; the server decides what is actually along that line.
	const FVector End = Start + Direction * Definition->Range;

	const UKBStatSheetComponent* Stats = ResolveStatSheet();
	Weapon.CooldownRemaining = Definition->GetCooldown(Weapon.Level) * (Stats ? Stats->GetCooldownMultiplier() : 1.f);
	Weapon.LastFireServerTime = GetServerTimeNow();

	const float Damage = Definition->GetDamage(Weapon.Level) * (Stats ? Stats->GetDamageMultiplier() : 1.f);

	if (Definition->Delivery == EKBWeaponDelivery::Projectile)
	{
		// Travels at the weapon's projectile speed, so a moving target can be missed.
		FireProjectileSpread(*Definition, Start, Direction, Damage, ResolveKillerState());
		return true;
	}

	// The tracer is recorded even on a miss, so the player gets the same feedback either way
	// and can see where their shot actually went.
	RecordTracer(Start, End, Definition->TracerColor);
	Director->ApplyDamageAlongSegment(Start, End, Definition->HitRadius, Damage, ResolveKillerState());
	return true;
}

// ---------------------------------------------------------------------------------------
// Debug: KB.Weapon.FireManual [distance]
//
// A headless run has no mouse, so the manual weapon path - the entire point of the hybrid
// design - cannot be exercised by just letting the game play itself. This fires it for real
// through the same entry point the trigger uses, with a synthetic aim point.
// ---------------------------------------------------------------------------------------

static void KBConsoleFireManual(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}

	const float Distance = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 800.f;

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APawn* Pawn = It->Get() ? It->Get()->GetPawn() : nullptr;
		if (!Pawn)
		{
			continue;
		}

		UKBWeaponInventoryComponent* Inventory = Pawn->FindComponentByClass<UKBWeaponInventoryComponent>();
		if (!Inventory)
		{
			continue;
		}

		const int32 Slot = Inventory->FindFirstManualWeaponSlot();
		if (Slot == INDEX_NONE)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("FireManual: no manual weapon owned"));
			return;
		}

		// Routed through the pawn rather than calling the inventory directly, so this
		// exercises the same path the trigger does - including the client-to-server RPC.
		// Calling the inventory straight would only ever work on the authority.
		if (AKBCharacter* Character = Cast<AKBCharacter>(Pawn))
		{
			Character->DebugFireManualWeapon(Distance);
			return;
		}

		UE_LOG(LogKillBugs, Warning, TEXT("FireManual: pawn is not an AKBCharacter"));
		return;
	}

	UE_LOG(LogKillBugs, Warning, TEXT("FireManual: no player pawn"));
}

static FAutoConsoleCommandWithWorldAndArgs KBConsoleFireManualCommand(
	TEXT("KB.Weapon.FireManual"),
	TEXT("KB.Weapon.FireManual [distance] - fire the local player's manual weapon."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsoleFireManual));

// ---------------------------------------------------------------------------------------
// Debug: KB.Debug.PlacePlayer <x> <y>
//
// A headless pawn never moves, so anything position-dependent - most importantly whether the
// muzzle follows the shooter - is untestable without a way to move it.
// ---------------------------------------------------------------------------------------

static void KBConsolePlacePlayer(const TArray<FString>& Args, UWorld* World)
{
	if (!World || Args.Num() < 2)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("PlacePlayer: expected <x> <y>"));
		return;
	}

	const FVector Target(FCString::Atof(*Args[0]), FCString::Atof(*Args[1]), 200.f);

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APawn* Pawn = It->Get() ? It->Get()->GetPawn() : nullptr;
		if (!Pawn)
		{
			continue;
		}

		Pawn->SetActorLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogKillBugs, Display, TEXT("PlacePlayer: pawn now at (%.0f,%.0f)"),
			Pawn->GetActorLocation().X, Pawn->GetActorLocation().Y);
		return;
	}
}

static FAutoConsoleCommandWithWorldAndArgs KBConsolePlacePlayerCommand(
	TEXT("KB.Debug.PlacePlayer"),
	TEXT("KB.Debug.PlacePlayer <x> <y> - teleport the local player's pawn."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsolePlacePlayer));
