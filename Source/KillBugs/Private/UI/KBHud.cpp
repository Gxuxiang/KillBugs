#include "UI/KBHud.h"

#include "Audio/KBAudioSubsystem.h"
#include "Combat/KBStatSheetComponent.h"
#include "Combat/KBWeaponInventoryComponent.h"
#include "EngineUtils.h"
#include "Swarm/KBEnemyDirector.h"
#include "Swarm/KBEnemyVisualizerComponent.h"
#include "Core/KBGameState.h"
#include "Core/KBPlayerController.h"
#include "Core/KBPlayerState.h"
#include "Data/KBCardDefinition.h"
#include "Data/KBWeaponDefinition.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Extraction/KBExtractionZone.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Interaction/KBChannelComponent.h"

namespace
{
	const FLinearColor Ink(0.92f, 0.94f, 0.98f, 1.f);
	const FLinearColor Dim(0.62f, 0.66f, 0.74f, 1.f);
	const FLinearColor PanelFill(0.06f, 0.07f, 0.10f, 0.92f);
	const FLinearColor PanelHover(0.16f, 0.20f, 0.30f, 0.96f);

	FLinearColor RarityColour(EKBCardRarity Rarity)
	{
		switch (Rarity)
		{
		case EKBCardRarity::Rare:      return FLinearColor(0.35f, 0.62f, 1.00f, 1.f);
		case EKBCardRarity::Epic:      return FLinearColor(0.72f, 0.42f, 1.00f, 1.f);
		case EKBCardRarity::Legendary: return FLinearColor(1.00f, 0.72f, 0.20f, 1.f);
		case EKBCardRarity::Common:
		default:                       return FLinearColor(0.75f, 0.78f, 0.82f, 1.f);
		}
	}

	FString PhaseLabel(EKBWavePhase Phase)
	{
		switch (Phase)
		{
		case EKBWavePhase::Warmup:     return TEXT("准备");
		case EKBWavePhase::WaveActive: return TEXT("虫潮");
		case EKBWavePhase::Explore:    return TEXT("清剿与搜刮");
		case EKBWavePhase::CardDraft:  return TEXT("选择一个升级");
		case EKBWavePhase::RunOver:    return TEXT("本局结束");
		default:                       return FString();
		}
	}

	/**
	 * Short form for the timeline segments, which have no room for the longer readout text.
	 * Kept separate from PhaseLabel so the two can be tuned independently.
	 */
	FString PhaseShortLabel(EKBWavePhase Phase)
	{
		switch (Phase)
		{
		case EKBWavePhase::Warmup:     return TEXT("准备");
		case EKBWavePhase::WaveActive: return TEXT("虫潮");
		case EKBWavePhase::Explore:    return TEXT("探索");
		case EKBWavePhase::CardDraft:  return TEXT("选卡");
		default:                       return FString();
		}
	}

	/** Segment fill colour per phase, so the bar is readable without reading the labels. */
	FLinearColor PhaseColour(EKBWavePhase Phase)
	{
		switch (Phase)
		{
		case EKBWavePhase::Warmup:     return FLinearColor(0.30f, 0.52f, 0.82f, 0.95f);
		case EKBWavePhase::WaveActive: return FLinearColor(0.78f, 0.26f, 0.22f, 0.95f);
		case EKBWavePhase::Explore:    return FLinearColor(0.30f, 0.62f, 0.55f, 0.95f);
		case EKBWavePhase::CardDraft:  return FLinearColor(0.60f, 0.40f, 0.90f, 0.95f);
		default:                       return FLinearColor::Gray;
		}
	}

	/** Card rarity is shown to the player, so it is translated here rather than read from the
	 *  enum's English display names. */
	FString RarityLabel(EKBCardRarity Rarity)
	{
		switch (Rarity)
		{
		case EKBCardRarity::Rare:      return TEXT("稀有");
		case EKBCardRarity::Epic:      return TEXT("史诗");
		case EKBCardRarity::Legendary: return TEXT("传说");
		case EKBCardRarity::Common:
		default:                       return TEXT("普通");
		}
	}

	/**
	 * Characters per line when wrapping a card description by hand.
	 *
	 * Sized for Chinese rather than counting bytes: a CJK glyph is roughly twice the width of
	 * a Latin one, so the count that looks right for English overflows the panel badly here.
	 */
	constexpr int32 CardDescriptionCharsPerLine = 15;

	/**
	 * Backpack panel and hotbar geometry.
	 *
	 * Plain constants rather than UPROPERTY(EditDefaultsOnly) like the older entries above,
	 * because this class has no Blueprint subclass and such a property is not editable anywhere
	 * - it would only look tunable. Same choice the lobby HUD made, and the same reason.
	 */
	constexpr float BackpackMargin = 24.f;
	constexpr float BackpackWidth = 300.f;
	constexpr float BackpackRowHeight = 44.f;
	constexpr float BackpackRowGap = 6.f;
	constexpr float BackpackHeaderHeight = 74.f;
	constexpr float BackpackMenuRowHeight = 30.f;

	constexpr float HotbarSlotSize = 56.f;
	constexpr float HotbarSlotGap = 8.f;
	constexpr float HotbarBottomMargin = 20.f;
}

AKBHud::AKBHud()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AKBHud::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas)
	{
		return;
	}

	CardRects.Reset();
	BackpackRows.Reset();
	BackpackRowRects.Reset();
	BackpackMenuActions.Reset();
	BackpackMenuRects.Reset();
	HotbarSlotRects.Reset();

	// The run is over: the summary replaces everything else rather than sitting on top of it.
	// Health bars over bugs and a draft panel would both be noise at this point, and leaving
	// CardRects populated would let a stray click during the countdown pick a card for a run
	// that has already finished.
	if (const UWorld* HudWorld = GetWorld())
	{
		if (const AKBGameState* HudRunState = HudWorld->GetGameState<AKBGameState>())
		{
			if (HudRunState->GetWavePhase() == EKBWavePhase::RunOver)
			{
				DrawRunSummary();
				return;
			}
		}
	}

	DrawTimeline();
	DrawEnemyHealthBars();
	DrawRescueCircles();
	DrawExtractionZone();
	DrawRunReadout();
	DrawPartyStatus();

	// The backpack panel and its hotbar. Before the extraction announcement and the card draft,
	// both of which are more urgent than a bag the player opened on purpose.
	DrawBackpackPanel();
	DrawHotbar();

	// Last before the draft panel: the announcement is the lowest-priority thing on screen, and
	// the cards have to be readable over it.
	DrawExtractionIndicator();
	DrawCardDraft();
}

