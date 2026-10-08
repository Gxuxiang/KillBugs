#include "Net/KBSessionSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "KBGameSettings.h"
#include "KillBugs.h"
#include "OnlineSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "SocketSubsystem.h"

// Interfaces/OnlineSessionInterface.h only forward-declares these four; the definitions live
// here. Without it every use of FOnlineSessionSettings/FOnlineSessionSearch is an incomplete
// type error - the interface header is written for callers that pass them straight through.
#include "OnlineSessionSettings.h"

namespace
{
	/**
	 * Our own session name, not the engine's NAME_GameSession.
	 *
	 * A session name is just an opaque key: it only has to match between the CreateSession and
	 * the FindSessions/JoinSession that talk about the same room, and the engine never
	 * interprets it. Naming it after the game keeps a future second session type (a party, say)
	 * from colliding with this one.
	 */
	const FName KBSessionName(TEXT("KillBugsGame"));

	/** JoinSession failure codes, phrased for the lobby's status line. */
	FString JoinResultText(EOnJoinSessionCompleteResult::Type Result)
	{
		switch (Result)
		{
		case EOnJoinSessionCompleteResult::Success:          return TEXT("成功");
		case EOnJoinSessionCompleteResult::SessionIsFull:    return TEXT("房间已满");
		case EOnJoinSessionCompleteResult::SessionDoesNotExist: return TEXT("房间已不存在");
		case EOnJoinSessionCompleteResult::CouldNotRetrieveAddress: return TEXT("拿不到服务器地址");
		case EOnJoinSessionCompleteResult::AlreadyInSession: return TEXT("已经在房间里了");
		case EOnJoinSessionCompleteResult::UnknownError:
		default:                                             return TEXT("未知错误");
		}
	}
}

UKBSessionSubsystem::~UKBSessionSubsystem() = default;

void UKBSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Reported once at startup because a missing subsystem is otherwise invisible: every action
	// below would fail with "no session interface" and the lobby would just look empty.
	if (!GetSessionInterface().IsValid())
	{
		UE_LOG(LogKillBugs, Warning,
			TEXT("Session: no online session interface. LAN hosting and discovery are dead. ")
			TEXT("Check [OnlineSubsystem] DefaultPlatformService in Config/DefaultEngine.ini."));
	}
}

void UKBSessionSubsystem::Deinitialize()
{
	// The session interface outlives this subsystem during shutdown, so a handle left behind
	// would call back into freed memory. Clearing is not optional.
	if (IOnlineSessionPtr Session = GetSessionInterface())
	{
		if (CreateCompleteHandle.IsValid())  { Session->ClearOnCreateSessionCompleteDelegate_Handle(CreateCompleteHandle); }
		if (FindCompleteHandle.IsValid())    { Session->ClearOnFindSessionsCompleteDelegate_Handle(FindCompleteHandle); }
		if (JoinCompleteHandle.IsValid())    { Session->ClearOnJoinSessionCompleteDelegate_Handle(JoinCompleteHandle); }
		if (DestroyCompleteHandle.IsValid()) { Session->ClearOnDestroySessionCompleteDelegate_Handle(DestroyCompleteHandle); }
	}

	CreateCompleteHandle.Reset();
	FindCompleteHandle.Reset();
	JoinCompleteHandle.Reset();
	DestroyCompleteHandle.Reset();

	Super::Deinitialize();
}

IOnlineSessionPtr UKBSessionSubsystem::GetSessionInterface() const
{
	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	return OnlineSubsystem ? OnlineSubsystem->GetSessionInterface() : nullptr;
}

APlayerController* UKBSessionSubsystem::GetLocalPlayerController() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetFirstLocalPlayerController() : nullptr;
}

int32 UKBSessionSubsystem::GetMaxPlayers() const
{
	return FMath::Max(1, KBSettings().MaxPlayers);
}

void UKBSessionSubsystem::SetState(EKBOnlineState NewState)
{
	if (State == NewState)
	{
		return;
	}

	State = NewState;
	OnOnlineStateChanged.Broadcast();
}

