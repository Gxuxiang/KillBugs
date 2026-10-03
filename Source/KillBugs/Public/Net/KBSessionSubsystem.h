#pragma once

#include "CoreMinimal.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "KBSessionSubsystem.generated.h"

class APlayerController;

/**
 * Where the lobby is in the host / find / join sequence. Drives the button states and the one
 * status line at the bottom of the lobby, so every transition has something on screen to
 * explain it - an async step that fails silently is the hardest thing to debug in this flow.
 */
UENUM(BlueprintType)
enum class EKBOnlineState : uint8
{
	/** In the lobby, not hosting and not looking for anything. */
	Offline,
	/** CreateSession is in flight. */
	Creating,
	/** This machine is the listen server. */
	Hosting,
	/** FindSessions is in flight. */
	Searching,
	/** JoinSession is in flight. */
	Joining,
	/** Connected to somebody else's listen server. */
	InSession,
};

/**
 * One row of the lobby's server list.
 *
 * Flattened on purpose: the HUD draws plain strings and ints and never sees an online type, so
 * replacing the session backend (Null today, Steam/EOS later) cannot reach into the UI.
 */
USTRUCT(BlueprintType)
struct FKBSessionEntry
{
	GENERATED_BODY()

	/** "192.168.1.20:7777" - what a client travels to, and the row's identity. */
	UPROPERTY(BlueprintReadOnly, Category = "KillBugs|Session")
	FString Address;

	/** Likely empty: the LAN beacon does not reliably carry the host's display name. */
	UPROPERTY(BlueprintReadOnly, Category = "KillBugs|Session")
	FString HostName;

	UPROPERTY(BlueprintReadOnly, Category = "KillBugs|Session")
	int32 OpenSlots = 0;

	UPROPERTY(BlueprintReadOnly, Category = "KillBugs|Session")
	int32 MaxSlots = 0;

	UPROPERTY(BlueprintReadOnly, Category = "KillBugs|Session")
	int32 PingMs = 0;

	/** Index into the live search results; what JoinSession() takes. Not an array position. */
	UPROPERTY(BlueprintReadOnly, Category = "KillBugs|Session")
	int32 ResultIndex = INDEX_NONE;
};

DECLARE_MULTICAST_DELEGATE(FOnKBSessionListChanged);
DECLARE_MULTICAST_DELEGATE(FOnKBOnlineStateChanged);

/**
 * Host / search / join, over LAN.
 *
 * Backed by OnlineSubsystemNull, which implements IOnlineSession *including* LAN discovery:
 * a session created with bIsLANMatch advertises itself over UDP (FOnlineSessionNull::UpdateLANStatus)
 * and a search with bIsLanQuery collects the replies. We consume the ordinary interface and never
 * touch the beacon, so moving to Steam or EOS later means changing this class and the
 * DefaultPlatformService line in Config/DefaultEngine.ini - nothing else.
 *
 * A GameInstanceSubsystem specifically so it SURVIVES the map change: hosting is "create a
 * session, then travel to the lobby as a listen server", and the session has to outlive the
 * travel that follows it.
 */
UCLASS()
class KILLBUGS_API UKBSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * Declared rather than implicit so its body is generated in the .cpp, where
	 * FOnlineSessionSearch is a complete type. TSharedPtr's destructor needs the full
	 * definition, and the engine header this includes only forward-declares it.
	 */
	virtual ~UKBSessionSubsystem() override;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---- Actions, all driven from the lobby UI or the KB.Lobby.* exec commands ---------------

	/**
	 * Opens a room and makes this machine the listen server.
	 *
	 * Creates the session first, then travels to the lobby with ?listen. The beacon starts
	 * advertising as soon as the session exists, so the order within the request does not
	 * matter - but the travel has to happen for anyone to be able to connect at all.
	 */
	void HostSession();

	/** Refreshes the server list. Safe to call while a previous search is still running. */
	void FindSessions();

	/** Joins a row of the last search. Index is a ResultIndex from GetSearchResults(). */
	void JoinSession(int32 ResultIndex);

	/** Debug and test path: connect straight to "host:port", skipping discovery entirely. */
	void JoinAddress(const FString& Address);

	/** Tears the session down and returns to the lobby. */
	void LeaveSession();

	// ---- What the lobby draws ---------------------------------------------------------------

	EKBOnlineState GetState() const { return State; }

	/**
	 * True when this machine is the one everyone else connects to. Only then can it start.
	 *
	 * Kept separate from State rather than derived from it: State is "what is happening right
	 * now" and goes back to Offline while a search runs, but hosting is a role that persists
	 * across that. Deriving one from the other made a host forget it was the host the moment
	 * it refreshed its own list.
	 */
	bool IsHosting() const { return bHosting; }

	const TArray<FKBSessionEntry>& GetSearchResults() const { return SearchEntries; }

	/** Last failure, already phrased for the player. Empty when nothing has gone wrong. */
	FString GetLastError() const { return LastError; }

	/** Room capacity, straight from UKBGameSettings::MaxPlayers. */
	int32 GetMaxPlayers() const;

	FOnKBSessionListChanged OnSessionListChanged;
	FOnKBOnlineStateChanged OnOnlineStateChanged;

