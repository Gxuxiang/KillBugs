#include "Core/KBGameMode.h"

#include "Core/KBCharacter.h"
#include "Core/KBGameState.h"
#include "Core/KBPlayerController.h"
#include "Combat/KBProjectileDirector.h"
#include "Combat/KBStatSheetComponent.h"
#include "Combat/KBWeaponInventoryComponent.h"
#include "Core/KBPlayerState.h"
#include "Containers/Ticker.h"
#include "Data/KBCardDefinition.h"
#include "Data/KBContentSubsystem.h"
#include "EngineUtils.h"
#include "Extraction/KBExtractionZone.h"
#include "GameFramework/Pawn.h"
#include "Interaction/KBChannelComponent.h"
#include "KillBugs.h"
#include "KBGameSettings.h"
#include "Loot/KBLootDirector.h"
#include "Net/KBSessionSubsystem.h"
#include "Swarm/KBEnemyDirector.h"
#include "UI/KBHud.h"

AKBGameMode::AKBGameMode()
{
	GameStateClass = AKBGameState::StaticClass();
	PlayerStateClass = AKBPlayerState::StaticClass();
	PlayerControllerClass = AKBPlayerController::StaticClass();
	DefaultPawnClass = AKBCharacter::StaticClass();

	// Listen-server co-op: the host is also a player. Seamless travel would try to carry the
	// live swarm across a level change, which nothing needs yet.
	bUseSeamlessTravel = false;

	EnemyDirectorClass = AKBEnemyDirector::StaticClass();
	ProjectileDirectorClass = AKBProjectileDirector::StaticClass();
	ExtractionZoneClass = AKBExtractionZone::StaticClass();
	LootDirectorClass = AKBLootDirector::StaticClass();
	HUDClass = AKBHud::StaticClass();

	// Must be set here, not in BeginPlay: tick functions are registered during actor
	// initialisation, so flipping bCanEverTick afterwards leaves the actor never ticking and
	// the wave clock silently frozen.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void AKBGameMode::BeginPlay()
{
	Super::BeginPlay();

	// Server only: a client must never spawn its own swarm or run its own wave clock.
	if (!HasAuthority() || !EnemyDirectorClass)
	{
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// The director simulates in world space and is never moved, so identity is correct.
	EnemyDirector = GetWorld()->SpawnActor<AKBEnemyDirector>(
		EnemyDirectorClass, FTransform::Identity, SpawnParams);

	// Bullets live in their own simulator. Spawned here, once, for the same reason as the
	// swarm: one actor holding a flat array beats an actor per projectile.
	if (ProjectileDirectorClass)
	{
		GetWorld()->SpawnActor<AKBProjectileDirector>(
			ProjectileDirectorClass, FTransform::Identity, SpawnParams);
	}

	// Everything lying on the ground, in one actor - the same shape as the swarm and the bullets.
	// Spawned before the extraction zone so the enemy director can be handed a pointer to it
	// below, at the same moment the two are wired together.
	if (LootDirectorClass)
	{
		LootDirector = GetWorld()->SpawnActor<AKBLootDirector>(
			LootDirectorClass, FTransform::Identity, SpawnParams);

		if (LootDirector && EnemyDirector)
		{
			// Injected rather than looked up per death: the answer never changes, and a lookup
			// on every kill would be work for a pointer that is already known.
			EnemyDirector->SetLootDirector(LootDirector);
		}
	}

	// The extraction zone exists from the start but is inactive: it is opened and closed on the
	// wave schedule, and it teleports between appearances rather than being spawned per attempt.
	if (ExtractionZoneClass)
	{
		ExtractionZone = GetWorld()->SpawnActor<AKBExtractionZone>(
			ExtractionZoneClass, FTransform::Identity, SpawnParams);

		if (ExtractionZone && ExtractionZone->GetChannel())
		{
			// Bound ONCE. AddDynamic does not de-duplicate, so rebinding per attempt would stack
			// handlers and a single completion would end the run several times over.
			ExtractionZone->GetChannel()->OnChannelComplete.AddDynamic(
				this, &AKBGameMode::HandleExtractionComplete);
			ExtractionZone->OnExtractionWindowExpired.AddDynamic(
				this, &AKBGameMode::HandleExtractionWindowExpired);
			ExtractionZone->OnExtractionStarted.AddDynamic(
				this, &AKBGameMode::HandleExtractionStarted);
		}
	}

	AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState)
	{
		return;
	}

	// Mirror the timings onto the replicated state so a client can draw a timeline without
	// having access to this GameMode.
	FKBPhaseTimings Timings;
	Timings.Warmup = KBSettings().WarmupSeconds;
	Timings.Wave = KBSettings().WaveSeconds;
	Timings.Explore = KBSettings().ExploreSeconds;
	Timings.CardDraft = KBSettings().DraftInputLockSeconds + KBSettings().DraftSeconds;
	RunState->SetPhaseTimingsServer(Timings);

	RunState->SetWaveIndexServer(0);
	RunState->SetWavePhaseServer(EKBWavePhase::Warmup,
		GetWorld()->GetTimeSeconds() + KBSettings().WarmupSeconds);
	UE_LOG(LogKillBugs, Display, TEXT("Run started: warmup %.0fs, then waves of %.0fs"),
		KBSettings().WarmupSeconds, KBSettings().WaveSeconds);

	// The setting is the wave number the PLAYER sees; the schedule is kept in internal wave
	// indices, which are displayed as +1 everywhere else. Converted once, here.
	ScheduleExtractionForWave(KBSettings().ExtractionFirstWave - 1);
}

void AKBGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasAuthority())
	{
		return;
	}

	AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState || !EnemyDirector)
	{
		return;
	}

	const float Now = GetWorld()->GetTimeSeconds();

	// Run over the moment nobody is left standing. Checked outside the phase switch because
	// it can happen at any point in the loop.
	if (RunState->GetWavePhase() != EKBWavePhase::RunOver && AreAllPlayersDowned())
	{
		EndRun(EKBRunResult::WipedOut);
		return;
	}

	switch (RunState->GetWavePhase())
	{
	case EKBWavePhase::Warmup:
		if (Now >= RunState->GetPhaseEndServerTime())
		{
			BeginCardDraft(NextWaveIndex);
		}
		break;

	case EKBWavePhase::CardDraft:
	{
		// Advance as soon as everyone has picked, or when the draft times out. Never wait on
		// a player who has walked away - and never leave them empty-handed either.
		if (!AnyPlayerAwaitingPick() || Now >= RunState->GetPhaseEndServerTime())
		{
			AutoResolveRemainingPicks();

			const int32 WaveToStart = NextWaveIndex;
			++NextWaveIndex;
			StartWave(WaveToStart);
		}
		break;
	}

	case EKBWavePhase::WaveActive:
		// A sustained flood for the whole wave rather than a fixed bag emptied early.
		TickSpawning(DeltaSeconds, KBSettings().BaseSpawnRate + KBSettings().SpawnRatePerWave * RunState->GetWaveIndex());
		if (Now >= RunState->GetPhaseEndServerTime())
		{
			EndWave();
		}
		break;

	case EKBWavePhase::Explore:
		// Normally a trickle - the real content of this phase is whatever survived the wave.
		//
		// The extraction flood is tied to the TEAM HOLDING THE CIRCLE, not to the zone merely
		// existing. That is deliberate and it was learned the hard way: with the flood on from the
		// moment the ring appeared, the hardest part became walking to it, and the zone spawns as
		// far from the team as the map allows. Tying it to the hold means the approach is an
		// ordinary explore, the wave answers the moment the players commit, and stepping back out
		// is a real way to regroup - at the cost of the open window, which resumes draining.
		if (ExtractionZone && ExtractionZone->IsZoneOpen()
			&& ExtractionZone->GetChannel() && ExtractionZone->GetChannel()->IsAdvancing())
		{
			TickSpawning(DeltaSeconds, KBSettings().ExtractionSpawnRate, KBSettings().ExtractionAliveCap);
		}
		else
		{
			TickSpawning(DeltaSeconds, KBSettings().ExploreAmbientSpawnRate);
		}

		if (Now >= RunState->GetPhaseEndServerTime())
		{
			// Draft AFTER exploring, so the choice is made knowing what the leftovers cost
			// and before the next flood arrives - not blind, the instant a wave ends.
			BeginCardDraft(NextWaveIndex);
		}
		break;

	case EKBWavePhase::RunOver:
		// The summary has been on screen for RunSummarySeconds; now everybody goes back.
		//
		// No extra "set a flag then travel a beat later" dance is needed here - unlike the lobby,
		// where the travel used to happen in the same frame as the click. The delay IS the
		// summary: the phase end time was set when the run ended, and the summary has been
		// drawn on every frame since.
		if (Now >= RunState->GetPhaseEndServerTime())
		{
			ReturnToLobby();
		}
		break;

	default:
		break;
	}
}

