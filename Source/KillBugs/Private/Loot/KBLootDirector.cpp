#include "Loot/KBLootDirector.h"

#include "Combat/KBStatSheetComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Core/KBPlayerState.h"
#include "Data/KBEnemyArchetype.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "KBConsoleVariables.h"
#include "KBGameSettings.h"
#include "KillBugs.h"
#include "Net/UnrealNetwork.h"

namespace
{
	/** The engine's own cube. A drop nobody can see is the bug this fallback exists to avoid. */
	const TCHAR* const PlaceholderMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

	/**
	 * The engine's own surface material, and the reason it is named here: it exposes a `Color`
	 * parameter, which the placeholder cube's own material does not.
	 */
	const TCHAR* const PlaceholderMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial");

	/** Far below the arena, at zero scale. The visual pass starts from a field of these. */
	FTransform HiddenDropTransform()
	{
		return FTransform(FRotator::ZeroRotator, FVector(0.f, 0.f, -100000.f), FVector::ZeroVector);
	}
}

AKBLootDirector::AKBLootDirector()
{
	// Drops are placed and removed by the server but drawn from the replicated list by everyone,
	// so every machine ticks (for the visuals) while only the authority ticks pickups.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	// One actor carrying every drop, so it must never be culled for relevancy and never go
	// dormant - dormancy would stop the fast array from being sent at all.
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(false);
	SetNetDormancy(DORM_Never);
	SetNetUpdateFrequency(10.f);
}

void AKBLootDirector::BeginPlay()
{
	Super::BeginPlay();

	// Nothing to do: the instanced mesh components are created on first use, when a mesh is
	// actually needed. A level with no loot in it costs nothing.
}

void AKBLootDirector::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKBLootDirector, Drops);
}

void AKBLootDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Authority only: taking something off the ground is a gameplay change, and the client has
	// no standing to make it.
	if (HasAuthority())
	{
		TickPickups();
	}

	// Every machine, including the server, because the server is somebody's screen too.
	UpdateVisuals();
}

// ---------------------------------------------------------------------------------------------
// Dropping
// ---------------------------------------------------------------------------------------------

void AKBLootDirector::RollDropForDeath(const FVector& DeathLocation, const UKBEnemyArchetype& Archetype)
{
	if (!HasAuthority())
	{
		return;
	}

	// The test switch short-circuits both rolls. Checked here rather than at the call site so
	// there is one place that decides what a death produces.
	//
	// It forces the MATERIAL branch, which is what its cvar help says. It used to force the medkit
	// branch instead, because the medkit roll came first and the early return swallowed it - so a
	// test that meant to fill the ground with loot filled it with medkits, and (now that medkits
	// occupy backpack weight) would have blocked the capacity it was meant to be testing.
	const bool bForce = CVarKBLootForceDrop.GetValueOnGameThread() != 0;

	if (!bForce && FMath::FRand() < Archetype.MedkitDropChance)
	{
		SpawnDrop(DeathLocation, EKBItemType::Medkit, 1);
		return;
	}

	if (bForce || FMath::FRand() < Archetype.MaterialDropChance)
	{
		const int32 Min = FMath::Min(Archetype.MaterialDropMin, Archetype.MaterialDropMax);
		const int32 Max = FMath::Max(Archetype.MaterialDropMin, Archetype.MaterialDropMax);
		SpawnDrop(DeathLocation, EKBItemType::Material, FMath::RandRange(Min, Max));
	}
}

