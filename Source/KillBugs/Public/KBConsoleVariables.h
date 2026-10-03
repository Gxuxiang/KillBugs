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
