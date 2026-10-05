#include "Lobby/KBLobbyGameMode.h"

#include "Containers/Ticker.h"
#include "Core/KBPlayerState.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "KillBugs.h"
#include "Lobby/KBLobbyGameState.h"
#include "Lobby/KBLobbyPlayerController.h"
#include "Net/KBSessionSubsystem.h"
#include "TimerManager.h"
#include "UI/KBLobbyHud.h"

AKBLobbyGameMode::AKBLobbyGameMode()
{
	GameStateClass = AKBLobbyGameState::StaticClass();
	PlayerStateClass = AKBPlayerState::StaticClass();
	PlayerControllerClass = AKBLobbyPlayerController::StaticClass();
	HUDClass = AKBLobbyHud::StaticClass();

	// No combat pawn, and no spectator pawn either.
	//
	// The combat pawn brings the camera, the weapon inventory, the stat sheet and the
	// click-to-fire binding - none of which belong in a lobby, and the fire binding in
	// particular would swallow the clicks the lobby buttons need. With no pawn at all there is
	// no fire binding in existence, so the controller's own click handling is unambiguous.
	//
	// A spectator pawn would only add a free-flying camera; the lobby HUD paints over the whole
	// screen anyway, so there is nothing worth flying it around.
	DefaultPawnClass = nullptr;
	bStartPlayersAsSpectators = false;
}

void AKBLobbyGameMode::PostLogin(APlayerController* NewPlayer)
{
	// PostLogin, not DispatchPostLogin: the latter is UE_DEPRECATED(5.6) and forwards here.
	Super::PostLogin(NewPlayer);

	AKBPlayerState* PlayerState = NewPlayer ? NewPlayer->GetPlayerState<AKBPlayerState>() : nullptr;
	if (!PlayerState)
	{
		return;
	}

	// The run assigns its own indices when the arena loads; these exist so the lobby list has a
	// stable sort key and so "the host" can be named as a number that both ends already agree on.
	PlayerState->SetKBPlayerIndex(NextPlayerIndex++);
	ClaimHostIfUnclaimed(PlayerState);

	UE_LOG(LogKillBugs, Display, TEXT("Lobby: %s joined (index %d, %d player(s))"),
		*PlayerState->GetPlayerName(), PlayerState->GetKBPlayerIndex(), GetNumPlayers());
}

void AKBLobbyGameMode::Logout(AController* Exiting)
{
	const AKBPlayerState* PlayerState = Exiting ? Exiting->GetPlayerState<AKBPlayerState>() : nullptr;
	if (PlayerState)
	{
		UE_LOG(LogKillBugs, Display, TEXT("Lobby: %s left"), *PlayerState->GetPlayerName());
	}

	// No host handover. The host is the listen server - they created the session and everyone
	// else connected to them - so if the host leaves, this process is going away and there is
	// nobody left to hand the role to. Players who merely disconnect are just players.
	Super::Logout(Exiting);
}

void AKBLobbyGameMode::ClaimHostIfUnclaimed(AKBPlayerState* NewPlayerState)
{
	AKBLobbyGameState* LobbyState = GetGameState<AKBLobbyGameState>();
	if (!LobbyState || !NewPlayerState || LobbyState->GetHostPlayerIndex() != INDEX_NONE)
	{
		return;
	}

	LobbyState->SetHostPlayerIndex(NewPlayerState->GetKBPlayerIndex());

	UE_LOG(LogKillBugs, Display, TEXT("Lobby: %s is the host"), *NewPlayerState->GetPlayerName());
}

bool AKBLobbyGameMode::RequestStartGame(AKBPlayerState* Requester)
{
	if (!HasAuthority() || !Requester)
	{
		return false;
	}

	const AKBLobbyGameState* LobbyState = GetGameState<AKBLobbyGameState>();
	if (!LobbyState || LobbyState->GetHostPlayerIndex() != Requester->GetKBPlayerIndex())
	{
		// Only the host starts the run. A client that asks anyway is ignored rather than
		// kicked: the request can only come from the lobby UI, so this is a stale click or a
		// tampered client, and neither is worth disturbing the other players' game over.
		UE_LOG(LogKillBugs, Warning, TEXT("Lobby: non-host asked to start; ignored"));
		return false;
	}

	// Checked here as well as on the host's screen. The button is drawn from the same
	// GameState this reads, so the two agree by construction - but a client can send the RPC
	// whether or not its button was enabled, and a run that starts over somebody who is still
	// choosing cards is exactly the race the ready-up exists to prevent.
	if (!LobbyState->AreAllPlayersReady())
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Lobby: start refused - not every player is ready"));
		return false;
	}

	UE_LOG(LogKillBugs, Display, TEXT("Lobby: starting the run with %d player(s)"), GetNumPlayers());

	// Deferred, not travelled here - see StartTravelDelaySeconds.
	BeginStart(KBTravel::ToArenaAsListenServer());
	return true;
}