void UKBSessionSubsystem::SetError(const FString& Message)
{
	LastError = Message;
	UE_LOG(LogKillBugs, Warning, TEXT("Session: %s"), *Message);
	OnOnlineStateChanged.Broadcast();
}

void UKBSessionSubsystem::TravelToMap(const FString& MapURL)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// A map change is the server's to make: ServerTravel moves everyone at once, which is what
	// carries the other players along (AGameModeBase::ProcessServerTravel issues a ClientTravel
	// to every connected controller). A solo player IS their own server, so this covers them.
	if (World->GetNetMode() == NM_Client)
	{
		// Only reachable if a client is somehow asked to change the map itself. Move this one
		// player rather than doing nothing.
		if (APlayerController* Controller = GetLocalPlayerController())
		{
			Controller->ClientTravel(MapURL, TRAVEL_Absolute);
		}
		return;
	}

	World->ServerTravel(MapURL);
}

void UKBSessionSubsystem::ConnectTo(const FString& Address)
{
	// ClientTravel, never ServerTravel - and that is not a matter of net mode.
	//
	// The address is "host:port", and AGameModeBase::CanServerTravel rejects anything containing
	// ':' or '\' outright because it is checking for a long package name. A joining client is
	// still standalone at this moment - it booted into the lobby on its own and has no
	// connection yet - so a "am I a client?" test picks ServerTravel and the join dies with
	// "CanServerTravel: FURL 192.168.x.y:7777 blocked". The URL decides, not the net mode.
	if (APlayerController* Controller = GetLocalPlayerController())
	{
		Controller->ClientTravel(Address, TRAVEL_Absolute);
	}
}

void UKBSessionSubsystem::HostSession()
{
	IOnlineSessionPtr Session = GetSessionInterface();
	if (!Session.IsValid())
	{
		SetError(TEXT("找不到在线子系统，无法创建房间"));
		return;
	}

	if (bHosting)
	{
		// Already the host; the only useful reading of a second press is "take me back to the
		// room", which matters after a failed travel.
		TravelToMap(KBTravel::ToLobbyAsListenServer());
		return;
	}

	// A named session left over from an earlier join or host is fatal to this: the engine
	// refuses with "Cannot create session 'KillBugsGame': session already exists" and the room
	// is never created - and because nothing says why on screen, it reads as the button being
	// broken. Clear it first, then come back here.
	if (Session->GetNamedSession(KBSessionName))
	{
		UE_LOG(LogKillBugs, Display, TEXT("Session: clearing a leftover session before hosting"));
		DestroySessionThen(EPendingSessionAction::Host);
		return;
	}

	CreateSessionNow();
}

void UKBSessionSubsystem::CreateSessionNow()
{
	IOnlineSessionPtr Session = GetSessionInterface();
	if (!Session.IsValid())
	{
		SetError(TEXT("找不到在线子系统，无法创建房间"));
		return;
	}

	LastError.Empty();
	SetState(EKBOnlineState::Creating);

	FOnlineSessionSettings Settings;
	Settings.NumPublicConnections = GetMaxPlayers();
	Settings.NumPrivateConnections = 0;
	Settings.bAllowJoinInProgress = true;
	Settings.bIsDedicated = false;
	Settings.bUsesPresence = false;
	Settings.bUseLobbiesIfAvailable = false;
	Settings.bAllowInvites = false;
	Settings.bAllowJoinViaPresence = false;
	Settings.bAllowJoinViaPresenceFriendsOnly = false;

	// Both of these are required, and they are ANDed rather than alternatives:
	// FOnlineSessionNull::NeedsToAdvertise (OnlineSessionInterfaceNull.cpp:280) is
	// `bShouldAdvertise && IsHost && (bIsLANMatch && ...)`. Dropping either one gives a session
	// that exists locally but never opens the beacon socket - every search on the LAN comes
	// back empty, with nothing in the log to say why.
	Settings.bShouldAdvertise = true;
	Settings.bIsLANMatch = true;

	// Drop a callback left over from an attempt that never completed. Registering a second one
	// without removing the first makes the completion run the handler twice for one request.
	if (CreateCompleteHandle.IsValid())
	{
		Session->ClearOnCreateSessionCompleteDelegate_Handle(CreateCompleteHandle);
		CreateCompleteHandle.Reset();
	}

	CreateCompleteHandle = Session->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateUObject(this, &UKBSessionSubsystem::HandleCreateSessionComplete));

	if (!Session->CreateSession(0, KBSessionName, Settings))
	{
		Session->ClearOnCreateSessionCompleteDelegate_Handle(CreateCompleteHandle);
		CreateCompleteHandle.Reset();
		SetError(TEXT("创建房间失败：会话接口拒绝了请求"));
		SetState(EKBOnlineState::Offline);
	}
}

