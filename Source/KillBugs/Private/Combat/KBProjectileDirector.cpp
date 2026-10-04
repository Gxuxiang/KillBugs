#include "Combat/KBProjectileDirector.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Core/KBPlayerState.h"
#include "Data/KBEnemyArchetype.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "KBConsoleVariables.h"
#include "KBGameSettings.h"
#include "KillBugs.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Materials/MaterialInterface.h"
#include "Swarm/KBEnemyDirector.h"
#include "UObject/ConstructorHelpers.h"

AKBProjectileDirector::AKBProjectileDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	// Replicated so clients get an instance to draw COSMETIC bullets from. Damage still only
	// ever happens on the server - see bCosmetic on FKBProjectile.
	bReplicates = true;

	// An engine sphere rather than a project mesh: a bullet is round, and LevelPrototyping
	// has no sphere. /Engine/BasicShapes is available in every project.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> BulletMesh(
		TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (BulletMesh.Succeeded())
	{
		ProjectileMesh = BulletMesh.Object;
	}
}

void AKBProjectileDirector::BeginPlay()
{
	Super::BeginPlay();

	// One instanced mesh for every bullet. Per-weapon colours would need per-instance custom
	// data and a material that reads it, which is not worth a second material asset yet.
	ProjectileInstances = NewObject<UInstancedStaticMeshComponent>(this, TEXT("ProjectileInstances"));
	ProjectileInstances->SetupAttachment(GetRootComponent());
	ProjectileInstances->SetMobility(EComponentMobility::Movable);

	if (ProjectileMesh)
	{
		ProjectileInstances->SetStaticMesh(ProjectileMesh);
	}

	// Must be the same material contract as the swarm: a material without
	// bUsedWithInstancedStaticMeshes is silently replaced by the engine default.
	if (UMaterialInterface* BulletMaterial =
		LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/KillBugs/Enemies/M_KBEnemy.M_KBEnemy")))
	{
		ProjectileInstances->SetMaterial(0, BulletMaterial);
	}

	// Scale deliberately left at 1: each bullet is sized per instance from its own hit radius
	// in UpdateVisuals, so a component-level scale would fight that.
	const FLinearColor Tint = KBSettings().ProjectileTint;
	ProjectileInstances->SetVectorParameterValueOnMaterials(
		TEXT("Base Color"), FVector(Tint.R, Tint.G, Tint.B));
	ProjectileInstances->SetScalarParameterValueOnMaterials(TEXT("Roughness"), 0.4f);
	ProjectileInstances->SetScalarParameterValueOnMaterials(TEXT("Metallic"), 0.f);

	// Bullets are presentation and never interact.
	ProjectileInstances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ProjectileInstances->SetGenerateOverlapEvents(false);
	ProjectileInstances->SetCastShadow(false);
	ProjectileInstances->SetCanEverAffectNavigation(false);

	ProjectileInstances->RegisterComponent();
}

AKBEnemyDirector* AKBProjectileDirector::ResolveEnemyDirector()
{
	if (CachedEnemyDirector.IsValid())
	{
		return CachedEnemyDirector.Get();
	}

	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AKBEnemyDirector> It(World); It; ++It)
		{
			CachedEnemyDirector = *It;
			return *It;
		}
	}

	return nullptr;
}

