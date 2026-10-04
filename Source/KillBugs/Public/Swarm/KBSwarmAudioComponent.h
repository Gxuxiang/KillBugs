#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "KBSwarmAudioComponent.generated.h"

class UAudioComponent;
class USoundBase;
class AKBEnemyDirector;

/**
 * Bug movement sound, played from a small pool of emitters bound to the nearest bugs.
 *
 * The problem this exists to solve: the swarm is hundreds of instances with no actors, and
 * hundreds of looping voices is not an option - it would exhaust the audio engine's source
 * limit long before it ran out of bugs. What a player actually needs to hear is not "every bug"
 * but "the bugs closing on me and which side they are on", and that is a handful of sources.
 *
 * So MaxMoveEmitters (6 by default) audio components are bound each update to the nearest
 * in-range bugs to the LOCAL listener, moved to those bugs' drawn positions, and given a volume
 * from distance. A bug that is heard is a bug that is near; everything else is silent, which is
 * also what makes the mix legible.
 *
 * Driven from a LOCAL observation, like the visualizer and the gore component beside it: the
 * positions come from UKBEnemyVisualizerComponent::GetViews(), which every machine has, so the
 * sound costs no bandwidth and needs no replication.
 *
 * Positional but with the engine's attenuation OFF - see the note in EnsureEmitters. The volume
 * curve is this component's own, so a bug is audible exactly out to its archetype's
 * MoveSoundRadius and not one unit further.
 */
UCLASS(ClassGroup = (KillBugs), meta = (BlueprintSpawnableComponent))
class KILLBUGS_API UKBSwarmAudioComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UKBSwarmAudioComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Emitters currently bound to a bug. For the perf log and for tests. */
	int32 GetActiveEmitterCount() const;

	/** Emitters in the pool, bound or not. */
	int32 GetEmitterPoolSize() const { return Emitters.Num(); }

	// ---- Budgets. See DefaultEngine.ini's "death/hit VFX pooling budget" note --------------

	/**
	 * How many bugs can be audible at once.
	 *
	 * This is a hard cap on audio voices for the whole swarm, which is the number that keeps a
	 * 450-bug wave from turning into noise. Raising it buys localisation at the cost of voices
	 * the weapon and impact sounds also need.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Audio", meta = (ClampMin = "0"))
	int32 MaxMoveEmitters = 6;

	/**
	 * How much closer a bug must be to take an emitter away from the bug holding it.
	 *
	 * Without this the nearest-N set is recomputed from scratch every update, and two bugs at
	 * nearly the same distance trade places repeatedly - each trade restarting a voice, which is
	 * audible as a rattle. 1.3 means a challenger must be 30% closer to steal a slot, so ties
	 * resolve in favour of whoever has it and the set is stable in practice.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Audio", meta = (ClampMin = "1.0"))
	float ReassignHysteresis = 1.3f;

	/**
	 * How often the binding is reconsidered, in seconds.
	 *
	 * The bugs' positions are re-read every frame (cheap - it is a map walk), but the binding is
	 * not: a set chosen 10 times a second is indistinguishable from one chosen 60 times a
	 * second, and recomputing it every frame would only make the hysteresis work harder.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Audio", meta = (ClampMin = "0.0"))
	float UpdateInterval = 0.1f;

	/**
	 * Seconds for an emitter's volume to reach a new target.
	 *
	 * A bug crossing in and out of range, or a slot being handed over, changes the target
	 * abruptly. Fading rather than stepping is what stops that reading as a click.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Audio", meta = (ClampMin = "0.0"))
	float FadeSeconds = 0.25f;

protected:
	/** Creates the emitter pool on first use. */
	void EnsureEmitters();

	/** Re-picks which bugs are audible. Runs at UpdateInterval, not every frame. */
	void Rebind(AKBEnemyDirector* Director, const FVector& Listener);

	/**
	 * Follows each bound bug and fades towards its target volume. Runs EVERY frame.
	 *
	 * Separate from Rebind on purpose: which bug a slot is bound to is a decision worth making
	 * ten times a second, but where that bug is right now is not - a voice repositioned ten
	 * times a second would step audibly across the stereo field as the bug runs past.
	 */
	void UpdateEmitters(float DeltaTime, AKBEnemyDirector* Director, const FVector& Listener,
	                    bool bHasListener);

	/**
	 * Resolves an archetype's movement sound and binds it to a slot.
	 *
	 * A step of its own because a slot has to be filled from three places - the free-slot pass,
	 * the steal pass, and a rebind - and the archetype lookup plus the pitch roll is exactly the
	 * part that must not drift between them.
	 */
	void BindCandidateToSlot(int32 Slot, AKBEnemyDirector* Director, int32 StableId,
	                         int32 ArchetypeIndex);

	/** Points a slot at a bug, restarting the voice only if the sound actually changes. */
	void BindSlot(int32 Slot, int32 StableId, USoundBase* Sound, float Radius, float Volume, float Pitch);

	/**
	 * Writes the state of the whole system when KB.Swarm.AudioLog is on - see the cvar.
	 *
	 * Reads the counters Rebind left behind rather than recomputing anything, so the line costs
	 * nothing when it is off and cannot disagree with what Rebind actually decided.
	 */
	void TickAudioLog(float DeltaTime, const FVector& Listener, bool bHasListener);

	/** Hands a slot back. The component is kept; it fades out and stops. */
	void UnbindSlot(int32 Slot);

	/** Stops and unbinds every slot. Used when there is no listener at all. */
	void UnbindAll();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UAudioComponent>> Emitters;

	/** StableId of the bug each slot is bound to, or INDEX_NONE when the slot is free. */
	TArray<int32> BoundStableIds;

	/** The sound each slot is currently playing, so a rebind can tell "same voice, move it". */
	UPROPERTY(Transient)
	TArray<TObjectPtr<USoundBase>> BoundSounds;

	/** Distance at which the bound bug's sound reaches silence, from its archetype. */
	TArray<float> BoundRadii;

	/** Distance at bind time, refreshed on every rebind; drives the volume target. */
	TArray<float> BoundDistances;

	/** Pitch rolled once per binding - see MoveSoundPitchMin. */
	TArray<float> BoundPitches;

	/** Gain at full volume for the bound bug's archetype. */
	TArray<float> BoundVolumes;

	/** Current and target gain per slot; the fade runs every frame between rebinds. */
	TArray<float> CurrentGains;
	TArray<float> TargetGains;

	float TimeSinceUpdate = 0.f;

	/**
	 * Set once the pool has been built OR has been decided against.
	 *
	 * A plain "is the pool empty" test cannot tell those apart, and with MaxMoveEmitters at 0 the
	 * pool stays empty forever - so EnsureEmitters would re-warn on every single tick.
	 */
	bool bPoolInitialised = false;

	// ---- Diagnostics. Written by Rebind, read by TickAudioLog. ----------------------------

	float TimeSinceAudioLog = 0.f;

	/** Bugs the visualizer is drawing at all. */
	int32 DebugViewCount = 0;

	/** Bugs that passed every test and were eligible for a voice. */
	int32 DebugCandidateCount = 0;

	/** Bugs skipped because their archetype has no MoveSound - see the filter in Rebind. */
	int32 DebugNoSoundCount = 0;

	/** Smallest distance among bugs that were eligible except for being out of range. */
	float DebugNearestOutOfRange = -1.f;
};