void AKBLobbyGameMode::BeginStart(const FString& TravelURL)
{
	UWorld* World = GetWorld();
	if (!World || PendingTravelURL.Len() > 0)
	{
		// Already committed. A second press inside the delay window must not book a second
		// travel - ServerTravel re-entered while the first is in flight is not something to
		// find out about empirically.
		return;
	}

	if (AKBLobbyGameState* LobbyState = GetGameState<AKBLobbyGameState>())
	{
		// Told BEFORE the wait starts, so every screen has switched to the loading screen by the
		// time the map actually changes. This is the whole reason the travel is deferred.
		LobbyState->SetStarting(true);
	}

	PendingTravelURL = TravelURL;

	World->GetTimerManager().SetTimer(
		StartTravelTimer, this, &AKBLobbyGameMode::DoStartTravel, StartTravelDelaySeconds, false);
}

void AKBLobbyGameMode::DoStartTravel()
{
	if (PendingTravelURL.Len() == 0)
	{
		return;
	}

	UE_LOG(LogKillBugs, Display, TEXT("Lobby: travelling to %s"), *PendingTravelURL);

	// Non-seamless travel, and that is fine here: ProcessServerTravel issues a ClientTravel to
	// every connected controller, so the others follow. bUseSeamlessTravel stays false because
	// the arena has nothing from the lobby worth carrying across.
	//
	// The last frame each machine drew before this is the loading screen, and it stays on screen
	// (the engine draws nothing of its own - TransitionMap is unset) until the arena's first
	// frame. That freeze is the loading screen doing its job.
	GetWorld()->ServerTravel(PendingTravelURL);
}

bool AKBLobbyGameMode::RequestSetReady(AKBPlayerState* Requester, bool bReady)
{
	if (!HasAuthority() || !Requester)
	{
		return false;
	}

	AKBLobbyGameState* LobbyState = GetGameState<AKBLobbyGameState>();
	if (!LobbyState)
	{
		return false;
	}

	const int32 PlayerIndex = Requester->GetKBPlayerIndex();
	if (PlayerIndex == INDEX_NONE)
	{
		// PostLogin has not numbered them yet, so there is nothing to mark ready.
		return false;
	}

	LobbyState->SetPlayerReady(PlayerIndex, bReady);

	UE_LOG(LogKillBugs, Display, TEXT("Lobby: %s is %s (%d/%d ready)"),
		*Requester->GetPlayerName(), bReady ? TEXT("ready") : TEXT("not ready"),
		LobbyState->ReadyCount(), LobbyState->PlayerArray.Num());

	return true;
}

bool AKBLobbyGameMode::AreAllPlayersReady() const
{
	const AKBLobbyGameState* LobbyState = GetGameState<AKBLobbyGameState>();
	return LobbyState && LobbyState->AreAllPlayersReady();
}

void AKBLobbyGameMode::SeedPlayerGold(AKBPlayerState* PlayerState, int32 InGold)
{
	if (!HasAuthority() || !PlayerState)
	{
		return;
	}

	// SeedGold latches after the first call, so a late or repeated seed cannot reshape a total
	// that has since been earned.
	PlayerState->SeedGold(InGold);
}

void AKBLobbyGameMode::StartSoloRun()
{
	if (!HasAuthority())
	{
		return;
	}

	// No session involved at all: a solo player is their own server already, so this is the
	// same travel the host does, minus the room everyone else would have joined.
	//
	// No ?listen, unlike the host's travel: nobody is coming, and opening a listen port for a
	// single-player run would leave the machine accepting connections it has no room for.
	UE_LOG(LogKillBugs, Display, TEXT("Lobby: starting a solo run"));

	// The same deferred path as the host's, for the same reason: solo is the case where the
	// player is staring straight at the button they just pressed, so a click that appears to do
	// nothing is at its worst here.
	BeginStart(KBTravel::ToArenaSolo());
}

