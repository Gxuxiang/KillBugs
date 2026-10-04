#include "KBConsoleVariables.h"

TAutoConsoleVariable<float> CVarKBSwarmPerfLog(
	TEXT("KB.Swarm.PerfLog"),
	0.f,
	TEXT("Seconds between swarm perf log lines (0 = off). Covers both the server-side "
	     "simulation and the visualizer that turns the replicated swarm into instances."),
	ECVF_Default);

TAutoConsoleVariable<float> CVarKBSwarmAudioLog(
	TEXT("KB.Swarm.AudioLog"),
	0.f,
	TEXT("Seconds between swarm audio log lines (0 = off). Reports the listener, how many bugs "
	     "were candidates, why the nearest one was or was not heard, and the state of every "
	     "emitter slot."),
	ECVF_Default);
