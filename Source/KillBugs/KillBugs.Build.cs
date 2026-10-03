using UnrealBuildTool;

public class KillBugs : ModuleRules
{
	public KillBugs(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",   // player input (IMC_Default, IA_Move, IA_Fire)
			"GameplayTags",    // weapon/passive/enemy tag filtering on cards
			"NetCore",         // FFastArraySerializer for the replicated swarm
			"PhysicsCore",     // FCollisionShape for spatial queries
			"UMG",             // HUD, card draft, wave banner
			"DeveloperSettings", // UKBEnemyPoolSettings
			"OnlineSubsystem", // IOnlineSession, behind UKBSessionSubsystem
			"OnlineSubsystemUtils" // the IpNetDriver the sessions hand off to
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"Niagara",         // death/hit VFX spawning
			"AssetRegistry"    // card discovery by scanning, so cards need no hand-kept list
		});

		// Deliberately NOT added:
		//   AIModule / NavigationSystem - the swarm uses UKBSpatialHash + custom steering,
		//     and AIModule pulls in a large dependency tree for no benefit here.
		//
		// OnlineSubsystem IS added now: the LAN lobby needs IOnlineSession. The backing
		// subsystem is OnlineSubsystemNull, selected by DefaultPlatformService in
		// Config/DefaultEngine.ini; only UKBSessionSubsystem talks to it, so swapping in
		// Steam/EOS later means changing that one class and that one ini line.
	}
}
