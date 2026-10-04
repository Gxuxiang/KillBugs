#include "Audio/KBListenerLocation.h"

#include "Camera/PlayerCameraManager.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

bool KBListener::FindLocation(const UWorld* World, FVector& OutLocation)
{
	if (!World)
	{
		return false;
	}

	UGameInstance* GameInstance = World->GetGameInstance();

	// Deliberately the engine call, not UKBSessionSubsystem::GetLocalPlayerController - that one
	// does the same thing but is private, and it is the lobby's internal helper for "which
	// controller should I drive a travel on". Widening it so audio could borrow it would tie two
	// unrelated systems together for one line.
	APlayerController* PlayerController =
		GameInstance ? GameInstance->GetFirstLocalPlayerController() : nullptr;

	if (!PlayerController || !PlayerController->PlayerCameraManager)
	{
		// No local player, or one that has not had its camera manager created yet. Both mean
		// nobody is listening, which callers treat as "play nothing".
		return false;
	}

	OutLocation = PlayerController->PlayerCameraManager->GetCameraLocation();
	return true;
}

float KBListener::HearingDistance(const FVector& Listener, const FVector& Source)
{
	// Dist2D, not Dist: see the note in the header. The height difference between a top-down
	// camera and the floor is larger than the hearing radius itself.
	return FVector::Dist2D(Listener, Source);
}