void AKBHud::DrawPartyStatus()
{
	const APlayerController* PlayerController = GetOwningPlayerController();
	const UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
	const AKBGameState* RunState = World ? World->GetGameState<AKBGameState>() : nullptr;
	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;

	if (!RunState || !Font || !Canvas)
	{
		return;
	}

	const AKBPlayerState* LocalState = PlayerController->GetPlayerState<AKBPlayerState>();

	// The local player is deliberately not in this list: their health is already the big bar in
	// the readout, and drawing it twice would make the panel read as five players in a four
	// player game.
	TArray<const AKBPlayerState*, TInlineAllocator<4>> Teammates;
	for (const APlayerState* PlayerState : RunState->PlayerArray)
	{
		const AKBPlayerState* KBState = Cast<AKBPlayerState>(PlayerState);
		if (KBState && KBState != LocalState)
		{
			Teammates.Add(KBState);
		}
	}

	if (Teammates.Num() == 0)
	{
		return;
	}

	constexpr float RowHeight = 34.f;
	constexpr float BarWidth = 170.f;
	constexpr float BarHeight = 12.f;
	constexpr float Margin = 20.f;

	// Bottom-anchored: the panel grows upward, so adding a player never pushes anything off the
	// bottom or moves a row that was already there.
	float Y = Canvas->SizeY - Margin - Teammates.Num() * RowHeight;

	for (const AKBPlayerState* Teammate : Teammates)
	{
		const APawn* Pawn = Teammate->GetPawn();
		const UKBStatSheetComponent* Stats =
			Pawn ? Pawn->FindComponentByClass<UKBStatSheetComponent>() : nullptr;

		const float Fraction = Stats ? Stats->GetHealthFraction() : 0.f;
		const bool bDowned = Teammate->IsDowned();

		// A downed teammate's bar is drawn drained regardless of how much health the component
		// still reports: "on the floor" is the state that matters to the player reading this,
		// and a half-full bar next to a body would say the opposite.
		const float Shown = bDowned ? 0.f : Fraction;

		const FString Name = Teammate->GetPlayerName();
		DrawText(Name, bDowned ? FLinearColor(0.90f, 0.36f, 0.32f, 1.f) : Dim,
			Margin, Y, Font, 1.0f, false);

		const float BarY = Y + 16.f;
		DrawRect(FLinearColor(0.10f, 0.10f, 0.12f, 0.9f), Margin, BarY, BarWidth, BarHeight);

		if (Shown > 0.f)
		{
			const FLinearColor BarColour = FLinearColor::LerpUsingHSV(
				FLinearColor(0.90f, 0.15f, 0.15f, 1.f),
				FLinearColor(0.30f, 0.85f, 0.35f, 1.f),
				Shown);
			DrawRect(BarColour, Margin, BarY, BarWidth * Shown, BarHeight);
		}

		// The word, not just the colour. Colour alone is the one signal a player with a colour
		// vision difference cannot read, and "my teammate is down" is worth more than a hue.
		if (bDowned)
		{
			DrawText(TEXT("倒地"), FLinearColor(0.90f, 0.36f, 0.32f, 1.f),
				Margin + BarWidth + 10.f, BarY - 2.f, Font, 1.0f, false);
		}

		Y += RowHeight;
	}
}

void AKBHud::DrawRescueCircles()
{
	const UWorld* World = GetWorld();
	const APlayerController* PlayerController = GetOwningPlayerController();
	if (!World || !PlayerController || !Canvas)
	{
		return;
	}

	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	const APlayerController* LocalController = PlayerController;

	// Iterated from the GameState's PlayerArray, NOT from World->GetPlayerControllerIterator().
	//
	// On a client the controller iterator only yields the LOCAL player's controller - remote
	// players' controllers do not exist as actorsthere - so a circle for a teammate would never
	// be found and the ring would only ever appear under your own body. PlayerArray is
	// replicated to every machine and carries every player's pawn, which is what the ring is
	// actually about.
	const AKBGameState* RunState = World->GetGameState<AKBGameState>();
	if (!RunState)
	{
		return;
	}

	for (const APlayerState* PlayerState : RunState->PlayerArray)
	{
		const APawn* Pawn = PlayerState ? PlayerState->GetPawn() : nullptr;
		if (!Pawn)
		{
			continue;
		}

		// Only downed players have this, and only while they are down - the component is
		// inactive the rest of the time, so this loop needs no extra "is he down" test.
		const UKBChannelComponent* Channel = Pawn->FindComponentByClass<UKBChannelComponent>();
		if (!Channel || !Channel->IsChannelActive())
		{
			continue;
		}

		// The ring belongs on the FLOOR, and the component sits at the owner's origin - which
		// for a character is the middle of its capsule, not its feet. Drawing there would put
		// the circle about half a body above the ground, which from a pitched camera reads as a
		// ring hanging in the air rather than one painted on the floor.
		FVector Centre = Channel->GetComponentLocation();
		if (const ACharacter* Character = Cast<ACharacter>(Pawn))
		{
			if (const UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
			{
				Centre.Z -= Capsule->GetScaledCapsuleHalfHeight();
			}
		}

		// A little above the floor, so the ring is not coplanar with it - the same reason the
		// slime decals sit 8 units up.
		Centre.Z += 4.f;

		const float Radius = Channel->Radius;
		const float Progress = Channel->GetProgress();

		// Projected once per segment endpoint and shared between neighbours, so the ring cannot
		// tear where two segments disagree about a point.
		constexpr int32 Segments = 48;
		TArray<FVector, TInlineAllocator<Segments>> Points;
		Points.SetNum(Segments);

		bool bAnyProjected = false;
		for (int32 Index = 0; Index < Segments; ++Index)
		{
			const float Angle = 2.f * PI * static_cast<float>(Index) / static_cast<float>(Segments);
			const FVector World_ = Centre + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.f) * Radius;
			Points[Index] = Project(World_);
			bAnyProjected |= Points[Index].Z > 0.f;
		}

		if (!bAnyProjected)
		{
			continue;
		}

		// The progress arc is drawn as a subset of the SAME points rather than as an arc of its
		// own, so it grows along the ring instead of slowly drifting off it.
		const int32 ProgressSegments = FMath::Clamp(FMath::RoundToInt(Progress * Segments), 0, Segments);

		for (int32 Index = 0; Index < Segments; ++Index)
		{
			const FVector& A = Points[Index];
			const FVector& B = Points[(Index + 1) % Segments];

			// Either end behind the camera drops the segment. Half a ring drawn through a point
			// that is not on screen produces a long streak across the whole view.
			if (A.Z <= 0.f || B.Z <= 0.f)
			{
				continue;
			}

			const bool bFilled = Index < ProgressSegments;
			const FLinearColor Colour = bFilled
				? FLinearColor(0.36f, 0.74f, 0.46f, 0.95f)
				: FLinearColor(0.55f, 0.62f, 0.75f, 0.35f);

			DrawLine(A.X, A.Y, B.X, B.Y, Colour, bFilled ? 4.f : 2.f);
		}

		if (!Font)
		{
			continue;
		}

		const FVector CentreProjected = Project(Centre);
		if (CentreProjected.Z <= 0.f)
		{
			continue;
		}

		// The label says what the player should DO, not what the system is doing. "等待队友" on
		// the body and "救助中" on everybody else's screen are the two things a player needs to
		// know, and neither is obvious from a ring alone.
		const bool bIsLocalBody = (PlayerState == LocalController->PlayerState);
		FString Label;
		FLinearColor LabelColour;

		if (Progress > 0.f)
		{
			Label = FString::Printf(TEXT("救助中 %d%%"), FMath::RoundToInt(Progress * 100.f));
			LabelColour = FLinearColor(0.36f, 0.74f, 0.46f, 1.f);
		}
		else if (bIsLocalBody)
		{
			Label = TEXT("等待队友靠近救援");
			LabelColour = FLinearColor(0.90f, 0.36f, 0.32f, 1.f);
		}
		else
		{
			Label = TEXT("队友倒地");
			LabelColour = FLinearColor(0.92f, 0.94f, 0.98f, 1.f);
		}

		float LabelWidth = 0.f;
		float LabelHeight = 0.f;
		GetTextSize(Label, LabelWidth, LabelHeight, Font, 1.1f);
		DrawText(Label, LabelColour, CentreProjected.X - LabelWidth * 0.5f,
			CentreProjected.Y - LabelHeight, Font, 1.1f, false);
	}
}

