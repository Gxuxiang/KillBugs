#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

/**
 * Seconds between swarm perf log lines; 0 disables them.
 *
 * A console variable rather than a per-actor setting on purpose: a client's director arrives
 * by replication, long after any -ExecCmds passed at startup has run, so anything set on the
 * instance would silently do nothing. Reading a cvar each frame works regardless of when the
 * swarm appears.
 *
 * Usage: KB.Swarm.PerfLog 2   (or feed it to -ExecCmds at launch)
 */
extern KILLBUGS_API TAutoConsoleVariable<float> CVarKBSwarmPerfLog;

/**
 * Seconds between swarm AUDIO log lines; 0 disables them.
 *
 * Exists because every failure mode of the swarm audio is silent in both senses: no sound plays,
 * and nothing is written anywhere saying why. Headless runs cannot hear anything either, so
 * without this the only evidence available is "I do not hear bugs", which does not distinguish
 * between no listener, no candidates in range, no sound assigned to the archetype, and the
 * emitter pool having decided to play something at a gain of 0.02.
 *
 * One line per interval reports all of those: the listener position, how many bugs were
 * candidates, why the nearest non-candidate was rejected, and the binding and gain of every
 * slot. A single line is usually enough to name the cause.
 *
 * Usage: KB.Swarm.AudioLog 1
 */
extern KILLBUGS_API TAutoConsoleVariable<float> CVarKBSwarmAudioLog;

/**
 * Testing switch: the player takes no damage at all.
 *
 * Usage: KB.Player.God 1
 *
 * A cvar rather than a UKBGameSettings entry on purpose. Everything in the settings is a design
 * knob - a value somebody chose - and this is not one; it is a tool for getting to the part of a
 * run you actually want to look at. Cvars are also the only debug switch this project exposes
 * that can be flipped mid-run from the console, which is the whole point of it.
 *
 * It is checked in UKBStatSheetComponent::ApplyDamage, which is the single funnel every source of
 * player damage already goes through - contact bites, and whatever lands later.
 */
extern KILLBUGS_API TAutoConsoleVariable<int32> CVarKBPlayerGod;

/**
 * Enables the swarm's flow field; 1 = on (default), 0 = off.
 *
 * Off means SimulateSwarm behaves exactly as it did before the field existed - not "the field is
 * empty", but the lookup is skipped entirely. That makes this the A/B switch for the only
 * question that matters about the cut: did routing around geometry change how the swarm feels in
 * the open arena? Run a flat-arena lap with it at 0 and at 1 and compare the perf log's sim
 * column and its routed count.
 *
 * A cvar rather than a setting because it is a comparison tool, not a design value, and because
 * it has to be flippable mid-run from the console.
 */
extern KILLBUGS_API TAutoConsoleVariable<int32> CVarKBSwarmFlowEnabled;

/**
 * Draws the flow field: blocked cells as red boxes, the direction field as green arrows.
 *
 * The field is invisible in normal play by design - it only steers bugs that have a wall between
 * them and their target - so without this there is no way to look at it at all. Default 0.
 *
 * Skipped when the app cannot render, so a -nullrhi run does not pay for draws nobody sees;
 * the cvar-gated log line is the instrument there.
 *
 * Usage: KB.Swarm.FlowDebug 1
 */
extern KILLBUGS_API TAutoConsoleVariable<int32> CVarKBSwarmFlowDebug;

/**
 * Testing switch: every bug death drops a material.
 *
 * Usage: KB.Loot.ForceDrop 1
 *
 * The per-archetype chance is a design value and stays low, which makes "did the drop path run
 * at all" a coin flip during a headless test. This makes it deterministic without touching the
 * content. Off by default.
 */
extern KILLBUGS_API TAutoConsoleVariable<int32> CVarKBLootForceDrop;
