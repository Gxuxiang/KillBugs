#include "Extraction/KBExtractionZone.h"

#include "Core/KBGameState.h"
#include "Core/KBPlayerState.h"
#include "EngineUtils.h"
#include "Interaction/KBChannelComponent.h"
#include "KillBugs.h"
#include "Net/UnrealNetwork.h"

AKBExtractionZone::AKBExtractionZone()
{
	// PrePhysics so the window is decremented against the same frame's positions the channel will
	// judge at PostPhysics, rather than a frame that has already moved on. The gate itself is
	// asked, never re-derived, so the two can never disagree about who is inside.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	Channel = CreateDefaultSubobject<UKBChannelComponent>(TEXT("ExtractionChannel"));
	Channel->SetupAttachment(SceneRoot);

	// The extraction gate and the extraction break policy, as UKBChannelComponent documents them.
	// Progress is kept when somebody steps out because four people holding one circle is hard
	// enough; the cost of leaving is paid in the open window instead, not in lost progress.
	Channel->Gate = EKBChannelGate::AllPlayersAliveInside;
	Channel->BreakPolicy = EKBChannelBreak::Pause;

	// There is one zone for the whole team, so it must never be culled for relevancy and never go
	// dormant - a dormancy nap would hide the ring from clients mid-extraction.
	bReplicates = true;
	bAlwaysRelevant = true;
	SetReplicateMovement(false);
	SetNetDormancy(DORM_Never);
	SetNetUpdateFrequency(10.f);
}

void AKBExtractionZone::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKBExtractionZone, ZoneCentre);
	DOREPLIFETIME(AKBExtractionZone, ZoneRadius);
	DOREPLIFETIME(AKBExtractionZone, OpenWindowSeconds);
	DOREPLIFETIME(AKBExtractionZone, OpenWindowRemaining);
}

void AKBExtractionZone::OnRep_ZoneCentre()
{
	// Movement replication is off (the zone teleports between appearances, which movement
	// replication handles badly), so the centre travels as an ordinary property and is applied
	// here. The channel is attached to the root, so moving the actor moves the ring with it.
	SetActorLocation(ZoneCentre);
}

void AKBExtractionZone::OnRep_ZoneRadius()
{
	if (Channel)
	{
		Channel->Radius = ZoneRadius;
	}
}

bool AKBExtractionZone::IsZoneOpen() const
{
	return Channel && Channel->IsChannelActive();
}

void AKBExtractionZone::OpenZone(const FVector& Centre, float WindowSeconds, float ProgressSeconds,
                                 float Radius)
{
	if (!HasAuthority() || !Channel)
	{
		return;
	}

	// Must go through a close first: SetChannelActive only clears Progress on a real edge, so
	// reopening an already-active zone would hand the team the previous attempt's progress.
	if (Channel->IsChannelActive())
	{
		return;
	}

	ZoneRadius = FMath::Max(Radius, 1.f);
	Channel->Radius = ZoneRadius;
	Channel->DurationSeconds = FMath::Max(ProgressSeconds, 0.1f);

	OpenWindowSeconds = FMath::Max(WindowSeconds, 0.f);
	OpenWindowRemaining = OpenWindowSeconds;
	bWindowFrozen = false;
	bStartAnnounced = false;
	LastProgressMilestone = -1;

	// On the authority the OnRep never fires, so the actor is moved here as well as on clients.
	SetActorLocation(Centre);
	ZoneCentre = Centre;

	// Everything else fails silently if this one call does not happen: the ring never appears, the
	// gate is never evaluated and the run never ends - all with no error.
	Channel->SetChannelActive(true);

	UE_LOG(LogKillBugs, Display,
		TEXT("Extraction zone opened at (%.0f, %.0f) radius %.0f, window %.1fs, progress %.1fs"),
		Centre.X, Centre.Y, ZoneRadius, OpenWindowSeconds, Channel->DurationSeconds);
}

void AKBExtractionZone::CloseZone(bool bExpired)
{
	if (!HasAuthority() || !Channel || !Channel->IsChannelActive())
	{
		return;
	}

	const float ProgressAtClose = Channel->GetProgress();

	Channel->SetChannelActive(false);
	OpenWindowRemaining = 0.f;
	bWindowFrozen = false;

	if (bExpired)
	{
		UE_LOG(LogKillBugs, Display,
			TEXT("Extraction window expired with progress %.2f - zone closed, run continues"),
			ProgressAtClose);

		// Deliberately NOT EndRun. The run is not over; the GameMode only reschedules.
		OnExtractionWindowExpired.Broadcast();
	}
}

