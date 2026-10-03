#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "KBLobbyGameState.generated.h"

/**
 * The lobby's replicated state.
 *
 * Built on plain AGameStateBase rather than AKBGameState: the run's phase clock and timings
 * mean nothing before the run starts, and carrying them would give the lobby a "现处什么阶段"
 * that is permanently lying.
 *
 * The player list needs nothing added - AGameStateBase::PlayerArray already replicates every
 * player and their name. The one thing that is NOT in there is who the host is, because on a
 * listen server only the server itself can tell which controller is local.
 */
UCLASS()
class KILLBUGS_API AKBLobbyGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** KBPlayerIndex of the host, or INDEX_NONE before the host has claimed it. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Lobby")
	int32 GetHostPlayerIndex() const { return HostPlayerIndex; }

	/** Server only; ignored on a client. */
	void SetHostPlayerIndex(int32 InHostPlayerIndex);

	/** Has this player pressed 准备? */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Lobby")
	bool IsPlayerReady(int32 KBPlayerIndex) const;

	/**
	 * True when there is at least one player and every one of them is ready.
	 *
	 * Computed here rather than on the GameMode because both ends need the same answer: the
	 * host enables 开始游戏 from it, and the server refuses to start without it. One
	 * implementation means the button and the rule cannot disagree.
	 */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Lobby")
	bool AreAllPlayersReady() const;

	/**
	 * How many players CURRENTLY in the room are ready.
	 *
	 * Counted over PlayerArray rather than read off ReadyPlayerIndices, so it cannot count
	 * somebody who has already left - which is the difference between "2/2 ready" and a room
	 * that looks stuck one short of starting.
	 */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Lobby")
	int32 ReadyCount() const;

	/** Server only. */
	void SetPlayerReady(int32 KBPlayerIndex, bool bReady);

private:
	/**
	 * An index rather than a pointer to the PlayerState.
	 *
	 * KBPlayerIndex is already replicated and stable, and every consumer of "is this me?"
	 * already has it - whereas a replicated actor reference would have to be resolved and
	 * null-checked at every use, and reads as unset while the actor is still being mapped.
	 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Lobby", meta = (AllowPrivateAccess = "true"))
	int32 HostPlayerIndex = INDEX_NONE;

	/**
	 * KBPlayerIndex of every player who has pressed 准备.
	 *
	 * A list on the GameState rather than a bReady flag on AKBPlayerState: ready is a lobby
	 * concept, and AKBPlayerState is shared with the run, where the flag would be dead weight
	 * that every reader has to know to ignore.
	 *
	 * Entries for players who have left are left in place on purpose - AreAllPlayersReady only
	 * ever asks about players currently in PlayerArray, so a stale index cannot hold the room
	 * hostage waiting for a press that will never come. At four players the litter is nothing.
	 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Lobby", meta = (AllowPrivateAccess = "true"))
	TArray<int32> ReadyPlayerIndices;
};
