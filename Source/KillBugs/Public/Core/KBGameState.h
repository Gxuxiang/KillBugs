#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "KBGameState.generated.h"

/**
 * Run phase. Deliberately a custom enum rather than AGameState::MatchState:
 * MatchState lives on AGameState (not AGameStateBase), so using it would force the heavier
 * AGameMode, and its six values do not map onto wave phases anyway.
 */
/**
 * The run cycle.
 *
 * Warmup -> WaveActive -> Explore -> CardDraft -> WaveActive -> ...
 *
 * Two things are deliberate and load-bearing:
 *  - The wave does NOT clear the swarm when it ends. Leftovers stay and have to be fought
 *    down during Explore, which is what makes Explore a mop-up-then-loot phase rather than a
 *    free shopping trip - and makes clearing efficiently compound into an easier next wave.
 *  - The card draft sits AFTER Explore, not right after the wave. Choosing an upgrade the
 *    instant a wave ends means choosing before you have seen what the leftovers demand.
 */
UENUM(BlueprintType)
enum class EKBWavePhase : uint8
{
	Warmup     UMETA(DisplayName = "Warmup"),
	WaveActive UMETA(DisplayName = "Wave Active"),
	Explore    UMETA(DisplayName = "Explore"),
	CardDraft  UMETA(DisplayName = "Card Draft"),
	RunOver    UMETA(DisplayName = "Run Over")
};

/**
 * Why the run ended.
 *
 * Replaces what used to be a dead end: entering RunOver logged a line and then nothing happened,
 * forever - no summary, no way back to the lobby, no restart. The phase alone cannot carry this
 * because a run ends in two different ways that need different words on screen, and extraction
 * (not built yet) will end a run *successfully*.
 *
 * InProgress is the value while the run is still going, and is also what a run that ended before
 * anything set this would read as - which is why it is the default rather than a "None".
 */
UENUM(BlueprintType)
enum class EKBRunResult : uint8
{
	InProgress UMETA(DisplayName = "In Progress"),
	/** Every player was down at the same moment. */
	WipedOut   UMETA(DisplayName = "Wiped Out"),
	/** The team held the extraction zone to the end of its timer. */
	Extracted  UMETA(DisplayName = "Extracted")
};

/** Fired on clients whenever the replicated phase changes, so UI can react without polling. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FKBOnWavePhaseChanged, EKBWavePhase, NewPhase);

/**
 * How long each phase of a wave cycle lasts.
 *
 * Mirrored onto the GameState because the GameMode does not replicate - a client drawing a
 * timeline has no other way to know how wide each segment should be.
 */
USTRUCT(BlueprintType)
struct FKBPhaseTimings
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timings")
	float Warmup = 6.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timings")
	float Wave = 45.f;

	/** Long on purpose: the leftovers have to be cleared before there is any time to loot. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timings")
	float Explore = 90.f;

	/** Includes the input lock, so the segment matches the phase it represents. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Timings")
	float CardDraft = 61.f;

	/** Total length of one full cycle, used to scale the timeline. */
	float GetCycleLength() const { return Warmup + Wave + Explore + CardDraft; }

	float GetPhaseDuration(EKBWavePhase Phase) const;
};

/**
 * Replicated run state. Everything here is server-authored and read-only on clients.
 */
UCLASS()
class KILLBUGS_API AKBGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AKBGameState();

	UFUNCTION(BlueprintPure, Category = "KillBugs|Run")
	EKBWavePhase GetWavePhase() const { return WavePhase; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Run")
	int32 GetWaveIndex() const { return WaveIndex; }

	/** Server world time at which the current phase ends; clients count down against it. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Run")
	float GetPhaseEndServerTime() const { return PhaseEndServerTime; }

	/** Server world time at which the current phase began, for progress bars. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Run")
	float GetPhaseStartServerTime() const { return PhaseStartServerTime; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Run")
	const FKBPhaseTimings& GetPhaseTimings() const { return Timings; }

	/** Server-only. Set once at run start. */
	void SetPhaseTimingsServer(const FKBPhaseTimings& InTimings);

	UPROPERTY(BlueprintAssignable, Category = "KillBugs|Run")
	FKBOnWavePhaseChanged OnWavePhaseChanged;

	/** How the run ended, or InProgress while it has not. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Run")
	EKBRunResult GetRunResult() const { return RunResult; }

	/**
	 * Records how the run ended. Server only, and latched: the first caller wins.
	 *
	 * "First caller wins" matters because the two ways a run can end are checked in different
	 * places - the wipe is polled every frame, while extraction will be triggered by the zone's
	 * own timer - and a wipe in the same frame the team extracted must not overwrite the good
	 * news with the bad.
	 */
	void SetRunResultServer(EKBRunResult Result);

	// ---- Server-only mutators -----------------------------------------------------------
	// The GameMode drives the phase machine through these rather than touching the
	// replicated fields directly, so there is exactly one place that authorises a change.

	void SetWavePhaseServer(EKBWavePhase NewPhase, float InPhaseEndServerTime);
	void SetWaveIndexServer(int32 NewWaveIndex);

	/**
	 * Server world time before which card picks are ignored. Set when a draft opens.
	 *
	 * Exists because the draft appears the instant a wave ends - which is exactly when the
	 * player is most likely mid-click. Without a beat to react, spamming fire through the
	 * wave transition silently picks a card for them.
	 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Cards")
	float DraftInputUnlockServerTime = 0.f;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Cards")
	bool IsCardDraftInputLocked() const;

	/** Server-only. */
	void SetDraftInputUnlockServerTime(float InServerTime);

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_WavePhase();

	UPROPERTY(ReplicatedUsing = OnRep_WavePhase, BlueprintReadOnly, Category = "KillBugs|Run")
	EKBWavePhase WavePhase = EKBWavePhase::Warmup;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Run")
	int32 WaveIndex = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Run")
	float PhaseEndServerTime = 0.f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Run")
	float PhaseStartServerTime = 0.f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Run")
	FKBPhaseTimings Timings;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Run")
	EKBRunResult RunResult = EKBRunResult::InProgress;

	// EnemiesAlive, RunSeed and WaveModifiers are added in Phase 2/4, once the swarm and the
	// wave definitions exist to populate them.
};