void AKBExtractionZone::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasAuthority() || !Channel || !Channel->IsChannelActive())
	{
		return;
	}

	const AKBGameState* RunState = GetWorld() ? GetWorld()->GetGameState<AKBGameState>() : nullptr;
	if (!RunState)
	{
		return;
	}

	// A wipe mid-extraction ends the run with the zone still open. Nothing draws during the
	// summary, but leaving the channel live would keep a server tick running for a zone that no
	// longer exists as far as the player is concerned.
	if (RunState->GetWavePhase() == EKBWavePhase::RunOver)
	{
		Channel->SetChannelActive(false);
		return;
	}

	// The single definition of "everybody is in", asked rather than re-implemented.
	const bool bGateOpen = Channel->EvaluateGate();

	if (bGateOpen != bWindowFrozen)
	{
		bWindowFrozen = bGateOpen;

		if (bGateOpen)
		{
			UE_LOG(LogKillBugs, Display,
				TEXT("Extraction window frozen (all players inside) at %.1fs"), OpenWindowRemaining);

			// The team committed. Announced once per attempt so stepping out and back in is not a
			// way to farm waves.
			if (!bStartAnnounced)
			{
				bStartAnnounced = true;
				OnExtractionStarted.Broadcast();
			}
		}
		else
		{
			// The progress in brackets is the whole point of this line: it proves the timer paused
			// rather than reset.
			UE_LOG(LogKillBugs, Display,
				TEXT("Extraction window resumed at %.1fs (progress held at %.2f)"),
				OpenWindowRemaining, Channel->GetProgress());
		}
	}

	// Progress milestones. Not mechanics - a headless run cannot see the bar move, so the only way
	// to prove the timer is advancing is to write it down.
	const int32 Milestone = FMath::FloorToInt(Channel->GetProgress() * 4.f);
	if (Milestone > LastProgressMilestone)
	{
		LastProgressMilestone = Milestone;
		UE_LOG(LogKillBugs, Display, TEXT("Extraction progress %d%% (window %s at %.1fs)"),
			Milestone * 25, bGateOpen ? TEXT("frozen") : TEXT("running"), OpenWindowRemaining);
	}

	// The gate being open is the channel's business - it advances Progress itself. The window is
	// ours, and it only moves while the gate is shut.
	if (bGateOpen)
	{
		return;
	}

	OpenWindowRemaining = FMath::Max(OpenWindowRemaining - DeltaSeconds, 0.f);

	if (OpenWindowRemaining <= 0.f)
	{
		CloseZone(true);
	}
}

namespace KBExtractionDebug
{
	/**
	 * One line describing the zone, for a headless run that draws no HUD.
	 *
	 * The per-player distances are informational - the authoritative answer is the gate's, printed
	 * first - but they are what tells "nobody is in" apart from "the gate is broken".
	 */
	void LogState(const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}

		AKBExtractionZone* Zone = nullptr;
		for (TActorIterator<AKBExtractionZone> It(World); It; ++It)
		{
			Zone = *It;
			break;
		}

		if (!Zone)
		{
			UE_LOG(LogKillBugs, Display, TEXT("Extract.State: no zone in this world"));
			return;
		}

		const UKBChannelComponent* Chan = Zone->GetChannel();

		UE_LOG(LogKillBugs, Display,
			TEXT("Extract.State: %s at (%.0f, %.0f) radius %.0f | gate %s | window %.1f/%.1fs | progress %.2f advancing %s"),
			Zone->IsZoneOpen() ? TEXT("OPEN") : TEXT("closed"),
			Zone->GetZoneCentre().X, Zone->GetZoneCentre().Y, Zone->GetZoneRadius(),
			(Chan && Chan->EvaluateGate()) ? TEXT("OPEN") : TEXT("closed"),
			Zone->GetOpenWindowRemaining(), Zone->GetOpenWindowSeconds(),
			Chan ? Chan->GetProgress() : 0.f, (Chan && Chan->IsAdvancing()) ? TEXT("yes") : TEXT("no"));

		const AKBGameState* RunState = World->GetGameState<AKBGameState>();
		if (!RunState)
		{
			return;
		}

		for (const APlayerState* PS : RunState->PlayerArray)
		{
			const AKBPlayerState* KBPS = Cast<AKBPlayerState>(PS);
			const APawn* Pawn = KBPS ? KBPS->GetPawn() : nullptr;
			if (!Pawn)
			{
				UE_LOG(LogKillBugs, Display, TEXT("Extract.State:   %s has no pawn"),
					*GetNameSafe(PS));
				continue;
			}

			const float Dist = FVector::Dist2D(Zone->GetZoneCentre(), Pawn->GetActorLocation());
			UE_LOG(LogKillBugs, Display, TEXT("Extract.State:   %s at %.0f units (%s, downed %s)"),
				*GetNameSafe(PS), Dist, (Dist <= Zone->GetZoneRadius()) ? TEXT("IN") : TEXT("out"),
				(KBPS && KBPS->IsDowned()) ? TEXT("yes") : TEXT("no"));
		}
	}
}

static FAutoConsoleCommandWithWorldAndArgs KBConsoleExtractStateCommand(
	TEXT("KB.Extract.State"),
	TEXT("KB.Extract.State - log the extraction zone's two clocks and where every player is."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBExtractionDebug::LogState));