// ---------------------------------------------------------------------------------------
// Debug: KB.Lobby.*
//
// A headless run draws no UI, so the lobby's buttons cannot be pressed by letting the game
// play itself - and a two-process host/join test is exactly what has to run headless on one
// machine. These call the same entry points the buttons do, so a green run here means the
// logic behind the buttons works, even though the drawing still needs a human to look at it.
// ---------------------------------------------------------------------------------------

namespace
{
	UKBSessionSubsystem* ResolveSessions(UWorld* World)
	{
		UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		return GameInstance ? GameInstance->GetSubsystem<UKBSessionSubsystem>() : nullptr;
	}

	/**
	 * The current game world, looked up fresh each time.
	 *
	 * Deliberately not the UWorld passed to the console command. These commands are all issued
	 * at startup, and the first thing one of them does is travel - which destroys the world the
	 * command was handed and builds a new one. Anything that captured that pointer would be
	 * looking at freed memory, or silently stop working, halfway through the flow it exists to
	 * drive.
	 */
	UWorld* KBResolveGameWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
			{
				if (UWorld* World = Context.World())
				{
					return World;
				}
			}
		}

		return nullptr;
	}

	/**
	 * Runs Condition every frame until it holds, then runs Action; gives up after a timeout.
	 *
	 * Debug-only plumbing, and it exists because -ExecCmds fire exactly once, right after the
	 * map loads. Everything interesting in this flow happens later than that: a LAN search
	 * answers in about five seconds, and another player arriving takes as long as they take.
	 * Without something that waits, a headless two-process run could only ever exercise the
	 * first command and never the join or the start.
	 */
	struct FKBDebugWaiter
	{
		TFunction<bool()> Condition;
		TFunction<void()> Action;
		double Deadline = 0.0;
		FString Description;
	};

	void KBWaitThen(const FString& Description, TFunction<bool()> Condition,
	                TFunction<void()> Action, float TimeoutSeconds)
	{
		TSharedPtr<FKBDebugWaiter> Waiter = MakeShared<FKBDebugWaiter>();
		Waiter->Condition = MoveTemp(Condition);
		Waiter->Action = MoveTemp(Action);
		Waiter->Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		Waiter->Description = Description;

		UE_LOG(LogKillBugs, Display, TEXT("Lobby: waiting - %s"), *Waiter->Description);

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Waiter](float) -> bool
		{
			if (FPlatformTime::Seconds() > Waiter->Deadline)
			{
				UE_LOG(LogKillBugs, Warning, TEXT("Lobby: timed out waiting - %s"),
					*Waiter->Description);
				return false;
			}

			// A travel tears the old world down and builds the new one, so there are frames with
			// no game world at all. That is not a failure - it is a frame with nothing to check.
			if (!KBResolveGameWorld())
			{
				return true;
			}

			if (Waiter->Condition())
			{
				UE_LOG(LogKillBugs, Display, TEXT("Lobby: ready - %s"), *Waiter->Description);
				Waiter->Action();
				return false;
			}

			return true;
		}));
	}
}

static void KBConsoleLobbyHost(const TArray<FString>& Args, UWorld* World)
{
	if (UKBSessionSubsystem* Sessions = ResolveSessions(World))
	{
		UE_LOG(LogKillBugs, Display, TEXT("Lobby.Host: creating a room"));
		Sessions->HostSession();
	}
}

static void KBConsoleLobbyFind(const TArray<FString>& Args, UWorld* World)
{
	if (UKBSessionSubsystem* Sessions = ResolveSessions(World))
	{
		UE_LOG(LogKillBugs, Display, TEXT("Lobby.Find: searching"));
		Sessions->FindSessions();
	}
}

static void KBConsoleLobbyJoin(const TArray<FString>& Args, UWorld* World)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Lobby.Join: expected <ip:port>"));
		return;
	}

	if (UKBSessionSubsystem* Sessions = ResolveSessions(World))
	{
		// The discovery-free path, kept deliberately: if the LAN beacon does not reach a second
		// process on the same machine, this still exercises joining, travelling and the map
		// handshake - everything after the search.
		Sessions->JoinAddress(Args[0]);
	}
}