void AKBHud::DrawExtractionZone()
{
	const APlayerController* PlayerController = GetOwningPlayerController();
	UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
	if (!World || !Canvas)
	{
		return;
	}

	if (!CachedExtractionZone.IsValid())
	{
		for (TActorIterator<AKBExtractionZone> It(World); It; ++It)
		{
			CachedExtractionZone = *It;
			break;
		}
	}

	const AKBExtractionZone* Zone = CachedExtractionZone.Get();
	if (!Zone || !Zone->IsZoneOpen())
	{
		return;
	}

	const UKBChannelComponent* Channel = Zone->GetChannel();
	if (!Channel)
	{
		return;
	}

	// The zone's origin is already on the floor (the GameMode places it at Z = 0), so unlike the
	// rescue ring there is no capsule to subtract - only the same small lift that keeps the line
	// off the plane it is painted on.
	const FVector Centre = Zone->GetZoneCentre() + FVector(0.f, 0.f, 4.f);
	const float Radius = Zone->GetZoneRadius();
	const float Progress = Channel->GetProgress();
	const bool bAdvancing = Channel->IsAdvancing();

	constexpr int32 Segments = 48;
	TArray<FVector, TInlineAllocator<Segments>> Points;
	Points.SetNum(Segments);

	bool bAnyProjected = false;
	for (int32 Index = 0; Index < Segments; ++Index)
	{
		const float Angle = 2.f * PI * static_cast<float>(Index) / static_cast<float>(Segments);
		const FVector Around = Centre + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.f) * Radius;
		Points[Index] = Project(Around);
		bAnyProjected |= Points[Index].Z > 0.f;
	}

	if (!bAnyProjected)
	{
		return;
	}

	// The progress arc is a subset of the ring's own points, so it grows along the ring rather
	// than drifting off it - the same reason the rescue ring does it this way.
	const int32 ProgressSegments = FMath::Clamp(FMath::RoundToInt(Progress * Segments), 0, Segments);

	for (int32 Index = 0; Index < Segments; ++Index)
	{
		const FVector& A = Points[Index];
		const FVector& B = Points[(Index + 1) % Segments];

		if (A.Z <= 0.f || B.Z <= 0.f)
		{
			continue;
		}

		// Three states, three colours, because each one asks the player to do something different:
		// standing in it (cyan, filling), waiting for the rest of the team (amber, draining), and
		// already done (the channel deactivates, so this is unreachable at 100%).
		FLinearColor Colour;
		if (Index < ProgressSegments)
		{
			Colour = FLinearColor(0.32f, 0.78f, 0.92f, 0.95f);
		}
		else if (bAdvancing)
		{
			Colour = FLinearColor(0.55f, 0.62f, 0.75f, 0.35f);
		}
		else
		{
			Colour = FLinearColor(0.86f, 0.68f, 0.28f, 0.45f);
		}

		DrawLine(A.X, A.Y, B.X, B.Y, Colour, (Index < ProgressSegments) ? 4.f : 2.f);
	}

	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!Font)
	{
		return;
	}

	const FVector CentreProjected = Project(Centre);
	if (CentreProjected.Z <= 0.f)
	{
		return;
	}

	FString Label;
	FLinearColor LabelColour;

	if (bAdvancing)
	{
		Label = FString::Printf(TEXT("撤离中 %d%%"), FMath::RoundToInt(Progress * 100.f));
		LabelColour = FLinearColor(0.32f, 0.78f, 0.92f, 1.f);
	}
	else if (Progress > 0.f)
	{
		// The pause wording is the whole reason the progress is drawn separately from the window:
		// without it, a stalled bar looks like a bug rather than like somebody standing outside.
		Label = FString::Printf(TEXT("撤离暂停 %d%% —— 等待全员进入"),
			FMath::RoundToInt(Progress * 100.f));
		LabelColour = FLinearColor(0.95f, 0.78f, 0.34f, 1.f);
	}
	else
	{
		Label = TEXT("撤离点已开启 —— 全员进入开始撤离");
		LabelColour = FLinearColor(0.92f, 0.94f, 0.98f, 1.f);
	}

	float LabelWidth = 0.f;
	float LabelHeight = 0.f;
	GetTextSize(Label, LabelWidth, LabelHeight, Font, 1.15f);
	DrawText(Label, LabelColour, CentreProjected.X - LabelWidth * 0.5f,
		CentreProjected.Y - LabelHeight * 1.7f, Font, 1.15f, false);

	// The window countdown, hidden while it is frozen. A number that has stopped moving reads as
	// a broken number; the "撤离中" wording above already says which clock is running.
	if (!bAdvancing)
	{
		const FString WindowLabel = FString::Printf(TEXT("撤离点剩余 %.0f 秒"),
			Zone->GetOpenWindowRemaining());

		float WindowWidth = 0.f;
		float WindowHeight = 0.f;
		GetTextSize(WindowLabel, WindowWidth, WindowHeight, Font, 1.0f);
		DrawText(WindowLabel, FLinearColor(0.86f, 0.68f, 0.28f, 1.f),
			CentreProjected.X - WindowWidth * 0.5f, CentreProjected.Y + WindowHeight * 0.6f,
			Font, 1.0f, false);
	}
}

void AKBHud::DrawExtractionIndicator()
{
	const APlayerController* PlayerController = GetOwningPlayerController();
	UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
	if (!World || !Canvas)
	{
		return;
	}

	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!Font)
	{
		return;
	}

	if (!CachedExtractionZone.IsValid())
	{
		for (TActorIterator<AKBExtractionZone> It(World); It; ++It)
		{
			CachedExtractionZone = *It;
			break;
		}
	}

	const AKBExtractionZone* Zone = CachedExtractionZone.Get();

	if (Zone && Zone->IsZoneOpen())
	{
		const FVector ZoneCentre = Zone->GetZoneCentre();
		const FVector Projected = Project(ZoneCentre);

		const bool bOnScreen = Projected.Z > 0.f
			&& Projected.X >= 0.f && Projected.X <= Canvas->SizeX
			&& Projected.Y >= 0.f && Projected.Y <= Canvas->SizeY;

		if (bOnScreen)
		{
			// The ring is already drawn where the player is looking; an arrow on top of it would
			// only be in the way.
			return;
		}

		// Screen-space direction to the zone, computed from the camera's own axes rather than from
		// the projection: behind the camera the projected XY is meaningless, and that is exactly
		// the case an edge arrow exists for.
		FVector CameraLocation;
		FRotator CameraRotation;
		PlayerController->GetPlayerViewPoint(CameraLocation, CameraRotation);

		const FRotationMatrix CameraBasis(CameraRotation);
		const FVector ToZone = (ZoneCentre - CameraLocation).GetSafeNormal();
		FVector2D ScreenDirection(
			FVector::DotProduct(ToZone, CameraBasis.GetUnitAxis(EAxis::Y)),
			-FVector::DotProduct(ToZone, CameraBasis.GetUnitAxis(EAxis::Z)));

		if (!ScreenDirection.Normalize())
		{
			ScreenDirection = FVector2D(0.f, -1.f);
		}

		const float Margin = 70.f;
		const FVector2D ScreenCentre(Canvas->SizeX * 0.5f, Canvas->SizeY * 0.5f);
		const float HalfWidth = FMath::Max(Canvas->SizeX * 0.5f - Margin, 1.f);
		const float HalfHeight = FMath::Max(Canvas->SizeY * 0.5f - Margin, 1.f);

		// Push the arrow out to whichever screen edge the direction hits first.
		const float Scale = FMath::Min(
			HalfWidth / FMath::Max(FMath::Abs(ScreenDirection.X), KINDA_SMALL_NUMBER),
			HalfHeight / FMath::Max(FMath::Abs(ScreenDirection.Y), KINDA_SMALL_NUMBER));

		const FVector2D Edge = ScreenCentre + ScreenDirection * Scale;
		const FVector2D Tip = Edge + ScreenDirection * 18.f;
		const FVector2D Back = Edge - ScreenDirection * 18.f;
		const FVector2D Side(-ScreenDirection.Y, ScreenDirection.X);

		const FLinearColor ArrowColour(0.32f, 0.78f, 0.92f, 0.95f);
		DrawLine(Tip.X, Tip.Y, (Back + Side * 13.f).X, (Back + Side * 13.f).Y, ArrowColour, 4.f);
		DrawLine(Tip.X, Tip.Y, (Back - Side * 13.f).X, (Back - Side * 13.f).Y, ArrowColour, 4.f);

		// Metres, because that is the unit a player thinks in; the world is in centimetres.
		float DistanceText = 0.f;
		if (const APawn* Pawn = PlayerController->GetPawn())
		{
			DistanceText = FVector::Dist2D(Pawn->GetActorLocation(), ZoneCentre) / 100.f;
		}

		const FString Label = FString::Printf(TEXT("撤离点 %.0f 米"), DistanceText);
		float LabelWidth = 0.f;
		float LabelHeight = 0.f;
		GetTextSize(Label, LabelWidth, LabelHeight, Font, 1.15f);

		// Pulled back towards the middle of the screen so the text never sits off the edge.
		const FVector2D LabelPosition = Edge - ScreenDirection * 46.f;
		DrawText(Label, FLinearColor(0.32f, 0.78f, 0.92f, 1.f),
			LabelPosition.X - LabelWidth * 0.5f, LabelPosition.Y - LabelHeight * 0.5f,
			Font, 1.15f, false);

		return;
	}

	// Not open (yet). If one is on the schedule, say how far off it is. This is the whole defence
	// against the zone feeling like an ambush: the player is told it is coming, in waves, which is
	// a unit they can count.
	const AKBGameState* RunState = World->GetGameState<AKBGameState>();
	if (!RunState)
	{
		return;
	}

	const int32 WavesAway = RunState->GetNextExtractionWaveIndex() - RunState->GetWaveIndex();
	if (WavesAway < 1)
	{
		return;
	}

	const FString Label = FString::Printf(TEXT("%d 波后出现撤离点"), WavesAway);
	float LabelWidth = 0.f;
	float LabelHeight = 0.f;
	GetTextSize(Label, LabelWidth, LabelHeight, Font, 1.2f);

	// Under the timeline, centred: out of the way of the readout in the corners, and directly
	// below the bar that is counting the waves down.
	DrawText(Label, FLinearColor(0.42f, 0.80f, 0.92f, 1.f),
		Canvas->SizeX * 0.5f - LabelWidth * 0.5f,
		TimelineTopMargin + TimelineHeight + 12.f, Font, 1.2f, false);
}

