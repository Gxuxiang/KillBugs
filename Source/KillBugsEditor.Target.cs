using UnrealBuildTool;
using System.Collections.Generic;

public class KillBugsEditorTarget : TargetRules
{
	public KillBugsEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;

		// Compile the game module's sources individually instead of as one unity blob.
		//
		// UBT's adaptive unity lumped all 22 KillBugs sources into a SINGLE translation unit
		// (the generated Module.KillBugs.cpp was 5 KB of nothing but #include lines), so one
		// cl.exe compiled the whole module on one core, and ANY one-line edit recompiled all 22
		// files - measured at about two minutes per edit. Without unity the same work spreads
		// across the machine's cores and an edit recompiles only the file that changed.
		//
		// This is a target-level setting, not a module-level one: ModuleRules has no
		// bUseUnityBuild, and putting it there fails the build with CS0103.
		//
		// The price is a slower first build and exposure of any latent missing #include that
		// unity was quietly covering up. If a build ever fails on a missing include, add the
		// include - do not switch this back on to hide it.
		bUseUnityBuild = false;

		ExtraModuleNames.AddRange( new string[] { "KillBugs" } );
	}
}