static void KBConsoleLobbyStart(const TArray<FString>& Args, UWorld* World)
{
	AKBLobbyGameMode* LobbyGameMode = World ? World->GetAuthGameMode<AKBLobbyGameMode>() : nullptr;
	if (!LobbyGameMode)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Lobby.Start: this world is not running the lobby game mode"));
		return;
	}

	// Optional: wait for this many players before starting. A host that starts with nobody in
	// the room is a legitimate thing to do by hand, but a two-process test needs the start to
	// happen AFTER the other process has joined, and there is no way to type a command into a
	// headless run.
	const int32 MinPlayers = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1;

	auto StartNow = []()
	{
		UWorld* LiveWorld = KBResolveGameWorld();
		AKBLobbyGameMode* LiveGameMode =
			LiveWorld ? LiveWorld->GetAuthGameMode<AKBLobbyGameMode>() : nullptr;
		if (!LiveGameMode)
		{
			return;
		}

		// The host is looked up and passed in rather than started unconditionally, so this goes
		// through the same "only the host may start" check the button does. A command that
		// bypassed it would be testing a path no player can take.
		AKBPlayerState* Host = nullptr;
		if (const AKBLobbyGameState* LobbyState = LiveWorld->GetGameState<AKBLobbyGameState>())
		{
			for (APlayerState* State : LobbyState->PlayerArray)
			{
				if (AKBPlayerState* KBState = Cast<AKBPlayerState>(State))
				{
					if (KBState->GetKBPlayerIndex() == LobbyState->GetHostPlayerIndex())
					{
						Host = KBState;
						break;
					}
				}
			}
		}

		if (!LiveGameMode->RequestStartGame(Host))
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Lobby.Start: refused - no host in this room"));
		}
	};

	if (MinPlayers <= 1)
	{
		StartNow();
		return;
	}

	// Waits for the room to actually be startable, not just populated: the GameMode refuses to
	// start unless everyone has readied, so a command that fired on player count alone would
	// just be rejected.
	KBWaitThen(FString::Printf(TEXT("%d player(s) in the room, all ready"), MinPlayers),
		[MinPlayers]() -> bool
		{
			const UWorld* LiveWorld = KBResolveGameWorld();
			const AKBLobbyGameState* LobbyState = LiveWorld
				? LiveWorld->GetGameState<AKBLobbyGameState>() : nullptr;
			return LobbyState &&
				LobbyState->PlayerArray.Num() >= MinPlayers &&
				LobbyState->AreAllPlayersReady();
		},
		MoveTemp(StartNow), 60.f);
}

static void KBConsoleLobbyReady(const TArray<FString>& Args, UWorld* World)
{
	// Toggles, exactly as the button does. An argument of 0 un-readies.
	const bool bWantReady = Args.Num() == 0 || FCString::Atoi(*Args[0]) != 0;

	// Waits for this machine to be part of somebody's game before pressing it. The ready flag
	// lives on the HOST's GameState and the travel into the room builds a fresh one, so a press
	// that landed before the join would be thrown away a frame later - which would look like
	// "the ready button does nothing" rather than like a race.
	KBWaitThen(TEXT("a connection to a host"),
		[]() -> bool
		{
			const UWorld* LiveWorld = KBResolveGameWorld();
			if (!LiveWorld)
			{
				return false;
			}

			const ENetMode Mode = LiveWorld->GetNetMode();
			const bool bHosting = (Mode == NM_ListenServer);
			const bool bJoining = (Mode == NM_Client);

			if (!bHosting && !bJoining)
			{
				return false; // standalone: not in anybody's room yet, including our own
			}

			for (FConstPlayerControllerIterator It = LiveWorld->GetPlayerControllerIterator(); It; ++It)
			{
				const APlayerController* Controller = It->Get();
				if (!Controller || !Controller->IsLocalController())
				{
					continue;
				}

				// Net mode alone is NOT enough on a client, and this cost a whole test run.
				// ClientTravel flips the mode to NM_Client the moment the address is queued
				// (UWorld::AttemptDeriveFromURL derives it from the pending URL), which is a
				// frame or two BEFORE the server is reached and before the host's world
				// replaces this one. Acting then readies up in the OLD world, and the travel
				// that follows builds a fresh GameState with the flag thrown away - so the
				// client arrived un-ready and the host waited out its whole timeout.
				//
				// A real UNetConnection only exists once the join has actually landed. The
				// listen server's own controller has none, hence the role split.
				return bHosting || Controller->GetNetConnection() != nullptr;
			}

			return false;
		},
		[bWantReady]()
		{
			UWorld* LiveWorld = KBResolveGameWorld();
			if (!LiveWorld)
			{
				return;
			}

			for (FConstPlayerControllerIterator It = LiveWorld->GetPlayerControllerIterator(); It; ++It)
			{
				if (AKBLobbyPlayerController* Controller =
					Cast<AKBLobbyPlayerController>(It->Get()))
				{
					if (Controller->IsLocalController())
					{
						// Routed through the RPC the button uses, so this exercises the real
						// path including the server-side "you may only ready yourself" check.
						Controller->ServerRequestSetReady(bWantReady);
						return;
					}
				}
			}
		}, 30.f);
}