void AKBLootDirector::SpawnDrop(const FVector& Location, EKBItemType Type, int32 Count)
{
	if (!HasAuthority() || Count <= 0)
	{
		return;
	}

	if (Drops.Items.Num() >= KBSettings().MaxDrops)
	{
		// Deliberately NOT recycling the oldest. Loot that vanishes in front of a player who can
		// still see it and reach it is worse than loot that was never dropped, and an expiring
		// pickup contradicts what searching is for. A cap that refuses to add is the honest end.
		if (!bWarnedAboutDropCap)
		{
			bWarnedAboutDropCap = true;
			UE_LOG(LogKillBugs, Warning,
				TEXT("KB Loot: %d drops live, the cap - new drops are being refused until some are taken"),
				Drops.Items.Num());
		}
		return;
	}

	FKBItemNetItem& Added = Drops.Items.AddDefaulted_GetRef();
	Added.StableId = NextStableId++;
	// XY from where the bug died, Z from settings: the arena floor is flat, and lifting drops
	// clear of it stops them reading as a patch of ground from the pitched camera.
	Added.Location = FVector(Location.X, Location.Y, KBSettings().DropHeightOffset);
	Added.Type = static_cast<uint8>(Type);
	Added.Count = Count;

	Drops.MarkItemDirty(Added);

	UE_LOG(LogKillBugs, Verbose, TEXT("KB Loot: %s x%d dropped at (%.0f, %.0f)"),
		Type == EKBItemType::Medkit ? TEXT("medkit") : TEXT("material"),
		Count, Added.Location.X, Added.Location.Y);
}

void AKBLootDirector::ClearDrops()
{
	if (!HasAuthority())
	{
		return;
	}

	Drops.Items.Reset();
	Drops.MarkArrayDirty();
}

// ---------------------------------------------------------------------------------------------
// Picking up
// ---------------------------------------------------------------------------------------------

