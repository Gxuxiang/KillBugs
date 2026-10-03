#include "UI/KBLobbyHud.h"

#include "Audio/KBAudioSubsystem.h"
#include "Core/KBPlayerState.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "KBGameSettings.h"
#include "Lobby/KBLobbyGameState.h"
#include "Net/KBSessionSubsystem.h"

/**
 * Named, NOT anonymous, and every use is qualified through the Style alias below.
 *
 * A unity build concatenates several .cpp files into one translation unit, which makes every
 * anonymous namespace in the blob the SAME namespace - and KBHud.cpp already defines Ink, Dim
 * and PanelFill that way. Leaving these anonymous is a redefinition; pulling them in with a
 * `using namespace` instead makes every use ambiguous against KBHud's. Qualifying through an
 * alias is the one form that cannot collide, however the files get grouped.
 */
namespace KBLobbyHudPrivate
{
	const FLinearColor Ink(0.92f, 0.94f, 0.98f, 1.f);
	const FLinearColor Dim(0.60f, 0.65f, 0.74f, 1.f);
	const FLinearColor Faint(0.38f, 0.42f, 0.50f, 1.f);
	const FLinearColor Backdrop(0.03f, 0.035f, 0.055f, 1.f);
	const FLinearColor PanelFill(0.06f, 0.07f, 0.10f, 1.f);
	const FLinearColor RuleFill(0.16f, 0.20f, 0.28f, 1.f);
	const FLinearColor RowFill(0.11f, 0.13f, 0.18f, 1.f);
	const FLinearColor RowHover(0.18f, 0.25f, 0.38f, 1.f);
	const FLinearColor PlayerRowFill(0.10f, 0.12f, 0.17f, 1.f);
	const FLinearColor ButtonFill(0.14f, 0.17f, 0.24f, 1.f);
	const FLinearColor ButtonHover(0.22f, 0.30f, 0.44f, 1.f);
	const FLinearColor ButtonDead(0.09f, 0.10f, 0.13f, 1.f);
	const FLinearColor Accent(0.40f, 0.66f, 1.00f, 1.f);
	const FLinearColor Good(0.36f, 0.74f, 0.46f, 1.f);
	const FLinearColor Bad(0.90f, 0.36f, 0.32f, 1.f);

	// Layout. Plain constants rather than UPROPERTY(EditDefaultsOnly): this class has no
	// Blueprint subclass, so such a property would not be editable anywhere and would only
	// look tunable. Same conclusion the projectile director reached.
	constexpr float PanelWidth = 880.f;
	constexpr float PanelHeight = 600.f;
	constexpr float Pad = 26.f;
	constexpr float ColumnGap = 22.f;
	constexpr float RightColumnWidth = 230.f;
	constexpr float HeaderHeight = 34.f;
	constexpr float RowHeight = 46.f;
	constexpr float RowGap = 8.f;
	constexpr int32 MaxVisibleRows = 6;
	constexpr float PlayerRowHeight = 36.f;
	constexpr float ButtonHeight = 46.f;
	constexpr float ButtonGap = 14.f;
	constexpr float RefreshButtonWidth = 130.f;
	constexpr float RefreshButtonHeight = 30.f;

	/** One line describing what the lobby is doing. Every state has a sentence. */
	FString StateLabel(EKBOnlineState State)
	{
		switch (State)
		{
		case EKBOnlineState::Creating:  return TEXT("正在创建房间…");
		case EKBOnlineState::Hosting:   return TEXT("你正在开房，其他人可以在局域网里搜到你");
		case EKBOnlineState::Searching: return TEXT("正在搜索局域网内的房间…");
		case EKBOnlineState::Joining:   return TEXT("正在加入房间…");
		case EKBOnlineState::InSession: return TEXT("已加入房间");
		case EKBOnlineState::Offline:
		default:                        return TEXT("未联机。创建房间，或从左侧列表加入");
		}
	}

	FLinearColor StateColour(EKBOnlineState State)
	{
		switch (State)
		{
		case EKBOnlineState::Hosting:
		case EKBOnlineState::InSession: return Good;
		case EKBOnlineState::Creating:
		case EKBOnlineState::Searching:
		case EKBOnlineState::Joining:   return Accent;
		case EKBOnlineState::Offline:
		default:                        return Dim;
		}
	}
}

namespace Style = KBLobbyHudPrivate;

AKBLobbyHud::AKBLobbyHud()
{
	// Redrawn every frame like any HUD, but nothing here needs a tick of its own.
	PrimaryActorTick.bCanEverTick = false;
}

UKBSessionSubsystem* AKBLobbyHud::GetSessions() const
{
	UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UKBSessionSubsystem>() : nullptr;
}

