#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * Where the local player is hearing from.
 *
 * The camera, NOT the pawn. This game's camera sits well above and behind the character, and
 * every spatial sound is panned relative to the listener - so a bug measured against the pawn
 * would be loud, and in the wrong place on screen, because it happens to be under the camera
 * rather than near the character.
 *
 * Returns false when there is no local player: a dedicated server, or a machine that has not
 * created its player controller yet. Every caller treats false as "do not play anything", which
 * is what keeps an entirely server-side swarm from paying for voices nobody can hear.
 *
 * Shared by UKBSwarmAudioComponent and UKBGoreComponent rather than written out in each: the
 * two must agree on what "the listener" means, or a movement sound and a death sound at the same
 * spot would not sound like they came from the same place.
 */
namespace KBListener
{
	// No KILLBUGS_API: this header has no generated counterpart to pull the macro in, and both
	// callers are inside this module, so nothing needs exporting.
	bool FindLocation(const UWorld* World, FVector& OutLocation);

	/**
	 * How far a source is, for the purpose of deciding whether it can be heard - measured on the
	 * GROUND PLANE, ignoring height.
	 *
	 * This is not the same as the distance the mixer uses, and using the mixer's distance here
	 * was a real bug: the listener is a spring-arm CAMERA about 1630 units above the character
	 * (CameraDistance 1800 at the -65 degree CameraPitch). A bug directly under the character is
	 * therefore already 1630 units from the listener, which ate 90% of an 1800-unit hearing
	 * radius before the bug had moved at all - and a bug any further than ~760 units away on the
	 * ground was rejected outright. The symptom was a swarm that was completely silent while the
	 * code looked correct.
	 *
	 * Ground distance is also the honest quantity for this game: the player reads the arena from
	 * above, and "how far away is that bug" means across the floor, not through the air.
	 *
	 * Distance for PANNING is still the true 3D one, and is the engine's business - the emitter
	 * is placed at the bug's real position, so left/right and near/far are unaffected by this.
	 */
	float HearingDistance(const FVector& Listener, const FVector& Source);
}
