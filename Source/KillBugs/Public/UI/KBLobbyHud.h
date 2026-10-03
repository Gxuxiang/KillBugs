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

	virtual void DrawHUD() override;

	/** EKBLobbyButton value, or INDEX_NONE. Empty until the first DrawHUD. */
	int32 HitTestButton(const FVector2D& ScreenPosition) const;

	/** Index into the session subsystem's search results, or INDEX_NONE. */
	int32 HitTestServerRow(const FVector2D& ScreenPosition) const;

private:
	/** The session subsystem, or null during teardown. */
	UKBSessionSubsystem* GetSessions() const;

	void DrawTitle(const FBox2D& Panel);
	void DrawServerList(const FBox2D& Panel);
	void DrawPlayerList(const FBox2D& Panel);
	void DrawButtons(const FBox2D& Panel);
	void DrawStatusLine(const FBox2D& Panel);

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
};