void AKBLobbyHud::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas)
	{
		return;
	}

	// Rebuilt from scratch every frame: the search results and the player list both change
	// without telling the HUD, and a stale rect is a click that lands on the wrong thing.
	ButtonRects.Init(FBox2D(), static_cast<int32>(EKBLobbyButton::Count));
	ServerRowRects.Reset();

	// Read once, so every hover test this frame agrees with every other one.
	float MouseX = 0.f;
	float MouseY = 0.f;
	const APlayerController* OwningController = GetOwningPlayerController();
	bHasMouse = OwningController && OwningController->GetMousePosition(MouseX, MouseY);
	MousePosition = bHasMouse ? FVector2D(MouseX, MouseY) : FVector2D::ZeroVector;

	DrawRect(Style::Backdrop, 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);

	// Opaque and painted over everything, deliberately: the lobby is a screen, not a place. That
	// is what lets the lobby map be a nearly empty level with no camera to place, and it means
	// nothing about the UI depends on what the map happens to contain.
	const FBox2D Panel(
		FVector2D((Canvas->SizeX - Style::PanelWidth) * 0.5f, (Canvas->SizeY - Style::PanelHeight) * 0.5f),
		FVector2D((Canvas->SizeX + Style::PanelWidth) * 0.5f, (Canvas->SizeY + Style::PanelHeight) * 0.5f));
	DrawPanel(Panel, Style::PanelFill);

	DrawTitle(Panel);
	DrawServerList(Panel);
	DrawPlayerList(Panel);
	DrawStatusLine(Panel);
	DrawButtons(Panel);

	UpdateHoverSound();
}

bool AKBLobbyHud::IsHovered(const FBox2D& Rect) const
{
	return bHasMouse && Rect.IsInside(MousePosition);
}

void AKBLobbyHud::UpdateHoverSound()
{
	// Buttons first: a row that slid under one would otherwise steal the cue.
	int32 HoveredButton = INDEX_NONE;
	int32 HoveredRow = INDEX_NONE;

	if (bHasMouse)
	{
		for (int32 Index = 0; Index < ButtonRects.Num(); ++Index)
		{
			if (ButtonRects[Index].IsInside(MousePosition))
			{
				HoveredButton = Index;
				break;
			}
		}

		if (HoveredButton == INDEX_NONE)
		{
			for (int32 Index = 0; Index < ServerRowRects.Num(); ++Index)
			{
				if (ServerRowRects[Index].IsInside(MousePosition))
				{
					HoveredRow = Index;
					break;
				}
			}
		}
	}

	if (HoveredButton == LastHoveredButton && HoveredRow == LastHoveredRow)
	{
		return;
	}

	LastHoveredButton = HoveredButton;
	LastHoveredRow = HoveredRow;

	if (HoveredButton == INDEX_NONE && HoveredRow == INDEX_NONE)
	{
		return;
	}

	// The card hover cue, reused rather than adding a lobby-specific sound asset: it is the
	// same "the cursor is on something you can press" signal.
	if (UKBAudioSubsystem* Audio = GetWorld() ? GetWorld()->GetSubsystem<UKBAudioSubsystem>() : nullptr)
	{
		Audio->PlayCardHover();
	}
}

void AKBLobbyHud::DrawPanel(const FBox2D& Rect, const FLinearColor& Fill)
{
	DrawRect(Fill, Rect.Min.X, Rect.Min.Y, Rect.Max.X - Rect.Min.X, Rect.Max.Y - Rect.Min.Y);
}

void AKBLobbyHud::DrawTitle(const FBox2D& Panel)
{
	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!Font)
	{
		return;
	}

	DrawText(TEXT("KILLBUGS  联机大厅"), Style::Ink, Panel.Min.X + Style::Pad, Panel.Min.Y + Style::Pad,
		Font, 1.35f, false);

	// A rule under the title, so the panel reads as a header and a body rather than a wall.
	const float RuleY = Panel.Min.Y + Style::Pad + 42.f;
	DrawRect(Style::RuleFill, Panel.Min.X + Style::Pad, RuleY,
		Style::PanelWidth - Style::Pad * 2.f, 2.f);
}