private:
	/**
	 * What to do once a leftover session has been torn down.
	 *
	 * Needed because OnlineSubsystemNull keeps a NAMED session on this machine after a
	 * successful join, not just after hosting - so joining a room leaves this client unable to
	 * create or join anything else ("Session (KillBugsGame) already exists, can't join twice").
	 * Tearing the old one down is asynchronous, so the action the player actually asked for has
	 * to be remembered and resumed from the destroy callback.
	 */
	enum class EPendingSessionAction : uint8
	{
		None,
		Host,
		Join,
		/** Destroy, then go back to the lobby. */
		Leave,
	};

	void SetState(EKBOnlineState NewState);
	void SetError(const FString& Message);

	IOnlineSessionPtr GetSessionInterface() const;
	APlayerController* GetLocalPlayerController() const;

	/** The real work, once we know no session is in the way. */
	void CreateSessionNow();
	void JoinSessionNow(int32 ResultIndex);

	/** Tears down whatever session exists, then resumes Action. */
	void DestroySessionThen(EPendingSessionAction Action, int32 JoinIndex = INDEX_NONE);

	/** Rebuilds the flattened list from the raw search and tells the lobby to redraw. */
	void RebuildSearchEntries();

	/** Changes the map for the whole game. Takes a long package name like "/Game/.../Lvl_Arena". */
	void TravelToMap(const FString& MapURL);

	/** Connects this machine to another. Takes a "host:port" address. */
	void ConnectTo(const FString& Address);

	void HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful);
	void HandleFindSessionsComplete(bool bWasSuccessful);
	void HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result);
	void HandleDestroySessionComplete(FName SessionName, bool bWasSuccessful);

	EKBOnlineState State = EKBOnlineState::Offline;

	/** See IsHosting(). Persistent role, not a transient activity. */
	bool bHosting = false;

	/** See EPendingSessionAction. Set while a leftover session is being cleared. */
	EPendingSessionAction PendingAction = EPendingSessionAction::None;
	int32 PendingJoinIndex = INDEX_NONE;

	FString LastError;

	TSharedPtr<FOnlineSessionSearch> SessionSearch;

	/** The flattened view of SessionSearch that the HUD actually reads. */
	TArray<FKBSessionEntry> SearchEntries;

	// Every AddOn*Delegate has to be cleared in Deinitialize: the session interface outlives
	// this subsystem during shutdown, and a stale handle into a destroyed object crashes.
	FDelegateHandle CreateCompleteHandle;
	FDelegateHandle FindCompleteHandle;
	FDelegateHandle JoinCompleteHandle;
	FDelegateHandle DestroyCompleteHandle;
};

/** Travel targets, in one place so the lobby and the GameMode cannot disagree. */
namespace KBTravel
{
	/**
	 * The run, with the other players following.
	 *
	 * The ?listen is NOT cosmetic and NOT redundant with "we are already a listen server".
	 * The net mode of the destination world is derived from this URL alone
	 * (UWorld::AttemptDeriveFromURL, World.cpp:9643): a URL with neither ?listen nor a host
	 * resolves to NM_Standalone. Travelling to the arena without it silently demotes the host
	 * to standalone and every client is dropped on arrival - which looks like a disconnection
	 * bug, not like a missing URL option.
	 */
	inline const TCHAR* ToArenaAsListenServer() { return TEXT("/Game/KillBugs/Maps/Lvl_Arena?listen"); }

	/** The run alone. Standalone is the correct net mode here, so no ?listen. */
	inline const TCHAR* ToArenaSolo() { return TEXT("/Game/KillBugs/Maps/Lvl_Arena"); }

	/** The lobby, as a listen server. Hosting is this plus a session. */
	inline const TCHAR* ToLobbyAsListenServer() { return TEXT("/Game/KillBugs/Maps/Lvl_Lobby?listen"); }

	/** The lobby as an ordinary standalone player - the state the game boots into. */
	inline const TCHAR* ToLobby() { return TEXT("/Game/KillBugs/Maps/Lvl_Lobby"); }
}
