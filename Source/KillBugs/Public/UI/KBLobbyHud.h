#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "KBLobbyHud.generated.h"

class UKBSessionSubsystem;

/**
 * What a click can land on.
 *
 * The enum lives here because the HUD owns the layout: the controller asks what was hit and
 * acts on the answer, but it never needs to know where anything is drawn.
 */
UENUM()
enum class EKBLobbyButton : uint8
{
	/** Ready up, or take it back. Only meaningful once in a room. */
	Ready,
	/** Open a room on this machine - create the session, then become the listen server. */
	Host,
	/** Search the LAN again. */
	Refresh,
	/** Host only: move everyone to the arena. */
	Start,
	/** The arena, alone, with no room at all. */
	Solo,
	/** Drop the room and come back here. */
	Leave,
	/** Show the shop, or go back to the lobby proper. */
	Shop,
	/** Start typing a host address, for the routers whose broadcast never arrives. */
	ManualJoin,
	/** Put this machine's own address on the clipboard, ready to send to the other player. */
	CopyAddress,

	Count,
};

/**
 * The lobby, drawn immediately with Canvas - same approach as AKBHud, and for the same reason.
 *
 * It paints an opaque panel over the whole screen rather than framing a 3D room, so the lobby
 * map can stay nearly empty and there is no camera to place or freeze. The map still exists
 * because a lobby has to be somewhere: it is what a player lands in before anyone has a room
 * to join.
 *
 * Rectangles are stored during DrawHUD and hit-tested by the controller afterwards, exactly
 * like AKBHud's cards - the previous frame's rects are always one frame old, which no player
 * can perceive, and it keeps hover and click reading the same numbers.
 */
UCLASS()
class KILLBUGS_API AKBLobbyHud : public AHUD
{
	GENERATED_BODY()

public:
	AKBLobbyHud();

	virtual void BeginPlay() override;
	virtual void DrawHUD() override;

	/** EKBLobbyButton value, or INDEX_NONE. Empty until the first DrawHUD. */
	int32 HitTestButton(const FVector2D& ScreenPosition) const;

	/** Index into the session subsystem's search results, or INDEX_NONE. */
	int32 HitTestServerRow(const FVector2D& ScreenPosition) const;

	/** Index into the drawn shop rows, or INDEX_NONE. */
	int32 HitTestShopRow(const FVector2D& ScreenPosition) const;

	/** The row at that index, or null. Read after a hit test to learn what was clicked. */
	const struct FKBShopRow* GetShopRow(int32 Index) const;

	bool IsShopOpen() const { return bShopOpen; }

	/** Opens or closes the shop. The panel body swaps; the frame around it does not move. */
	void ToggleShop() { bShopOpen = !bShopOpen; ShopMessage.Reset(); }

	/** Why the last click was refused, drawn on the existing status line. */
	const FString& GetShopMessage() const { return ShopMessage; }
	void SetShopMessage(const FString& InMessage) { ShopMessage = InMessage; }

	// ---- Typing a host address -------------------------------------------------------------
	//
	// The state lives here rather than on the controller, the same way bShopOpen does: this
	// class already owns what the lobby is showing, and the controller is the thing that drives
	// it. What makes this one different is that the CONTROLLER has to route keystrokes into it,
	// which is why IsTypingAddress exists for it to ask.

	bool IsTypingAddress() const { return bTypingAddress; }
	void BeginAddressEntry();
	void CancelAddressEntry();
	void AppendAddressChar(TCHAR Character);
	void BackspaceAddress();

	/** Appends the clipboard, filtered to what an address can contain. */
	void PasteAddressFromClipboard();

	/** The typed text, and stops typing. The caller owns it from here. */
	FString TakeAddressBuffer();

	/** The address to show and to copy: the best candidate, or empty. */
	FString GetPreferredLocalAddress() const;

	/** All of them, best first - drawn so a wrong guess can be corrected. */
	const TArray<FString>& GetLocalAddresses() const { return LocalAddresses; }

private:
	/** The session subsystem, or null during teardown. */
	UKBSessionSubsystem* GetSessions() const;

	void DrawTitle(const FBox2D& Panel);

