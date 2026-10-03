#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/KBGameState.h"
#include "KBAudioSubsystem.generated.h"

class UAudioComponent;

/**
 * Run music and UI one-shots.
 *
 * Music follows the replicated wave phase, and every machine polls its OWN GameState, so the
 * host and each client independently reach the same track without any audio replication.
 *
 * The two beds are kept alive as looping audio components and crossfaded by volume rather
 * than stopped and started: restarting a sustain every phase change would cut it off audibly.
 */
UCLASS()
class KILLBUGS_API UKBAudioSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	/** Card hover. Called only when the hovered card CHANGES, not every frame. */
	void PlayCardHover();

	/** Card picked. */
	void PlayCardPick();

	/** Draft popped open. */
	void PlayCardDraftOpen();

	// ---- Introspection, for the audio self-check ------------------------------------------

	/** True while a track is actually sounding. Used to verify the beds really sustain. */
	bool IsWaveMusicPlaying() const;
	bool IsExploreMusicPlaying() const;

	float GetWaveVolume() const { return WaveVolume; }
	float GetExploreVolume() const { return ExploreVolume; }

private:
	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> WaveMusicComponent;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> ExploreMusicComponent;

	/** Current and target gains, driven by the phase. */
	float WaveVolume = 0.f;
	float ExploreVolume = 0.f;
	float TargetWaveVolume = 0.f;
	float TargetExploreVolume = 0.f;

	/** Last phase acted on, so the transition work happens once per change. */
	EKBWavePhase LastPhase = EKBWavePhase::Warmup;
	bool bPhaseInitialised = false;

	/** Ensures a track is created once and then kept. */
	void EnsureMusicComponents();
	void ApplyPhase(EKBWavePhase Phase);

	/** Starts a component if it has stopped - see the note in Tick. */
	void KeepAlive(UAudioComponent* Component);
};
