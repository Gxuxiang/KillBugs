#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "UI/KBBackpackModel.h"
#include "KBHud.generated.h"

class UKBCardDefinition;

/**
 * Prototype HUD, drawn immediately with Canvas.
 *
 * Canvas rather than UMG on purpose: the card draft and the run readout are the only two
 * pieces of UI the game has so far, and building a widget tree in C++ or hand-authoring
 * Blueprints would be more machinery than the content justifies. Replace with UMG once the
 * look matters - the data it reads is all on the replicated PlayerState/GameState, so a UMG
 * version needs no gameplay changes.
 */
UCLASS()
class KILLBUGS_API AKBHud : public AHUD
{
	GENERATED_BODY()

public:
	AKBHud();

	virtual void DrawHUD() override;

	/**
	 * Screen-space rectangles of the offered cards, in the same order as the player's
	 * PendingCardChoices. Empty when no draft is open.
	 *
	 * Populated during DrawHUD and read by the player controller for click hit-testing.
	 */
	const TArray<FBox2D>& GetCardRects() const { return CardRects; }

	/** Index of the card under the given screen position, or INDEX_NONE. */
	int32 HitTestCard(const FVector2D& ScreenPosition) const;

	// ---- Backpack panel --------------------------------------------------------------------

	/**
	 * Index of the backpack row under the given screen position, or INDEX_NONE.
	 *
	 * Empty while the panel is closed, which is what makes the controller's hit tests fail
	 * harmlessly when B has not been pressed.
	 */
	int32 HitTestBackpackRow(const FVector2D& ScreenPosition) const;

	/**
	 * Which menu entry is under the given position, or None.
	 *
	 * The entries are ordered by the same array the drawing walks, so the entry that is drawn
	 * and the entry that is clicked cannot disagree - the reason the rows are built once.
	 */
	EKBBackpackAction HitTestBackpackMenu(const FVector2D& ScreenPosition) const;

	/** The row that was drawn at this index last frame, or null. */
	const FKBBackpackRow* GetBackpackRow(int32 Index) const;

	/**
	 * Index of the hotbar slot under the given screen position, or INDEX_NONE.
	 *
	 * Only ever consulted while the backpack panel is open. The bar itself is always drawn, and
	 * a bar that ate left-clicks would turn "shoot at something at the bottom of the screen" into
	 * "select slot 3".
	 */
	int32 HitTestHotbarSlot(const FVector2D& ScreenPosition) const;

protected:
	/** Segmented bar across the top showing where the wave cycle is. */
	void DrawTimeline();

	/** Floats a small bar over every bug that is not at full health. */
	void DrawEnemyHealthBars();

	void DrawRunReadout();

	/**
	 * A health bar per teammate, down the bottom-left.
	 *
	 * The readout above is about YOU - your level, your gold, your health. In a co-op run the
	 * other thing a player needs at a glance is whether the person next to them is about to go
	 * down, and until this existed there was no way to tell short of watching them die.
	 *
	 * Anchored to the BOTTOM of the screen rather than stacked under the readout, because the
	 * readout's height depends on what it is currently showing (the phase countdown comes and
	 * goes) and anything placed after it would shift around with it.
	 *
	 * Read from the GameState's PlayerArray, not from the player controller iterator - on a client
	 * that iterator only yields the local player, which is the bug the rescue ring shipped with.
	 */
	void DrawPartyStatus();
	void DrawCardDraft();

	/**
	 * The rescue ring every downed player projects, with its progress.
	 *
	 * Without this the rescue mechanic is invisible: a downed player sees their camera keep
	 * running while nothing on screen says "somebody can pick you up here", and a teammate has
	 * no way to know where the body is or how far a rescue has got. The logic was verified
	 * server-side long before this existed, which is exactly why it has to be drawn - a mechanic
	 * nobody can see is a mechanic nobody can use.
	 */
	void DrawRescueCircles();