void UKBSessionSubsystem::HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful)
{
	if (IOnlineSessionPtr Session = GetSessionInterface())
	{
		Session->ClearOnCreateSessionCompleteDelegate_Handle(CreateCompleteHandle);
	}
	CreateCompleteHandle.Reset();

	if (!bWasSuccessful)
	{
		SetError(TEXT("创建房间失败"));
		SetState(EKBOnlineState::Offline);
		return;
	}

	bHosting = true;
	SetState(EKBOnlineState::Hosting);

	UE_LOG(LogKillBugs, Display, TEXT("Session: hosting on LAN, %d slot(s)"), GetMaxPlayers());

	// Creating the session does not make this machine reachable - being a listen server does,
	// and that is decided when the map loads. Hence the travel.
	TravelToMap(KBTravel::ToLobbyAsListenServer());
}

void UKBSessionSubsystem::FindSessions()
{
	IOnlineSessionPtr Session = GetSessionInterface();
	if (!Session.IsValid())
	{
		SetError(TEXT("找不到在线子系统，无法搜索房间"));
		return;
	}

	// A refresh pressed while a search is still running would swap SessionSearch under the old
	// search's callback, so the old one is cancelled first.
	//
	// The handle has to be cleared HERE, not just in the completion handler. CancelFindSessions
	// never fires OnFindSessionsComplete, so the handler's own Clear never runs for a cancelled
	// search and the callback stays registered for good. Four quick presses of 刷新 therefore
	// left four live callbacks, and the next completion ran the handler four times - which is
	// exactly what showed up in the log as "found 1 room(s)" printed four times in the same
	// millisecond.
	if (FindCompleteHandle.IsValid())
	{
		Session->ClearOnFindSessionsCompleteDelegate_Handle(FindCompleteHandle);
		FindCompleteHandle.Reset();
	}

	if (State == EKBOnlineState::Searching)
	{
		Session->CancelFindSessions();
	}

	LastError.Empty();
	SetState(EKBOnlineState::Searching);

	SessionSearch = MakeShared<FOnlineSessionSearch>();
	SessionSearch->MaxSearchResults = 32;

	// LAN query, not an online-service query: this is what routes through the beacon and
	// gathers replies from every host on the local network.
	SessionSearch->bIsLanQuery = true;

	FindCompleteHandle = Session->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateUObject(this, &UKBSessionSubsystem::HandleFindSessionsComplete));

	if (!Session->FindSessions(0, SessionSearch.ToSharedRef()))
	{
		Session->ClearOnFindSessionsCompleteDelegate_Handle(FindCompleteHandle);
		FindCompleteHandle.Reset();
		SetError(TEXT("搜索房间失败：会话接口拒绝了请求"));
		SetState(EKBOnlineState::Offline);
	}
}

void UKBSessionSubsystem::HandleFindSessionsComplete(bool bWasSuccessful)
{
	if (IOnlineSessionPtr Session = GetSessionInterface())
	{
		Session->ClearOnFindSessionsCompleteDelegate_Handle(FindCompleteHandle);
	}
	FindCompleteHandle.Reset();

	RebuildSearchEntries();

	if (!bWasSuccessful)
	{
		SetError(TEXT("搜索房间失败"));
		SetState(EKBOnlineState::Offline);
		OnSessionListChanged.Broadcast();
		return;
	}

	UE_LOG(LogKillBugs, Display, TEXT("Session: found %d room(s) on the LAN"), SearchEntries.Num());

	// Back to whatever we were: a host that refreshes is still the host.
	SetState(bHosting ? EKBOnlineState::Hosting : EKBOnlineState::Offline);
	OnSessionListChanged.Broadcast();
}

