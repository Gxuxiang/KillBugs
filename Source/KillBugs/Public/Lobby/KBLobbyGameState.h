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

	/**
	 * True from the moment the run is committed to until the lobby map is torn down.
	 *
	 * The HUD swaps the whole lobby for a loading screen on this. Without it the click produced
	 * no visible change at all on ANY machine: the server called ServerTravel in the same frame
	 * it handled the request, so the world was already being torn down before a frame could be
	 * drawn - the host saw the lobby freeze mid-click, and everyone else saw nothing happen and
	 * no reason given.
	 *
	 * Replicated rather than local, because the host is the only one who can press the button and
	 * everyone else deserves to be told why the lobby just disappeared.
	 */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Lobby")
	bool IsStarting() const { return bStarting; }

	/** Server only. */
	void SetStarting(bool bInStarting);

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

	/**
	 * Latched, never cleared. The lobby exists to reach the arena and has no way back, so once
	 * this is set the only thing left that can happen is the map change.
	 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Lobby", meta = (AllowPrivateAccess = "true"))
	bool bStarting = false;
};