void AKBProjectileDirector::FireProjectile(const FVector& Start, const FVector& Direction,
                                           float Speed, float Damage, float HitRadius,
                                           float MaxRange, AKBPlayerState* InKiller,
                                           UNiagaraSystem* InImpactEffect, USoundBase* InImpactSound,
                                           bool bCosmetic)
{
	if (Projectiles.Num() >= MaxProjectiles)
	{
		return;
	}

	// A cosmetic bullet only ever exists on a client, so gating it on authority would reject
	// exactly the case it is for. Real bullets remain server-only: a client must never create
	// one, because it must never resolve damage.
	if (!bCosmetic && !HasAuthority())
	{
		return;
	}

	FVector Heading = Direction;
	Heading.Z = 0.f;
	if (!Heading.Normalize())
	{
		return; // degenerate direction; nothing sensible to shoot at
	}

	// Logged after the effect component is created (see the end of this function for the effect
	// itself), reporting whether one was. "fx=no" against a ProjectileEffect set in the ini is
	// the one failure that is otherwise completely invisible: the bullets still fly, the damage
	// still lands, and only the look is wrong.
	static int32 DiagnosticShots = 0;
	const bool bDiagnose = DiagnosticShots < 10;
	if (bDiagnose)
	{
		++DiagnosticShots;
	}

	// Start just ahead of the shooter so the bolt visibly leaves the character instead of
	// appearing inside them, and low so it sits in the same plane the bugs are drawn on.
	const FVector Muzzle = Start + Heading * 60.f;

	FKBProjectile& Projectile = Projectiles.AddDefaulted_GetRef();
	Projectile.Location = FVector(Muzzle.X, Muzzle.Y, 45.f);
	Projectile.PreviousLocation = Projectile.Location;
	Projectile.Velocity = Heading * FMath::Max(Speed, 1.f);
	Projectile.Damage = Damage;
	Projectile.Radius = HitRadius;
	Projectile.RemainingRange = MaxRange;
	Projectile.Killer = InKiller;
	Projectile.ImpactEffect = InImpactEffect;
	Projectile.ImpactSound = InImpactSound;
	Projectile.bCosmetic = bCosmetic;

	// The bullet's own look. Built here rather than in UpdateVisuals so that every bullet owns
	// its component from the frame it is born: UpdateVisuals runs after the simulation step, and
	// a bullet created and destroyed inside one frame would otherwise never be drawn at all.
	//
	// LoadSynchronous is per shot, not cached at BeginPlay, so the property stays editable on
	// the class while the game runs. On an asset already in memory it is a map lookup.
	if (UNiagaraSystem* Effect = KBSettings().ProjectileEffect.LoadSynchronous())
	{
		UNiagaraComponent* Component = NewObject<UNiagaraComponent>(this);
		Component->SetupAttachment(GetRootComponent());
		Component->SetMobility(EComponentMobility::Movable);

		// The effect is authored at its own size, NOT scaled to the hit radius the way the
		// sphere is. The sphere had to be sized from the radius because a plain ball gives no
		// other clue how big the hitbox is; a Niagara system is art, and stretching it to a
		// number would fight whoever made it. Author it at roughly twice the weapon's HitRadius
		// (rifle 18, shotgun 22) if it should read as the size of what it hits.
		Component->SetAsset(Effect);

		// Set through the setter: bAutoDestroy is private on UNiagaraComponent, because the
		// engine wants to decide for itself whether a finished system is safe to collect.
		// Off, because this component's lifetime is the bullet's, and Tick destroys it.
		Component->SetAutoDestroy(false);

		Component->RegisterComponent();
		Component->SetWorldLocation(Projectile.Location);
		Component->Activate();

		Projectile.EffectComponent = Component;
	}

	if (bDiagnose)
	{
		const FVector PawnLocation = InKiller && InKiller->GetPawn()
			? InKiller->GetPawn()->GetActorLocation() : FVector::ZeroVector;
		// The killer's index is the only thing that says WHICH player fired this. Position does
		// not distinguish them: with one PlayerStart both pawns spawn on top of each other.
		const AKBPlayerState* KillerState = InKiller;
		UE_LOG(LogKillBugs, Display,
			TEXT("Bullet #%d spawn=(%.0f,%.0f) shooterAt=(%.0f,%.0f) dir=(%.2f,%.2f) killer=%d fx=%s"),
			DiagnosticShots, Start.X, Start.Y, PawnLocation.X, PawnLocation.Y,
			Heading.X, Heading.Y,
			KillerState ? KillerState->GetKBPlayerIndex() : -1,
			Projectile.EffectComponent ? TEXT("yes") : TEXT("no"));
	}
}