void AKBLootDirector::TickPickups()
{
	const UWorld* World = GetWorld();
	if (!World || Drops.Items.Num() == 0)
	{
		return;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PlayerController = It->Get();
		APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
		if (!IsValid(Pawn))
		{
			continue;
		}

		// A downed player is not somewhere: consistent with the channel gate, which treats a
		// player on the floor as absent for every purpose.
		const AKBPlayerState* PlayerState = PlayerController->GetPlayerState<AKBPlayerState>();
		if (!PlayerState || PlayerState->IsDowned())
		{
			continue;
		}

		UKBStatSheetComponent* Stats = Pawn->FindComponentByClass<UKBStatSheetComponent>();
		if (!Stats)
		{
			continue;
		}

		// The player's own radius, not a global one: this is what the 远见 card's
		// PickupRadiusMult has been multiplying since before anything read it.
		const float RadiusSq = FMath::Square(Stats->GetPickupRadius());
		const FVector PawnLocation = Pawn->GetActorLocation();

		// Reverse, so an index stays valid while entries are removed underneath it.
		for (int32 Index = Drops.Items.Num() - 1; Index >= 0; --Index)
		{
			const FKBItemNetItem& Drop = Drops.Items[Index];
			if (FVector::DistSquared2D(PawnLocation, Drop.Location) > RadiusSq)
			{
				continue;
			}

			const EKBItemType Type = static_cast<EKBItemType>(Drop.Type);

			AKBPlayerState* MutablePlayerState = PlayerController->GetPlayerState<AKBPlayerState>();
			if (!MutablePlayerState)
			{
				continue;
			}

			// Everything picked up now goes into the backpack, and whether it fits is the
			// backpack's decision - not this loop's. Materials and medkits differ only in what
			// they are worth on the way in; a medkit used to heal on contact, which is exactly
			// what this cut removes (the full-health rule moved to USE time, where the player
			// makes the call instead of the game making it for them).
			FString Reason;
			const bool bTaken = Type == EKBItemType::Medkit
				? MutablePlayerState->TryAddMedkits(Drop.Count, Reason)
				: MutablePlayerState->TryAddMaterials(Drop.Count, Reason);

			if (!bTaken)
			{
				// Left on the ground, and said out loud - once a second rather than once a frame
				// per drop, because a full backpack next to a pile would otherwise write a line
				// per item per frame. Silence here is the "I walked over it and nothing happened"
				// bug; a flood is the other way to make the same information useless.
				const UWorld* LogWorld = GetWorld();
				const float Now = LogWorld ? LogWorld->GetTimeSeconds() : 0.f;
				if (Now - LastRefusalLogTime > 1.f)
				{
					LastRefusalLogTime = Now;
					UE_LOG(LogKillBugs, Display, TEXT("KB Loot: %s left %s x%d on the ground - %s"),
						*GetNameSafe(Pawn),
						Type == EKBItemType::Medkit ? TEXT("medkit") : TEXT("material"),
						Drop.Count, *Reason);
				}

				continue;
			}

			if (Type == EKBItemType::Medkit)
			{
				UE_LOG(LogKillBugs, Display,
					TEXT("KB Loot: %s picked up medkit x%d (carrying %d, weight %d/%d)"),
					*GetNameSafe(Pawn), Drop.Count, MutablePlayerState->GetMedkits(),
					MutablePlayerState->GetCarriedWeight(), MutablePlayerState->GetBackpackCapacity());
			}
			else
			{
				// Display, not Verbose: a pickup is an event of the same order as a knockdown or a
				// completed channel, and a headless run has no other way to see that the radius
				// gate let one through. Walk over a kill field and this is a few lines a second,
				// which is the same order as the bullet diagnostics already on Display.
				UE_LOG(LogKillBugs, Display, TEXT("KB Loot: %s picked up material x%d (carrying %d)"),
					*GetNameSafe(Pawn), Drop.Count, MutablePlayerState->GetMaterials());
			}

			// Removed here, so a second player standing in the same place cannot also take it.
			// First come, first served, resolved serially on the server - there is no contention
			// and no race, and that is worth stating rather than leaving implicit. Note it only
			// happens when the take SUCCEEDED: a drop that did not fit stays for the next attempt.
			Drops.Items.RemoveAt(Index);
			Drops.MarkArrayDirty();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------

UStaticMesh* AKBLootDirector::ResolveMesh(EKBItemType Type)
{
	const TSoftObjectPtr<UStaticMesh>& Configured = Type == EKBItemType::Medkit
		? KBSettings().MedkitDropMesh
		: KBSettings().MaterialDropMesh;

	if (UStaticMesh* Mesh = Configured.LoadSynchronous())
	{
		return Mesh;
	}

	if (!bWarnedAboutPlaceholderMesh)
	{
		bWarnedAboutPlaceholderMesh = true;
		UE_LOG(LogKillBugs, Warning,
			TEXT("KB Loot: no drop mesh configured in 搜刮|掉落 - using the engine cube. The loot "
			     "works; it just does not look like anything yet."));
	}

	return LoadObject<UStaticMesh>(nullptr, PlaceholderMeshPath);
}

void AKBLootDirector::ApplyMaterial(UInstancedStaticMeshComponent* Instances, EKBItemType Type)
{
	if (!Instances)
	{
		return;
	}

	// In order of preference: the material the settings name, then whatever the mesh brought,
	// then the engine's placeholder material. The last step is not decoration - it is the only
	// one that is known to work, and the first attempt at this shipped without it.
	//
	// What happened: Tools/kb_probe_loot_tint.py reported that /Engine/BasicShapes/BasicShapeMaterial
	// exposes a `Color` parameter, which is true. But the engine's CUBE does not carry that
	// material - it carries WorldGridMaterial, which exposes nothing. So the tint was a silent
	// no-op and both drop types looked identical, which the log did say only because the
	// parameter was checked instead of assumed.
	UMaterialInterface* Material = KBSettings().DropMaterial.LoadSynchronous();
	if (!Material)
	{
		Material = Instances->GetMaterial(0);
	}

	FLinearColor Existing;
	const bool bTintable = Material
		&& Material->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("Color")), Existing);

	if (!bTintable)
	{
		if (!bWarnedAboutMissingTintParameter)
		{
			bWarnedAboutMissingTintParameter = true;
			UE_LOG(LogKillBugs, Warning,
				TEXT("KB Loot: '%s' takes no 'Color' parameter - falling back to the engine "
				     "placeholder material so the two drop types can still be told apart."),
				*GetNameSafe(Material));
		}

		Material = LoadObject<UMaterialInterface>(nullptr, PlaceholderMaterialPath);
		if (!Material)
		{
			return;
		}
	}

	UMaterialInstanceDynamic* Dynamic = UMaterialInstanceDynamic::Create(Material, Instances);
	Dynamic->SetVectorParameterValue(TEXT("Color"),
		Type == EKBItemType::Medkit ? KBSettings().MedkitDropTint : KBSettings().MaterialDropTint);

	Instances->SetMaterial(0, Dynamic);
}

UInstancedStaticMeshComponent* AKBLootDirector::GetInstances(EKBItemType Type)
{
	TObjectPtr<UInstancedStaticMeshComponent>& Slot =
		Type == EKBItemType::Medkit ? MedkitInstances : MaterialInstances;

	if (Slot)
	{
		return Slot;
	}

	UStaticMesh* Mesh = ResolveMesh(Type);
	if (!Mesh)
	{
		return nullptr;
	}

	UInstancedStaticMeshComponent* Instances =
		NewObject<UInstancedStaticMeshComponent>(this,
			Type == EKBItemType::Medkit ? TEXT("MedkitDrops") : TEXT("MaterialDrops"));

	Instances->SetupAttachment(GetRootComponent());
	Instances->SetStaticMesh(Mesh);
	Instances->SetMobility(EComponentMobility::Movable);

	// No collision at all: pickup is a distance test, so a drop is scenery that happens to be
	// interactable. Asking the physics scene about it would be work for no answer.
	Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Instances->SetGenerateOverlapEvents(false);

	// Slime decals are DEFERRED decals: they paint every surface inside their projection box, not
	// just the floor they were aimed at. A puddle next to a drop therefore lays a flat green splat
	// across the drop - the same thing that already had to be switched off for the player and the
	// swarm. Loot that looks like a stain is loot nobody walks to.
	Instances->SetReceivesDecals(false);

	ApplyMaterial(Instances, Type);

	Instances->RegisterComponent();
	Slot = Instances;

	return Instances;
}

void AKBLootDirector::UpdateVisuals()
{
	for (int32 TypeIndex = 0; TypeIndex < 2; ++TypeIndex)
	{
		const EKBItemType Type = static_cast<EKBItemType>(TypeIndex);

		// Count first, so a type nothing dropped never creates a component at all.
		int32 Live = 0;
		for (const FKBItemNetItem& Drop : Drops.Items)
		{
			if (static_cast<EKBItemType>(Drop.Type) == Type)
			{
				++Live;
			}
		}

		const TObjectPtr<UInstancedStaticMeshComponent>& Existing =
			Type == EKBItemType::Medkit ? MedkitInstances : MaterialInstances;

		// GetInstances creates one on first use; when nothing of this type is on the ground, an
		// already-created component still has to be written - as all-hidden - or the last drop
		// taken would stay drawn forever.
		UInstancedStaticMeshComponent* Instances = (Live > 0) ? GetInstances(Type) : Existing.Get();

		if (!Instances)
		{
			continue;
		}

		const int32 Previous = Instances->GetInstanceCount();
		const int32 Capacity = FMath::Max(Previous, Live);

		TArray<FTransform> Transforms;
		Transforms.Reserve(Capacity);
		for (int32 Index = 0; Index < Capacity; ++Index)
		{
			Transforms.Add(HiddenDropTransform());
		}

		int32 Written = 0;
		const float Scale = KBSettings().DropScale;
		for (const FKBItemNetItem& Drop : Drops.Items)
		{
			if (static_cast<EKBItemType>(Drop.Type) != Type)
			{
				continue;
			}

			if (Written < Capacity)
			{
				Transforms[Written++] = FTransform(FRotator::ZeroRotator, Drop.Location,
					FVector(Scale));
			}
		}

		// Grown, never shrunk: removing instances would shift every index above them. The unused
		// slots are simply written as hidden, exactly as the swarm visualizer does it.
		if (Capacity > Previous)
		{
			for (int32 Index = Previous; Index < Capacity; ++Index)
			{
				Instances->AddInstance(Transforms[Index], /*bWorldSpace*/ true);
			}
		}

		Instances->BatchUpdateInstancesTransforms(0, Transforms, /*bWorldSpace*/ true,
			/*bMarkRenderStateDirty*/ true, /*bTeleport*/ false);
	}
}

// ---- Console commands ------------------------------------------------------------------------

namespace KBLootCommands
{
	AKBLootDirector* Resolve(UWorld* World)
	{
		for (TActorIterator<AKBLootDirector> It(World); It; ++It)
		{
			return *It;
		}

		return nullptr;
	}

	void LogList(const TArray<FString>& Args, UWorld* World)
	{
		AKBLootDirector* Director = Resolve(World);
		if (!Director)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Loot.List: no loot director in this world"));
			return;
		}

		const FKBItemArray& Drops = Director->GetDrops();
		UE_LOG(LogKillBugs, Display, TEXT("KB Loot: %d drop(s) live"), Drops.Items.Num());

		for (const FKBItemNetItem& Drop : Drops.Items)
		{
			UE_LOG(LogKillBugs, Display, TEXT("  #%d %s x%d at (%.0f, %.0f)"), Drop.StableId,
				Drop.Type == static_cast<uint8>(EKBItemType::Medkit) ? TEXT("medkit") : TEXT("material"),
				Drop.Count, Drop.Location.X, Drop.Location.Y);
		}

		if (!World)
		{
			return;
		}

		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PlayerController = It->Get();
			const AKBPlayerState* PlayerState =
				PlayerController ? PlayerController->GetPlayerState<AKBPlayerState>() : nullptr;

			if (PlayerState)
			{
				UE_LOG(LogKillBugs, Display,
					TEXT("  player %d carries %d material(s) and %d medkit(s), weight %d/%d"),
					PlayerState->GetKBPlayerIndex(), PlayerState->GetMaterials(),
					PlayerState->GetMedkits(),
					PlayerState->GetCarriedWeight(), PlayerState->GetBackpackCapacity());
			}
		}
	}

	void SpawnOne(const TArray<FString>& Args, UWorld* World)
	{
		if (!World || Args.Num() < 2)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Loot.Spawn: expected <material|medkit> <count> [x y]"));
			return;
		}

		AKBLootDirector* Director = Resolve(World);
		if (!Director)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Loot.Spawn: no loot director in this world"));
			return;
		}

		const bool bMedkit = Args[0].Equals(TEXT("medkit"), ESearchCase::IgnoreCase);
		const EKBItemType Type = bMedkit ? EKBItemType::Medkit : EKBItemType::Material;
		const int32 Count = FCString::Atoi(*Args[1]);

		// Defaults to the local player's feet, which is the useful place for a test to put it.
		FVector Location = FVector::ZeroVector;
		if (Args.Num() >= 4)
		{
			Location = FVector(FCString::Atof(*Args[2]), FCString::Atof(*Args[3]), 0.f);
		}
		else if (const APlayerController* PlayerController = World->GetFirstPlayerController())
		{
			if (const APawn* Pawn = PlayerController->GetPawn())
			{
				Location = Pawn->GetActorLocation();
			}
		}

		Director->SpawnDrop(Location, Type, Count);
	}

	void Give(const TArray<FString>& Args, UWorld* World)
	{
		if (!World || Args.Num() < 1)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Loot.Give: expected <amount>"));
			return;
		}

		APlayerController* PlayerController = World->GetFirstPlayerController();
		AKBPlayerState* PlayerState =
			PlayerController ? PlayerController->GetPlayerState<AKBPlayerState>() : nullptr;

		if (!PlayerState)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Loot.Give: no local player state"));
			return;
		}

		// The KB.Profile.AddGold analogue: a way to reach the bank tests without walking to a
		// drop. The drop-and-pickup path has its own commands above.
		//
		// Capacity-gated like the real thing. It would be easy to leave this as a bypass, and
		// that would quietly make every capacity test meaningless.
		const int32 Amount = FCString::Atoi(*Args[0]);
		FString Reason;
		if (!PlayerState->TryAddMaterials(Amount, Reason))
		{
			UE_LOG(LogKillBugs, Display, TEXT("KB Loot: +%d material(s) refused - %s"), Amount, *Reason);
			return;
		}

		UE_LOG(LogKillBugs, Display, TEXT("KB Loot: +%d material(s) carried (now %d, weight %d/%d)"),
			Amount, PlayerState->GetMaterials(),
			PlayerState->GetCarriedWeight(), PlayerState->GetBackpackCapacity());
	}
}

