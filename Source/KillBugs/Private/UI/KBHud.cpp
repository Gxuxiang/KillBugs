#include "UI/KBHud.h"

#include "Audio/KBAudioSubsystem.h"
#include "Combat/KBStatSheetComponent.h"
#include "Combat/KBWeaponInventoryComponent.h"
#include "EngineUtils.h"
#include "Swarm/KBEnemyDirector.h"
#include "Swarm/KBEnemyVisualizerComponent.h"
#include "Core/KBGameState.h"
#include "Core/KBPlayerState.h"
#include "Data/KBCardDefinition.h"
#include "Data/KBWeaponDefinition.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
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
	DrawRunReadout();
	DrawPartyStatus();
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

	// Where the looted inventory will be listed once 搜 exists. Named here rather than left
	// blank so the empty state is obviously a missing feature and not a rendering bug.
	const FString Pending = TEXT("（搜刮到的物品将在后续版本列在这里）");
	float PendingWidth = 0.f;
	float PendingHeight = 0.f;
	GetTextSize(Pending, PendingWidth, PendingHeight, Font, 0.9f);
	DrawText(Pending, FLinearColor(0.30f, 0.33f, 0.40f, 1.f),
		CentreX - PendingWidth * 0.5f, CentreY + 160.f, Font, 0.9f, false);
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