void AKBHud::DrawRunSummary()
{
	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!Font)
	{
		return;
	}

	const AKBGameState* RunState = GetWorld() ? GetWorld()->GetGameState<AKBGameState>() : nullptr;
	if (!RunState)
	{
		return;
	}

	const float CentreX = Canvas->SizeX * 0.5f;
	const float CentreY = Canvas->SizeY * 0.5f;

	// A dim over everything, so the arena behind it reads as background rather than as a place
	// still being played.
	DrawRect(FLinearColor(0.03f, 0.035f, 0.055f, 0.82f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);

	const EKBRunResult Result = RunState->GetRunResult();

	// The verdict, in the largest thing on screen - this is the one line the player is looking
	// for. Colour carries it too, so it reads before the text does.
	FString Title;
	FLinearColor TitleColour;
	switch (Result)
	{
	case EKBRunResult::Extracted:
		Title = TEXT("撤离成功");
		TitleColour = FLinearColor(0.36f, 0.74f, 0.46f, 1.f);
		break;

	case EKBRunResult::WipedOut:
		Title = TEXT("全队阵亡");
		TitleColour = FLinearColor(0.90f, 0.36f, 0.32f, 1.f);
		break;

	case EKBRunResult::InProgress:
	default:
		// Reachable only if something entered RunOver without naming a result. Naming it rather
		// than leaving the screen blank is what makes that bug visible instead of silent.
		Title = TEXT("本局结束");
		TitleColour = FLinearColor(0.92f, 0.94f, 0.98f, 1.f);
		break;
	}

	float TitleWidth = 0.f;
	float TitleHeight = 0.f;
	GetTextSize(Title, TitleWidth, TitleHeight, Font, 2.2f);
	DrawText(Title, TitleColour, CentreX - TitleWidth * 0.5f, CentreY - 150.f, Font, 2.2f, false);

	// This player's own numbers. Read from the local PlayerState rather than the GameState,
	// because XP and gold are per player.
	if (const APlayerController* PlayerController = GetOwningPlayerController())
	{
		if (const AKBPlayerState* PlayerState = PlayerController->GetPlayerState<AKBPlayerState>())
		{
			float LineY = CentreY - 60.f;

			const FString LevelLine = FString::Printf(TEXT("等级 %d"), PlayerState->GetKBLevel());
			float LineWidth = 0.f;
			float LineHeight = 0.f;
			GetTextSize(LevelLine, LineWidth, LineHeight, Font, 1.3f);
			DrawText(LevelLine, FLinearColor(0.92f, 0.94f, 0.98f, 1.f),
				CentreX - LineWidth * 0.5f, LineY, Font, 1.3f, false);

			LineY += 40.f;
			const FString Earnings = FString::Printf(TEXT("经验 %d      金币 %d"),
				PlayerState->GetXP(), PlayerState->GetGold());
			GetTextSize(Earnings, LineWidth, LineHeight, Font, 1.1f);
			DrawText(Earnings, FLinearColor(0.60f, 0.65f, 0.74f, 1.f),
				CentreX - LineWidth * 0.5f, LineY, Font, 1.1f, false);

			LineY += 46.f;
			const FString WaveLine = FString::Printf(TEXT("坚持到第 %d 波"), RunState->GetWaveIndex() + 1);
			GetTextSize(WaveLine, LineWidth, LineHeight, Font, 1.0f);
			DrawText(WaveLine, FLinearColor(0.38f, 0.42f, 0.50f, 1.f),
				CentreX - LineWidth * 0.5f, LineY, Font, 1.0f, false);
		}
	}

	// Countdown to the lobby. There is no way to skip it and that is deliberate: this is a
	// co-op run, and letting one player press "continue" would leave the others behind.
	const float Remaining = FMath::Max(0.f, RunState->GetPhaseEndServerTime() - RunState->GetServerWorldTimeSeconds());
	const FString Countdown = FString::Printf(TEXT("%.0f 秒后返回大厅…"), Remaining);
	float CountdownWidth = 0.f;
	float CountdownHeight = 0.f;
	GetTextSize(Countdown, CountdownWidth, CountdownHeight, Font, 1.0f);
	DrawText(Countdown, FLinearColor(0.60f, 0.65f, 0.74f, 1.f),
		CentreX - CountdownWidth * 0.5f, CentreY + 120.f, Font, 1.0f, false);

	// What the loot rule actually did to the player. This is the only place the two endings
	// differ in a way that costs something, so it is the one line on this screen that is not
	// just a number - it is the answer to "was the walk out worth it".
	const APlayerController* LocalController = GetOwningPlayerController();
	const AKBPlayerState* LocalPlayerState =
		LocalController ? LocalController->GetPlayerState<AKBPlayerState>() : nullptr;

	if (LocalPlayerState)
	{
		const int32 Carried = LocalPlayerState->GetMaterials();
		const int32 CarriedMedkits = LocalPlayerState->GetMedkits();

		// Medkits are named beside the materials because a wipe now costs them too, and "you
		// lost 3 medkits" is a different sentence from "you lost 120 materials".
		FString LootLine;
		FLinearColor LootColour;

		if (Result == EKBRunResult::Extracted)
		{
			LootLine = FString::Printf(TEXT("带出材料 %d      药包 %d"), Carried, CarriedMedkits);
			LootColour = FLinearColor(0.36f, 0.74f, 0.46f, 1.f);
		}
		else if (Result == EKBRunResult::WipedOut)
		{
			LootLine = (Carried > 0 || CarriedMedkits > 0)
				? FString::Printf(TEXT("损失材料 %d      药包 %d"), Carried, CarriedMedkits)
				: TEXT("什么也没带出来");
			LootColour = (Carried > 0 || CarriedMedkits > 0)
				? FLinearColor(0.90f, 0.36f, 0.32f, 1.f)
				: FLinearColor(0.45f, 0.48f, 0.55f, 1.f);
		}

		if (!LootLine.IsEmpty())
		{
			float LootWidth = 0.f;
			float LootHeight = 0.f;
			GetTextSize(LootLine, LootWidth, LootHeight, Font, 1.1f);
			DrawText(LootLine, LootColour, CentreX - LootWidth * 0.5f, CentreY + 150.f,
				Font, 1.1f, false);
		}
	}
}

