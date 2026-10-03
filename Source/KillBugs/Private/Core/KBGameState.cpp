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
