#pragma once

#include "CoreMinimal.h"
#include "Core/KBGameState.h"
#include "GameFramework/GameModeBase.h"
#include "KBGameMode.generated.h"

class AKBEnemyDirector;
class AKBExtractionZone;
class AKBLootDirector;

/**
 * Server-only authority for the run: wave scheduling, card rolling, rewards, respawn and
 * player join/leave. A GameMode never replicates, so nothing here is safe to read on a client.
 *
 * Wave scheduling and card rolling land in Phase 3/4; Phase 1 only needs the class wiring
 * (default pawn/controller/state classes) and stable per-player run indices.
 */
UCLASS()
class KILLBUGS_API AKBGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AKBGameMode();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Swarm")
	AKBEnemyDirector* GetEnemyDirector() const { return EnemyDirector; }

	/** Server-only. Called by AKBPlayerController::ServerPickCard. Returns true if applied. */
	bool ApplyCardChoice(class AKBPlayerState* PlayerState, int32 ChoiceIndex);

	/**
	 * Ends the run, whatever ended it - a wipe, or (once extraction exists) a successful exit.
	 *
	 * The single funnel both endings go through, and the reason it is public: the wipe is detected
	 * here in Tick, while extraction will be triggered by an actor out in the world, and the two
	 * must produce exactly the same summary and the same way back to the lobby. If they each did
	 * their own thing, one of them would drift.
	 *
	 * Idempotent. The result is latched on the GameState, so a second call - or a wipe landing in
	 * the same frame as a successful extraction - is ignored rather than overwriting the verdict.
	 */
	void EndRun(EKBRunResult Result);

	/**
	 * Server-only. Sends everyone back to the lobby once the summary has been read.
	 *
	 * Guarded by bReturningToLobby, which is NOT belt-and-braces: ServerTravel does not take
	 * effect until the current frame ends, so the RunOver branch that calls this keeps running
	 * for every tick until the world actually changes. Measured in a two-process test, that was
	 * five ServerTravel calls in the same second. One is the intent; five is a pile of pending
	 * travels resolving against a world that is already going away.
	 */
	void ReturnToLobby();

	/** Latches the first ReturnToLobby; see the note there. */
	bool bReturningToLobby = false;

	// ---- Headless test entry points ------------------------------------------------------
	//
	// A headless run draws no HUD and cannot walk anywhere, so the extraction has to be drivable
	// and observable from the console. Registered as KB.Extract.* at the bottom of the .cpp.

	static void ConsoleExtractOpenNow(const TArray<FString>& Args, UWorld* World);
	static void ConsoleExtractGather(const TArray<FString>& Args, UWorld* World);
	static void ConsoleExtractScatter(const TArray<FString>& Args, UWorld* World);
	static void ConsoleExtractFail(const TArray<FString>& Args, UWorld* World);
	static void ConsoleExtractSchedule(const TArray<FString>& Args, UWorld* World);
	static void ConsoleExtractSelfTest(const TArray<FString>& Args, UWorld* World);
	static void ConsoleRunWipe(const TArray<FString>& Args, UWorld* World);

protected:
	/** Monotonic counter feeding AKBPlayerState::KBPlayerIndex. */
	int32 NextPlayerIndex = 0;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Swarm")
	TSubclassOf<AKBEnemyDirector> EnemyDirectorClass;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Combat")
	TSubclassOf<class AKBProjectileDirector> ProjectileDirectorClass;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Extraction")
	TSubclassOf<AKBExtractionZone> ExtractionZoneClass;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Loot")
	TSubclassOf<AKBLootDirector> LootDirectorClass;

	UPROPERTY(BlueprintReadOnly, Category = "KillBugs|Swarm")
	TObjectPtr<AKBEnemyDirector> EnemyDirector;

	UPROPERTY(BlueprintReadOnly, Category = "KillBugs|Extraction")
	TObjectPtr<AKBExtractionZone> ExtractionZone;

	UPROPERTY(BlueprintReadOnly, Category = "KillBugs|Loot")
	TObjectPtr<AKBLootDirector> LootDirector;

	// ---- Tuning --------------------------------------------------------------------------
	//
	// Every value that used to live here - phase timings, spawn rates and distances, phase
	// scaling, draft rules - now lives in UKBGameSettings, at:
	//
	//     Project Settings -> Game -> KillBugs
	//     Config/DefaultGame.ini  [/Script/KillBugs.KBGameSettings]
	//
	// They were moved because EditDefaultsOnly on a class with no Blueprint subclass is not
	// editable anywhere: changing the spawn rate meant editing C++ and recompiling the module.

private:
	float SpawnAccumulator = 0.f;

	/** The wave the next draft is choosing for; incremented when that wave actually starts. */
	int32 NextWaveIndex = 0;

	/** The wave the currently open draft was rolled for, used to re-validate picks. */
	int32 DraftWaveIndex = 0;

	void StartWave(int32 WaveIndex);
	void EndWave();
	void BeginExplore();

	/** Spawns at Rate bugs per second, held back by MaxAliveDuringWave. */
	void TickSpawning(float DeltaSeconds, float Rate, int32 MaxAliveOverride = -1);

	// ---- Extraction ----------------------------------------------------------------------
	//
	// The GameMode owns WHEN and WHERE; AKBExtractionZone owns the two clocks. The zone never
	// calls EndRun or looks the GameMode up - it broadcasts, and these handlers decide.

	/** Opens the zone at the scheduled wave's Explore phase and steps the pressure up. */
	void OpenExtraction();

	/** The team held the circle: the run ends the same way a wipe does, from the same funnel. */
	UFUNCTION()
	void HandleExtractionComplete();

	/** The open window ran out. The run CONTINUES; the zone is only put back on the schedule. */
	UFUNCTION()
	void HandleExtractionWindowExpired();

	/** The team committed - everybody is in and the countdown is running. Answers with a wave. */
	UFUNCTION()
	void HandleExtractionStarted();

	/** Server-only. Sets which wave the zone opens in; -1 disables it for the rest of the run. */
	void ScheduleExtractionForWave(int32 WaveIndex);

	/** Where the zone may appear. Replace THIS when rooms exist; nothing else needs to change. */
	TArray<FVector> BuildExtractionCandidates() const;

	/** The candidate farthest from the nearest player, so it is never a free walk away. */
	FVector SelectExtractionLocation() const;

	/** Opens the draft for the given wave. See the phase order in EKBWavePhase. */
	void BeginCardDraft(int32 ForWaveIndex);
	void ApplyCardEffect(class AKBPlayerState* PlayerState, const class UKBCardDefinition& Card);
	bool AnyPlayerAwaitingPick() const;
	void AutoResolveRemainingPicks();

	/** True only when there is at least one player and every one of them is down. */
	bool AreAllPlayersDowned() const;

	/** Brings anyone downed back at full health; called at the start of each wave. */
	// ReviveDownedPlayers() used to be here - it stood every downed player back up at the start
	// of each wave. Rescue is now a teammate standing in the circle a downed player projects
	// (UKBChannelComponent on AKBCharacter), so there is nothing left for the GameMode to do
	// about it and the function was deleted.
};