void AKBHud::DrawEnemyHealthBars()
{
	const APlayerController* PlayerController = GetOwningPlayerController();
	UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}

	if (!CachedEnemyDirector.IsValid())
	{
		for (TActorIterator<AKBEnemyDirector> It(World); It; ++It)
		{
			CachedEnemyDirector = *It;
			break;
		}
	}

	const AKBEnemyDirector* Director = CachedEnemyDirector.Get();
	const UKBEnemyVisualizerComponent* Visualizer = Director
		? Director->FindComponentByClass<UKBEnemyVisualizerComponent>() : nullptr;

	if (!Visualizer)
	{
		return;
	}

	for (const TPair<int32, FKBEnemyView>& Pair : Visualizer->GetViews())
	{
		const FKBEnemyView& View = Pair.Value;

		// Full health shows nothing at all. With hundreds of bugs on screen, a bar over every
		// one of them is noise that hides the ones actually worth shooting.
		if (View.HealthPct >= 255)
		{
			continue;
		}

		const FVector Projected = Project(View.DisplayLocation + FVector(0.f, 0.f, EnemyBarWorldHeight));

		// Z <= 0 means the point is behind the camera.
		if (Projected.Z <= 0.f)
		{
			continue;
		}

		const float Fraction = static_cast<float>(View.HealthPct) / 255.f;
		const float BarX = Projected.X - EnemyBarWidth * 0.5f;
		const float BarY = Projected.Y;

		DrawRect(FLinearColor(0.05f, 0.05f, 0.07f, 0.75f), BarX - 1.f, BarY - 1.f,
			EnemyBarWidth + 2.f, EnemyBarHeight + 2.f);

		// Green through to red as it drops, so a nearly-dead Brute is obvious at a glance.
		const FLinearColor Fill = FLinearColor::LerpUsingHSV(
			FLinearColor(0.90f, 0.20f, 0.20f, 1.f),
			FLinearColor(0.35f, 0.85f, 0.35f, 1.f),
			Fraction);

		DrawRect(Fill, BarX, BarY, EnemyBarWidth * Fraction, EnemyBarHeight);
	}
}

void AKBHud::DrawTimeline()
{
	const APlayerController* PlayerController = GetOwningPlayerController();
	const AKBGameState* RunState = PlayerController && PlayerController->GetWorld()
		? PlayerController->GetWorld()->GetGameState<AKBGameState>() : nullptr;

	if (!RunState)
	{
		return;
	}

	const EKBWavePhase Phase = RunState->GetWavePhase();
	if (Phase == EKBWavePhase::RunOver)
	{
		return;
	}

	const FKBPhaseTimings& Timings = RunState->GetPhaseTimings();
	const float BarWidth = Canvas->SizeX * TimelineWidthFraction;
	const float StartX = (Canvas->SizeX - BarWidth) * 0.5f;
	const float Y = TimelineTopMargin;
	const float Now = RunState->GetServerWorldTimeSeconds();

	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;

	// Draws one segment, with a playhead fill when it is the phase we are actually in.
	auto DrawSegment = [&](EKBWavePhase SegmentPhase, float SegmentX, float SegmentWidth, bool bIsCurrent)
	{
		const FLinearColor Base = bIsCurrent
			? FLinearColor(0.16f, 0.18f, 0.24f, 0.95f)
			: FLinearColor(0.08f, 0.09f, 0.12f, 0.80f);
		DrawRect(Base, SegmentX, Y, SegmentWidth, TimelineHeight);

		if (bIsCurrent)
		{
			const float Duration = Timings.GetPhaseDuration(SegmentPhase);
			const float Elapsed = FMath::Clamp(Now - RunState->GetPhaseStartServerTime(), 0.f, Duration);
			const float Fill = Duration > 0.f ? Elapsed / Duration : 0.f;

			DrawRect(PhaseColour(SegmentPhase), SegmentX, Y, SegmentWidth * Fill, TimelineHeight);

			// Remaining seconds to the right of the bar, so the player can plan around it.
			if (Font)
			{
				const float Remaining = FMath::Max(RunState->GetPhaseEndServerTime() - Now, 0.f);
				DrawText(FString::Printf(TEXT("%.0f"), Remaining),
					Ink, SegmentX + SegmentWidth + 12.f, Y + 6.f, Font, 1.0f, false);
			}
		}

		// Label, only when the segment is wide enough to hold it without clipping.
		if (Font && SegmentWidth > 54.f)
		{
			const FString Label = PhaseShortLabel(SegmentPhase);
			float TextWidth = 0.f;
			float TextHeight = 0.f;
			GetTextSize(Label, TextWidth, TextHeight, Font, 0.85f);
			DrawText(Label, bIsCurrent ? Ink : Dim,
				SegmentX + (SegmentWidth - TextWidth) * 0.5f,
				Y + (TimelineHeight - TextHeight) * 0.5f, Font, 0.85f, false);
		}
	};

	if (Phase == EKBWavePhase::Warmup)
	{
		// Warmup happens once per run, not once per wave, so it is not part of the cycle bar.
		DrawSegment(EKBWavePhase::Warmup, StartX, BarWidth, true);
	}
	else
	{
		// The repeating cycle: choose, survive, mop up and loot. Warmup is deliberately absent
		// because it happens once per run rather than once per wave.
		static const EKBWavePhase Cycle[] = {
			EKBWavePhase::CardDraft, EKBWavePhase::WaveActive, EKBWavePhase::Explore
		};

		const float CycleLength = Timings.CardDraft + Timings.Wave + Timings.Explore;
		if (CycleLength <= 0.f)
		{
			return;
		}

		float Offset = 0.f;
		for (const EKBWavePhase SegmentPhase : Cycle)
		{
			const float SegmentWidth = BarWidth * Timings.GetPhaseDuration(SegmentPhase) / CycleLength;
			DrawSegment(SegmentPhase, StartX + Offset, SegmentWidth, SegmentPhase == Phase);
			Offset += SegmentWidth;
		}
	}
}

