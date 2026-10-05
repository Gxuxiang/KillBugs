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

TAutoConsoleVariable<int32> CVarKBPlayerGod(
	TEXT("KB.Player.God"),
	0,
	TEXT("1 = the player takes no damage (default 0). Testing switch: KB.Player.God 1 to walk "
	     "through a wave, 0 to fight it."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarKBLootForceDrop(
	TEXT("KB.Loot.ForceDrop"),
	0,
	TEXT("1 = every bug death drops a material, whatever the archetype's chance says (default 0). "
	     "Testing switch, so a headless run can prove the drop path ran without rolling dice."),
	ECVF_Default);