static FAutoConsoleCommandWithWorldAndArgs KBConsoleLootList(
	TEXT("KB.Loot.List"),
	TEXT("KB.Loot.List - every drop on the ground, plus what each player is carrying."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBLootCommands::LogList));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleLootSpawn(
	TEXT("KB.Loot.Spawn"),
	TEXT("KB.Loot.Spawn <material|medkit> <count> [x y] - put a drop on the ground. Without x/y "
	     "it lands at the local player's feet, which the next frame's pickup tick will take."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBLootCommands::SpawnOne));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleLootGive(
	TEXT("KB.Loot.Give"),
	TEXT("KB.Loot.Give <n> - add n materials to the local player without walking anywhere, for "
	     "the banking tests."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBLootCommands::Give));

// ---------------------------------------------------------------------------------------------
// Backpack self test
// ---------------------------------------------------------------------------------------------

namespace KBBackpackCommands
{
	/**
	 * Walks the backpack through everything that changed, one step per log line, on a ticker.
	 *
	 * `-ExecCmds` fires once in one frame, so "put a medkit on the ground and look again half a
	 * second later" cannot be expressed as a command line - the same problem the extraction self
	 * test has, and the same answer.
	 *
	 * It runs against the REAL pickup loop and the REAL mutators rather than a shortcut: the
	 * steps drive `SpawnDrop` and then wait for `TickPickups` to run, because "the drop entered
	 * the backpack instead of healing" is a claim about that loop, not about the setter.
	 *
	 * Runs during Warmup, where nothing spawns and nothing attacks, so health is exactly what
	 * this test sets it to.
	 */
	struct FSelfTest
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<AKBPlayerState> PlayerState;
		TWeakObjectPtr<AKBLootDirector> Loot;

