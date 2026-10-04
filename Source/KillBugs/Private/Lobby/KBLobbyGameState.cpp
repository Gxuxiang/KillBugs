#include "Lobby/KBLobbyGameState.h"

#include "Core/KBPlayerState.h"
#include "Net/UnrealNetwork.h"

void AKBLobbyGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AKBLobbyGameState, HostPlayerIndex);
	DOREPLIFETIME(AKBLobbyGameState, ReadyPlayerIndices);
	DOREPLIFETIME(AKBLobbyGameState, bStarting);
}

void AKBLobbyGameState::SetStarting(bool bInStarting)
{
	if (!HasAuthority())
	{
		return;
	}

	bStarting = bInStarting;
}

void AKBLobbyGameState::SetHostPlayerIndex(int32 InHostPlayerIndex)
{
	if (!HasAuthority())
	{
		return;
	}

	HostPlayerIndex = InHostPlayerIndex;
}

bool AKBLobbyGameState::IsPlayerReady(int32 KBPlayerIndex) const
{
	return KBPlayerIndex != INDEX_NONE && ReadyPlayerIndices.Contains(KBPlayerIndex);
}

void AKBLobbyGameState::SetPlayerReady(int32 KBPlayerIndex, bool bReady)
{
	if (!HasAuthority() || KBPlayerIndex == INDEX_NONE)
	{
		return;
	}

	const int32 Existing = ReadyPlayerIndices.IndexOfByKey(KBPlayerIndex);

	if (bReady && Existing == INDEX_NONE)
	{
		ReadyPlayerIndices.Add(KBPlayerIndex);
	}
	else if (!bReady && Existing != INDEX_NONE)
	{
		ReadyPlayerIndices.RemoveAt(Existing);
	}
}

int32 AKBLobbyGameState::ReadyCount() const
{
	int32 Count = 0;

	for (const APlayerState* PlayerState : PlayerArray)
	{
		const AKBPlayerState* KBPlayerState = Cast<AKBPlayerState>(PlayerState);
		if (KBPlayerState && IsPlayerReady(KBPlayerState->GetKBPlayerIndex()))
		{
			++Count;
		}
	}

	return Count;
}

bool AKBLobbyGameState::AreAllPlayersReady() const
{
	// Nobody in the room is not "everyone is ready": there is nobody to start the run for, and
	// reporting true here would light up 开始游戏 on a host sitting alone in a fresh lobby.
	if (PlayerArray.Num() == 0)
	{
		return false;
	}

	for (const APlayerState* PlayerState : PlayerArray)
	{
		const AKBPlayerState* KBPlayerState = Cast<AKBPlayerState>(PlayerState);
		if (!KBPlayerState || !IsPlayerReady(KBPlayerState->GetKBPlayerIndex()))
		{
			return false;
		}
	}

	return true;
}