void AKBHud::DrawRunReadout()
{
	const APlayerController* PlayerController = GetOwningPlayerController();
	const AKBGameState* RunState = PlayerController ? PlayerController->GetWorld()
		? PlayerController->GetWorld()->GetGameState<AKBGameState>() : nullptr : nullptr;
	const AKBPlayerState* PlayerState = PlayerController
		? PlayerController->GetPlayerState<AKBPlayerState>() : nullptr;

	if (!RunState)
	{
		return;
	}

	UFont* SmallFont = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!SmallFont)
	{
		return;
	}

	float Y = ReadoutMargin;

	// Wave and phase.
	const FString WaveLine = FString::Printf(TEXT("第 %d 波      %s"),
		RunState->GetWaveIndex() + 1, *PhaseLabel(RunState->GetWavePhase()));
	DrawText(WaveLine, Ink, ReadoutMargin, Y, SmallFont, 1.35f, false);
	Y += 34.f;

	// Countdown to the end of the current phase - the number the player actually plays to.
	const float Remaining = RunState->GetPhaseEndServerTime()
		- RunState->GetServerWorldTimeSeconds();
	if (Remaining > 0.f)
	{
		DrawText(FString::Printf(TEXT("%.0f"), Remaining), Dim, ReadoutMargin, Y, SmallFont, 1.2f, false);
		Y += 30.f;
	}

	if (!PlayerState)
	{
		return;
	}

	Y += 10.f;
	DrawText(FString::Printf(TEXT("等级 %d"), PlayerState->GetKBLevel()),
		Ink, ReadoutMargin, Y, SmallFont, 1.15f, false);
	Y += 26.f;

	DrawText(FString::Printf(TEXT("经验 %d      金币 %d"), PlayerState->GetXP(), PlayerState->GetGold()),
		Dim, ReadoutMargin, Y, SmallFont, 1.0f, false);
	Y += 26.f;

	// What is being carried, shown at zero as well. The number is what tells the player that the
	// things on the ground are worth walking to, and a readout that only appears once you have
	// some teaches nothing to the player who has none.
	const int32 Weight = PlayerState->GetCarriedWeight();
	const int32 Capacity = PlayerState->GetBackpackCapacity();

	DrawText(FString::Printf(TEXT("材料 %d      药包 %d"), PlayerState->GetMaterials(), PlayerState->GetMedkits()),
		(PlayerState->GetMaterials() > 0 || PlayerState->GetMedkits() > 0) ? Ink : Dim,
		ReadoutMargin, Y, SmallFont, 1.0f, false);
	Y += 24.f;

	// The one number that decides whether the next thing on the ground is worth walking to. Red
	// at the cap, reusing the summary screen's warning colour - an inline literal rather than a
	// new name in the anonymous namespace above, which is exactly where KBHud.cpp and
	// KBLobbyHud.cpp collided under unity builds before.
	const bool bFull = Weight >= Capacity;
	DrawText(FString::Printf(TEXT("重量 %d / %d"), Weight, Capacity),
		bFull ? FLinearColor(0.90f, 0.36f, 0.32f, 1.f) : Dim,
		ReadoutMargin, Y, SmallFont, 0.95f, false);
	Y += 34.f;

	const APawn* Pawn = PlayerController->GetPawn();

	// Health bar. Interpolated red-to-green so the state reads without parsing the number.
	if (Pawn)
	{
		if (const UKBStatSheetComponent* Stats = Pawn->FindComponentByClass<UKBStatSheetComponent>())
		{
			const float Fraction = Stats->GetHealthFraction();
			const float BarWidth = 240.f;
			const float BarHeight = 14.f;

			DrawRect(FLinearColor(0.10f, 0.10f, 0.12f, 0.9f), ReadoutMargin, Y, BarWidth, BarHeight);

			const FLinearColor BarColour = FLinearColor::LerpUsingHSV(
				FLinearColor(0.90f, 0.15f, 0.15f, 1.f),
				FLinearColor(0.30f, 0.85f, 0.35f, 1.f),
				Fraction);
			DrawRect(BarColour, ReadoutMargin, Y, BarWidth * Fraction, BarHeight);

			Y += BarHeight + 8.f;
			DrawText(FString::Printf(TEXT("%.0f / %.0f"), Stats->GetHealth(), Stats->GetMaxHealth()),
				Dim, ReadoutMargin, Y, SmallFont, 0.9f, false);
			Y += 30.f;
		}
	}

	// Weapons, each with a cooldown bar and its level, so a new card's effect is visible
	// immediately.
	//
	// The manual weapon's bar is the one that matters: it is the only weapon the player is
	// deciding when to fire, so without it there is no way to tell "the shot missed" from
	// "the trigger was ignored because it is still cooling".
	if (Pawn)
	{
		if (const UKBWeaponInventoryComponent* Inventory =
			Pawn->FindComponentByClass<UKBWeaponInventoryComponent>())
		{
			const TArray<FKBOwnedWeapon>& Weapons = Inventory->GetWeapons();

			for (int32 Index = 0; Index < Weapons.Num(); ++Index)
			{
				const FKBOwnedWeapon& Weapon = Weapons[Index];
				const bool bManual = Weapon.Definition
					&& Weapon.Definition->Behavior == EKBWeaponBehavior::Manual;

				const FString Name = Weapon.Definition
					? Weapon.Definition->DisplayName.ToString() : TEXT("?");
				const FString Label = Weapon.Level > 1
					? FString::Printf(TEXT("%s%s  Lv%d"), bManual ? TEXT("[手动] ") : TEXT(""), *Name, Weapon.Level)
					: FString::Printf(TEXT("%s%s"), bManual ? TEXT("[手动] ") : TEXT(""), *Name);

				DrawText(Label, bManual ? Ink : Dim, ReadoutMargin, Y, SmallFont, 0.95f, false);
				Y += 20.f;

				const float BarWidth = 170.f;
				const float BarHeight = 6.f;
				const float CooldownFraction = Inventory->GetCooldownFraction(Index);

				DrawRect(FLinearColor(0.10f, 0.10f, 0.12f, 0.85f), ReadoutMargin, Y, BarWidth, BarHeight);

				// Amber while cooling, green the instant it is ready.
				const FLinearColor Fill = CooldownFraction > 0.f
					? FLinearColor(0.85f, 0.62f, 0.18f, 0.95f)
					: FLinearColor(0.28f, 0.78f, 0.38f, 0.9f);

				DrawRect(Fill, ReadoutMargin, Y, BarWidth * (CooldownFraction > 0.f ? CooldownFraction : 1.f), BarHeight);

				Y += BarHeight + 12.f;
			}
		}
	}
}

void AKBHud::DrawHotbar()
{
	const APlayerController* PlayerController = GetOwningPlayerController();
	const AKBPlayerController* KBController = Cast<AKBPlayerController>(PlayerController);
	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;

	if (!Canvas || !Font || !KBController)
	{
		return;
	}

	const AKBPlayerState* PlayerState = PlayerController->GetPlayerState<AKBPlayerState>();
	constexpr int32 Slots = AKBPlayerController::HotbarSlots;

	const float TotalWidth = Slots * HotbarSlotSize + (Slots - 1) * HotbarSlotGap;
	const float StartX = (Canvas->SizeX - TotalWidth) * 0.5f;
	const float Y = Canvas->SizeY - HotbarBottomMargin - HotbarSlotSize;

	// The bar is always on screen, not only when the panel is open: it is the answer to "which
	// key uses what", and a key reference you have to open a menu to read is not a reference.
	int32 PickedIndex = INDEX_NONE;
	const AKBPlayerController::EKBBackpackPick PickedKind = KBController->GetBackpackPick(PickedIndex);

	for (int32 SlotIndex = 0; SlotIndex < Slots; ++SlotIndex)
	{
		const float X = StartX + SlotIndex * (HotbarSlotSize + HotbarSlotGap);

		const FBox2D SlotRect(FVector2D(X, Y), FVector2D(X + HotbarSlotSize, Y + HotbarSlotSize));
		HotbarSlotRects.Add(SlotRect);

		// The slot being carried around by the click-to-move gesture, lit up so it is obvious
		// what is in hand and what a second click will swap it with.
		const bool bPicked = PickedKind == AKBPlayerController::EKBBackpackPick::HotbarSlot
			&& PickedIndex == SlotIndex;

		DrawRect(bPicked ? PanelHover : PanelFill, X, Y, HotbarSlotSize, HotbarSlotSize);

		// The key number, small and dim in the corner, so it reads as a binding rather than as
		// part of the item's name.
		DrawText(FString::Printf(TEXT("%d"), SlotIndex + 1), Dim, X + 5.f, Y + 2.f, Font, 0.75f, false);

		EKBItemType Type;
		if (!KBController->GetHotbarSlot(SlotIndex, Type))
		{
			continue;
		}

		const int32 Count = !PlayerState ? 0
			: (Type == EKBItemType::Medkit ? PlayerState->GetMedkits() : PlayerState->GetMaterials());

		// Dimmed at zero rather than hidden: the slot is still assigned, and "I have none left"
		// is a different thing from "that key does nothing".
		DrawText(KBItemDisplayName(Type), Count > 0 ? Ink : Dim,
			X + 7.f, Y + HotbarSlotSize * 0.5f - 9.f, Font, 0.85f, false);

		if (Count > 0)
		{
			const FString CountText = FString::Printf(TEXT("%d"), Count);
			float CountWidth = 0.f;
			float CountHeight = 0.f;
			GetTextSize(CountText, CountWidth, CountHeight, Font, 0.8f);
			DrawText(CountText, Dim, X + HotbarSlotSize - 6.f - CountWidth, Y + HotbarSlotSize - 20.f,
				Font, 0.8f, false);
		}
	}
}