void AKBGameMode::EndRun(EKBRunResult Result)
{
	if (!HasAuthority())
	{
		return;
	}

	AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState || RunState->GetWavePhase() == EKBWavePhase::RunOver)
	{
		// Already over. A run that has ended must not be re-ended, or a wipe in the frame after
		// a successful extraction would rewrite the verdict and also push the return-to-lobby
		// timer out again.
		return;
	}

	RunState->SetRunResultServer(Result);
	RunState->SetWavePhaseServer(EKBWavePhase::RunOver,
		GetWorld()->GetTimeSeconds() + KBSettings().RunSummarySeconds);

	// The run ends where the world stops, not where the summary starts.
	//
	// Nothing else stops it: the swarm keeps chewing and the player keeps walking, aiming and
	// shooting all the way through the twelve seconds the summary is on screen. That is what a
	// successful extraction looked like - a "撤离成功" panel over a fight still in progress.
	//
	// The swarm is FROZEN, not culled. Culling would be the obvious way to make the arena go
	// quiet, but a removal from the replicated array is exactly how every machine detects a death
	// - so culling 186 survivors reads as 186 simultaneous kills and throws a field of splatter
	// into the summary frame. Freezing stops the same things and removes nothing.
	//
	// The player freeze is replicated, because movement is client-predicted (see
	// AKBCharacter::bRunOver). Both endings get all of it: the run is over, and it is over the
	// same way whichever door it left by.
	if (EnemyDirector)
	{
		EnemyDirector->SetSimulationFrozen(true);

		// Logged because the difference between freezing and culling is invisible from outside:
		// a headless run cannot see the arena, and "0 bugs" and "200 bugs standing still" both
		// look like nothing happening.
		UE_LOG(LogKillBugs, Display, TEXT("Run over: %d bug(s) frozen in place"),
			EnemyDirector->GetEnemyCount());
	}

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APawn* Pawn = It->Get() ? It->Get()->GetPawn() : nullptr;
		if (AKBCharacter* Character = Cast<AKBCharacter>(Pawn))
		{
			Character->SetRunOverServer(true);
		}
	}

	UE_LOG(LogKillBugs, Display, TEXT("Run over (%s) at wave %d - returning to the lobby in %.0fs"),
		Result == EKBRunResult::Extracted ? TEXT("extracted") : TEXT("wiped out"),
		RunState->GetWaveIndex() + 1, KBSettings().RunSummarySeconds);

	// What the summary screen is showing, written to the log as well.
	//
	// Not redundant: DrawHUD never runs under -nullrhi, so without this there is no way to check
	// from a headless run that the numbers on that screen are the ones that were earned - and
	// the level in particular is derived (AKBPlayerState::ComputeLevelForXP), so it is exactly
	// the sort of thing that is wrong while looking right.
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PlayerController = It->Get();
		const AKBPlayerState* PlayerState =
			PlayerController ? PlayerController->GetPlayerState<AKBPlayerState>() : nullptr;

		if (PlayerState)
		{
			UE_LOG(LogKillBugs, Display,
				TEXT("  summary for player %d: level %d | xp %d | gold %d"),
				PlayerState->GetKBPlayerIndex(), PlayerState->GetKBLevel(),
				PlayerState->GetXP(), PlayerState->GetGold());
		}
	}
}