void UKBSessionSubsystem::RebuildSearchEntries()
{
	SearchEntries.Reset();

	if (!SessionSearch.IsValid())
	{
		return;
	}

	IOnlineSessionPtr Session = GetSessionInterface();
	if (!Session.IsValid())
	{
		return;
	}

	for (int32 Index = 0; Index < SessionSearch->SearchResults.Num(); ++Index)
	{
		const FOnlineSessionSearchResult& Result = SessionSearch->SearchResults[Index];

		FKBSessionEntry Entry;
		Entry.ResultIndex = Index;
		Entry.PingMs = Result.PingInMs;
		Entry.MaxSlots = Result.Session.SessionSettings.NumPublicConnections;
		Entry.OpenSlots = Result.Session.NumOpenPublicConnections;
		Entry.HostName = Result.Session.OwningUserName;

		// The address IS the row - it is the only thing we can actually join by - so a result
		// we cannot resolve is dropped rather than shown as an unjoinable entry.
		if (!Session->GetResolvedConnectString(Result, NAME_GamePort, Entry.Address))
		{
			UE_LOG(LogKillBugs, Warning, TEXT("Session: dropping result %d, no connect string"), Index);
			continue;
		}

		SearchEntries.Add(MoveTemp(Entry));
	}
}

void UKBSessionSubsystem::JoinSession(int32 ResultIndex)
{
	IOnlineSessionPtr Session = GetSessionInterface();
	if (!Session.IsValid() || !SessionSearch.IsValid() ||
		!SessionSearch->SearchResults.IsValidIndex(ResultIndex))
	{
		SetError(TEXT("这个房间已经不在列表里了，刷新一下"));
		return;
	}

	// The reason a client could join a room exactly once and then be refused forever: a
	// successful join leaves a NAMED session on this machine too - not just on the host - and
	// every later join is then rejected with "Session (KillBugsGame) already exists, can't join
	// twice". Nothing about that message reaches the player, so it reads as the room refusing
	// them. Clear it, then resume the join.
	if (Session->GetNamedSession(KBSessionName))
	{
		UE_LOG(LogKillBugs, Display, TEXT("Session: clearing a leftover session before joining"));
		DestroySessionThen(EPendingSessionAction::Join, ResultIndex);
		return;
	}

	JoinSessionNow(ResultIndex);
}

void UKBSessionSubsystem::JoinSessionNow(int32 ResultIndex)
{
	IOnlineSessionPtr Session = GetSessionInterface();
	if (!Session.IsValid() || !SessionSearch.IsValid() ||
		!SessionSearch->SearchResults.IsValidIndex(ResultIndex))
	{
		SetError(TEXT("这个房间已经不在列表里了，刷新一下"));
		return;
	}

	LastError.Empty();
	SetState(EKBOnlineState::Joining);

	if (JoinCompleteHandle.IsValid())
	{
		Session->ClearOnJoinSessionCompleteDelegate_Handle(JoinCompleteHandle);
		JoinCompleteHandle.Reset();
	}

	JoinCompleteHandle = Session->AddOnJoinSessionCompleteDelegate_Handle(
		FOnJoinSessionCompleteDelegate::CreateUObject(this, &UKBSessionSubsystem::HandleJoinSessionComplete));

	if (!Session->JoinSession(0, KBSessionName, SessionSearch->SearchResults[ResultIndex]))
	{
		Session->ClearOnJoinSessionCompleteDelegate_Handle(JoinCompleteHandle);
		JoinCompleteHandle.Reset();
		SetError(TEXT("加入房间失败：会话接口拒绝了请求"));
		SetState(EKBOnlineState::Offline);
	}
}

