#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KBExtractionZone.generated.h"

class UKBChannelComponent;

/** Fired on the server when the open window runs out. The run continues; the GameMode reschedules. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FKBOnExtractionWindowExpired);

/**
 * Fired once per attempt, the moment the gate first opens - i.e. when the players commit.
 *
 * Once per ATTEMPT, not per gate edge: the teams steps out and back in freely (that is what the
 * pause policy is for), and answering every re-entry with a fresh wave would make walking in and
 * out a way to farm bugs.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FKBOnExtractionStarted);

/**
 * The extraction zone: a spot the whole team has to hold, with two clocks that never run at once.
 *
 *   OpenWindow    runs while the gate is NOT satisfied, and running out means the attempt FAILED:
 *                 the zone closes, the run continues, and the GameMode puts it back on the
 *                 schedule. It is the team's budget of "time spent not all being in the circle".
 *   Progress      runs while the gate IS satisfied (every player alive and inside). Filling it
 *                 means the team got out.
 *
 * They are mutually exclusive by construction: while the gate is open the window is frozen, so the
 * whole episode can never take longer than OpenWindow + Duration. That is what lets the caller
 * size a single phase to fit it (see AKBGameMode::BeginExplore) instead of straddling phases.
 *
 * Without the window the "pause instead of reset" break policy would cost nothing - a team could
 * stand outside the circle forever and walk in whenever it liked. The window is what gives leaving
 * a price.
 *
 * The zone itself is deliberately dumb about what any of it means: it broadcasts and the GameMode
 * decides, exactly as AKBEnemyDirector does. It does not call EndRun and does not look the GameMode
 * up. It also does not re-derive the gate - it asks the channel, so "who counts as inside" has one
 * definition (and one place that knows a downed player does not).
 */
UCLASS()
class KILLBUGS_API AKBExtractionZone : public AActor
{
	GENERATED_BODY()

public:
	AKBExtractionZone();

	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * Opens the zone at Centre. Server only.
	 *
	 * Refuses if it is already open: a fresh opening must go through CloseZone first, because
	 * SetChannelActive clears progress only on an actual edge - calling SetChannelActive(true) on
	 * an already-active channel is a no-op and the previous progress would carry over.
	 */
	void OpenZone(const FVector& Centre, float WindowSeconds, float ProgressSeconds, float Radius);

	/** Closes the zone. Server only. bExpired broadcasts OnExtractionWindowExpired. */
	void CloseZone(bool bExpired);

	UFUNCTION(BlueprintPure, Category = "KillBugs|Extraction")
	bool IsZoneOpen() const;

	UFUNCTION(BlueprintPure, Category = "KillBugs|Extraction")
	FVector GetZoneCentre() const { return ZoneCentre; }

	UFUNCTION(BlueprintPure, Category = "KillBugs|Extraction")
	float GetZoneRadius() const { return ZoneRadius; }

	/** The window's full length, for the HUD's bar. */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Extraction")
	float GetOpenWindowSeconds() const { return OpenWindowSeconds; }

	/**
	 * What is left of the window.
	 *
	 * Frozen while the gate is open, so the HUD does not need a separate "is it frozen" flag: it
	 * reads Channel->IsAdvancing(), and this number simply stops moving.
	 */
	UFUNCTION(BlueprintPure, Category = "KillBugs|Extraction")
	float GetOpenWindowRemaining() const { return OpenWindowRemaining; }

	UKBChannelComponent* GetChannel() const { return Channel; }

	/** Server-side only, and only while the gate is closed. */
	UPROPERTY(BlueprintAssignable, Category = "KillBugs|Extraction")
	FKBOnExtractionWindowExpired OnExtractionWindowExpired;

	/** Server-side only. The team committed: the gate opened for the first time this attempt. */
	UPROPERTY(BlueprintAssignable, Category = "KillBugs|Extraction")
	FKBOnExtractionStarted OnExtractionStarted;

protected:
	UFUNCTION()
	void OnRep_ZoneCentre();

	UFUNCTION()
	void OnRep_ZoneRadius();

	/**
	 * The shared zone primitive, configured for extraction: every player, alive, inside, and
	 * progress kept (not reset) when somebody steps out.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "KillBugs|Extraction")
	TObjectPtr<UKBChannelComponent> Channel;

	UPROPERTY(ReplicatedUsing = OnRep_ZoneCentre, BlueprintReadOnly, Category = "KillBugs|Extraction")
	FVector ZoneCentre = FVector::ZeroVector;

	/**
	 * Replicated even though the channel has a Radius of its own, because the channel's is
	 * EditDefaultsOnly and is NOT replicated. A client would otherwise draw (and measure) the ring
	 * at the CDO's default radius. OnRep_ZoneRadius copies this into the channel so the two agree.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_ZoneRadius, BlueprintReadOnly, Category = "KillBugs|Extraction")
	float ZoneRadius = 350.f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Extraction")
	float OpenWindowSeconds = 0.f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Extraction")
	float OpenWindowRemaining = 0.f;

private:
	/** Edge-tracking for the freeze/resume log, and to know which clock is running. */
	bool bWindowFrozen = false;

	/** Whether OnExtractionStarted has fired for the current attempt. Cleared by OpenZone. */
	bool bStartAnnounced = false;

	/** Highest progress milestone already logged (0..4, i.e. 0/25/50/75/100%). Logging only. */
	int32 LastProgressMilestone = -1;
};