void AKBGameMode::ReturnToLobby()
{
	UWorld* World = GetWorld();
	if (!World || bReturningToLobby)
	{
		return;
	}

	// Latched BEFORE the travel, not after: ServerTravel only takes effect at the end of the
	// frame, so without this the RunOver branch calls it again on every tick until the world
	// changes. A two-process test caught exactly that - five "travelling back to the lobby"
	// lines inside one second, each one queueing another pending travel onto a world that was
	// already on its way out.
	bReturningToLobby = true;

	// Listen servers go back to a lobby that is still joinable; a solo run goes back to an
	// ordinary lobby.
	//
	// The ?listen is not cosmetic and not redundant with "we are already a listen server": the
	// net mode of the destination world is derived from the URL alone, so travelling without it
	// silently demotes the host to standalone and drops every client on arrival. See the note on
	// KBTravel::ToArenaAsListenServer, which learned the same lesson the hard way.
	const bool bWasListenServer = GetNetMode() == NM_ListenServer;
	const TCHAR* Target = bWasListenServer ? KBTravel::ToLobbyAsListenServer() : KBTravel::ToLobby();

	UE_LOG(LogKillBugs, Display, TEXT("Run done: travelling back to the lobby (%s)"),
		bWasListenServer ? TEXT("listen") : TEXT("solo"));

	// NOTE: this is a non-seamless travel (bUseSeamlessTravel is false), so every PlayerState is
	// destroyed on arrival and Gold/XP/level go with it. That is acceptable while the economy is
	// still per-run, but the design has gold persisting across runs - so when the stash lands,
	// this is the place it has to be banked, BEFORE the travel.
	World->ServerTravel(Target);
}

void AKBGameMode::StartWave(int32 WaveIndex)
{
	AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState || !EnemyDirector)
	{
		return;
	}

	// NOTE: this is where ReviveDownedPlayers() used to be called - every downed player stood
	// back up for free at the start of the next wave. That is gone: getting somebody up is now
	// something a teammate does, by standing in the circle their body projects
	// (UKBChannelComponent on AKBCharacter). A downed player who is never reached stays down,
	// which is also what makes the extraction rule bite - nobody can extract while a teammate
	// is on the floor.

	EnemyDirector->SetWaveIndex(WaveIndex);

	// Back to full strength for the flood.
	EnemyDirector->SetEnemyScaling(KBSettings().WaveEnemyDamageTakenScale, KBSettings().WaveEnemySpeedScale);

	// Start with a full bug's worth of credit so the first spawn does not wait a whole
	// interval before the wave visibly begins.
	SpawnAccumulator = 1.f;

	RunState->SetWaveIndexServer(WaveIndex);
	RunState->SetWavePhaseServer(EKBWavePhase::WaveActive,
		GetWorld()->GetTimeSeconds() + KBSettings().WaveSeconds);

	UE_LOG(LogKillBugs, Display, TEXT("Wave %d started: %.1f bugs/sec for %.0fs (%.0f alive carried over)"),
		WaveIndex, KBSettings().BaseSpawnRate + KBSettings().SpawnRatePerWave * WaveIndex, KBSettings().WaveSeconds,
		static_cast<float>(EnemyDirector->GetEnemyCount()));
}

void AKBGameMode::EndWave()
{
	AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState || !EnemyDirector)
	{
		return;
	}

	// Deliberately NO cull.
	//
	// The leftovers ARE the explore phase: the player has to fight their way clear before
	// there is any time to loot, and how many are left is a direct reading of how well they
	// cleared during the wave. Culling here would turn Explore into a free shopping trip and
	// throw that feedback away.
	const int32 Remaining = EnemyDirector->GetEnemyCount();

	UE_LOG(LogKillBugs, Display, TEXT("Wave %d over with %d bug(s) still alive to clear"),
		RunState->GetWaveIndex(), Remaining);

	BeginExplore();
}

void AKBGameMode::BeginExplore()
{
	AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState || !EnemyDirector)
	{
		return;
	}

	// Soften everything still standing, including bugs spawned during the wave: the point of
	// Explore is to clear the leftovers AND have time left to search.
	EnemyDirector->SetEnemyScaling(KBSettings().ExploreEnemyDamageTakenScale, KBSettings().ExploreEnemySpeedScale);

	// An extraction wave's Explore phase has to last the whole episode. The two clocks never run
	// at once, so the episode is at most window + progress; sizing the phase to fit it here is
	// what keeps it from straddling a boundary. Otherwise the phase would flip to the card draft
	// with the team still holding the circle - the draft would appear mid-hold, and its input lock
	// would land right when the gate needs everybody to keep still.
	const bool bExtractionWave = RunState->GetWaveIndex() == RunState->GetNextExtractionWaveIndex();

	float ExploreDuration = KBSettings().ExploreSeconds;
	if (bExtractionWave)
	{
		const float EpisodeLength = KBSettings().ExtractionOpenWindowSeconds
			+ KBSettings().ExtractionSeconds
			+ KBSettings().ExtractionPhaseTailSeconds;

		ExploreDuration = FMath::Max(ExploreDuration, EpisodeLength);
	}

	RunState->SetWavePhaseServer(EKBWavePhase::Explore,
		GetWorld()->GetTimeSeconds() + ExploreDuration);

	UE_LOG(LogKillBugs, Display,
		TEXT("Explore: %.0fs, %d leftover(s) at %.1fx damage taken / %.2fx speed"),
		ExploreDuration, EnemyDirector->GetEnemyCount(),
		KBSettings().ExploreEnemyDamageTakenScale, KBSettings().ExploreEnemySpeedScale);

	if (bExtractionWave)
	{
		OpenExtraction();
	}
}

