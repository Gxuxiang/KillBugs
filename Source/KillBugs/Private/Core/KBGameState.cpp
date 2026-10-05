#include "Core/KBGameState.h"

#include "Net/UnrealNetwork.h"

AKBGameState::AKBGameState()
{
}

void AKBGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKBGameState, DraftInputUnlockServerTime);
	DOREPLIFETIME(AKBGameState, WavePhase);
	DOREPLIFETIME(AKBGameState, WaveIndex);
	DOREPLIFETIME(AKBGameState, PhaseEndServerTime);
	DOREPLIFETIME(AKBGameState, PhaseStartServerTime);
	DOREPLIFETIME(AKBGameState, Timings);
	DOREPLIFETIME(AKBGameState, RunResult);
	DOREPLIFETIME(AKBGameState, NextExtractionWaveIndex);
}

void AKBGameState::SetNextExtractionWaveIndexServer(int32 InWaveIndex)
{
	if (!HasAuthority())
	{
		return;
	}

	NextExtractionWaveIndex = InWaveIndex;
}

void AKBGameState::SetRunResultServer(EKBRunResult Result)
{
	if (!HasAuthority())
	{
		return;
	}

	// Latched on purpose - see the note in the header. A wipe checked in the same frame that the
	// team finished extracting must not turn a success into a failure.
	if (RunResult != EKBRunResult::InProgress)
	{
		return;
	}

	RunResult = Result;

	// Broadcast here as well as in the OnRep, for the same reason SetWavePhaseServer does: on a
	// listen server the authority never receives its own rep, and the host is a player whose
	// materials have to be banked like anybody else's.
	OnRunResultChanged.Broadcast(RunResult);
}

float FKBPhaseTimings::GetPhaseDuration(EKBWavePhase Phase) const
{
	switch (Phase)
	{
	case EKBWavePhase::Warmup:     return Warmup;
	case EKBWavePhase::WaveActive: return Wave;
	case EKBWavePhase::Explore:    return Explore;
	case EKBWavePhase::CardDraft:  return CardDraft;
	default:                       return 0.f;
	}
}

void AKBGameState::SetPhaseTimingsServer(const FKBPhaseTimings& InTimings)
{
	if (!HasAuthority())
	{
		return;
	}

	Timings = InTimings;
}

void AKBGameState::OnRep_WavePhase()
{
	OnWavePhaseChanged.Broadcast(WavePhase);
}

void AKBGameState::OnRep_RunResult()
{
	OnRunResultChanged.Broadcast(RunResult);
}

void AKBGameState::SetWavePhaseServer(EKBWavePhase NewPhase, float InPhaseEndServerTime)
{
	if (!HasAuthority())
	{
		return;
	}

	PhaseEndServerTime = InPhaseEndServerTime;

	// Recorded on every call, even when the phase itself is unchanged: the end time can move
	// (a draft ending early because everyone picked), and a stale start would make the
	// timeline's progress jump.
	PhaseStartServerTime = GetServerWorldTimeSeconds();

	if (WavePhase == NewPhase)
	{
		return;
	}

	WavePhase = NewPhase;

	// On a listen server the local player is also the server, so no OnRep will arrive for
	// this change - broadcast locally or the host's UI never hears about it.
	OnWavePhaseChanged.Broadcast(WavePhase);
}

bool AKBGameState::IsCardDraftInputLocked() const
{
	// Compared against the replicated server clock, so a client reaches the same verdict as
	// the server without needing its own timer.
	return GetServerWorldTimeSeconds() < DraftInputUnlockServerTime;
}

void AKBGameState::SetDraftInputUnlockServerTime(float InServerTime)
{
	if (!HasAuthority())
	{
		return;
	}

	DraftInputUnlockServerTime = InServerTime;
}

void AKBGameState::SetWaveIndexServer(int32 NewWaveIndex)
{
	if (!HasAuthority())
	{
		return;
	}

	WaveIndex = NewWaveIndex;
}
