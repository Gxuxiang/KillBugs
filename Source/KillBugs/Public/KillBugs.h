#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/** Log category for the KillBugs game module. */
DECLARE_LOG_CATEGORY_EXTERN(LogKillBugs, Log, All);

/**
 * Primary game module.
 *
 * This exists rather than a bare IMPLEMENT_PRIMARY_GAME_MODULE so that module load/unload is
 * observable in the log. "Result: Succeeded" from the build proves the DLL linked; only this
 * log line proves the editor actually loaded it, which is the thing that silently fails when
 * the module name and the .uproject "Modules" entry disagree.
 */
class FKillBugsModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
