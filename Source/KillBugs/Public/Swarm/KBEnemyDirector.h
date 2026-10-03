#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Swarm/KBSwarmTypes.h"
#include "KBEnemyDirector.generated.h"

class UKBEnemyArchetype;
class UKBEnemyVisualizerComponent;
class UKBGoreComponent;
class AKBPlayerState;

/**
 * The entire bug swarm, in one Actor.
 *
 * Server side it owns a flat array of plain structs and simulates them in a single Tick - no
 * Actor per bug, no CharacterMovementComponent, no physics, no navmesh, no per-actor tick.
 * The whole thing replicates as ONE FFastArraySerializer property.
 *
 * That is what makes 300-500 enemies viable with 4 players: there is no per-actor relevancy
 * pass, no actor channel churn, and a client joining mid-wave receives the swarm as a single
 * property update instead of hundreds of channel opens.
 */
UCLASS()
class KILLBUGS_API AKBEnemyDirector : public AActor
{
	GENERATED_BODY()

public:
	AKBEnemyDirector();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- Server API --------------------------------------------------------------------

	void SetWaveIndex(int32 InWaveIndex) { CurrentWaveIndex = InWaveIndex; }

	/** Replaces the live swarm with Count bugs scattered through an annulus around Centre. */
	void SpawnSwarm(int32 Count, const FVector& Centre, float SpawnRadius);

	/** Adds Count bugs to the live swarm without disturbing it. Used by test commands. */
	void SpawnBatch(int32 Count, const FVector& Centre, float SpawnRadius);

	/**
	 * Adds Count bugs at least MinDistance from every player, so nothing materialises in view.
	 *
	 * Centred on the players rather than on the arena: what matters is what is on screen, and
	 * that follows the player. Falls back to the arena centre when nobody is alive.
	 */
	void SpawnBatchOffscreen(int32 Count, float MinDistance, float MaxDistance);

	/** Average position of living players, or the arena centre if there are none. */
	FVector GetSwarmFocusLocation() const;

	/** Removes every remaining bug at once - the wave-clear path. */
	void CullAllRemaining();

	/**
	 * Damage entry point. Weapons never learn how enemies are represented; they come here.
	 *
	 * Rewards the killer on death, because this is the only place that still knows which
	 * archetype died - the sim entry is gone by the time the caller regains control.
	 * Returns true if this hit killed the bug.
	 */
	bool ApplyDamageToEnemy(int32 SimIndex, float Damage, AKBPlayerState* Killer = nullptr);

	// ---- Phase scaling -------------------------------------------------------------------

	/**
	 * Scales how much damage enemies take, and how fast they move, on top of their archetype
	 * stats. The GameMode sets this on every phase change.
	 *
	 * Implemented as an incoming-damage multiplier rather than by rewriting each bug's health:
	 * bugs spawned during a wave are still alive when Explore starts, so their health would
	 * have to be re-derived mid-flight, and any bug already wounded would need its current
	 * health rescaled too. A multiplier on incoming damage gets the same result with no
	 * per-bug bookkeeping.
	 */
	void SetEnemyScaling(float InDamageTakenScale, float InSpeedScale);