static void KBConsoleLobbyJoinFound(const TArray<FString>& Args, UWorld* World)
{
	UKBSessionSubsystem* Sessions = ResolveSessions(World);
	if (!Sessions)
	{
		UE_LOG(LogKillBugs, Warning, TEXT("Lobby.JoinFound: no session subsystem"));
		return;
	}

	const int32 Index = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;

	// Search, then join the first result: one command that exercises the whole discovery path,
	// which is the half of the flow the button-driven UI cannot reach headlessly.
	Sessions->FindSessions();

	KBWaitThen(FString::Printf(TEXT("a LAN search result at index %d"), Index),
		[Index]() -> bool
		{
			UKBSessionSubsystem* LiveSessions = ResolveSessions(KBResolveGameWorld());
			return LiveSessions && LiveSessions->GetSearchResults().IsValidIndex(Index);
		},
		[Index]()
		{
			if (UKBSessionSubsystem* LiveSessions = ResolveSessions(KBResolveGameWorld()))
			{
				LiveSessions->JoinSession(LiveSessions->GetSearchResults()[Index].ResultIndex);
			}
		}, 20.f);
}

static void KBConsoleLobbySolo(const TArray<FString>& Args, UWorld* World)
{
	if (AKBLobbyGameMode* LobbyGameMode = World ? World->GetAuthGameMode<AKBLobbyGameMode>() : nullptr)
	{
		LobbyGameMode->StartSoloRun();
	}
}

static FAutoConsoleCommandWithWorldAndArgs KBConsoleLobbyHostCommand(
	TEXT("KB.Lobby.Host"),
	TEXT("KB.Lobby.Host - create a LAN room and become the listen server."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsoleLobbyHost));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleLobbyFindCommand(
	TEXT("KB.Lobby.Find"),
	TEXT("KB.Lobby.Find - search the LAN for rooms."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsoleLobbyFind));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleLobbyJoinCommand(
	TEXT("KB.Lobby.Join"),
	TEXT("KB.Lobby.Join <ip:port> - connect directly, skipping discovery."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsoleLobbyJoin));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleLobbyStartCommand(
	TEXT("KB.Lobby.Start"),
	TEXT("KB.Lobby.Start [minPlayers] - start the run as the host's button does. Waits for "
	     "minPlayers players AND everyone ready, since the GameMode refuses to start otherwise."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsoleLobbyStart));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleLobbyReadyCommand(
	TEXT("KB.Lobby.Ready"),
	TEXT("KB.Lobby.Ready [0] - ready up (or un-ready with 0), as the button does."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsoleLobbyReady));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleLobbyJoinFoundCommand(
	TEXT("KB.Lobby.JoinFound"),
	TEXT("KB.Lobby.JoinFound [index] - search the LAN, then join that result."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsoleLobbyJoinFound));

static FAutoConsoleCommandWithWorldAndArgs KBConsoleLobbySoloCommand(
	TEXT("KB.Lobby.Solo"),
	TEXT("KB.Lobby.Solo - travel straight to the arena with no room."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&KBConsoleLobbySolo));