	/** This machine's banked totals, in the title bar. */
	void DrawStash(const FBox2D& Panel, float RuleY);
	void DrawServerList(const FBox2D& Panel);

	/**
	 * The shop, drawn in the space the room list and player list occupy.
	 *
	 * A mode switch rather than an overlay or a bigger panel: the same panel changing what it
	 * shows is already how the loading screen works, and keeping the geometry means the title,
	 * the stash line, the status line and the buttons do not move when the shop opens.
	 */
	void DrawShop(const FBox2D& Panel);
	void DrawPlayerList(const FBox2D& Panel);

	/**
	 * This machine's address, and the way to join one by hand.
	 *
	 * It exists because discovery is not always available: OnlineSubsystemNull finds rooms with a
	 * UDP broadcast, and plenty of routers drop broadcast between two wireless clients while
	 * forwarding unicast perfectly - so the list stays empty while a direct connection works.
	 * The verified symptom was exactly that: KB.Lobby.Join to the same address connected
	 * immediately. This gives that path a face, so it does not need a console.
	 */
	void DrawManualJoin(const FBox2D& Panel);

	void DrawButtons(const FBox2D& Panel);
	void DrawStatusLine(const FBox2D& Panel);

	/**
	 * The screen shown once the host has committed to starting the run.
	 *
	 * Replaces the lobby entirely rather than sitting on top of it: the buttons are gone, so
	 * there is nothing left to click, and the last frame drawn before the map change is this
	 * one - it is what stays frozen on screen for the whole arena load.
	 */
	void DrawLoadingScreen();

	/** One button. Returns the rect it drew, so the caller can store it for hit testing. */
	FBox2D DrawButton(const FBox2D& Rect, const FString& Label, bool bEnabled, bool bHovered);

	/** Filled panel with a lighter top stripe, matching KBHud's language. */
	void DrawPanel(const FBox2D& Rect, const FLinearColor& Fill);

	/** True when the cursor is over the rect this frame. */
	bool IsHovered(const FBox2D& Rect) const;

	/**
	 * Plays the hover sound when the cursor ENTERS something, not every frame it stays there.
	 *
	 * Run after the rects are built rather than during drawing, because the sound is about the
	 * change from last frame and the rects are only known once they have been drawn.
	 */
	void UpdateHoverSound();

	/** Screen-space rectangles of the buttons, indexed by EKBLobbyButton. */
	TArray<FBox2D> ButtonRects;

	/** Screen-space rectangles of the server rows, in the same order as the search results. */
	TArray<FBox2D> ServerRowRects;

	/** True while the panel is showing the shop instead of the room and player lists. */
	bool bShopOpen = false;

	/**
	 * The shop's rows and their rectangles, in the same order.
	 *
	 * Both rebuilt once per frame from one BuildShopRows call, so the row drawn and the row hit
	 * are the same object rather than two lists that have to be kept in step.
	 */
	TArray<struct FKBShopRow> ShopRows;
	TArray<FBox2D> ShopRowRects;

	/** Why the last shop click was refused. Drawn on the status line the lobby already has. */
	FString ShopMessage;

	/** True while the player is typing a host address into the lobby. */
	bool bTypingAddress = false;

	/** What they have typed so far. Only meaningful while bTypingAddress. */
	FString AddressBuffer;

	/**
	 * This machine's own addresses, resolved once in BeginPlay.
	 *
	 * Not per frame: the answer cannot change without the adapter changing, and asking the
	 * socket subsystem every frame to draw a line that never moves would be silly.
	 */
	TArray<FString> LocalAddresses;

	/** Cursor position for this frame, read once so every hit test agrees. */
	FVector2D MousePosition = FVector2D::ZeroVector;
	bool bHasMouse = false;

	/**
	 * What the hover sound last fired for, so it fires once on ENTER rather than every frame
	 * the cursor sits still - the same trick AKBHud uses for its cards.
	 *
	 * Encoded as (buttonIndex+1) and (rowIndex+1) so 0 can mean "nothing", which avoids a
	 * second pair of "was anything hovered" flags.
	 */
	int32 LastHoveredButton = INDEX_NONE;
	int32 LastHoveredRow = INDEX_NONE;
	int32 LastHoveredShopRow = INDEX_NONE;
};