void AKBProjectileDirector::FireShot(const FVector& Start, const TArray<FVector>& Directions,
                                     float Speed, float Damage, float HitRadius, float MaxRange,
                                     AKBPlayerState* InKiller, UNiagaraSystem* InImpactEffect,
                                     USoundBase* InImpactSound)
{
	if (!HasAuthority() || Directions.Num() == 0)
	{
		return;
	}

	// Normalised HERE, once, and the same vector is then used for both the real bullets and the
	// multicast - so what a client draws is exactly what the server fired.
	//
	// Not left to the caller: the auto-weapon path hands over the raw muzzle-to-target delta,
	// which is hundreds of units long. FVector_NetQuantizeNormal only carries components in
	// -1..1, so anything larger is CLAMPED to 1 by WriteFixedCompressedFloat and its
	// NetSerialize reports failure. Every pellet of a shotgun then arrives pointing the same
	// diagonal, and the log fills with "Native NetSerialize ... failed" warnings. Normalising
	// at this boundary keeps the two in step even if a caller forgets.
	//
	// Z is flattened to match what FireProjectile does to its own copy.
	TArray<FVector_NetQuantizeNormal> PackedDirections;
	PackedDirections.Reserve(Directions.Num());

	for (const FVector& Direction : Directions)
	{
		FVector Heading = Direction;
		Heading.Z = 0.f;
		if (!Heading.Normalize())
		{
			continue; // degenerate; FireProjectile would refuse it too
		}

		FireProjectile(Start, Heading, Speed, Damage, HitRadius, MaxRange, InKiller,
		               InImpactEffect, InImpactSound);
		PackedDirections.Add(Heading);
	}

	// Nothing survived, so there is nothing to tell clients to draw either.
	if (PackedDirections.Num() == 0)
	{
		return;
	}

	MulticastPlayShot(Start, PackedDirections, Speed, MaxRange, HitRadius);
}

void AKBProjectileDirector::MulticastPlayShot_Implementation(FVector_NetQuantize Start,
                                                             const TArray<FVector_NetQuantizeNormal>& Directions,
                                                             float Speed, float MaxRange,
                                                             float HitRadius)
{
	// The server already has the real bullets; drawing these too would double every shot.
	if (HasAuthority())
	{
		return;
	}

	for (const FVector_NetQuantizeNormal& Direction : Directions)
	{
		FireProjectile(Start, Direction, Speed, /*Damage*/ 0.f, HitRadius, MaxRange,
		               /*InKiller*/ nullptr, nullptr, nullptr, /*bCosmetic*/ true);
	}
}

void AKBProjectileDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Clients run this too, to fly the cosmetic bullets the multicast gave them. Only the
	// damage step inside the loop is authority-only.
	const bool bAuthority = HasAuthority();

	const double StartSeconds = FPlatformTime::Seconds();

	AKBEnemyDirector* Enemies = ResolveEnemyDirector();
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;

	for (int32 Index = Projectiles.Num() - 1; Index >= 0; --Index)
	{
		FKBProjectile& Projectile = Projectiles[Index];

		const FVector Step = Projectile.Velocity * DeltaSeconds;
		const float StepLength = Step.Size();

		Projectile.PreviousLocation = Projectile.Location;
		Projectile.Location += Step;
		Projectile.RemainingRange -= StepLength;

		// Moved here rather than in UpdateVisuals: this is the position the hit test just used,
		// so the effect is drawn exactly where the bullet is - and where it is about to be
		// tested from next frame. A frame of lag between the two is the one thing a bullet
		// travelling 2600 units a second cannot hide.
		if (Projectile.EffectComponent)
		{
			// Moving the component is only half of it: a Niagara system whose particles are
			// simulated in WORLD space emits them at whatever point the component was at when
			// they were born, and leaves them there. The bullet then reads as a puff at the
			// muzzle - the component verifiably travels, the effect does not - and the fix is not
			// here but on the emitter's Local Space flag.
			Projectile.EffectComponent->SetWorldLocation(Projectile.Location);
		}

		bool bConsumed = false;

		// Damage is the server's alone, and never for a cosmetic bullet: a client resolving
		// its own hits would drift out of agreement with the server that owns the swarm.
		if (bAuthority && !Projectile.bCosmetic && Enemies && StepLength > KINDA_SMALL_NUMBER)
		{
			// Damage resolves HERE, on arrival - not when the shot was fired. Reusing the
			// swarm's own swept-sphere test means bullets share its hit definition exactly.
			FVector HitLocation = Projectile.Location;
			bConsumed = Enemies->ApplyDamageAlongSegment(
				Projectile.PreviousLocation, Projectile.Location,
				Projectile.Radius, Projectile.Damage, Projectile.Killer.Get(), &HitLocation);

			if (bConsumed)
			{
				SpawnImpactFeedback(Projectile, HitLocation);
			}
		}

		if (bConsumed || Projectile.RemainingRange <= 0.f)
		{
			// Destroyed explicitly rather than left to the GC: a bullet dies hundreds of times a
			// run, and a component per bullet waiting on a collection pass would accumulate for
			// as long as the wave lasts. RemoveAtSwap then moves the last bullet into this slot,
			// which is safe - its own component travels with it in the struct.
			if (Projectile.EffectComponent)
			{
				Projectile.EffectComponent->DestroyComponent();
			}

			Projectiles.RemoveAtSwap(Index);
		}
	}

	UpdateVisuals();

	const float PerfLogInterval = CVarKBSwarmPerfLog.GetValueOnGameThread();
	if (PerfLogInterval > 0.f)
	{
		PerfSeconds += FPlatformTime::Seconds() - StartSeconds;
		PerfPeakInFlight = FMath::Max(PerfPeakInFlight, Projectiles.Num());
		++PerfFrames;
		PerfElapsed += DeltaSeconds;

		if (PerfElapsed >= PerfLogInterval)
		{
			const double Frames = FMath::Max(PerfFrames, 1);
			UE_LOG(LogKillBugs, Display,
				TEXT("Projectiles | %d in flight (peak %d) | %.3f ms | %.0f fps"),
				Projectiles.Num(), PerfPeakInFlight,
				PerfSeconds / Frames * 1000.0, Frames / PerfElapsed);

			PerfSeconds = 0.0;
			PerfFrames = 0;
			PerfElapsed = 0.f;
			PerfPeakInFlight = 0;
		}
	}
}

