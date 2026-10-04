#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "KBGoreComponent.generated.h"

class UAudioComponent;
class UDecalComponent;
class UMaterialInstanceDynamic;
class UKBEnemyArchetype;
class AKBEnemyDirector;

/**
 * What a dead bug leaves behind: a burst where it died, and slime on the ground afterwards.
 *
 * Driven from a LOCAL observation, not from a replicated event.
 *
 * A death is not replicated as an event - over the network it is only the removal of an item
 * from the swarm's replicated array. What every machine does have at that moment is
 * UKBEnemyVisualizerComponent's view of that bug: it purges the view when the bug stops being
 * replicated, and until the purge the view still holds the bug's last drawn position and its
 * archetype. That is everything the splat needs, so the effect costs no bandwidth at all -
 * the same reasoning as UKBAudioSubsystem deriving its music from a replicated phase instead
 * of replicating audio.
 *
 * The cost of deriving rather than being told: the random jitter (decal angle and size) is
 * rolled per machine, so two players see slightly different puddles. Both are cosmetic and
 * both mark the same spot at the same moment.
 *
 * What this class owns is the part that must be bounded - a bug dies somewhere in the arena
 * hundreds of times a wave, so both the decals and the bursts need a ceiling.
 */
UCLASS(ClassGroup = (KillBugs), meta = (BlueprintSpawnableComponent))
class KILLBUGS_API UKBGoreComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UKBGoreComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * A bug has stopped being drawn. Location is where it was last seen, ArchetypeIndex is its
	 * archetype.
	 *
	 * Called for EVERY purge the visualizer does, including the ones that are not deaths, so
	 * the filtering lives here rather than in the caller - see the implementation.
	 */
	void OnBugDied(const FVector& Location, int32 ArchetypeIndex);

	/** How many decals are currently on the ground. For the perf log and for tests. */
	int32 GetActiveDecalCount() const;

	// ---- Budgets. See DefaultEngine.ini's "death/hit VFX pooling budget" note --------------

	/**
	 * Hard ceiling on slime puddles. The oldest is recycled once it is reached.
	 *
	 * This is the number that keeps a long run playable: the arena is repainted constantly and
	 * nothing else ever removes a decal, so without a cap the count is just "how many bugs have
	 * died this session".
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Gore", meta = (ClampMin = "0"))
	int32 MaxDecals = 64;

	/** How long a puddle stays before it has faded away completely. */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Gore", meta = (ClampMin = "0.5"))
	float DecalLifetimeSeconds = 20.f;

	/** The last stretch of that lifetime over which it fades, so it never pops out. */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Gore", meta = (ClampMin = "0.0"))
	float DecalFadeSeconds = 3.f;

	/**
	 * Niagara bursts allowed per frame.
	 *
	 * r.Niagara.MaxSystemInstances caps how many systems may EXIST, but it does not stop a
	 * frame from trying to start hundreds at once - and a wave ending does exactly that. This
	 * is the per-frame gate DefaultEngine.ini has been claiming exists since the swarm VFX
	 * work; the decals are deliberately not gated by it, because a puddle is cheap and a
	 * censored puddle is a visible omission.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Gore", meta = (ClampMin = "0"))
	int32 MaxSplatsPerFrame = 8;

	/**
	 * Death sounds allowed per frame, budgeted apart from MaxSplatsPerFrame.
	 *
	 * The two costs are not comparable: a Niagara system is a few hundred particles that the
	 * renderer batches, while a voice is a mixer channel. A wave ending culls hundreds of bugs
	 * in one frame, and eight particles systems in that frame is a spectacle whereas eight
	 * overlapping death cries is a mess - so this is the smaller number by design.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Gore", meta = (ClampMin = "0"))
	int32 MaxDeathSoundsPerFrame = 3;

	/**
	 * How far from the local listener a death is still worth a voice.
	 *
	 * Set well beyond the movement sounds' radius on purpose: a death across the arena is a
	 * useful piece of information ("the wave is thinning over there"), while a movement sound
	 * that far away is only noise. This is also the main voice-budget saving - most deaths in a
	 * 450-bug wave happen off-screen.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Gore", meta = (ClampMin = "1.0"))
	float MaxDeathSoundDistance = 4000.f;

	/**
	 * Concurrent death voices, which is the real ceiling - MaxDeathSoundsPerFrame only stops a
	 * single frame from spiking.
	 *
	 * When every voice is busy a death is DROPPED rather than cutting a sound already playing.
	 * A missing death cry in a crowd is unnoticeable; one that stops half way through is not.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "KillBugs|Gore", meta = (ClampMin = "0"))
	int32 MaxDeathSoundVoices = 8;

protected:
	/** Builds the decal pool on first use. */
	void EnsureDecalPool(const AKBEnemyDirector* Director);

	/**
	 * Claims a pool slot - a free one, or the oldest once the pool is saturated.
	 *
	 * Returns an index rather than the component because the caller needs the slot to reach
	 * this puddle's dynamic material too.
	 */
	int32 AcquireDecalSlot();

	/** Drives each live decal's fade and retires the expired ones. */
	void UpdateDecals();

	/** Builds the death-voice pool on first use. */
	void EnsureDeathSoundPool();

	/**
	 * A voice that has finished, or INDEX_NONE when they are all still sounding.
	 *
	 * Voice count is bounded here rather than by a counter: a component that has stopped is
	 * provably free, whereas a count would have to be kept in step with sounds of unknown
	 * length.
	 */
	int32 AcquireDeathSoundSlot();

	/**
	 * Plays an archetype's death sound at Location, if there is a voice and a listener for it.
	 *
	 * Split out of OnBugDied because the two budgets that gate it - the per-frame cap and the
	 * voice pool - are audio's own, and mixing them into the Niagara and decal section above is
	 * how a budget ends up gating the wrong thing.
	 */
	void PlayDeathSound(const UKBEnemyArchetype& Archetype, const FVector& Location);

	UPROPERTY(Transient)
	TArray<TObjectPtr<UDecalComponent>> DecalPool;

	/**
	 * One dynamic material per slot, kept rather than fetched back off the component.
	 *
	 * CreateDynamicMaterialInstance does set the result as the decal's material, so it could be
	 * recovered by casting GetDecalMaterial() - but that relies on the engine keeping the two in
	 * step, and the fade writes to this every frame.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> DecalMIDs;

	/** World time each slot was claimed at; a negative value means the slot is free. */
	TArray<float> DecalSpawnTimes;

	/** The pool is a ring: when every slot is busy this one is the oldest, and gets recycled. */
	int32 NextDecalSlot = 0;

	int32 ActiveDecals = 0;

	/** Splats started this frame, reset at the top of every tick. */
	int32 SplatsThisFrame = 0;

	/**
	 * Death voices, pooled exactly like the decals and for the same reason: a bug dies hundreds
	 * of times a wave, and creating an audio component per death would be the per-actor churn
	 * this project avoids everywhere else.
	 *
	 * Each one has its attenuation switched off so the volume curve is this component's own -
	 * see the note in EnsureDeathSoundPool.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAudioComponent>> DeathSoundPool;

	/** Death voices started this frame, reset at the top of every tick. */
	int32 DeathSoundsThisFrame = 0;
};