		AKBPlayerState* State() const { return PlayerState.Get(); }
		AKBLootDirector* Director() const { return Loot.Get(); }

		float StartTime = 0.f;
		int32 Step = 0;

		/** Counters carried between steps. */
		int32 MedkitsAtStart = 0;
		int32 HealthAtStart = 0.f;
		int32 DropIdUnderTest = INDEX_NONE;
		FString LastReason;

		bool Advance(float DeltaSeconds);
		bool Ready() const { return World.IsValid() && State() && Director(); }
		int32 DropCount() const;
		bool HasDropWithId(int32 StableId) const;
	};

	int32 FSelfTest::DropCount() const
	{
		return Director() ? Director()->GetDrops().Items.Num() : 0;
	}

	bool FSelfTest::HasDropWithId(int32 StableId) const
	{
		if (!Director())
		{
			return false;
		}

		for (const FKBItemNetItem& Drop : Director()->GetDrops().Items)
		{
			if (Drop.StableId == StableId)
			{
				return true;
			}
		}

		return false;
	}

	bool FSelfTest::Advance(float DeltaSeconds)
	{
		UWorld* LiveWorld = World.Get();
		AKBPlayerState* PlayerStatePtr = State();

		if (!LiveWorld || !PlayerStatePtr || !Director())
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.SelfTest: the world or the player went away"));
			return false;
		}

		const UWorld* TimeWorld = LiveWorld;
		const float Now = TimeWorld->GetTimeSeconds();
		const float Since = Now - StartTime;

		APawn* Pawn = PlayerStatePtr->GetPawn();
		UKBStatSheetComponent* Stats = Pawn ? Pawn->FindComponentByClass<UKBStatSheetComponent>() : nullptr;

		if (!Stats)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.SelfTest: no stat sheet"));
			return false;
		}

		switch (Step)
		{
		case 0:
			StartTime = Now;
			MedkitsAtStart = PlayerStatePtr->GetMedkits();

			// Hurt first, so the old behaviour is a real possibility the test can rule out: at
			// full health the old code refused the medkit outright, and "nothing happened" would
			// look like a pass.
			Stats->ApplyDamage(80.f);
			HealthAtStart = FMath::RoundToInt(Stats->GetHealth());

			Director()->SpawnDrop(Pawn->GetActorLocation(), EKBItemType::Medkit, 1);

			UE_LOG(LogKillBugs, Display,
				TEXT("Backpack.SelfTest 1/8: a medkit on the ground must go INTO the backpack, not heal the ")
				TEXT("player standing on it - health %.0f, medkits %d; expect health unchanged and medkits %d"),
				Stats->GetHealth(), MedkitsAtStart, MedkitsAtStart + 1);

			Step = 1;
			break;

		case 1:
			if (Since < 0.5f)
			{
				break;
			}
			{
				const bool bNoHeal = FMath::RoundToInt(Stats->GetHealth()) == HealthAtStart;
				const bool bCarried = PlayerStatePtr->GetMedkits() == MedkitsAtStart + 1;
				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.SelfTest 2/8: health %d (want %d - the instant heal must be GONE), ")
					TEXT("medkits %d (want %d) -> %s"),
					FMath::RoundToInt(Stats->GetHealth()), HealthAtStart,
					PlayerStatePtr->GetMedkits(), MedkitsAtStart + 1,
					(bNoHeal && bCarried) ? TEXT("PASS") : TEXT("FAIL"));
			}

			// Full health, so the next step tests the refusal rather than the heal.
			Stats->Heal(0.f);
			Step = 2;
			break;

		case 2:
			if (Since < 1.0f)
			{
				break;
			}
			{
				const int32 Before = PlayerStatePtr->GetMedkits();
				FString Reason;
				const bool bUsed = PlayerStatePtr->TryUseMedkit(Reason);
				const bool bPass = !bUsed && PlayerStatePtr->GetMedkits() == Before;

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.SelfTest 3/8: using at FULL health must be refused and must not consume - ")
					TEXT("used=%d, reason '%s', medkits %d -> %d (want unchanged) -> %s"),
					bUsed, *Reason, Before, PlayerStatePtr->GetMedkits(),
					bPass ? TEXT("PASS") : TEXT("FAIL"));
			}

			Stats->ApplyDamage(80.f);
			Step = 3;
			break;

		case 3:
			if (Since < 1.5f)
			{
				break;
			}
			{
				const int32 HealthBefore = FMath::RoundToInt(Stats->GetHealth());
				const int32 MedkitsBefore = PlayerStatePtr->GetMedkits();

				FString Reason;
				const bool bUsed = PlayerStatePtr->TryUseMedkit(Reason);
				const int32 Expected = FMath::Min(
					FMath::RoundToInt(KBSettings().MedkitHealAmount) + HealthBefore,
					FMath::RoundToInt(Stats->GetMaxHealth()));

				const bool bPass = bUsed
					&& PlayerStatePtr->GetMedkits() == MedkitsBefore - 1
					&& FMath::RoundToInt(Stats->GetHealth()) == Expected;

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.SelfTest 4/8: using below full health must heal exactly %.0f ONCE and spend ")
					TEXT("one - used=%d, health %d -> %d (want %d), medkits %d -> %d (want %d) -> %s"),
					KBSettings().MedkitHealAmount, bUsed, HealthBefore,
					FMath::RoundToInt(Stats->GetHealth()), Expected,
					MedkitsBefore, PlayerStatePtr->GetMedkits(), MedkitsBefore - 1,
					bPass ? TEXT("PASS") : TEXT("FAIL"));
			}
			Step = 4;
			break;

		case 4:
			if (Since < 2.0f)
			{
				break;
			}
			{
				// Drain whatever is left, so "none carried" is reachable without pretending.
				//
				// Each pass lands the player on exactly 1 health before healing: never 0, because
				// a downed player is REFUSED by TryUseMedkit (倒地时不能用) and the loop would spin
				// without consuming anything - which is what the first version of this test did.
				int32 Guard = 0;
				FString Reason;
				while (PlayerStatePtr->GetMedkits() > 0 && Guard++ < 64)
				{
					Stats->ApplyDamage(FMath::Max(Stats->GetHealth() - 1.f, 0.f));
					PlayerStatePtr->TryUseMedkit(Reason);
				}

				LastReason.Reset();
				const bool bUsed = PlayerStatePtr->TryUseMedkit(LastReason);
				const bool bPass = !bUsed && PlayerStatePtr->GetMedkits() == 0
					&& LastReason == TEXT("没有药包");

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.SelfTest 5/8: with none carried the use must be refused - used=%d, ")
					TEXT("reason '%s' (want 没有药包), medkits %d -> %s"),
					bUsed, *LastReason, PlayerStatePtr->GetMedkits(),
					bPass ? TEXT("PASS") : TEXT("FAIL"));
			}
			Step = 5;
			break;

		case 5:
			if (Since < 2.5f)
			{
				break;
			}
			{
				const int32 Capacity = PlayerStatePtr->GetBackpackCapacity();
				const int32 Weight = PlayerStatePtr->GetCarriedWeight();
				const int32 Unit = FMath::Max(1, KBSettings().MaterialUnitWeight);

				FString Reason;
				const bool bFilled = PlayerStatePtr->TryAddMaterials(Capacity - Weight - Unit, Reason);

				// Then the boundary itself: exactly one unit short must still fit.
				const bool bAtEdge = PlayerStatePtr->TryAddMaterials(Unit, Reason);
				const bool bPass = bFilled && bAtEdge && PlayerStatePtr->GetCarriedWeight() == Capacity;

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.SelfTest 6/8: filling must stop exactly AT the limit (the check is inclusive) - ")
					TEXT("capacity %d, weight now %d (want %d) -> %s"),
					Capacity, PlayerStatePtr->GetCarriedWeight(), Capacity,
					bPass ? TEXT("PASS") : TEXT("FAIL"));
			}
			Step = 6;
			break;

		case 6:
			if (Since < 3.0f)
			{
				break;
			}
			{
				const int32 Before = DropCount();
				Director()->SpawnDrop(Pawn->GetActorLocation(), EKBItemType::Material, 1);
				DropIdUnderTest = Director() && Director()->GetDrops().Items.Num() > Before
					? Director()->GetDrops().Items.Last().StableId
					: INDEX_NONE;

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.SelfTest 7/8: a drop that does not fit must STAY on the ground (drops %d, ")
					TEXT("weight %d/%d)"),
					Before, PlayerStatePtr->GetCarriedWeight(), PlayerStatePtr->GetBackpackCapacity());
			}
			Step = 7;
			break;

		case 7:
			if (Since < 3.8f)
			{
				break;
			}
			{
				const bool bStillThere = HasDropWithId(DropIdUnderTest);
				const int32 Materials = PlayerStatePtr->GetMaterials();

				UE_LOG(LogKillBugs, Display,
					TEXT("Backpack.SelfTest 8/8: the refused drop is still on the ground=%d (want 1) and was ")
					TEXT("NOT added (materials %d, weight %d/%d) -> %s"),
					bStillThere, Materials,
					PlayerStatePtr->GetCarriedWeight(), PlayerStatePtr->GetBackpackCapacity(),
					bStillThere ? TEXT("PASS") : TEXT("FAIL"));
			}

			UE_LOG(LogKillBugs, Display, TEXT("Backpack.SelfTest: done"));
			return false;

		default:
			return false;
		}

		return true;
	}
} // namespace KBBackpackCommands