// =============================================================================================
// Extraction
//
// The GameMode decides WHEN and WHERE and what a finished attempt means. The zone
// (AKBExtractionZone) owns the two clocks and reports back through its delegates; it never calls
// EndRun itself and never looks this class up.
// =============================================================================================

void AKBGameMode::ScheduleExtractionForWave(int32 WaveIndex)
{
	AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!HasAuthority() || !RunState)
	{
		return;
	}

	RunState->SetNextExtractionWaveIndexServer(WaveIndex);

	if (WaveIndex >= 0)
	{
		UE_LOG(LogKillBugs, Display, TEXT("Extraction scheduled: wave %d (%d wave(s) from now)"),
			WaveIndex + 1, FMath::Max(WaveIndex - RunState->GetWaveIndex(), 0));
	}
	else
	{
		UE_LOG(LogKillBugs, Display, TEXT("Extraction taken off the schedule"));
	}
}

TArray<FVector> AKBGameMode::BuildExtractionCandidates() const
{
	// A grid over the arena floor, inset far enough that a whole zone fits inside the walls.
	//
	// Deliberately crude: the arena is one flat plane, so every candidate set is equally
	// arbitrary. It lives in one function precisely so it can be replaced - when the maze exists
	// this becomes "the centres of the rooms", and SelectExtractionLocation and its callers do not
	// change at all.
	const float Half = FMath::Max(
		KBSettings().ArenaHalfExtent - KBSettings().ExtractionEdgeMargin - KBSettings().ExtractionRadius,
		100.f);

	constexpr int32 Steps = 5;
	TArray<FVector> Candidates;
	Candidates.Reserve(Steps * Steps);

	for (int32 X = 0; X < Steps; ++X)
	{
		for (int32 Y = 0; Y < Steps; ++Y)
		{
			const float FX = (static_cast<float>(X) / (Steps - 1)) * 2.f - 1.f;
			const float FY = (static_cast<float>(Y) / (Steps - 1)) * 2.f - 1.f;
			Candidates.Add(FVector(FX * Half, FY * Half, 0.f));
		}
	}

	return Candidates;
}

FVector AKBGameMode::SelectExtractionLocation() const
{
	const TArray<FVector> Candidates = BuildExtractionCandidates();
	if (Candidates.Num() == 0)
	{
		return FVector::ZeroVector;
	}

	// Where the players actually are, gathered once. Living or downed does not matter: a downed
	// player is still somewhere the team has to come back for, and a zone next to them is no
	// journey at all.
	TArray<FVector, TInlineAllocator<4>> PlayerLocations;
	if (const AKBGameState* RunState = GetGameState<AKBGameState>())
	{
		for (const APlayerState* PS : RunState->PlayerArray)
		{
			const AKBPlayerState* KBPS = Cast<AKBPlayerState>(PS);
			const APawn* Pawn = KBPS ? KBPS->GetPawn() : nullptr;
			if (Pawn)
			{
				PlayerLocations.Add(Pawn->GetActorLocation());
			}
		}
	}

	// Nobody to be far away from. The swarm's focus is itself the arena centre when there are no
	// living players, which is as good an origin as any.
	if (PlayerLocations.Num() == 0 && EnemyDirector)
	{
		PlayerLocations.Add(EnemyDirector->GetSwarmFocusLocation());
	}

	// Score = distance to the NEAREST player. Maximising it is exactly "farthest from the team",
	// and it cannot be gamed by one player standing on a candidate: the others still hold the
	// score down.
	int32 BestIndex = 0;
	float BestScore = -1.f;

	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		float Nearest = TNumericLimits<float>::Max();
		for (const FVector& PlayerLocation : PlayerLocations)
		{
			Nearest = FMath::Min(Nearest, FVector::Dist2D(Candidates[Index], PlayerLocation));
		}

		if (Nearest > BestScore)
		{
			BestScore = Nearest;
			BestIndex = Index;
		}
	}

	return Candidates[BestIndex];
}

void AKBGameMode::OpenExtraction()
{
	if (!HasAuthority() || !ExtractionZone || !EnemyDirector)
	{
		return;
	}

	const FVector Location = SelectExtractionLocation();

	// Nothing else happens here on purpose. Opening the zone is not an event the swarm reacts to:
	// the answer is to the team committing, which arrives later as OnExtractionStarted. This is
	// what keeps the walk there free of a flood the team has not yet earned.
	ExtractionZone->OpenZone(Location, KBSettings().ExtractionOpenWindowSeconds,
		KBSettings().ExtractionSeconds, KBSettings().ExtractionRadius);
}

void AKBGameMode::HandleExtractionStarted()
{
	// The players are all inside and the countdown is running. This is the moment the design
	// answers, and it is a moment rather than a condition: a wave arrives at once, and the
	// sustained pressure is the spawn rate the Explore branch switches to while the gate holds.
	if (!HasAuthority() || !EnemyDirector)
	{
		return;
	}

	// Everybody who committed gets a full bar.
	//
	// The hold is thirty seconds under a swarm that only shows up because they did - and the walk
	// there is what costs them. Measured in a real run: the team reached the zone with 18 of the
	// 60 window seconds left and most of their health gone, then died holding at 79%. Charging the
	// walk and the hold out of the same bar is two punishments for one decision. The extraction is
	// where a run is supposed to become winnable, so it starts at full.
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PlayerController = It->Get();
		APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
		UKBStatSheetComponent* Stats =
			Pawn ? Pawn->FindComponentByClass<UKBStatSheetComponent>() : nullptr;

		if (Stats)
		{
			// Heal(0) means "to full"; see UKBStatSheetComponent::Heal.
			Stats->Heal(0.f);
			UE_LOG(LogKillBugs, Display, TEXT("Extraction started: %s topped up to full"),
				*GetNameSafe(Pawn));
		}
	}

	if (KBSettings().ExtractionOpeningBurst > 0)
	{
		EnemyDirector->SpawnBatchOffscreen(KBSettings().ExtractionOpeningBurst,
			KBSettings().MinSpawnDistance, KBSettings().MaxSpawnDistance);

		UE_LOG(LogKillBugs, Display, TEXT("Extraction started: %d bugs incoming, then %.0f/s"),
			KBSettings().ExtractionOpeningBurst, KBSettings().ExtractionSpawnRate);
	}
}