void AKBLobbyHud::DrawServerList(const FBox2D& Panel)
{
	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!Font)
	{
		return;
	}

	const UKBSessionSubsystem* Sessions = GetSessions();

	const float Left = Panel.Min.X + Style::Pad;
	const float Top = Panel.Min.Y + Style::Pad + 62.f;
	const float Width = Style::PanelWidth - Style::Pad * 2.f - Style::RightColumnWidth - Style::ColumnGap;

	// The header carries the count: "搜到 0 个" and "还没搜过" look identical otherwise.
	const int32 Found = Sessions ? Sessions->GetSearchResults().Num() : 0;
	DrawText(FString::Printf(TEXT("局域网房间（%d）"), Found), Style::Dim, Left, Top, Font, 0.95f, false);

	// Refresh sits on the header rather than with the other buttons: it acts on this list, and
	// anywhere else makes the player hunt for it.
	const FBox2D RefreshRect(
		FVector2D(Left + Width - Style::RefreshButtonWidth, Top - 6.f),
		FVector2D(Left + Width, Top - 6.f + Style::RefreshButtonHeight));
	const bool bCanRefresh = Sessions && Sessions->GetState() != EKBOnlineState::Creating;
	ButtonRects[static_cast<int32>(EKBLobbyButton::Refresh)] =
		DrawButton(RefreshRect, TEXT("刷新"), bCanRefresh, IsHovered(RefreshRect));

	const float RowsTop = Top + Style::HeaderHeight;

	if (!Sessions || Sessions->GetSearchResults().Num() == 0)
	{
		// A message rather than an empty box: an empty list with no explanation reads as broken.
		const bool bSearching = Sessions && Sessions->GetState() == EKBOnlineState::Searching;
		DrawText(bSearching ? TEXT("搜索中…") : TEXT("没有找到房间。点「刷新」再搜一次。"),
			Style::Faint, Left + 4.f, RowsTop + 14.f, Font, 0.95f, false);
		return;
	}

	const TArray<FKBSessionEntry>& Entries = Sessions->GetSearchResults();
	const int32 Visible = FMath::Min(Entries.Num(), Style::MaxVisibleRows);

	for (int32 Index = 0; Index < Visible; ++Index)
	{
		const FKBSessionEntry& Entry = Entries[Index];
		const float RowY = RowsTop + Index * (Style::RowHeight + Style::RowGap);
		const FBox2D RowRect(FVector2D(Left, RowY), FVector2D(Left + Width, RowY + Style::RowHeight));

		DrawRect(IsHovered(RowRect) ? Style::RowHover : Style::RowFill, RowRect.Min.X, RowRect.Min.Y,
			RowRect.Max.X - RowRect.Min.X, RowRect.Max.Y - RowRect.Min.Y);

		// The address is the row's identity - it is the only thing we can actually join by, and
		// the LAN beacon does not reliably carry a display name to show instead.
		DrawText(Entry.Address, Style::Ink, RowRect.Min.X + 16.f, RowRect.Min.Y + 13.f, Font, 1.0f, false);

		const FString Slots = FString::Printf(TEXT("%d/%d 人"),
			Entry.MaxSlots - Entry.OpenSlots, Entry.MaxSlots);
		float SlotsWidth = 0.f;
		float SlotsHeight = 0.f;
		GetTextSize(Slots, SlotsWidth, SlotsHeight, Font, 0.9f);
		DrawText(Slots, Style::Dim, RowRect.Max.X - 16.f - SlotsWidth, RowRect.Min.Y + 14.f, Font, 0.9f, false);

		ServerRowRects.Add(RowRect);
	}

	if (Entries.Num() > Style::MaxVisibleRows)
	{
		DrawText(FString::Printf(TEXT("… 另有 %d 个"), Entries.Num() - Style::MaxVisibleRows),
			Style::Faint, Left + 4.f,
			RowsTop + Style::MaxVisibleRows * (Style::RowHeight + Style::RowGap) + 2.f, Font, 0.85f, false);
	}
}