void AKBHud::DrawBackpackPanel()
{
	const APlayerController* PlayerController = GetOwningPlayerController();
	const AKBPlayerController* KBController = Cast<AKBPlayerController>(PlayerController);
	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;

	if (!Canvas || !Font || !KBController || !KBController->IsBackpackOpen())
	{
		return;
	}

	const AKBPlayerState* PlayerState = PlayerController->GetPlayerState<AKBPlayerState>();
	if (!PlayerState)
	{
		return;
	}

	// Built once, used for both drawing and hit-testing - the same contract the lobby's shop
	// rows follow. Building it twice is how a panel acts on the row above the one you clicked.
	BuildBackpackRows(*PlayerState, BackpackRows);

	const int32 Rows = FMath::Max(1, BackpackRows.Num());
	const float PanelHeight = BackpackHeaderHeight + Rows * (BackpackRowHeight + BackpackRowGap);

	// Anchored to the right edge, which nothing else in this HUD is: every other element is on
	// the left, the centre or the bottom, so the panel has this whole side to itself.
	const FBox2D Panel(
		FVector2D(Canvas->SizeX - BackpackMargin - BackpackWidth, BackpackMargin),
		FVector2D(Canvas->SizeX - BackpackMargin, BackpackMargin + PanelHeight));

	DrawRect(PanelFill, Panel.Min.X, Panel.Min.Y, BackpackWidth, PanelHeight);

	DrawText(TEXT("背包"), Ink, Panel.Min.X + 14.f, Panel.Min.Y + 10.f, Font, 1.15f, false);

	// The weight line, reusing the readout's own language rather than inventing a second way to
	// say the same number. Red at the cap: it is the one number that decides whether the next
	// thing on the ground is worth walking to.
	const int32 Weight = PlayerState->GetCarriedWeight();
	const int32 Capacity = PlayerState->GetBackpackCapacity();
	const FString WeightLine = FString::Printf(TEXT("重量 %d / %d"), Weight, Capacity);

	float WeightWidth = 0.f;
	float WeightHeight = 0.f;
	GetTextSize(WeightLine, WeightWidth, WeightHeight, Font, 0.95f);
	DrawText(WeightLine, Weight >= Capacity ? FLinearColor(0.90f, 0.36f, 0.32f, 1.f) : Dim,
		Panel.Max.X - 14.f - WeightWidth, Panel.Min.Y + 12.f, Font, 0.95f, false);

	float MouseX = 0.f;
	float MouseY = 0.f;
	const bool bHasMouse = PlayerController->GetMousePosition(MouseX, MouseY);
	const FVector2D Cursor(MouseX, MouseY);

	int32 PickIndex = INDEX_NONE;
	const AKBPlayerController::EKBBackpackPick PickKind = KBController->GetBackpackPick(PickIndex);

	if (BackpackRows.Num() == 0)
	{
		DrawText(TEXT("背包是空的"), Dim, Panel.Min.X + 14.f, Panel.Min.Y + BackpackHeaderHeight - 26.f,
			Font, 0.95f, false);
		return;
	}

	for (int32 Index = 0; Index < BackpackRows.Num(); ++Index)
	{
		const FKBBackpackRow& Row = BackpackRows[Index];
		const float RowY = Panel.Min.Y + BackpackHeaderHeight + Index * (BackpackRowHeight + BackpackRowGap);
		const FBox2D RowRect(FVector2D(Panel.Min.X + 10.f, RowY),
		                     FVector2D(Panel.Max.X - 10.f, RowY + BackpackRowHeight));

		// Lit while held by the click-to-move gesture, hovered otherwise - the two are different
		// states and only one of them means "clicking again will move this".
		const bool bPicked = PickKind == AKBPlayerController::EKBBackpackPick::BackpackRow
			&& PickIndex == Index;
		const bool bHovered = bHasMouse && RowRect.IsInside(Cursor);

		DrawRect((bPicked || bHovered) ? PanelHover : FLinearColor(0.11f, 0.13f, 0.18f, 0.95f),
			RowRect.Min.X, RowRect.Min.Y, RowRect.Max.X - RowRect.Min.X, RowRect.Max.Y - RowRect.Min.Y);

		DrawText(Row.Label, Ink, RowRect.Min.X + 10.f, RowY + BackpackRowHeight * 0.5f - 10.f,
			Font, 1.0f, false);

		float RightWidth = 0.f;
		float RightHeight = 0.f;
		GetTextSize(Row.RightLabel, RightWidth, RightHeight, Font, 0.9f);
		DrawText(Row.RightLabel, Dim, RowRect.Max.X - 10.f - RightWidth,
			RowY + BackpackRowHeight * 0.5f - 9.f, Font, 0.9f, false);

		BackpackRowRects.Add(RowRect);
	}

	DrawText(TEXT("左键点起，再点热键栏放下　右键＝菜单　B 关闭"), Dim,
		Panel.Min.X + 14.f, Panel.Max.Y - 24.f, Font, 0.85f, false);

	// ---- The row menu ----------------------------------------------------------------------
	const int32 MenuRow = KBController->GetOpenBackpackMenuRow();
	if (!BackpackRows.IsValidIndex(MenuRow))
	{
		return;
	}

	const FKBBackpackRow& MenuRowData = BackpackRows[MenuRow];

	// Materials offer no equip entry at all rather than a greyed-out one: a menu is a promise
	// about what you can do, and this cut has no verb that spends a material from your hand.
	if (MenuRowData.bCanEquip)
	{
		BackpackMenuActions.Add(EKBBackpackAction::Equip);
	}
	BackpackMenuActions.Add(EKBBackpackAction::Drop);

	const int32 EntryCount = BackpackMenuActions.Num();

	const float MenuWidth = 150.f;
	const float MenuHeight = EntryCount * BackpackMenuRowHeight;
	const FVector2D Anchor = KBController->GetBackpackMenuPosition();

	// Kept inside the screen: a menu that opens off the right edge cannot be clicked at all.
	const float MenuX = FMath::Clamp(Anchor.X, 0.f, FMath::Max(0.f, Canvas->SizeX - MenuWidth));
	const float MenuY = FMath::Clamp(Anchor.Y, 0.f, FMath::Max(0.f, Canvas->SizeY - MenuHeight));

	DrawRect(FLinearColor(0.04f, 0.05f, 0.08f, 0.98f), MenuX, MenuY, MenuWidth, MenuHeight);

	for (int32 Entry = 0; Entry < EntryCount; ++Entry)
	{
		const float EntryY = MenuY + Entry * BackpackMenuRowHeight;
		const FBox2D EntryRect(FVector2D(MenuX, EntryY),
		                       FVector2D(MenuX + MenuWidth, EntryY + BackpackMenuRowHeight));

		const EKBBackpackAction Action = BackpackMenuActions[Entry];
		const bool bHovered = bHasMouse && EntryRect.IsInside(Cursor);

		if (bHovered)
		{
			DrawRect(PanelHover, EntryRect.Min.X, EntryRect.Min.Y, MenuWidth, BackpackMenuRowHeight);
		}

		DrawText(Action == EKBBackpackAction::Equip ? TEXT("装备到热键栏") : TEXT("丢弃一个"),
			Ink, EntryRect.Min.X + 10.f, EntryY + BackpackMenuRowHeight * 0.5f - 9.f, Font, 0.95f, false);

		BackpackMenuRects.Add(EntryRect);
	}
}

