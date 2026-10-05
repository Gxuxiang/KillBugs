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

	/**
	 * Server only. Puts a player's banked total onto their fresh lobby PlayerState.
	 *
	 * Reached from AKBLobbyPlayerController::ServerSeedGold, which is the client's own claim about
	 * a file only it can read. Nothing here validates that claim - see AKBPlayerState::SeedGold
	 * for why that is the accepted shape rather than a gap.
	 */
	void SeedPlayerGold(AKBPlayerState* PlayerState, int32 InGold);

protected:
	/** Monotonic counter feeding AKBPlayerState::KBPlayerIndex, as in the arena's GameMode. */
	int32 NextPlayerIndex = 0;

	/** Server only: the first player to arrive becomes the host. */
	void ClaimHostIfUnclaimed(AKBPlayerState* NewPlayerState);

	/**
	 * How long the lobby is held up before the map change, so the loading screen is actually
	 * seen.
	 *
	 * THIS DELAY IS THE FEATURE, not padding. The start used to call ServerTravel in the same
	 * frame it handled the request, which tore the world down before a single frame could be
	 * drawn: the click looked broken on every machine, and remote clients were never even told
	 * it had been pressed. Setting a flag and travelling in the same breath does not fix that
	 * either - the flag needs a net update to reach anyone, and there is no update after the
	 * world is gone.
	 *
	 * Half a second is several updates at any sane lobby tick rate, and is far below the time the
	 * arena map takes to load, so it costs nothing perceptible.
	 *
	 * A plain constant rather than a UPROPERTY: this class has no Blueprint subclass, so an
	 * EditDefaultsOnly property here would be unreachable and would only look tunable - the same
	 * conclusion AKBProjectileDirector and AKBLobbyHud reached.
	 */
	static constexpr float StartTravelDelaySeconds = 0.5f;

	/** Marks the lobby as starting and books the travel. Server only. */
	void BeginStart(const FString& TravelURL);

	/** The booked travel, run once the delay above has elapsed. */
	void DoStartTravel();

	/** Destination booked by BeginStart. Empty means no travel is pending. */
	FString PendingTravelURL;

	FTimerHandle StartTravelTimer;
};
