#include "KillBugs.h"

DEFINE_LOG_CATEGORY(LogKillBugs);

void FKillBugsModule::StartupModule()
{
	FDefaultGameModuleImpl::StartupModule();

	UE_LOG(LogKillBugs, Log, TEXT("KillBugs module started"));
}

void FKillBugsModule::ShutdownModule()
{
	UE_LOG(LogKillBugs, Log, TEXT("KillBugs module shut down"));

	FDefaultGameModuleImpl::ShutdownModule();
}

IMPLEMENT_PRIMARY_GAME_MODULE( FKillBugsModule, KillBugs, "KillBugs" );
