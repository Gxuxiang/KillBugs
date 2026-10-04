#include "Interaction/KBChannelComponent.h"

#include "Core/KBGameState.h"
#include "Core/KBPlayerState.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "KillBugs.h"
#include "Net/UnrealNetwork.h"

UKBChannelComponent::UKBChannelComponent()
{
	PrimaryComponentTick.bCanEverTick = true;

	// After the movement that put the players where they are, so "is anybody inside" is asked
	// about the positions the player is actually looking at rather than last frame's.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

	// Replicated so every client can draw the ring and the progress; the LOGIC is server-only
	// (see TickComponent), because two machines deciding independently when a revive finished
	// would be two machines disagreeing about who is standing up.
	SetIsReplicatedByDefault(true);
}

void UKBChannelComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UKBChannelComponent, bActive);
	DOREPLIFETIME(UKBChannelComponent, Progress);
	DOREPLIFETIME(UKBChannelComponent, bAdvancing);
}

void UKBChannelComponent::OnRep_Active()
{
	// Client side only - the server sets the flag directly and never takes this path.
	//
	// If a client's log never shows this line, the flag did not replicate and the ring was never
	// going to appear no matter what the HUD does. If it does show and the ring still is not
	// drawn, the problem is in the HUD's search. Those are the only two possibilities and this
	// line is what separates them.
	UE_LOG(LogKillBugs, Display, TEXT("%s: channel active replicated -> %s"),
		*GetNameSafe(GetOwner()), bActive ? TEXT("ACTIVE") : TEXT("idle"));
}

void UKBChannelComponent::SetChannelActive(bool bInActive)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	if (bActive == bInActive)
	{
		return;
	}

	bActive = bInActive;

	// Cleared on BOTH edges. Switching a zone off and on again should start over: the progress
	// belonged to a set of players who have since moved, and resuming it would hand the next
	// person a head start they did not earn.
	Progress = 0.f;
	bAdvancing = false;

	// Logged because activation is rare (once per knockdown, once per extraction zone opening)
	// and because this whole class fails SILENTLY otherwise: a zone that never ticks, a gate
	// that is never satisfied and a channel that completes with nothing bound to it all look
	// identical from the outside - like nothing happening.
	UE_LOG(LogKillBugs, Display,
		TEXT("%s: channel %s (radius %.0f, %.1fs, gate %d, break %d)"),
		*GetNameSafe(GetOwner()), bActive ? TEXT("ACTIVE") : TEXT("idle"),
		Radius, DurationSeconds, static_cast<int32>(Gate), static_cast<int32>(BreakPolicy));

	if (bActive)
	{
		LogWhoIsInRange();
	}
}

void UKBChannelComponent::LogWhoIsInRange() const
{
	const UWorld* World = GetWorld();
	const AKBGameState* RunState = World ? World->GetGameState<AKBGameState>() : nullptr;
	if (!RunState)
	{
		UE_LOG(LogKillBugs, Display, TEXT("%s:   no game state to measure against"),
			*GetNameSafe(GetOwner()));
		return;
	}

	const FVector Here = GetComponentLocation();

	// The same "whose zone is this" lookup EvaluateGate uses, so the log and the gate can never
	// disagree about which player is the owner.
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	const AKBPlayerState* OwnerState =
		OwnerPawn ? OwnerPawn->GetPlayerState<AKBPlayerState>() : nullptr;

	// One line per player, at activation only.
	//
	// This exists because the failure it describes is otherwise INVISIBLE: a zone that opens and
	// a gate that is never satisfied produces no log at all, and reads exactly like a channel
	// that never ticked. The distance is the whole question - a teammate 300 units away and a
	// teammate with no pawn both close the gate, and only this distinguishes them.
	for (const APlayerState* PlayerState : RunState->PlayerArray)
	{
		const AKBPlayerState* KBPlayerState = Cast<AKBPlayerState>(PlayerState);
		if (!KBPlayerState || KBPlayerState == OwnerState)
		{
			continue;
		}

		const APawn* Pawn = KBPlayerState->GetPawn();
		if (!Pawn)
		{
			UE_LOG(LogKillBugs, Display, TEXT("%s:   player %d has no pawn"),
				*GetNameSafe(GetOwner()), KBPlayerState->GetKBPlayerIndex());
			continue;
		}

		UE_LOG(LogKillBugs, Display,
			TEXT("%s:   player %d at %.0f units (%s, downed %s)"),
			*GetNameSafe(GetOwner()), KBPlayerState->GetKBPlayerIndex(),
			FVector::Dist2D(Here, Pawn->GetActorLocation()),
			FVector::Dist2D(Here, Pawn->GetActorLocation()) <= Radius ? TEXT("IN") : TEXT("out"),
			KBPlayerState->IsDowned() ? TEXT("yes") : TEXT("no"));
	}
}