void AKBGameMode::HandleExtractionComplete()
{
	// Extraction is the other half of "a run ended", and it enters the same funnel as a wipe so
	// both produce the same summary and the same trip home. EndRun latches the result, so a wipe
	// landing in this same frame cannot overwrite it with the bad news.
	UE_LOG(LogKillBugs, Display, TEXT("Extraction succeeded - ending the run"));
	EndRun(EKBRunResult::Extracted);
}

void AKBGameMode::HandleExtractionWindowExpired()
{
	// NOT an ending. The attempt failed: the zone is closed already, the run carries on, and the
	// next opportunity is one interval away.
	AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState)
	{
		return;
	}

	// Nothing to undo: the enemy scaling was never touched for the extraction, and the spawn rate
	// falls back on its own the moment the gate closes, because it is keyed off the hold.
	ScheduleExtractionForWave(RunState->GetWaveIndex() + FMath::Max(KBSettings().ExtractionWaveInterval, 1));
}

// ---- Headless test entry points ------------------------------------------------------------
//
// Registered below as KB.Extract.*. Static members rather than free functions because they need
// the private schedule and the zone pointer, and because this is how AKBEnemyDirector exposes its
// own console commands.

namespace KBExtractCommands
{
	AKBGameMode* ResolveGameMode(UWorld* World)
	{
		return World ? World->GetAuthGameMode<AKBGameMode>() : nullptr;
	}

	void MoveEveryPlayer(UWorld* World, const FVector& Target)
	{
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APawn* Pawn = It->Get() ? It->Get()->GetPawn() : nullptr;
			if (!Pawn)
			{
				continue;
			}

			Pawn->SetActorLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);
			UE_LOG(LogKillBugs, Display, TEXT("Extract: moved %s to (%.0f, %.0f)"),
				*GetNameSafe(Pawn), Target.X, Target.Y);
		}
	}

	AKBExtractionZone* FindZone(UWorld* World)
	{
		for (TActorIterator<AKBExtractionZone> It(World); It; ++It)
		{
			return *It;
		}

		return nullptr;
	}

	/**
	 * Walks the whole state machine on a timer, logging what each step is supposed to prove.
	 *
	 * -ExecCmds fire once, in a single frame, so a sequence that needs "and then the player steps
	 * out for a second" cannot be expressed as a command line. This is the same problem
	 * KBLobbyGameMode's waiter solves for the lobby, and the same answer: a ticker.
	 *
	 * It runs during Warmup on purpose. Nothing spawns before the first card draft, so the swarm
	 * cannot kill the subject mid-test and turn an extraction into a wipe - the point here is the
	 * zone's two clocks, not surviving them.
	 */
	struct FSelfTest
	{
		TWeakObjectPtr<UWorld> World;
		float Elapsed = 0.f;
		int32 Step = 0;

		bool Advance(float DeltaSeconds)
		{
			Elapsed += DeltaSeconds;

			UWorld* LiveWorld = World.Get();
			AKBExtractionZone* Zone = LiveWorld ? FindZone(LiveWorld) : nullptr;
			if (!Zone)
			{
				UE_LOG(LogKillBugs, Warning, TEXT("Extract.SelfTest: the zone went away"));
				return false;
			}

			switch (Step)
			{
			case 0:
				UE_LOG(LogKillBugs, Display,
					TEXT("Extract.SelfTest 1/6: opening 3000 units away (window 4s) - the window must DRAIN and expire"));
				Zone->OpenZone(FVector(3000.f, 0.f, 0.f), 4.f, 3.f, KBSettings().ExtractionRadius);
				Step = 1;
				break;

			case 1:
				if (Elapsed >= 5.f)
				{
					UE_LOG(LogKillBugs, Display,
						TEXT("Extract.SelfTest 2/6: the window must be gone - zone CLOSED and the run STILL GOING"));
					Step = 2;
				}
				break;

			case 2:
				if (Elapsed >= 6.f)
				{
					UE_LOG(LogKillBugs, Display,
						TEXT("Extract.SelfTest 3/6: reopening under the team (window 20s, progress 3s) - must FREEZE, then progress"));
					Zone->OpenZone(FVector(0.f, 0.f, 0.f), 20.f, 3.f, KBSettings().ExtractionRadius);
					Step = 3;
				}
				break;

			case 3:
				if (Elapsed >= 7.5f)
				{
					UE_LOG(LogKillBugs, Display,
						TEXT("Extract.SelfTest 4/6: scattering the team - progress must be HELD, the window RESUMED"));
					MoveEveryPlayer(LiveWorld, FVector(0.f, 3000.f, 200.f));
					Step = 4;
				}
				break;

			case 4:
				if (Elapsed >= 9.f)
				{
					UE_LOG(LogKillBugs, Display,
						TEXT("Extract.SelfTest 5/6: gathering back - progress must resume from where it stopped and finish"));
					MoveEveryPlayer(LiveWorld, Zone->GetZoneCentre());
					Step = 5;
				}
				break;

			case 5:
				UE_LOG(LogKillBugs, Display,
					TEXT("Extract.SelfTest 6/6: done - the run should end itself with 'extracted'"));
				return false;

			default:
				return false;
			}

			return true;
		}
	};
}