void UKBSessionSubsystem::HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result)
{
	IOnlineSessionPtr Session = GetSessionInterface();
	if (Session.IsValid())
	{
		Session->ClearOnJoinSessionCompleteDelegate_Handle(JoinCompleteHandle);
	}
	JoinCompleteHandle.Reset();

	if (Result != EOnJoinSessionCompleteResult::Success)
	{
		SetError(FString::Printf(TEXT("加入房间失败：%s"), *JoinResultText(Result)));
		SetState(EKBOnlineState::Offline);
		return;
	}

	FString ConnectString;
	if (!Session.IsValid() || !Session->GetResolvedConnectString(KBSessionName, ConnectString))
	{
		SetError(TEXT("加入成功，但拿不到服务器地址"));
		SetState(EKBOnlineState::Offline);
		return;
	}

	// A "...:0" address is what an EDITOR PIE host advertises. PIE's net driver really is
	// listening (the log says "listening on port 17777"), but it reports no port to the
	// session, so the LAN beacon carries 0 - and the client then dutifully connects to port 0,
	// fails instantly, and NOTHING appears on screen. That is indistinguishable from the room
	// ignoring you, which is exactly how it was reported.
	//
	// The address is the one thing this class cannot fake, so it refuses it by name.
	if (ConnectString.EndsWith(TEXT(":0")))
	{
		UE_LOG(LogKillBugs, Warning,
			TEXT("Session: '%s' advertises port 0 - almost certainly an editor PIE host. ")
			TEXT("Host with a standalone game process instead."), *ConnectString);

		SetError(TEXT("这个房间没有可连接的端口（房主多半是在编辑器里 PIE 开的房）。"
			"请让房主用独立游戏进程开房：UnrealEditor.exe <项目>.uproject -game"));
		SetState(EKBOnlineState::Offline);
		return;
	}

	LastError.Empty();
	SetState(EKBOnlineState::InSession);

	UE_LOG(LogKillBugs, Display, TEXT("Session: joining %s"), *ConnectString);

	// The map is not in the URL on purpose. The join handshake sends a client that is on the
	// wrong level to the server's level by itself, so hard-coding a map here would be a second
	// source of truth that goes stale the moment the host moves on to the arena.
	ConnectTo(ConnectString);
}

void UKBSessionSubsystem::JoinAddress(const FString& Address)
{
	// Debug and test path. Deliberately bypasses JoinSession: there is no search result here,
	// and the point is to exercise everything AFTER discovery when discovery is the part
	// misbehaving - for instance two clients on one machine, where the LAN beacon's broadcast
	// may only reach one of them.
	LastError.Empty();
	SetState(EKBOnlineState::InSession);

	UE_LOG(LogKillBugs, Display, TEXT("Session: joining %s directly, skipping discovery"), *Address);

	ConnectTo(Address);
}

FString UKBSessionSubsystem::NormalizeJoinAddress(const FString& Typed)
{
	FString Trimmed = Typed;
	Trimmed.TrimStartAndEndInline();

	if (Trimmed.IsEmpty())
	{
		return Trimmed;
	}

	// Already has a port? Leave it exactly as typed. Guessing here would quietly redirect a
	// deliberate choice.
	if (Trimmed.Contains(TEXT(":")))
	{
		return Trimmed;
	}

	return FString::Printf(TEXT("%s:%d"), *Trimmed, DefaultGamePort);
}

TArray<FString> UKBSessionSubsystem::GetLocalIPv4Addresses()
{
	TArray<FString> Result;

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		return Result;
	}

	TArray<TSharedPtr<FInternetAddr>> AdapterAddresses;
	if (!SocketSubsystem->GetLocalAdapterAddresses(AdapterAddresses))
	{
		return Result;
	}

	for (const TSharedPtr<FInternetAddr>& Address : AdapterAddresses)
	{
		if (!Address.IsValid() || Address->GetProtocolType() != FNetworkProtocolTypes::IPv4)
		{
			continue;
		}

		// Filtered on the STRING rather than on the address structure: it is the same value the
		// player will read and type, so what is filtered and what is shown can never disagree.
		// 127.x is this machine, and 169.254.x is a DHCP failure - neither is dialable.
		const FString Text = Address->ToString(false);
		if (Text.StartsWith(TEXT("127.")) || Text.StartsWith(TEXT("169.254.")))
		{
			continue;
		}

		Result.AddUnique(Text);
	}

	// Best first. A heuristic, and labelled as one: a home LAN is almost always 192.168.x, a
	// corporate one 10.x, and the rest of 172.x is where virtual adapters (WSL, Docker, VMware)
	// sit - which is exactly the one you do NOT want to read out loud.
	auto Rank = [](const FString& Ip)
	{
		if (Ip.StartsWith(TEXT("192.168."))) { return 0; }
		if (Ip.StartsWith(TEXT("10.")))      { return 1; }
		if (Ip.StartsWith(TEXT("172.")))     { return 2; }
		return 3;
	};

	Result.Sort([&Rank](const FString& A, const FString& B) { return Rank(A) < Rank(B); });

	return Result;
}

