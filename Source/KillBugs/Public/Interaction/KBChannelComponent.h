#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "KBChannelComponent.generated.h"

/**
 * Who has to be standing in the zone for it to count.
 *
 * An enum rather than a delegate or a virtual override because the three things this is for -
 * rescuing a teammate, extracting as a team, opening a door - differ only in this predicate and
 * in the break policy below. A predicate that has to be implemented in C++ per use would mean the
 * zone, the radius test, the timer and the replication all get rewritten each time, which is the
 * duplication this class exists to prevent.
 */
UENUM(BlueprintType)
enum class EKBChannelGate : uint8
{
	/** At least one living player. Doors, switches, anything one person can do. */
	AnyPlayer        UMETA(DisplayName = "Any player"),

	/**
	 * At least one living player who is not this component's owner.
	 *
	 * The owner's own pawn does NOT count, which is what makes this the rescue gate: a downed
	 * player lying inside their own rescue zone must not be able to revive themselves.
	 */
	AnyOtherPlayer   UMETA(DisplayName = "Any other player"),

	/**
	 * Every player in the game, all of them alive and all of them inside.
	 *
	 * The extraction gate, and the "alive" half is not decoration: a downed player cannot walk
	 * into the zone, so requiring everyone to be up is what forces the team to rescue their
	 * teammate before they can leave.
	 */
	AllPlayersAliveInside UMETA(DisplayName = "All players, alive, inside")
};

/** What happens to the progress when the gate stops being satisfied. */
UENUM(BlueprintType)
enum class EKBChannelBreak : uint8
{
	/** Progress is kept; it simply stops advancing. Extraction. */
	Pause UMETA(DisplayName = "Pause"),

	/**
	 * Progress is thrown away. Rescue.
	 *
	 * Deliberately the opposite policy to extraction. Four people keeping themselves inside one
	 * circle is hard and a pause is the only humane answer; one person stepping away from a
	 * body they were reviving is a decision, and restarting it is what makes that decision cost
	 * something.
	 */
	Reset UMETA(DisplayName = "Reset")
};

/** Fired once when the channel reaches its duration. Server-side. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FKBOnChannelComplete);

/**
 * "Stand in the zone for N seconds while a condition holds, then something happens."
 *
 * The shared shape behind rescuing a teammate, extracting from a run, and (later) opening a room.
 * Built once because all three are otherwise the same code with different words in it: a radius
 * test, a timer, a policy for what happens when the condition breaks, and enough replication that
 * every player can see the progress.
 *
 * Inactive by default. A component that starts counting the moment it exists would have a downed
 * player's rescue zone ticking before they have fallen over, so the owner decides when it is live
 * - see SetChannelActive.
 */
UCLASS(ClassGroup = (KillBugs), meta = (BlueprintSpawnableComponent))
class KILLBUGS_API UKBChannelComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UKBChannelComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** How far from this component's origin a player still counts as inside. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Channel", meta = (ClampMin = "1.0"))
	float Radius = 300.f;

	/** How long the gate must hold before OnChannelComplete fires. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Channel", meta = (ClampMin = "0.1"))
	float DurationSeconds = 4.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Channel")
	EKBChannelGate Gate = EKBChannelGate::AnyPlayer;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Channel")
	EKBChannelBreak BreakPolicy = EKBChannelBreak::Pause;

	/** Fired on the server when the channel completes. */
	UPROPERTY(BlueprintAssignable, Category = "KillBugs|Channel")
	FKBOnChannelComplete OnChannelComplete;

	/**
	 * Turns the channel on or off. Server only.
	 *
	 * Deactivating clears the progress: a zone that is switched off and on again should start
	 * over, not resume something the player has long since walked away from.
	 */
	UFUNCTION(BlueprintCallable, Category = "KillBugs|Channel")
	void SetChannelActive(bool bInActive);

	UFUNCTION(BlueprintPure, Category = "KillBugs|Channel")
	bool IsChannelActive() const { return bActive; }

	/** 0..1, for the HUD's ring. Replicated, so clients can draw it. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Channel")
	float GetProgress() const { return Progress; }

	/** True while the gate is satisfied and the timer is running. Drives the HUD's wording. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Channel")
	bool IsAdvancing() const { return bAdvancing; }

	/**
	 * Whether the gate is satisfied right now.
	 *
	 * Public because the extraction HUD wants to say "waiting for everyone" without needing the
	 * timer to be running yet.
	 */
	bool EvaluateGate() const;

protected:
	/** Turns the gate's answer into progress, applying BreakPolicy when it says no. */
	void Advance(float DeltaTime);

	/** True when this player counts as inside for the gate's purposes. */
	bool IsPlayerInsideAndCounts(const class AKBPlayerState& PlayerState) const;

	/**
	 * Writes where every other player is, relative to this zone, at the moment it opens.
	 *
	 * A gate that never opens and a channel that never ticked produce the same nothing in a log,
	 * so the distance is written down at the one moment it is cheap and the answer is most
	 * useful. See the implementation for what it is for.
	 */
	void LogWhoIsInRange() const;

	/**
	 * Runs on clients when the active flag arrives.
	 *
	 * Exists because "the ring does not appear on a client" has two completely different causes -
	 * the flag never replicated, or the HUD never found the component - and from the outside they
	 * look identical. This line tells them apart, and it is the only client-side evidence
	 * available: DrawHUD does not run headlessly, so the HUD itself cannot be asked.
	 */
	UFUNCTION()
	void OnRep_Active();

	UPROPERTY(ReplicatedUsing = OnRep_Active)
	bool bActive = false;

	UPROPERTY(Replicated)
	float Progress = 0.f;

	/**
	 * Replicated rather than derived on the client: a client can work out whether its own eyes
	 * say the gate is satisfied, but not whether the SERVER agreed, and the two disagreeing is
	 * exactly what a laggy "why is the bar not moving" complaint is made of.
	 */
	UPROPERTY(Replicated)
	bool bAdvancing = false;
};