void AKBGameMode::ConsoleExtractSelfTest(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}

	TSharedPtr<KBExtractCommands::FSelfTest> Test = MakeShared<KBExtractCommands::FSelfTest>();
	Test->World = World;

	UE_LOG(LogKillBugs, Display, TEXT("Extract.SelfTest: started"));

	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Test](float DeltaSeconds) -> bool
		{
			return Test->Advance(DeltaSeconds);
		}));
}

void AKBGameMode::ConsoleExtractOpenNow(const TArray<FString>& Args, UWorld* World)
{
	AKBGameMode* Mode = KBExtractCommands::ResolveGameMode(World);
	if (!Mode || !Mode->ExtractionZone)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Extract.OpenNow: no extraction zone in this world"));
		return;
	}

	auto ArgOrDefault = [&Args](int32 Index, float Fallback)
	{
		return Args.IsValidIndex(Index) ? FCString::Atof(*Args[Index]) : Fallback;
	};

	const bool bExplicitLocation = Args.Num() >= 2;
	const FVector Location = bExplicitLocation
		? FVector(ArgOrDefault(0, 0.f), ArgOrDefault(1, 0.f), 0.f)
		: Mode->SelectExtractionLocation();

	// The overrides exist so a test does not have to sit through the real 60 + 30 seconds.
	Mode->ExtractionZone->OpenZone(Location,
		ArgOrDefault(2, KBSettings().ExtractionOpenWindowSeconds),
		ArgOrDefault(3, KBSettings().ExtractionSeconds),
		KBSettings().ExtractionRadius);
}

void AKBGameMode::ConsoleExtractGather(const TArray<FString>& Args, UWorld* World)
{
	AKBGameMode* Mode = KBExtractCommands::ResolveGameMode(World);
	if (!Mode || !Mode->ExtractionZone || !Mode->ExtractionZone->IsZoneOpen())
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Extract.Gather: no zone is open"));
		return;
	}

	KBExtractCommands::MoveEveryPlayer(World, Mode->ExtractionZone->GetZoneCentre());
}

void AKBGameMode::ConsoleExtractScatter(const TArray<FString>& Args, UWorld* World)
{
	AKBGameMode* Mode = KBExtractCommands::ResolveGameMode(World);
	if (!Mode || !Mode->ExtractionZone)
	{
		return;
	}

	const float Distance = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 1500.f;
	const FVector Centre = Mode->ExtractionZone->GetZoneCentre();

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APawn* Pawn = It->Get() ? It->Get()->GetPawn() : nullptr;
		if (!Pawn)
		{
			continue;
		}

		// Straight out from the centre, so the gate closes without anybody leaving the arena.
		FVector Away = Pawn->GetActorLocation() - Centre;
		Away.Z = 0.f;
		FVector Direction = Away.GetSafeNormal();
		if (Direction.IsNearlyZero())
		{
			Direction = FVector(1.f, 0.f, 0.f);
		}

		const FVector Target = Centre + Direction * Distance;
		Pawn->SetActorLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogKillBugs, Display, TEXT("Extract: moved %s to (%.0f, %.0f)"), *GetNameSafe(Pawn),
			Target.X, Target.Y);
	}
}

void AKBGameMode::ConsoleExtractFail(const TArray<FString>& Args, UWorld* World)
{
	AKBGameMode* Mode = KBExtractCommands::ResolveGameMode(World);
	if (!Mode || !Mode->ExtractionZone)
	{
		return;
	}

	// Exercises the failure path without waiting out the window.
	Mode->ExtractionZone->CloseZone(true);
}

void AKBGameMode::ConsoleExtractSchedule(const TArray<FString>& Args, UWorld* World)
{
	AKBGameMode* Mode = KBExtractCommands::ResolveGameMode(World);
	AKBGameState* RunState = World ? World->GetGameState<AKBGameState>() : nullptr;
	if (!Mode || !RunState)
	{
		return;
	}

	const int32 WavesFromNow = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1;
	Mode->ScheduleExtractionForWave(RunState->GetWaveIndex() + WavesFromNow);
}

void AKBGameMode::ConsoleRunWipe(const TArray<FString>& Args, UWorld* World)
{
	AKBGameMode* Mode = KBExtractCommands::ResolveGameMode(World);
	if (!Mode)
	{
		return;
	}

	// Ends the run the way a team wipe does, without having to down four players first. EndRun
	// is idempotent and latches the result, so this cannot corrupt a run that already ended
	// some other way.
	Mode->EndRun(EKBRunResult::WipedOut);
}

void AKBGameMode::BeginCardDraft(int32 ForWaveIndex)
{
	AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState)
	{
		return;
	}

	// Remembered so ApplyCardChoice re-validates against the same wave the cards were rolled
	// for. GetWaveIndex() is still the PREVIOUS wave at this point, since the draft now runs
	// before the wave it is choosing for.
	DraftWaveIndex = ForWaveIndex;

	UKBContentSubsystem* Content = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UKBContentSubsystem>() : nullptr;

	if (!Content)
	{
		// Nothing to offer, so do not trap the run in an empty draft.
		const int32 WaveToStart = NextWaveIndex;
		++NextWaveIndex;
		StartWave(WaveToStart);
		return;
	}

	for (APlayerState* PlayerState : RunState->PlayerArray)
	{
		AKBPlayerState* KBPlayerState = Cast<AKBPlayerState>(PlayerState);
		if (!KBPlayerState)
		{
			continue;
		}

		TArray<TObjectPtr<UKBCardDefinition>> Choices;
		Content->RollCardChoices(*KBPlayerState, ForWaveIndex, KBSettings().CardsPerDraft, Choices);
		KBPlayerState->SetPendingCardChoices(Choices);
	}

	// The lock eats into the player's thinking time, so it is added on top rather than
	// subtracted from the draft window.
	const float Now = GetWorld()->GetTimeSeconds();
	RunState->SetDraftInputUnlockServerTime(Now + KBSettings().DraftInputLockSeconds);
	RunState->SetWavePhaseServer(EKBWavePhase::CardDraft, Now + KBSettings().DraftInputLockSeconds + KBSettings().DraftSeconds);

	UE_LOG(LogKillBugs, Display,
		TEXT("Card draft for wave %d: %.1fs input lock, then %.0fs to choose"),
		ForWaveIndex, KBSettings().DraftInputLockSeconds, KBSettings().DraftSeconds);
}

