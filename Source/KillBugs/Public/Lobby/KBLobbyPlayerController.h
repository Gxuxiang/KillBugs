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

	/** Server RPC. Starts the run; the GameMode ignores this unless the caller is the host. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestStart();

	/** Server RPC. Readies this player up, or takes it back. Anyone may do this for themselves. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestSetReady(bool bReady);

	/** Server RPC. Starts a run with no room involved, for solo play. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestSolo();

private:
	/** Routes a click at the lobby's buttons and server rows. */
	void HandleLobbyClick();

	/** Builds the click action and its mapping at runtime, as AKBPlayerController does. */
	void BuildRuntimeInput();

	/** Plays the pick cue locally on an action that is going to happen. */
	void PlayClickFeedback();

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ClickAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> RuntimeMappingContext;
};