	/**
	 * The extraction ring on the floor, with its two clocks.
	 *
	 * A ring alone would be a lie: the readout has to say WHICH clock is running, because the
	 * difference between "the window is draining while we are not all in" and "we are filling the
	 * progress bar" is the whole mechanic, and both look identical from a circle on the ground.
	 * The window number is shown as the "still have time" countdown; the progress is the arc.
	 */
	void DrawExtractionZone();

	/**
	 * The pre-announcement and the off-screen arrow for the extraction zone.
	 *
	 * The zone appears somewhere far from the team on purpose, so "it is on screen" cannot be
	 * assumed - and a mechanic the player cannot find is a mechanic that does not exist. The
	 * announcement ("N 波后出现撤离点") is what stops it from feeling like an ambush.
	 */
	void DrawExtractionIndicator();

	/**
	 * The end-of-run summary, shown while the phase is RunOver.
	 *
	 * It has to be on screen for the whole RunSummarySeconds window, because that window is the
	 * return-to-lobby delay - whatever is not drawn here is simply never seen. It is also the
	 * only place the run's outcome is stated: without it a wipe just froze the game silently.
	 */
	void DrawRunSummary();
	void DrawCard(const UKBCardDefinition& Card, const FBox2D& Rect, bool bHovered, int32 Index);

	/** The carried-items panel on the right. Only drawn while the controller says it is open. */
	void DrawBackpackPanel();

	/** The consumable bar along the bottom. Always drawn - it is a key reference, not a mode. */
	void DrawHotbar();

	TArray<FBox2D> CardRects;

	/**
	 * The backpack panel's rows, and the rectangles they were drawn at.
	 *
	 * Rebuilt every frame and read by both the drawing and the hit tests, the contract the
	 * lobby's shop rows established. Cleared with the rest at the top of DrawHUD, which is also
	 * what makes the panel's hit tests miss during the end-of-run summary.
	 */
	TArray<FKBBackpackRow> BackpackRows;
	TArray<FBox2D> BackpackRowRects;

	/** Menu entries in draw order, parallel to BackpackMenuRects. Only filled when one is open. */
	TArray<EKBBackpackAction> BackpackMenuActions;
	TArray<FBox2D> BackpackMenuRects;

	/** Where the hotbar slots were drawn, in slot order. Rebuilt with the rest each frame. */
	TArray<FBox2D> HotbarSlotRects;

	// --- Layout ---------------------------------------------------------------------------

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|HUD")
	FVector2D CardSize = FVector2D(340.f, 440.f);

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|HUD")
	float CardGap = 40.f;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|HUD")
	float ReadoutMargin = 28.f;

	/** Fraction of the screen width the timeline spans. */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|HUD", meta = (ClampMin = "0.2", ClampMax = "1.0"))
	float TimelineWidthFraction = 0.62f;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|HUD")
	float TimelineHeight = 30.f;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|HUD")
	float TimelineTopMargin = 18.f;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|HUD")
	float EnemyBarWidth = 44.f;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|HUD")
	float EnemyBarHeight = 6.f;

	/** How far above a bug's feet its bar floats, in world units. */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|HUD")
	float EnemyBarWorldHeight = 130.f;

	/** Cached so the HUD is not iterating the world every frame. */
	TWeakObjectPtr<const class AKBEnemyDirector> CachedEnemyDirector;

	/** Resolved on first use, like the director; the zone is spawned once, at run start. */
	TWeakObjectPtr<const class AKBExtractionZone> CachedExtractionZone;

	/**
	 * Card the cursor was over last frame, so the hover sound fires once on ENTER rather than
	 * every frame the cursor sits still.
	 */
	int32 LastHoveredCardIndex = INDEX_NONE;

	/**
	 * Whether the draft-open sting has played for the current draft.
	 *
	 * Tied to the cards actually being drawn rather than to the phase change: the input lock
	 * hides them for a beat, and a sound with nothing on screen is just confusing.
	 */
	bool bDraftOpenSoundPlayed = false;
};