int32 AKBHud::HitTestBackpackRow(const FVector2D& ScreenPosition) const
{
	for (int32 Index = 0; Index < BackpackRowRects.Num(); ++Index)
	{
		if (BackpackRowRects[Index].IsInside(ScreenPosition))
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

EKBBackpackAction AKBHud::HitTestBackpackMenu(const FVector2D& ScreenPosition) const
{
	for (int32 Index = 0; Index < BackpackMenuRects.Num(); ++Index)
	{
		if (BackpackMenuRects[Index].IsInside(ScreenPosition))
		{
			return BackpackMenuActions.IsValidIndex(Index) ? BackpackMenuActions[Index]
			                                               : EKBBackpackAction::None;
		}
	}

	return EKBBackpackAction::None;
}

int32 AKBHud::HitTestHotbarSlot(const FVector2D& ScreenPosition) const
{
	for (int32 Index = 0; Index < HotbarSlotRects.Num(); ++Index)
	{
		if (HotbarSlotRects[Index].IsInside(ScreenPosition))
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

const FKBBackpackRow* AKBHud::GetBackpackRow(int32 Index) const
{
	return BackpackRows.IsValidIndex(Index) ? &BackpackRows[Index] : nullptr;
}

void AKBHud::DrawCardDraft()
{
	const APlayerController* PlayerController = GetOwningPlayerController();
	const AKBPlayerState* PlayerState = PlayerController
		? PlayerController->GetPlayerState<AKBPlayerState>() : nullptr;

	if (!PlayerState || !PlayerState->HasPendingCardDraft())
	{
		// Reset so the next draft gets its sting again.
		bDraftOpenSoundPlayed = false;
		LastHoveredCardIndex = INDEX_NONE;
		return;
	}

	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;

	// Cards stay hidden through the grace period. Showing them while clicks are ignored would
	// invite the very misclick the lock exists to prevent - the player would see their choice
	// land on the wrong card and have no idea why.
	const AKBGameState* RunState = PlayerController->GetWorld()
		? PlayerController->GetWorld()->GetGameState<AKBGameState>() : nullptr;

	if (RunState && RunState->IsCardDraftInputLocked())
	{
		DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.55f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);

		if (Font)
		{
			const FString Notice = TEXT("升级选择即将出现…");
			float TextWidth = 0.f;
			float TextHeight = 0.f;
			GetTextSize(Notice, TextWidth, TextHeight, Font, 1.1f);
			DrawText(Notice, Dim, (Canvas->SizeX - TextWidth) * 0.5f,
				(Canvas->SizeY - TextHeight) * 0.5f, Font, 1.1f, false);
		}
		return;
	}

	UKBAudioSubsystem* Audio = PlayerController->GetWorld()
		? PlayerController->GetWorld()->GetSubsystem<UKBAudioSubsystem>() : nullptr;

	if (Audio && !bDraftOpenSoundPlayed)
	{
		bDraftOpenSoundPlayed = true;
		Audio->PlayCardDraftOpen();
	}

	const TArray<TObjectPtr<UKBCardDefinition>>& Choices = PlayerState->PendingCardChoices;
	const int32 Count = Choices.Num();

	const float TotalWidth = CardSize.X * Count + CardGap * (Count - 1);
	const float StartX = (Canvas->SizeX - TotalWidth) * 0.5f;
	const float StartY = (Canvas->SizeY - CardSize.Y) * 0.5f + 40.f;

	// Dim everything behind the draft so it is unambiguous that the run is waiting on you.
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.55f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);

	float MouseX = 0.f;
	float MouseY = 0.f;
	const bool bHasMouse = PlayerController->GetMousePosition(MouseX, MouseY);

	CardRects.Reserve(Count);
	int32 HoveredIndex = INDEX_NONE;

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FBox2D Rect(FVector2D(StartX + Index * (CardSize.X + CardGap), StartY),
		                  FVector2D(StartX + Index * (CardSize.X + CardGap) + CardSize.X, StartY + CardSize.Y));

		const bool bHovered = bHasMouse && Rect.IsInside(FVector2D(MouseX, MouseY));
		if (bHovered)
		{
			HoveredIndex = Index;
		}

		DrawCard(*Choices[Index], Rect, bHovered, Index);
		CardRects.Add(Rect);
	}

	// Only on entering a different card, so the tick does not repeat while the cursor rests.
	if (HoveredIndex != LastHoveredCardIndex)
	{
		LastHoveredCardIndex = HoveredIndex;
		if (Audio && HoveredIndex != INDEX_NONE)
		{
			Audio->PlayCardHover();
		}
	}
}

void AKBHud::DrawCard(const UKBCardDefinition& Card, const FBox2D& Rect, bool bHovered, int32 Index)
{
	const float X = Rect.Min.X;
	const float Y = Rect.Min.Y;
	const float W = Rect.Max.X - Rect.Min.X;
	const float H = Rect.Max.Y - Rect.Min.Y;

	DrawRect(bHovered ? PanelHover : PanelFill, X, Y, W, H);

	// Rarity stripe along the top edge.
	const FLinearColor Accent = RarityColour(Card.Rarity);
	DrawRect(Accent, X, Y, W, bHovered ? 10.f : 6.f);

	UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
	if (!Font)
	{
		return;
	}

	float TextY = Y + 34.f;
	const float TextX = X + 22.f;

	DrawText(RarityLabel(Card.Rarity), Accent, TextX, TextY, Font, 0.85f, false);
	TextY += 34.f;

	DrawText(Card.Title.ToString(), Ink, TextX, TextY, Font, 1.3f, false);
	TextY += 44.f;

	// Description wraps by hand: Canvas has no automatic layout, and a card whose text runs
	// off the side is worse than a slightly ragged one.
	const FString Description = Card.Description.ToString();
	int32 Cursor = 0;
	while (Cursor < Description.Len() && TextY < Y + H - 70.f)
	{
		const int32 Take = FMath::Min(CardDescriptionCharsPerLine, Description.Len() - Cursor);
		DrawText(Description.Mid(Cursor, Take), Dim, TextX, TextY, Font, 0.9f, false);
		Cursor += Take;
		TextY += 24.f;
	}

	const FString Hint = bHovered
		? FString::Printf(TEXT("点击选择   [%d]"), Index + 1)
		: FString::Printf(TEXT("[%d]"), Index + 1);
	DrawText(Hint, bHovered ? Ink : Dim, TextX, Y + H - 44.f, Font, 0.95f, false);
}

int32 AKBHud::HitTestCard(const FVector2D& ScreenPosition) const
{
	for (int32 Index = 0; Index < CardRects.Num(); ++Index)
	{
		if (CardRects[Index].IsInside(ScreenPosition))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}
