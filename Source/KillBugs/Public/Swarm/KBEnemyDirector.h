#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Swarm/KBFlowField.h"
#include "Swarm/KBSwarmTypes.h"
#include "KBEnemyDirector.generated.h"

class UKBEnemyArchetype;
class UKBEnemyVisualizerComponent;
class UKBGoreComponent;
class UKBSwarmAudioComponent;
class AKBPlayerState;
class AKBLootDirector;

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

	/**
	 * Stops the swarm where it stands: no steering, no separation, no biting.
	 *
	 * Used when a run ends. Freezing rather than culling is deliberate - see the note in Tick.
	 * Nothing about it needs replicating, because clients never simulate: they render the
	 * replicated array, which simply stops changing.
	 */
	void SetSimulationFrozen(bool bInFrozen) { bSimulationFrozen = bInFrozen; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Swarm")
	float GetEnemyDamageTakenScale() const { return EnemyDamageTakenScale; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Swarm")
	float GetEnemySpeedScale() const { return EnemySpeedScale; }

	// ---- Flow field (server only) --------------------------------------------------------

	/**
	 * Marks the swarm's obstacle mask stale; the next tick re-bakes it from the world.
	 *
	 * THE call the maze cut makes when it has finished building geometry. It is not optional and
	 * its absence is silent: everything keeps running, and the field quietly describes the arena
	 * it was baked from. Call it unconditionally at the end of a build, never conditionally.
	 */
	void InvalidateFlowObstacles();

	// ---- Flow field diagnostics ----------------------------------------------------------
	// Public so the console commands and the self test can drive the field directly. Reading a
	// field by simulating 30 bugs and watching where they end up is not a test anybody can
	// debug; these let a caller ask the field a question and get an answer.

	int32 GetFlowCellsX() const { return FlowField.GetCellsX(); }
	int32 GetFlowCellsY() const { return FlowField.GetCellsY(); }
	float GetFlowCellSize() const { return FlowField.GetCellSize(); }
	FVector GetFlowCellCentre(int32 X, int32 Y) const { return FlowField.GetCellCentre(X, Y); }
	int32 GetFlowBlockedCellCount() const { return FlowField.GetBlockedCellCount(); }
	int32 GetFlowReachableCellCount() const { return FlowField.GetReachableCellCount(); }
	FIntPoint GetFlowCellFor(const FVector& Location) const;

	bool FlowLineOfSightIsClear(const FVector& Start, const FVector& End) const
	{
		return FlowField.LineOfSightIsClear(Start, End);
	}

	bool IsFlowBlockedAt(const FVector& Location) const { return FlowField.IsBlockedAt(Location); }

	void InjectFlowBlockedCells(const TArray<FIntPoint>& Cells) { FlowField.InjectBlockedCells(Cells); }
	void ClearFlowObstacles() { FlowField.ClearObstacles(); }

	/** Bakes the mask from the world right now and re-solves. Returns the blocked-cell count. */
	int32 BakeFlowObstaclesNow();

	/** Re-collects the sources and re-solves, without touching the obstacle mask. */
	void RebuildFlowFieldNow();

	/**
	 * Spawns a blocking box the flow field can see, and returns it.
	 *
	 * The demo and test path: a wall that exists only for this session, so routing can be watched
	 * in a window instead of inferred from a log. Movable mobility because a Static actor cannot
	 * be scaled after spawn - and it still blocks ECC_WorldStatic, which is all the bake asks.
	 */
	AActor* SpawnFlowTestWall(const FVector& Centre, const FVector& Extent);
	void DestroyFlowTestWalls();

	/** Runs the bake's query at one point and reports every channel and every thing it hits. */
	void ProbeFlowQuery(const FVector& Location);

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

	/**
	 * Where the bugs are actually being DRAWN. Read by the swarm audio component, which has to
	 * follow the interpolated position rather than the replicated one - see GetViews.
	 */
	UKBEnemyVisualizerComponent* GetVisualizer() const { return Visualizer; }

	/**
	 * Where deaths are turned into loot. Set once by AKBGameMode::BeginPlay.
	 *
	 * A weak pointer and an injected dependency rather than a lookup, because the lookup would
	 * have to happen on every death and the answer never changes.
	 *
	 * Defined in the .cpp: assigning to a TWeakObjectPtr needs the complete type, and this
	 * header only forward-declares it.
	 */
	void SetLootDirector(AKBLootDirector* InLootDirector);

	// ---- Console commands (public so the delegates can bind) -----------------------------

	// FConsoleCommandWithWorldAndArgsDelegate takes only (Args, World) - there is no
	// FOutputDevice parameter, so results are reported through UE_LOG instead.
	static void ConsoleSetSwarmCount(const TArray<FString>& Args, UWorld* World);
	static void ConsoleKillEnemies(const TArray<FString>& Args, UWorld* World);
	static void ConsoleCullSwarm(const TArray<FString>& Args, UWorld* World);
	static void ConsoleFlowRebake(const TArray<FString>& Args, UWorld* World);
	static void ConsoleFlowProbe(const TArray<FString>& Args, UWorld* World);
	static void ConsoleFlowWall(const TArray<FString>& Args, UWorld* World);
	static void ConsoleFlowWallClear(const TArray<FString>& Args, UWorld* World);
	static void ConsoleFlowSelfTest(const TArray<FString>& Args, UWorld* World);

protected:
	/** The whole swarm. One property on one always-relevant actor. */
	UPROPERTY(Replicated)
	FKBEnemyArray ReplicatedEnemies;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Swarm")
	TObjectPtr<UKBEnemyVisualizerComponent> Visualizer;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Swarm")
	TObjectPtr<UKBGoreComponent> Gore;

	/**
	 * Bug movement sound. The third sibling, and the one that is purely local: the visualizer
	 * says where the bugs are drawn, and this turns the nearest few into voices.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Swarm")
	TObjectPtr<UKBSwarmAudioComponent> SwarmAudio;

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

	/**
	 * Height the obstacle bake probes at. Above the floor (a plane at Z=0), below the top of the
	 * arena's 400-tall walls - 100 is in the middle of both, and the swarm simulates at Z=0, so
	 * this is a property of the probe rather than of the bugs. Cell size and rebuild cadence are
	 * in UKBGameSettings, where they can actually be changed (this actor is spawned in code).
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Swarm")
	float FlowProbeHeight = 100.f;

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

	/**
	 * Which way the nearest player is, for bugs that have a wall in the way. See KBFlowField.h.
	 *
	 * The separate grid from the separation one is deliberate: 200 cm cells are fine for a
	 * neighbour query and too coarse for a corridor, and the two are rebuilt on cadences three
	 * orders of magnitude apart (every frame vs ten times a second).
	 */
	FKBFlowField FlowField;
	float FlowRebuildAccumulator = 0.f;

	/** Reused so collecting the field's sources never allocates. */
	TArray<FVector> FlowSourceScratch;

	/** Test walls spawned by KB.Swarm.FlowWall / KB.Swarm.FlowSelfTest, so they can be cleared. */
	UPROPERTY()
	TArray<TObjectPtr<AActor>> FlowTestWalls;

	int32 NextStableId = 1;
	int32 CurrentWaveIndex = 0;

	float NetSyncAccumulator = 0.f;
	uint32 NetSyncFrame = 0;

	/** Reused across frames so the separation query never allocates. */
	TArray<int32> SeparationScratch;

	// Perf log accumulators; driven by CVarKBSwarmPerfLog (see KBConsoleVariables.h).
	double PerfSimSeconds = 0.0;
	double PerfNetSeconds = 0.0;
	double PerfFlowSeconds = 0.0;

	/**
	 * How many bugs took the field branch since the last log flush.
	 *
	 * The number that answers "is the field doing anything at all" - on the flat arena it should
	 * read 0, because nothing ever blocks a sight line. Meaningless unless KB.Swarm.PerfLog > 0,
	 * since it is reset when a line is printed.
	 */
	int32 PerfFlowRouted = 0;

	int32 PerfFrames = 0;
	float PerfElapsed = 0.f;

	/** See SetEnemyScaling. Baselines of 1 mean "exactly the archetype's numbers". */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Combat")
	float EnemyDamageTakenScale = 1.f;

	/** Set when the run ends. See SetSimulationFrozen. */
	bool bSimulationFrozen = false;

	/** Weak: the loot director belongs to the world, not to this actor. See SetLootDirector. */
	TWeakObjectPtr<AKBLootDirector> LootDirector;

	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Combat")
	float EnemySpeedScale = 1.f;

	void SimulateSwarm(float DeltaSeconds);
	void SyncReplication(float DeltaSeconds);

	/**
	 * Bakes the obstacle mask if it is stale, then re-solves on the configured cadence.
	 *
	 * Called from Tick BEFORE SimulateSwarm and outside its stat scope, so a tenth-of-a-second
	 * rebuild spike does not disappear into the per-frame simulation average. Deliberately not
	 * inside SimulateSwarm, which returns early when the swarm is empty - a test that drives an
	 * empty field still has to be able to rebuild it.
	 */
	void RefreshFlowField(float DeltaSeconds);

	/** Every valid player pawn's position, or the arena centre when there are none. */
	void CollectFlowSources();

	/** Gated by CVarKBSwarmFlowDebug, compiled out of shipping, and skipped when it cannot render. */
	void DrawFlowFieldDebug() const;

	/** Bites any player standing in the swarm. Runs on its own slower cadence. */
	void TickContactDamage(float DeltaSeconds);

	float ContactDamageAccumulator = 0.f;
	TArray<int32> ContactScratch;

	/** Nearest living player, or null. Re-validated on every think, never cached blindly. */
	AActor* SelectTargetFor(const FKBEnemySim& Enemy) const;

	/** Appends to Sim and ReplicatedEnemies together, preserving the index-parallel invariant. */
	int32 AddEnemy(const FVector& Location, int32 ArchetypeIndex);
};