void AKBLobbyHud::DrawPlayerList(const FBox2D& Panel)
{
	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!Font)
	{
		return;
	}

	const APlayerController* OwningController = GetOwningPlayerController();
	const AKBLobbyGameState* LobbyState =
		GetWorld() ? GetWorld()->GetGameState<AKBLobbyGameState>() : nullptr;
	const AKBPlayerState* LocalState =
		OwningController ? OwningController->GetPlayerState<AKBPlayerState>() : nullptr;

	const float Left = Panel.Min.X + Style::PanelWidth - Style::Pad - Style::RightColumnWidth;
	const float Top = Panel.Min.Y + Style::Pad + 62.f;

	const int32 Count = LobbyState ? LobbyState->PlayerArray.Num() : 0;
	DrawText(FString::Printf(TEXT("玩家（%d/%d）"), Count, FMath::Max(1, KBSettings().MaxPlayers)),
		Style::Dim, Left, Top, Font, 0.95f, false);

	if (!LobbyState)
	{
		return;
	}

	for (int32 Index = 0; Index < LobbyState->PlayerArray.Num(); ++Index)
	{
		const AKBPlayerState* PlayerState = Cast<AKBPlayerState>(LobbyState->PlayerArray[Index]);
		if (!PlayerState)
		{
			continue;
		}

		const float RowY = Top + Style::HeaderHeight + Index * Style::PlayerRowHeight;
		const bool bHost = PlayerState->GetKBPlayerIndex() == LobbyState->GetHostPlayerIndex();
		const bool bLocal = PlayerState == LocalState;
		const bool bReady = LobbyState->IsPlayerReady(PlayerState->GetKBPlayerIndex());

		FString Label = PlayerState->GetPlayerName();
		if (bLocal)
		{
			// Marked because the list is otherwise four identical-looking names, and the first
			// question anyone asks is "which one am I".
			Label += TEXT("（我）");
		}
		if (bHost)
		{
			Label += TEXT("  主机");
		}

		DrawRect(Style::PlayerRowFill, Left, RowY, Style::RightColumnWidth,
			Style::PlayerRowHeight - 6.f);
		DrawText(Label, bHost ? Style::Accent : Style::Ink, Left + 12.f, RowY + 7.f, Font, 0.9f, false);

		// The ready mark is drawn separately, on the right, rather than appended to the name:
		// it has to be visible at a glance from across the room, and the host is reading this
		// list to decide whether 开始游戏 will do anything. Name colour is already spoken for
		// by "is this the host", so the state needs its own mark in its own colour.
		const FString ReadyMark = bReady ? TEXT("✓") : TEXT("—");
		float MarkWidth = 0.f;
		float MarkHeight = 0.f;
		GetTextSize(ReadyMark, MarkWidth, MarkHeight, Font, 0.95f);
		DrawText(ReadyMark, bReady ? Style::Good : Style::Faint,
			Left + Style::RightColumnWidth - 14.f - MarkWidth, RowY + 7.f, Font, 0.95f, false);
	}
}

void AKBLobbyHud::DrawStatusLine(const FBox2D& Panel)
{
	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!Font)
	{
		return;
	}

	const UKBSessionSubsystem* Sessions = GetSessions();
	const EKBOnlineState State = Sessions ? Sessions->GetState() : EKBOnlineState::Offline;

	const float Left = Panel.Min.X + Style::Pad;
	const float Y = Panel.Min.Y + Style::PanelHeight - Style::Pad - Style::ButtonHeight - 40.f;

	DrawText(Style::StateLabel(State), Style::StateColour(State), Left, Y, Font, 0.95f, false);

	// The ready tally, right-aligned on the same line. The player list already marks each
	// player individually, but "2/2 ready" is the one number that answers "why is 开始游戏 still
	// grey" without counting ticks down a list.
	const AKBLobbyGameState* LobbyState =
		GetWorld() ? GetWorld()->GetGameState<AKBLobbyGameState>() : nullptr;
	const bool bInRoom = (Sessions && Sessions->IsHosting()) ||
	                     State == EKBOnlineState::InSession;

	if (LobbyState && bInRoom)
	{
		const FString Ready = FString::Printf(TEXT("已准备 %d/%d"),
			LobbyState->ReadyCount(), LobbyState->PlayerArray.Num());
		float ReadyWidth = 0.f;
		float ReadyHeight = 0.f;
		GetTextSize(Ready, ReadyWidth, ReadyHeight, Font, 0.95f);

		const bool bAll = LobbyState->AreAllPlayersReady();
		DrawText(Ready, bAll ? Style::Good : Style::Dim,
			Panel.Min.X + Style::PanelWidth - Style::Pad - ReadyWidth, Y, Font, 0.95f, false);
	}

	// Failures are shown in place of nothing at all, so one is never silent - an async step that
	// fails without saying so is the hardest thing to diagnose in this whole flow.
	if (Sessions && !Sessions->GetLastError().IsEmpty())
	{
		DrawText(Sessions->GetLastError(), Style::Bad, Left, Y + 22.f, Font, 0.9f, false);
	}
}

