#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "KBLobbyGameMode.generated.h"

class AKBPlayerState;

/**
 * The lobby: gather players, then hand them to the arena.
 *
 * Deliberately NOT derived from AKBGameMode. That one owns the run - it spawns the swarm and
 * the projectile director in BeginPlay and drives the wave clock from Tick. Nothing here wants
 * any of that, so inheriting would mean overriding nearly everything to switch it off, and
 * every future change to the wave machine would have to be re-checked against a mode that
 * never uses it.
 *
 * It does reuse AKBPlayerState: the lobby needs the same stable per-player index and player
 * name the run uses.
 *
 * This class is selected by the lobby map's World Settings, not by the project's
 * GlobalDefaultGameMode - the arena keeps running AKBGameMode. See Tools/kb_setup_lobby.py.
 */
UCLASS()
class KILLBUGS_API AKBLobbyGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AKBLobbyGameMode();

	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	/**
	 * Server only. Moves everyone to the arena, but only if BOTH the caller is the host and
	 * every player in the room has readied up.
	 *
	 * The all-ready half is checked here and not only on the host's screen, so the rule the
	 * button shows is the rule the server enforces - a host cannot be talked into starting
	 * over a player who is still deciding, and the button and the rule cannot drift apart.
	 *
	 * Returns whether the run actually started, so the caller can tell a stale click from a
	 * real one instead of guessing.
	 */
	bool RequestStartGame(AKBPlayerState* Requester);

	/** Server only. Anyone may ready up for themselves, and un-ready just as freely. */
	bool RequestSetReady(AKBPlayerState* Requester, bool bReady);

	/** Server only. The GameState's answer, for callers that hold the GameMode not the state. */
	bool AreAllPlayersReady() const;

	/** Server only: starts the run without a session, for solo play and for testing. */
	void StartSoloRun();

protected:
	/** Monotonic counter feeding AKBPlayerState::KBPlayerIndex, as in the arena's GameMode. */
	int32 NextPlayerIndex = 0;

	/** Server only: the first player to arrive becomes the host. */
	void ClaimHostIfUnclaimed(AKBPlayerState* NewPlayerState);
};
