#include "Core/KBGameMode.h"

#include "Core/KBCharacter.h"
#include "Core/KBGameState.h"
#include "Core/KBPlayerController.h"
#include "Combat/KBProjectileDirector.h"
#include "Combat/KBStatSheetComponent.h"
#include "Combat/KBWeaponInventoryComponent.h"
#include "Core/KBPlayerState.h"
#include "Data/KBCardDefinition.h"
#include "Data/KBContentSubsystem.h"
#include "GameFramework/Pawn.h"
#include "KillBugs.h"
#include "KBGameSettings.h"
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
		// A trickle only. The real content of this phase is whatever survived the wave.
		TickSpawning(DeltaSeconds, KBSettings().ExploreAmbientSpawnRate);
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

	RunState->SetWavePhaseServer(EKBWavePhase::Explore,
		GetWorld()->GetTimeSeconds() + KBSettings().ExploreSeconds);

	UE_LOG(LogKillBugs, Display,
		TEXT("Explore: %.0fs, %d leftover(s) at %.1fx damage taken / %.2fx speed"),
		KBSettings().ExploreSeconds, EnemyDirector->GetEnemyCount(),
		KBSettings().ExploreEnemyDamageTakenScale, KBSettings().ExploreEnemySpeedScale);
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

void AKBGameMode::TickSpawning(float DeltaSeconds, float Rate)
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

	if (EnemyDirector->GetEnemyCount() >= KBSettings().MaxAliveDuringWave)
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
