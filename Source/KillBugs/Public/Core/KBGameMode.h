#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "KBGameMode.generated.h"

class AKBEnemyDirector;

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

protected:
	/** Monotonic counter feeding AKBPlayerState::KBPlayerIndex. */
	int32 NextPlayerIndex = 0;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Swarm")
	TSubclassOf<AKBEnemyDirector> EnemyDirectorClass;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Combat")
	TSubclassOf<class AKBProjectileDirector> ProjectileDirectorClass;

	UPROPERTY(BlueprintReadOnly, Category = "KillBugs|Swarm")
	TObjectPtr<AKBEnemyDirector> EnemyDirector;

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
	void TickSpawning(float DeltaSeconds, float Rate);

	/** Opens the draft for the given wave. See the phase order in EKBWavePhase. */
	void BeginCardDraft(int32 ForWaveIndex);
	void ApplyCardEffect(class AKBPlayerState* PlayerState, const class UKBCardDefinition& Card);
	bool AnyPlayerAwaitingPick() const;
	void AutoResolveRemainingPicks();

	/** True only when there is at least one player and every one of them is down. */
	bool AreAllPlayersDowned() const;

	/** Brings anyone downed back at full health; called at the start of each wave. */
	void ReviveDownedPlayers();
};
