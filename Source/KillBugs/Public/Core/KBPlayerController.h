#pragma once

#include "CoreMinimal.h"
#include "Core/KBGameState.h"
#include "GameFramework/PlayerController.h"
#include "KBPlayerController.generated.h"

class UInputMappingContext;
class UInputAction;

/**
 * Player controller. Owns the cursor-to-world aiming contract and, from Phase 5, the
 * replicated aim yaw and the manual-weapon fire RPC.
 *
 * Controllers default to bOnlyRelevantToOwner, so the replicated AimYaw below costs one
 * connection per player rather than one per client - which is why aim is a replicated
 * property and not a per-frame RPC.
 */
UCLASS()
class KILLBUGS_API AKBPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AKBPlayerController();

	UFUNCTION(BlueprintPure, Category = "KillBugs|Aim")
	float GetAimYaw() const { return AimYaw; }

	/** Server-authoritative. Client calls are ignored; Phase 5 routes this through an RPC. */
	UFUNCTION(BlueprintCallable, Category = "KillBugs|Aim")
	void SetAimYaw(float InAimYaw);

	/** The pawn binds this to drive manual weapons. May be null if setup failed. */
	UInputAction* GetFireAction() const { return FireAction; }

	/**
	 * Card draft pick. Reliable, because losing it would stall the run until the draft times
	 * out and the server picked for them.
	 */
	UFUNCTION(Server, Reliable, WithValidation, BlueprintCallable, Category = "KillBugs|Cards")
	void ServerPickCard(int32 ChoiceIndex);

	/**
	 * If a draft is open and the cursor is over a card, picks it.
	 *
	 * Driven from the pawn's existing fire binding rather than a separate click handler: that
	 * binding is known to work, whereas a fresh unbound-key check depends on the input system
	 * tracking a key nothing has declared an interest in.
	 */
	bool TryPickCardUnderCursor();

	/**
	 * This machine's run is over: fold its earnings into the local profile and write them down.
	 *
	 * Bound to AKBGameState::OnWavePhaseChanged, which fires on the host (broadcast locally by
	 * SetWavePhaseServer) and on a client (broadcast by OnRep_WavePhase) - so one handler covers
	 * both, and the listen server is not a special case.
	 */
	UFUNCTION()
	void HandleWavePhaseChanged(EKBWavePhase NewPhase);

protected:
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Builds FireAction and its mapping context in code; see the member comments. */
	void BuildRuntimeInput();

	/**
	 * Binds HandleWavePhaseChanged once the GameState exists.
	 *
	 * Retried from Tick rather than done in BeginPlay, because on a client the GameState can
	 * arrive after the controller begins play - and a missed bind would silently mean "this
	 * machine never banks its gold", which is exactly the class of failure that looks like
	 * nothing happening.
	 */
	void TryBindToRunState();

	bool bBoundToRunState = false;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "KillBugs|Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	/**
	 * Fire input, constructed at runtime rather than authored as assets.
	 *
	 * UInputMappingContext::MapKey is a runtime BlueprintCallable, so a mapping context and
	 * its action can be built in code - which avoids hand-authoring two more assets and
	 * re-authoring them whenever the binding changes. Replace with real assets if the project
	 * ever needs remappable bindings.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> FireAction;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> RuntimeMappingContext;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Aim")
	float AimYaw = 0.f;
};
