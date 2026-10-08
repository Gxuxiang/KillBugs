#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "KBLobbyPlayerController.generated.h"

class UInputAction;
class UInputMappingContext;

/**
 * Drives the lobby UI.
 *
 * Not derived from AKBPlayerController: that one exists to serve a combat pawn - it builds the
 * fire action for the pawn to bind, and its click chain is the card draft's. None of that has
 * anything to do here, and inheriting it would leave a fire action in the input stack of a
 * screen that must not fire.
 *
 * So this controller builds its own click and binds it itself. That is only unambiguous
 * because the lobby spawns no pawn (see AKBLobbyGameMode's constructor): with nobody else
 * binding the mouse button, a click can only mean the UI.
 */
UCLASS()
class KILLBUGS_API AKBLobbyPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AKBLobbyPlayerController();

	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;

	/**
	 * Only overridden to feed the host-address field.
	 *
	 * Enhanced Input has no text-entry concept, and the lobby has no UMG widget to type into -
	 * the whole screen is Canvas. So the keystrokes are taken here, while the HUD says it is
	 * typing, and passed through untouched at every other moment.
	 */
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;

	/** Server RPC. Starts the run; the GameMode ignores this unless the caller is the host. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestStart();

	/** Server RPC. Readies this player up, or takes it back. Anyone may do this for themselves. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestSetReady(bool bReady);

	/** Server RPC. Starts a run with no room involved, for solo play. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestSolo();

	/**
	 * Server RPC. Hands this player's banked profile total to the server so the lobby's fresh
	 * PlayerState can carry it.
	 *
	 * The value comes from this machine's save file, which is why it is the client that sends it
	 * rather than the server that reads it - and why the server cannot verify it. That is the
	 * accepted cost of each machine owning its own profile; see AKBPlayerState::SeedGold.
	 *
	 * Lobby-only by construction, and that is the safety property: the arena uses a different
	 * PlayerControllerClass, and travel here is non-seamless, so no instance of this class exists
	 * in the arena for a client to call this on mid-run. The server also only acts when the
	 * GameMode is the lobby's, and the seed itself latches.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerSeedGold(int32 InGold);

private:
	/** Routes a click at the lobby's buttons and server rows. */
	void HandleLobbyClick();

	/** Routes a click at a shop row. Only reached while the shop panel is open. */
	void HandleShopClick(class AKBLobbyHud* LobbyHud);

	/** Turns what was typed into a connection, or into a reason it cannot be one. */
	void CommitManualJoin(class AKBLobbyHud* LobbyHud);

	/** Puts this machine's address on the clipboard, so it can be sent to the other player. */
	void CopyLocalAddress(class AKBLobbyHud* LobbyHud);

	/** The cursor, or the origin when the mouse is not in the viewport. */
	FVector2D CursorPosition() const;

	/** Builds the click action and its mapping at runtime, as AKBPlayerController does. */
	void BuildRuntimeInput();

	/** Plays the pick cue locally on an action that is going to happen. */
	void PlayClickFeedback();

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ClickAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> RuntimeMappingContext;
};
