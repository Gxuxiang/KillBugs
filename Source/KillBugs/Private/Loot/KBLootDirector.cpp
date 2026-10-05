#include "Loot/KBLootDirector.h"

#include "Combat/KBStatSheetComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
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
	const bool bForce = CVarKBLootForceDrop.GetValueOnGameThread() != 0;

	if (bForce || FMath::FRand() < Archetype.MedkitDropChance)
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

			// A medkit at full health is left where it is. Picking it up would spend it for
			// nothing, and an automatic pickup means the player never chose to - so the choice is
			// made for them, and it is "not yet".
			if (Type == EKBItemType::Medkit && Stats->GetHealthFraction() >= 1.f)
			{
				continue;
			}

			if (Type == EKBItemType::Medkit)
			{
				const float Before = Stats->GetHealth();
				Stats->Heal(KBSettings().MedkitHealAmount);

				UE_LOG(LogKillBugs, Display, TEXT("KB Loot: %s picked up a medkit (health %.0f -> %.0f)"),
					*GetNameSafe(Pawn), Before, Stats->GetHealth());
			}
			else if (AKBPlayerState* MutablePlayerState = PlayerController->GetPlayerState<AKBPlayerState>())
			{
				MutablePlayerState->AddMaterials(Drop.Count);

				// Display, not Verbose: a pickup is an event of the same order as a knockdown or a
				// completed channel, and a headless run has no other way to see that the radius
				// gate let one through. Walk over a kill field and this is a few lines a second,
				// which is the same order as the bullet diagnostics already on Display.
				UE_LOG(LogKillBugs, Display, TEXT("KB Loot: %s picked up material x%d (carrying %d)"),
					*GetNameSafe(Pawn), Drop.Count, MutablePlayerState->GetMaterials());
			}

			// Removed here, so a second player standing in the same place cannot also take it.
			// First come, first served, resolved serially on the server - there is no contention
			// and no race, and that is worth stating rather than leaving implicit.
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
				UE_LOG(LogKillBugs, Display, TEXT("  player %d carries %d material(s)"),
					PlayerState->GetKBPlayerIndex(), PlayerState->GetMaterials());
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
		PlayerState->AddMaterials(FCString::Atoi(*Args[0]));

		UE_LOG(LogKillBugs, Display, TEXT("KB Loot: +%d material(s) carried (now %d)"),
			FCString::Atoi(*Args[0]), PlayerState->GetMaterials());
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