void UKBSessionSubsystem::LeaveSession()
{
	if (IOnlineSessionPtr Session = GetSessionInterface())
	{
		if (Session->GetNamedSession(KBSessionName))
		{
			DestroySessionThen(EPendingSessionAction::Leave);
			return;
		}
	}

	// No session to destroy (an ordinary client, or nothing was ever hosted): just go back.
	bHosting = false;
	SetState(EKBOnlineState::Offline);
	TravelToMap(KBTravel::ToLobby());
}

void UKBSessionSubsystem::DestroySessionThen(EPendingSessionAction Action, int32 JoinIndex)
{
	IOnlineSessionPtr Session = GetSessionInterface();
	if (!Session.IsValid())
	{
		SetError(TEXT("找不到在线子系统，无法清理旧房间"));
		return;
	}

	PendingAction = Action;
	PendingJoinIndex = JoinIndex;

	if (DestroyCompleteHandle.IsValid())
	{
		Session->ClearOnDestroySessionCompleteDelegate_Handle(DestroyCompleteHandle);
		DestroyCompleteHandle.Reset();
	}

	DestroyCompleteHandle = Session->AddOnDestroySessionCompleteDelegate_Handle(
		FOnDestroySessionCompleteDelegate::CreateUObject(this, &UKBSessionSubsystem::HandleDestroySessionComplete));

	if (!Session->DestroySession(KBSessionName))
	{
		Session->ClearOnDestroySessionCompleteDelegate_Handle(DestroyCompleteHandle);
		DestroyCompleteHandle.Reset();
		PendingAction = EPendingSessionAction::None;
		PendingJoinIndex = INDEX_NONE;
		SetError(TEXT("清理旧房间失败，请重试"));
	}
}

void UKBSessionSubsystem::HandleDestroySessionComplete(FName SessionName, bool bWasSuccessful)
{
	if (IOnlineSessionPtr Session = GetSessionInterface())
	{
		Session->ClearOnDestroySessionCompleteDelegate_Handle(DestroyCompleteHandle);
	}
	DestroyCompleteHandle.Reset();

	if (!bWasSuccessful)
	{
		// Worth saying out loud: the room is still advertised on the LAN even though the player
		// asked to leave, so others will keep seeing a game that is no longer really there.
		UE_LOG(LogKillBugs, Warning, TEXT("Session: DestroySession failed; the room may still be advertised"));
	}

	// Whatever the player actually asked for, it was waiting on this. Resume it now that the
	// named session is gone - continuing anyway on failure is deliberate, because a failed
	// destroy is exactly when the follow-up action has the best chance of working around it
	// (the engine treats an existing session as fatal, and one that half-exists still counts).
	const EPendingSessionAction Action = PendingAction;
	const int32 JoinIndex = PendingJoinIndex;
	PendingAction = EPendingSessionAction::None;
	PendingJoinIndex = INDEX_NONE;

	switch (Action)
	{
	case EPendingSessionAction::Host:
		bHosting = false;
		CreateSessionNow();
		return;

	case EPendingSessionAction::Join:
		JoinSessionNow(JoinIndex);
		return;

	case EPendingSessionAction::Leave:
	default:
		bHosting = false;
		SetState(EKBOnlineState::Offline);
		TravelToMap(KBTravel::ToLobby());
		return;
	}
}
