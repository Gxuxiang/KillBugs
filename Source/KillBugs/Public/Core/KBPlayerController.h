#pragma once

#include "CoreMinimal.h"
#include "Core/KBGameState.h"
#include "GameFramework/PlayerController.h"
#include "Loot/KBLootTypes.h"
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

	/** The pawn binds this to use a medkit. May be null if setup failed. */
	UInputAction* GetUseItemAction() const { return UseItemAction; }

	/** The pawn binds these to use the hotbar slots. Any may be null if setup failed. */
	UInputAction* GetHotbarAction(int32 SlotIndex) const
	{
		return HotbarActions.IsValidIndex(SlotIndex) ? HotbarActions[SlotIndex] : nullptr;
	}

	/** The pawn binds this to open/close the backpack panel. */
	UInputAction* GetBackpackAction() const { return BackpackAction; }

	/** The pawn binds this to open a row's menu. */
	UInputAction* GetBackpackMenuAction() const { return BackpackMenuAction; }

	// ---- The hotbar ------------------------------------------------------------------------

	/**
	 * Slots for USABLE consumables, not for weapons. Weapons get their own bar later.
	 *
	 * Six for the same accidental reason the weapon list is capped at six, but the two numbers
	 * are unrelated - this one is fixed because the input bindings are written out one per slot,
	 * so a configurable count could disagree with the keys that exist.
	 */
	static constexpr int32 HotbarSlots = 6;

	/** Bounds check for the fixed-size array below. */
	static bool IsValidHotbarSlot(int32 SlotIndex) { return SlotIndex >= 0 && SlotIndex < HotbarSlots; }

	/** Which consumable a number key uses. False means the slot is empty. */
	bool GetHotbarSlot(int32 SlotIndex, EKBItemType& OutType) const;

	/** Puts an item in a slot, removing it from any other slot it occupied. */
	void SetHotbarSlot(int32 SlotIndex, EKBItemType Type);
	void ClearHotbarSlot(int32 SlotIndex);

	/** Swaps two slots outright, contents and all. */
	void SwapHotbarSlots(int32 A, int32 B);

	// ---- Click-to-move ---------------------------------------------------------------------
	//
	// One gesture, three outcomes: pick something up (a backpack row or a hotbar slot), then put
	// it down somewhere. Backpack -> slot assigns it to the key you chose; slot -> backpack takes
	// it off the bar; slot -> slot swaps the two. Doing it this way means "choose the key",
	// "unequip" and "reorder" are the same interaction rather than three.
	//
	// Public because the mouse handler is not the only caller - the console commands drive the
	// same functions, which is the only way any of this can be checked in a headless run.

	enum class EKBBackpackPick : uint8
	{
		None,
		BackpackRow,
		HotbarSlot
	};

	void PickHotbarSlot(int32 SlotIndex);

	/** Only rows that can be equipped are worth picking up; others are refused with a reason. */
	void PickBackpackRow(int32 RowIndex);

	void ClearBackpackPick();

	EKBBackpackPick GetBackpackPick(int32& OutIndex) const { OutIndex = PickedIndex; return PickedKind; }

	/** Puts whatever is picked into this slot. Returns whether anything moved. */
	bool ApplyPickToHotbarSlot(int32 SlotIndex);

	/** Takes whatever is picked off the bar. Returns whether anything moved. */
	bool ApplyPickToBackpack();

	// ---- Backpack panel --------------------------------------------------------------------

	bool IsBackpackOpen() const { return bBackpackOpen; }
	void ToggleBackpack() { bBackpackOpen = !bBackpackOpen; }

	/**
	 * Right-click: open the menu for whatever row is under the cursor.
	 *
	 * A no-op unless the panel is open and the cursor is over a row, so a stray right-click
	 * during a fight costs nothing.
	 */
	void OpenBackpackMenuUnderCursor();

	/**
	 * Left-click, tried BEFORE the trigger arms - the panel has to consume the click that
	 * chooses a menu entry, or picking an entry also fires the gun. Returns true if the click
	 * was the panel's, in which case the caller must not fire.
	 *
	 * Only a click that actually lands on something is consumed, so clicking empty space still
	 * shoots - the same bargain the card draft makes.
	 */
	bool TryHandleBackpackClick();

	/** Row whose menu is open, or INDEX_NONE. The HUD draws it; the click handler dispatches it. */
	int32 GetOpenBackpackMenuRow() const { return OpenBackpackMenuRow; }
	FVector2D GetBackpackMenuPosition() const { return BackpackMenuPosition; }

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

	/**
	 * The run's verdict: bank the materials, or say out loud that they are being lost.
	 *
	 * Gold is deliberately NOT handled here - it banks on either ending, in
	 * HandleWavePhaseChanged. Materials are the first thing in the game whose fate depends on
	 * which way the run ended, and that is the whole point of searching.
	 */
	UFUNCTION()
	void HandleRunResultChanged(EKBRunResult NewResult);

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

	/**
	 * Use-item input, built beside FireAction and for the same reasons.
	 *
	 * Q rather than a function key: the left hand is on WASD, so anything the player presses
	 * mid-fight should be reachable without moving it.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> UseItemAction;

	/**
	 * Six separate actions, one per number key.
	 *
	 * Deliberately NOT one Axis1D action mapped to six keys: every key would report the same
	 * value and the handler could not tell which digit was pressed. Distinguishing them that way
	 * needs a scalar modifier per key, which is the "one action with modifiers" design this file
	 * already rejects for fire and use.
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInputAction>> HotbarActions;

	UPROPERTY(Transient)
	TObjectPtr<UInputAction> BackpackAction;

	/** Right mouse. Opens a row's menu; never a trigger, so it needs no click arbitration. */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> BackpackMenuAction;

	/**
	 * The hotbar contents - LOCAL state, not replicated, and that is deliberate.
	 *
	 * It is a key mapping ("the 1 key uses a medkit"), not world state: using the item still goes
	 * to the server, which validates it against the backpack it owns. A server that knew which
	 * slot the player had assigned would learn nothing it could act on.
	 */
	TOptional<EKBItemType> Hotbar[HotbarSlots];

	bool bBackpackOpen = false;

	/** What the click-to-move gesture has picked up and is waiting to put down. */
	EKBBackpackPick PickedKind = EKBBackpackPick::None;
	int32 PickedIndex = INDEX_NONE;

	/** Which row's menu is open, and where it was drawn. Both are local HUD state. */
	int32 OpenBackpackMenuRow = INDEX_NONE;
	FVector2D BackpackMenuPosition = FVector2D::ZeroVector;

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> RuntimeMappingContext;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "KillBugs|Aim")
	float AimYaw = 0.f;
};