bool AKBGameMode::AnyPlayerAwaitingPick() const
{
	const AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState)
	{
		return false;
	}

	for (const APlayerState* PlayerState : RunState->PlayerArray)
	{
		const AKBPlayerState* KBPlayerState = Cast<AKBPlayerState>(PlayerState);
		if (KBPlayerState && !KBPlayerState->HasPickedCard())
		{
			return true;
		}
	}


	return false;
}

void AKBGameMode::AutoResolveRemainingPicks()
{
	AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState)
	{
		return;
	}

	for (APlayerState* PlayerState : RunState->PlayerArray)
	{
		AKBPlayerState* KBPlayerState = Cast<AKBPlayerState>(PlayerState);
		if (!KBPlayerState || KBPlayerState->HasPickedCard())
		{
			continue;
		}

		const int32 ChoiceCount = KBPlayerState->PendingCardChoices.Num();
		if (ChoiceCount <= 0)
		{
			// Nothing was on offer, so there is nothing to grant.
			KBPlayerState->MarkCardPicked();
			continue;
		}

		// A RANDOM offer, not the first one. Always granting slot 0 would quietly make the
		// first card the "default" pick and reward not choosing - and the player would never
		// notice the pattern.
		const int32 RandomIndex = FMath::RandHelper(ChoiceCount);
		UE_LOG(LogKillBugs, Display,
			TEXT("Player %d did not pick in time; granting a random card (%d of %d)"),
			KBPlayerState->GetKBPlayerIndex(), RandomIndex + 1, ChoiceCount);

		ApplyCardChoice(KBPlayerState, RandomIndex);
	}
}

bool AKBGameMode::AreAllPlayersDowned() const
{
	const AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!RunState)
	{
		return false;
	}

	int32 Count = 0;
	for (const APlayerState* PlayerState : RunState->PlayerArray)
	{
		const AKBPlayerState* KBPlayerState = Cast<AKBPlayerState>(PlayerState);
		if (!KBPlayerState)
		{
			continue;
		}

		++Count;
		if (!KBPlayerState->IsDowned())
		{
			return false;
		}
	}

	// Vacuously "all downed" with nobody in the game, which is not a run over - that is just
	// a server that nobody has joined yet.
	return Count > 0;
}

bool AKBGameMode::ApplyCardChoice(AKBPlayerState* PlayerState, int32 ChoiceIndex)
{
	const AKBGameState* RunState = GetGameState<AKBGameState>();
	if (!PlayerState || !RunState || PlayerState->HasPickedCard())
	{
		return false;
	}

	if (!PlayerState->PendingCardChoices.IsValidIndex(ChoiceIndex))
	{
		return false;
	}

	UKBCardDefinition* Card = PlayerState->PendingCardChoices[ChoiceIndex];
	UKBContentSubsystem* Content = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UKBContentSubsystem>() : nullptr;

	if (!Card || !Content)
	{
		return false;
	}

	// Re-validate instead of trusting the roll: the client had this card on screen for the
	// whole draft window, and a disconnect or a death could have made it stale by now.
	// Validated against the wave the draft was rolled for, not the currently-running one.
	if (!Content->IsCardEligible(*PlayerState, *Card, DraftWaveIndex))
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Rejected stale card choice %d from player %d"),
			ChoiceIndex, PlayerState->GetKBPlayerIndex());
		PlayerState->MarkCardPicked();
		return false;
	}

	ApplyCardEffect(PlayerState, *Card);
	PlayerState->RecordTakenCard(Card);
	PlayerState->MarkCardPicked();
	return true;
}

void AKBGameMode::ApplyCardEffect(AKBPlayerState* PlayerState, const UKBCardDefinition& Card)
{
	UKBWeaponInventoryComponent* Inventory = PlayerState->GetWeaponInventory();
	APawn* Pawn = PlayerState->GetPawn();

	switch (Card.Effect)
	{
	case EKBCardEffect::AddWeapon:
		if (Inventory)
		{
			Inventory->GrantWeapon(Card.Weapon);
		}
		break;

	case EKBCardEffect::UpgradeWeapon:
		// GrantWeapon levels a weapon the player already owns, so one entry point covers both.
		if (Inventory)
		{
			Inventory->GrantWeapon(Card.RequiresWeapon);
		}
		break;

	case EKBCardEffect::StatBoost:
		if (UKBStatSheetComponent* Stats = Pawn ? Pawn->FindComponentByClass<UKBStatSheetComponent>() : nullptr)
		{
			Stats->ApplyMods(Card.StatMods);
		}
		break;

	case EKBCardEffect::Heal:
		if (UKBStatSheetComponent* Stats = Pawn ? Pawn->FindComponentByClass<UKBStatSheetComponent>() : nullptr)
		{
			// HealAmount <= 0 means "to full", which is what the authored heal cards use.
			Stats->Heal(Card.HealAmount);
		}
		break;
	}

	UE_LOG(LogKillBugs, Display, TEXT("Player %d took '%s'"),
		PlayerState->GetKBPlayerIndex(), *Card.Title.ToString());
}

