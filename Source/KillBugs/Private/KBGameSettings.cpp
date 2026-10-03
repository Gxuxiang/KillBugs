#include "KBGameSettings.h"

UKBGameSettings::UKBGameSettings()
{
	CategoryName = FName(TEXT("Game"));
	SectionName = FName(TEXT("KillBugs"));
}

const UKBGameSettings& UKBGameSettings::Get()
{
	// GetDefault is a CDO lookup, so this is cheap enough to call at the point of use rather
	// than caching values around the codebase - which is what made them hard to change.
	const UKBGameSettings* Settings = GetDefault<UKBGameSettings>();
	check(Settings);
	return *Settings;
}