bool UKBChannelComponent::IsPlayerInsideAndCounts(const AKBPlayerState& PlayerState) const
{
	// Downed players never count, for either gate that asks about "players".
	//
	// They cannot walk, so for the extraction gate this is what forces the team to pick somebody
	// up before they can leave. For the rescue gate it is the difference between reviving a
	// teammate and a pile of bodies reviving each other.
	if (PlayerState.IsDowned())
	{
		return false;
	}

	const APawn* Pawn = PlayerState.GetPawn();
	if (!Pawn)
	{
		// No pawn - logged out, or not spawned yet. Not somewhere; not inside.
		return false;
	}

	// Dist2D, not Dist. Everything in this game happens on one floor and the zones are drawn on
	// the ground, so the height difference between two players is noise. The same reasoning as
	// KBListener::HearingDistance.
	return FVector::Dist2D(GetComponentLocation(), Pawn->GetActorLocation()) <= Radius;
}

bool UKBChannelComponent::EvaluateGate() const
{
	const UWorld* World = GetWorld();
	const AKBGameState* RunState = World ? World->GetGameState<AKBGameState>() : nullptr;
	if (!RunState)
	{
		return false;
	}

	// The owner's own index, for the gate that excludes it.
	const AActor* Owner = GetOwner();
	const APawn* OwnerPawn = Cast<APawn>(Owner);
	const AKBPlayerState* OwnerState =
		OwnerPawn ? OwnerPawn->GetPlayerState<AKBPlayerState>() : nullptr;

	int32 Inside = 0;
	for (const APlayerState* PlayerState : RunState->PlayerArray)
	{
		const AKBPlayerState* KBPlayerState = Cast<AKBPlayerState>(PlayerState);
		if (!KBPlayerState)
		{
			continue;
		}

		if (Gate == EKBChannelGate::AnyOtherPlayer && KBPlayerState == OwnerState)
		{
			continue;
		}

		if (Gate == EKBChannelGate::AllPlayersAliveInside)
		{
			// The strict gate needs EVERY player accounted for, so the first one who is down or
			// outside ends it - there is nothing to count up to.
			if (!IsPlayerInsideAndCounts(*KBPlayerState))
			{
				return false;
			}
			continue;
		}

		if (IsPlayerInsideAndCounts(*KBPlayerState))
		{
			++Inside;
		}
	}

	if (Gate == EKBChannelGate::AllPlayersAliveInside)
	{
		// Vacuously satisfied with nobody in the game. That is a server nobody has joined, not a
		// team that has extracted - the same trap AreAllPlayersDowned has to avoid.
		return RunState->PlayerArray.Num() > 0;
	}

	return Inside > 0;
}

void UKBChannelComponent::Advance(float DeltaTime)
{
	const bool bGateOpen = EvaluateGate();

	// Logged on the EDGES only. A line per tick would drown the log; a line when the answer
	// changes is exactly the history of the channel, and it is the difference between "nothing
	// happened" and "the gate was never open" - which are otherwise the same to look at.
	if (bGateOpen != bAdvancing)
	{
		UE_LOG(LogKillBugs, Display, TEXT("%s: gate %s (progress %.2f)"),
			*GetNameSafe(GetOwner()), bGateOpen ? TEXT("OPEN") : TEXT("closed"), Progress);
	}

	bAdvancing = bGateOpen;

	if (!bGateOpen)
	{
		if (BreakPolicy == EKBChannelBreak::Reset)
		{
			Progress = 0.f;
		}

		// Pause simply leaves Progress where it is.
		return;
	}

	const float Duration = FMath::Max(DurationSeconds, 0.1f);
	Progress = FMath::Min(Progress + DeltaTime / Duration, 1.f);

	if (Progress >= 1.f)
	{
		// Deactivated rather than left running, so a completed channel does not fire again on the
		// next tick. Whatever the completion does is responsible for switching it back on if it
		// wants to repeat.
		bActive = false;
		bAdvancing = false;

		UE_LOG(LogKillBugs, Display, TEXT("%s: channel complete after %.1fs"),
			*GetNameSafe(GetOwner()), Duration);

		OnChannelComplete.Broadcast();
	}
}

void UKBChannelComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                        FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Server-only.
	//
	// The timer is the authority's to keep, and the results it produces - a revive, a run ending -
	// are too. A client that ran its own would be predicting a state change it has no business
	// predicting, and the two would disagree during exactly the lag that matters.
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	if (!bActive)
	{
		return;
	}

	Advance(DeltaTime);
}