void AKBLobbyHud::DrawButtons(const FBox2D& Panel)
{
	const UKBSessionSubsystem* Sessions = GetSessions();
	const EKBOnlineState State = Sessions ? Sessions->GetState() : EKBOnlineState::Offline;
	const bool bHosting = Sessions && Sessions->IsHosting();
	const bool bBusy = State == EKBOnlineState::Creating ||
	                   State == EKBOnlineState::Joining ||
	                   State == EKBOnlineState::Searching;
	const bool bOffline = State == EKBOnlineState::Offline;

	struct FButtonSpec
	{
		EKBLobbyButton Button;
		FString Label;
		bool bEnabled;
	};

	// Per-player state the ready button and the start gate both read.
	const AKBLobbyGameState* LobbyState =
		GetWorld() ? GetWorld()->GetGameState<AKBLobbyGameState>() : nullptr;
	const APlayerController* OwningController = GetOwningPlayerController();
	const AKBPlayerState* LocalState =
		OwningController ? OwningController->GetPlayerState<AKBPlayerState>() : nullptr;

	const bool bInRoom = bHosting || State == EKBOnlineState::InSession;
	const int32 LocalIndex = LocalState ? LocalState->GetKBPlayerIndex() : INDEX_NONE;
	const bool bLocalReady = LobbyState && LobbyState->IsPlayerReady(LocalIndex);
	const bool bAllReady = LobbyState && LobbyState->AreAllPlayersReady();

	// Start is host-only and a client sees it greyed rather than absent: a button that vanishes
	// leaves the player wondering whether they missed it.
	//
	// It is lit on !bBusy, NOT on "the lobby is idle" - an earlier version gated it on the state
	// being Offline, which a host never is, so the button was dead grey for everyone including
	// the host it exists for. The headless tests drove KB.Lobby.Start and never went through
	// this predicate, so nothing caught it but a human looking at the screen.
	const FButtonSpec Specs[] =
	{
		{ EKBLobbyButton::Ready, bLocalReady ? TEXT("取消准备") : TEXT("准备"), bInRoom && !bBusy },
		{ EKBLobbyButton::Host,  TEXT("创建房间"), !bBusy && !bInRoom },
		{ EKBLobbyButton::Start, TEXT("开始游戏"), bHosting && !bBusy && bAllReady },
		{ EKBLobbyButton::Solo,  TEXT("单机开始"), bOffline && !bHosting },
		{ EKBLobbyButton::Leave, TEXT("离开房间"), bInRoom },
	};

	const int32 ButtonCount = UE_ARRAY_COUNT(Specs);
	const float ButtonWidth = (Style::PanelWidth - Style::Pad * 2.f -
		Style::ButtonGap * (ButtonCount - 1)) / ButtonCount;
	const float Y = Panel.Min.Y + Style::PanelHeight - Style::Pad - Style::ButtonHeight;

	for (int32 Index = 0; Index < ButtonCount; ++Index)
	{
		const float X = Panel.Min.X + Style::Pad + Index * (ButtonWidth + Style::ButtonGap);
		const FBox2D Rect(FVector2D(X, Y), FVector2D(X + ButtonWidth, Y + Style::ButtonHeight));

		const int32 ButtonIndex = static_cast<int32>(Specs[Index].Button);
		ButtonRects[ButtonIndex] = DrawButton(Rect, Specs[Index].Label, Specs[Index].bEnabled,
			IsHovered(Rect));
	}
}

FBox2D AKBLobbyHud::DrawButton(const FBox2D& Rect, const FString& Label, bool bEnabled, bool bHovered)
{
	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!Font)
	{
		return Rect;
	}

	const float X = Rect.Min.X;
	const float Y = Rect.Min.Y;
	const float W = Rect.Max.X - Rect.Min.X;
	const float H = Rect.Max.Y - Rect.Min.Y;

	DrawRect(!bEnabled ? Style::ButtonDead : (bHovered ? Style::ButtonHover : Style::ButtonFill),
		X, Y, W, H);

	// A lit stripe only while the button can actually do something, so "greyed out" is legible
	// at a glance rather than only on close reading.
	if (bEnabled)
	{
		DrawRect(Style::Accent, X, Y, W, 3.f);
	}

	float TextWidth = 0.f;
	float TextHeight = 0.f;
	GetTextSize(Label, TextWidth, TextHeight, Font, 1.0f);

	DrawText(Label, bEnabled ? Style::Ink : Style::Faint,
		X + (W - TextWidth) * 0.5f, Y + (H - TextHeight) * 0.5f, Font, 1.0f, false);

	return Rect;
}

int32 AKBLobbyHud::HitTestButton(const FVector2D& ScreenPosition) const
{
	for (int32 Index = 0; Index < ButtonRects.Num(); ++Index)
	{
		if (ButtonRects[Index].IsInside(ScreenPosition))
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

int32 AKBLobbyHud::HitTestServerRow(const FVector2D& ScreenPosition) const
{
	for (int32 Index = 0; Index < ServerRowRects.Num(); ++Index)
	{
		if (ServerRowRects[Index].IsInside(ScreenPosition))
		{
			return Index;
		}
	}

	return INDEX_NONE;
}