	UFUNCTION(BlueprintPure, Category = "KillBugs|Swarm")
	float GetEnemyDamageTakenScale() const { return EnemyDamageTakenScale; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Swarm")
	float GetEnemySpeedScale() const { return EnemySpeedScale; }

	// ---- Queries used by weapons ---------------------------------------------------------

	/**
	 * Nearest living bug within MaxRange, or INDEX_NONE.
	 *
	 * A linear scan rather than a grid walk: there are only ~500 bugs, and a weapon fires a
	 * few times a second, so this costs a few thousand distance checks per second in total.
	 * The grid earns its keep for the per-frame separation pass, not for this.
	 *
	 * The returned index is only valid until the next death, so callers must use it in the
	 * same frame - which every instant-hit weapon does.
	 */
	int32 FindNearestEnemy(const FVector& Origin, float MaxRange, FVector& OutLocation) const;

	/** Damages every bug whose centre is within Radius of Centre. Returns the kill count. */
	int32 ApplyRadialDamage(const FVector& Centre, float Radius, float Damage, AKBPlayerState* Killer = nullptr);

	/**
	 * Damages the first bug intersected by a swept sphere from Start to End.
	 *
	 * OutHitLocation, when supplied, receives where the hit landed - impact effects need a
	 * position and re-deriving it at the call site would mean duplicating the hit test.
	 */
	bool ApplyDamageAlongSegment(const FVector& Start, const FVector& End, float Radius, float Damage,
	                             AKBPlayerState* Killer = nullptr, FVector* OutHitLocation = nullptr);

	UFUNCTION(BlueprintPure, Category = "KillBugs|Swarm")
	int32 GetEnemyCount() const { return Sim.Num(); }

	// ---- Read access (valid on server AND client) ---------------------------------------

	const FKBEnemyArray& GetReplicatedEnemies() const { return ReplicatedEnemies; }
	TArrayView<const FKBEnemySim> GetSimView() const { return Sim; }
	const TArray<TObjectPtr<UKBEnemyArchetype>>& GetArchetypes() const { return Archetypes; }

	/**
	 * Death splatter and slime. The visualizer calls into this when a bug stops being drawn.
	 *
	 * A getter rather than the visualizer reaching for a sibling through GetOwner(): the two
	 * components should not have to agree on their own names.
	 */
	UKBGoreComponent* GetGoreComponent() const { return Gore; }

	// ---- Console commands (public so the delegates can bind) -----------------------------

	// FConsoleCommandWithWorldAndArgsDelegate takes only (Args, World) - there is no
	// FOutputDevice parameter, so results are reported through UE_LOG instead.
	static void ConsoleSetSwarmCount(const TArray<FString>& Args, UWorld* World);
	static void ConsoleKillEnemies(const TArray<FString>& Args, UWorld* World);

protected:
	/** The whole swarm. One property on one always-relevant actor. */
	UPROPERTY(Replicated)
	FKBEnemyArray ReplicatedEnemies;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Swarm")
	TObjectPtr<UKBEnemyVisualizerComponent> Visualizer;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Swarm")
	TObjectPtr<UKBGoreComponent> Gore;

	/**
	 * Bug types. Populated from the generated archetype assets in the constructor so Phase 2
	 * runs with zero editor steps; once a Blueprint subclass exists this should become a
	 * designer-editable list.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Swarm")
	TArray<TObjectPtr<UKBEnemyArchetype>> Archetypes;

	// ---- Simulation tuning ---------------------------------------------------------------
	//
	// Swarm capacity, arena size, contact damage and the grid are all in UKBGameSettings
	// (Project Settings -> Game -> KillBugs). What remains here is internal steering detail
	// that has no meaningful knob to turn.

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Swarm")
	float GridCellSize = 200.f;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Swarm")
	float SeparationRadius = 55.f;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Swarm")
	float SeparationStrength = 1.1f;

	// ---- Contact damage ------------------------------------------------------------------
	// Cadence, reach and the simultaneous-biter cap are in UKBGameSettings -> Combat|Contact.

	/** How far (cm) a bug must move before it is worth telling clients about. */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Net")
	float NetPositionThreshold = 2.f;

	/** How often the swarm property is refreshed. 15 Hz is the plan's bandwidth budget. */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Net")
	float NetSyncInterval = 1.f / 15.f;

	/**
	 * Only 1 in N bugs is considered per net update.
	 *
	 * THE main smoothness/bandwidth dial. At 15 Hz with stride 2, each bug's position updates
	 * at ~7.5 Hz, halving worst-case bandwidth (500 bugs x ~12 B x 15 Hz = 90 KB/s, right on
	 * the engine's default ceiling) at the cost of a longer interpolation window.
	 * Keep UKBEnemyVisualizerComponent::InterpolationTime roughly at
	 * NetSyncInterval * NetStaggerStride so the visual catches each update as the next lands.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Net", meta = (ClampMin = "1", ClampMax = "8"))
	int32 NetStaggerStride = 2;

private:
	TArray<FKBEnemySim> Sim;
	FKBEnemyGrid Grid;

	int32 NextStableId = 1;
	int32 CurrentWaveIndex = 0;

	float NetSyncAccumulator = 0.f;
	uint32 NetSyncFrame = 0;

	/** Reused across frames so the separation query never allocates. */
	TArray<int32> SeparationScratch;

	// Perf log accumulators; driven by CVarKBSwarmPerfLog (see KBConsoleVariables.h).
	double PerfSimSeconds = 0.0;
	double PerfNetSeconds = 0.0;
	int32 PerfFrames = 0;
	float PerfElapsed = 0.f;

	/** See SetEnemyScaling. Baselines of 1 mean "exactly the archetype's numbers". */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Combat")
	float EnemyDamageTakenScale = 1.f;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Combat")
	float EnemySpeedScale = 1.f;

	void SimulateSwarm(float DeltaSeconds);
	void SyncReplication(float DeltaSeconds);

	/** Bites any player standing in the swarm. Runs on its own slower cadence. */
	void TickContactDamage(float DeltaSeconds);

	float ContactDamageAccumulator = 0.f;
	TArray<int32> ContactScratch;

	/** Nearest living player, or null. Re-validated on every think, never cached blindly. */
	AActor* SelectTargetFor(const FKBEnemySim& Enemy) const;

	/** Appends to Sim and ReplicatedEnemies together, preserving the index-parallel invariant. */
	int32 AddEnemy(const FVector& Location, int32 ArchetypeIndex);
};