void AKBGameMode::TickSpawning(float DeltaSeconds, float Rate, int32 MaxAliveOverride)
{
	if (!EnemyDirector || Rate <= 0.f)
	{
		return;
	}

	// Accumulate fractional bugs rather than spawning on a fixed tick, so the rate is exactly
	// what the designer typed regardless of frame rate.
	SpawnAccumulator += DeltaSeconds * Rate;
	if (SpawnAccumulator < 1.f)
	{
		return;
	}

	// Extraction raises the ceiling as well as the rate: at the normal wave cap the flood would
	// stall the moment it got going. The director clamps everything against MaxEnemies regardless,
	// so a too-high value here is harmless rather than an overrun.
	const int32 MaxAlive = MaxAliveOverride > 0 ? MaxAliveOverride : KBSettings().MaxAliveDuringWave;

	if (EnemyDirector->GetEnemyCount() >= MaxAlive)
	{
		// At the ceiling. Drop the accumulated credit rather than banking it, or the moment a
		// player kills one bug a whole backlog would land at once.
		SpawnAccumulator = 0.f;
		return;
	}

	const int32 Batch = FMath::Clamp(FMath::FloorToInt(SpawnAccumulator), 1, KBSettings().SpawnBatchSize);
	SpawnAccumulator -= Batch;

	EnemyDirector->SpawnBatchOffscreen(Batch, KBSettings().MinSpawnDistance, KBSettings().MaxSpawnDistance);
}

void AKBGameMode::PostLogin(APlayerController* NewPlayer)
{
	// NOTE: PostLogin, not DispatchPostLogin - the latter is UE_DEPRECATED(5.6).
	Super::PostLogin(NewPlayer);

	if (AKBPlayerState* KBPlayerState = NewPlayer ? NewPlayer->GetPlayerState<AKBPlayerState>() : nullptr)
	{
		KBPlayerState->SetKBPlayerIndex(NextPlayerIndex++);
	}

	// Restart the warmup for a newcomer.
	//
	// A client in its own process takes real time to launch and load the arena. If the warmup
	// clock keeps running through that, the first card draft lands seconds after they connect
	// and their window has not settled yet, so they never see the cards - the draft resolves
	// for them unseen. Restarting the clock gives every player the same run-up, whenever they
	// arrive.
	if (AKBGameState* RunState = GetGameState<AKBGameState>())
	{
		if (RunState->GetWavePhase() == EKBWavePhase::Warmup)
		{
			RunState->SetWavePhaseServer(EKBWavePhase::Warmup,
				GetWorld()->GetTimeSeconds() + KBSettings().WarmupSeconds);

			UE_LOG(LogKillBugs, Display,
				TEXT("Warmup restarted: a player joined, giving them %.0fs to load"),
				KBSettings().WarmupSeconds);
		}
	}
}

void AKBGameMode::Logout(AController* Exiting)
{
	// Nothing to do here, and that is a conclusion rather than an omission.
	//
	// An earlier note claimed this had to decrement the card-draft tally and destroy the pawn.
	// It does not: APlayerState::Destroyed() calls GameState->RemovePlayerState() on the way
	// out (Engine/Private/PlayerState.cpp:209), so a departing player leaves PlayerArray
	// entirely. Every gate
	// in this class reads PlayerArray - AnyPlayerAwaitingPick, AreAllPlayersDowned,
	// ReviveDownedPlayers - so a waver left open by someone who quits closes as soon as the
	// engine tears their state down, and the draft's timeout covers the rest. The pawn goes
	// with the controller the engine destroys alongside it, so the swarm has nothing dangling
	// to chase.
	//
	// The one thing that IS worth saying out loud: a client leaving mid-run shrinks the team
	// their surviving teammates are playing in, and nothing here pauses the wave for it. That
	// is the intended behaviour for a co-op run, not a gap to be filled.
	UE_LOG(LogKillBugs, Display, TEXT("Player left the run: %s"),
		Exiting && Exiting->PlayerState ? *Exiting->PlayerState->GetPlayerName() : TEXT("<unknown>"));

	Super::Logout(Exiting);
}

// ---- KB.Extract.* ----------------------------------------------------------------------------
//
// The command-line equivalent of what a player does with their feet, following the KB.Lobby.*
// precedent: a headless process has no HUD and no input, so the only way to exercise extraction
// is to drive it the same way the buttons drive the lobby.

static FAutoConsoleCommandWithWorldAndArgs KBConsoleExtractOpenNow(
	TEXT("KB.Extract.OpenNow"),
	TEXT("KB.Extract.OpenNow [x] [y] [windowSeconds] [progressSeconds] - open the zone now. "
	     "Without x/y it uses the same location the schedule would pick. The time overrides exist "
	     "so a test need not sit through the real window and hold."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AKBGameMode::ConsoleExtractOpenNow));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleExtractGather(
	TEXT("KB.Extract.Gather"),
	TEXT("KB.Extract.Gather - teleport every player onto the zone centre, satisfying the gate."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AKBGameMode::ConsoleExtractGather));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleExtractScatter(
	TEXT("KB.Extract.Scatter"),
	TEXT("KB.Extract.Scatter [distance] - move every player that far out from the zone centre "
	     "(default 1500), closing the gate."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AKBGameMode::ConsoleExtractScatter));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleExtractFail(
	TEXT("KB.Extract.Fail"),
	TEXT("KB.Extract.Fail - expire the zone now, exercising the failure path without the wait. "
	     "The run must CONTINUE."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AKBGameMode::ConsoleExtractFail));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleExtractSchedule(
	TEXT("KB.Extract.Schedule"),
	TEXT("KB.Extract.Schedule [wavesFromNow] - put the zone on the schedule (default: next wave)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AKBGameMode::ConsoleExtractSchedule));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleRunWipe(
	TEXT("KB.Run.Wipe"),
	TEXT("KB.Run.Wipe - end the run as a team wipe, without downing anybody. For reaching the "
	     "wipe branch (materials are lost, not banked) in a headless test."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AKBGameMode::ConsoleRunWipe));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleExtractSelfTest(
	TEXT("KB.Extract.SelfTest"),
	TEXT("KB.Extract.SelfTest - walk the whole extraction state machine on a timer: open far and "
	     "let the window expire, reopen under the team and watch it freeze and fill, scatter to "
	     "pause it, gather to finish. Run it during warmup. Read the log - each step says what it "
	     "is supposed to prove."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AKBGameMode::ConsoleExtractSelfTest));
