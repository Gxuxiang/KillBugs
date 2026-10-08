#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"

/**
 * Own stat group, so the swarm's cost is visible as `stat KillBugs` rather than being buried
 * in an aggregate. The Phase 2 perf gate (swarm tick < 1.5 ms) is read from these counters.
 */
DECLARE_STATS_GROUP(TEXT("KillBugs"), STATGROUP_KillBugs, STATCAT_Advanced);

DECLARE_CYCLE_STAT_EXTERN(TEXT("Swarm Sim"), STAT_KB_SwarmSim, STATGROUP_KillBugs, KILLBUGS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Swarm Net Sync"), STAT_KB_SwarmNetSync, STATGROUP_KillBugs, KILLBUGS_API);
// Kept separate from Swarm Sim on purpose: rebuilding the flow field happens once every tenth of
// a second, so it would be invisible inside a 600-bug tick average - and the whole point of
// measuring it is to find out whether that tenth-of-a-second cadence is the right one.
DECLARE_CYCLE_STAT_EXTERN(TEXT("Swarm Flow Field"), STAT_KB_SwarmFlow, STATGROUP_KillBugs, KILLBUGS_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Swarm Visuals"), STAT_KB_SwarmVisuals, STATGROUP_KillBugs, KILLBUGS_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Swarm Count"), STAT_KB_SwarmCount, STATGROUP_KillBugs, KILLBUGS_API);