void AKBProjectileDirector::SpawnImpactFeedback(const FKBProjectile& Projectile, const FVector& HitLocation)
{
	// Impact effects fire only on a real hit - a miss plays nothing, which is how the player
	// tells a hit from a miss without reading damage numbers.
	if (UNiagaraSystem* Effect = Projectile.ImpactEffect.LoadSynchronous())
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			this, Effect, HitLocation, FRotator::ZeroRotator, FVector::OneVector,
			/*bAutoDestroy*/ true, /*bAutoActivate*/ true);
	}

	// Sound is rate-limited; see ImpactSoundMinInterval.
	if (USoundBase* Sound = Projectile.ImpactSound.LoadSynchronous())
	{
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		if (Now - LastImpactSoundTime >= ImpactSoundMinInterval)
		{
			LastImpactSoundTime = Now;
			UGameplayStatics::PlaySoundAtLocation(this, Sound, HitLocation);
		}
	}
}

void AKBProjectileDirector::UpdateVisuals()
{
	if (!ProjectileInstances)
	{
		return;
	}

	// Rebuilt wholesale every frame rather than pooled with stable slots like the swarm.
	//
	// That is safe here in a way it was not for the bugs: AddInstances writes transforms
	// directly instead of interpolating, so a slot changing which bullet it represents cannot
	// smear - and with a handful of bullets in flight the rebuild is free.
	ProjectileInstances->ClearInstances();

	// Reset(), NOT Reset(N).
	//
	// TArray::Reset(NewSize) sets the length without constructing the elements - the entries
	// are uninitialised memory. Emplacing the real transforms on top of that left the buffer
	// holding N garbage transforms followed by N real ones, and all 2N were handed to
	// AddInstances. Uninitialised floats can be NaN, and a NaN in an instanced mesh's bounds
	// poisons the primitive's culling - which is how six real bullets end up drawing as one or
	// two on screen while the data behind them is perfectly correct.
	TransformScratch.Reset();

	for (const FKBProjectile& Projectile : Projectiles)
	{
		// This bullet is drawn by its own effect. Adding an instance as well would put a sphere
		// inside the effect, which is precisely what swapping to Niagara was meant to stop.
		if (Projectile.EffectComponent)
		{
			continue;
		}

		// Drawn AT THE HITBOX SIZE, not at an arbitrary small size.
		//
		// A bullet much smaller than the radius it actually tests against reads as hitting
		// thin air: enemies die while the visible bolt is still well short of them. That is
		// exactly what a 14-unit visual with a 140-unit hit radius looks like, and it is the
		// most confusing thing a projectile can do.
		const float Diameter = Projectile.Radius * 2.f;

		// The source mesh is 100 units across. Flattened on Z so it reads as a bolt rather
		// than a crate, while staying honest about its size in the plane that matters.
		const float UnitScale = Diameter / 100.f;
		TransformScratch.Emplace(FRotator::ZeroRotator, Projectile.Location,
		                         FVector(UnitScale, UnitScale, UnitScale * 0.5f));
	}

	if (TransformScratch.Num() > 0)
	{
		ProjectileInstances->AddInstances(TransformScratch, /*bWorldSpace*/ true, /*bUpdateNavigation*/ false);
	}

}
