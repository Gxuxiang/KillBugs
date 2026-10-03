#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
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

protected:
	/** Segmented bar across the top showing where the wave cycle is. */
	void DrawTimeline();

	/** Floats a small bar over every bug that is not at full health. */
	void DrawEnemyHealthBars();

	void DrawRunReadout();
	void DrawCardDraft();
	void DrawCard(const UKBCardDefinition& Card, const FBox2D& Rect, bool bHovered, int32 Index);

	TArray<FBox2D> CardRects;

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
