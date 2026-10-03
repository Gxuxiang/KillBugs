#include "KBConsoleVariables.h"

TAutoConsoleVariable<float> CVarKBSwarmPerfLog(
	TEXT("KB.Swarm.PerfLog"),
	0.f,
	TEXT("Seconds between swarm perf log lines (0 = off). Covers both the server-side "
	     "simulation and the visualizer that turns the replicated swarm into instances."),
	ECVF_Default);