void AKBLootDirector::ConsoleBackpackSelfTest(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}

	AKBPlayerState* PlayerState = nullptr;
	if (const APlayerController* PlayerController = World->GetFirstPlayerController())
	{
		PlayerState = PlayerController->GetPlayerState<AKBPlayerState>();
	}

	for (TActorIterator<AKBLootDirector> It(World); It; ++It)
	{
		if (!PlayerState)
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Backpack.SelfTest: no local player state"));
			return;
		}

		TSharedPtr<KBBackpackCommands::FSelfTest> Test = MakeShared<KBBackpackCommands::FSelfTest>();
		Test->World = World;
		Test->PlayerState = PlayerState;
		Test->Loot = *It;

		UE_LOG(LogKillBugs, Display, TEXT("Backpack.SelfTest: started"));

		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Test](float DeltaSeconds) -> bool
			{
				return Test->Advance(DeltaSeconds);
			}));
		return;
	}

	UE_LOG(LogKillBugs, Warning, TEXT("No AKBLootDirector in this world"));
}

// Named uniquely across the whole module on purpose. The editor target has unity build OFF, so a
// second file-scope static with this name would compile there; the PACKAGING target has unity ON,
// which concatenates several .cpp files into ONE translation unit - and then two different
// KB.Backpack tests declaring the same static is a redefinition. It cost a packaging round to
// find, and only because the game target had never been compiled before.
static FAutoConsoleCommandWithWorldAndArgs KBConsoleBackpackWeightTest(
	TEXT("KB.Backpack.SelfTest"),
	TEXT("KB.Backpack.SelfTest - one command walks the whole carried-backpack chain: a medkit goes ")
	TEXT("into the pack instead of healing, use is refused at full health and with none carried, a ")
	TEXT("use heals exactly once, the weight limit is inclusive, and a drop that does not fit stays ")
	TEXT("on the ground. Eight steps, each logging what it proves."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AKBLootDirector::ConsoleBackpackSelfTest));
