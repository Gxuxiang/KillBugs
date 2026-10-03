"""
Generates the KillBugs lobby level.

Run headlessly (with the editor CLOSED, and after the C++ has compiled - the script needs
unreal.KBLobbyGameMode to exist in the running editor):

  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

A script rather than a hand-authored .umap so the lobby is reproducible, exactly like the
arena (see kb_setup_arena.py). It only ever writes to /Game/KillBugs/Maps.

The room is deliberately minimal - floor, light, a PlayerStart - because the lobby HUD paints
an opaque panel over the entire screen. Nothing here is ever the thing the player looks at, so
walls, props and a camera would be scenery for a room nobody stands in. What the level DOES
have to carry is the GameMode override: that is what makes a player land in the lobby rather
than straight into a wave.
"""

import unreal

MAP_PATH = "/Game/KillBugs/Maps/Lvl_Lobby"

# A room, not an arena: no swarm spawns here and nobody walks anywhere.
FLOOR_HALF_SIZE = 900.0

# The GameMode this map must run. The arena keeps the project default from DefaultEngine.ini
# (GlobalDefaultGameMode=/Script/KillBugs.KBGameMode); this override is what gives the lobby
# its own mode without either map knowing about the other.
LOBBY_GAME_MODE_NAME = "KBLobbyGameMode"
LOBBY_GAME_MODE_PATH = "/Script/KillBugs." + LOBBY_GAME_MODE_NAME

PREFIX = "[KBLobbySetup] "


def log(message):
    unreal.log(PREFIX + str(message))


def warn(message):
    unreal.log_warning(PREFIX + str(message))


def load_asset(path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        warn("could not load asset: {}".format(path))
    return asset


def spawn(actor_class, location, rotation=unreal.Rotator(0.0, 0.0, 0.0), label=None):
    actor = actor_subsystem.spawn_actor_from_class(actor_class, location, rotation)
    if actor is None:
        warn("failed to spawn {}".format(actor_class))
        return None
    if label:
        actor.set_actor_label(label)
    return actor


def _try_set(obj, property_name, value):
    """
    Sets a property, reporting rather than raising.

    Component property names differ between engine versions and are easy to get wrong, and a
    missing one should degrade the lighting rather than abort the whole lobby build.
    """
    try:
        obj.set_editor_property(property_name, value)
        return True
    except Exception as error:  # noqa: BLE001 - report and carry on
        warn("could not set {}: {}".format(property_name, error))
        return False


def scale_to_size(actor, mesh, target_size):
    """
    Scale a static mesh actor so its bounds match target_size (a Vector of world units).

    Any target component of 0 means "leave that axis at scale 1" - SM_Plane has zero Z extent,
    so a degenerate axis must not abandon the whole scale.
    """
    extent = mesh.get_bounds().box_extent

    scale = unreal.Vector(1.0, 1.0, 1.0)
    for axis, target in (("x", target_size.x), ("y", target_size.y), ("z", target_size.z)):
        if target <= 0.0:
            continue
        half = getattr(extent, axis)
        if half <= 0.0:
            warn("mesh has zero extent on {}; leaving that axis unscaled".format(axis))
            continue
        setattr(scale, axis, target / (half * 2.0))

    actor.set_actor_scale3d(scale)


# ---------------------------------------------------------------------------------------
# Subsystems
# ---------------------------------------------------------------------------------------
level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

if level_subsystem is None or actor_subsystem is None:
    raise RuntimeError("editor subsystems unavailable - this script must run in a full "
                       "editor session, not a commandlet")

# ---------------------------------------------------------------------------------------
# New empty level
#
# Same idempotent shape as the arena's: re-running clears the actors this script owns and
# leaves the level's own scaffolding alone. WorldSettings in particular must survive, because
# the GameMode override lives on it.
# ---------------------------------------------------------------------------------------
OWNED_ACTOR_CLASSES = (
    unreal.StaticMeshActor,
    unreal.DirectionalLight,
    unreal.SkyLight,
    unreal.SkyAtmosphere,
    unreal.PlayerStart,
)

if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
    # Delete-then-create does NOT work: delete_asset returns before the deletion has
    # propagated, so an immediate new_level fails validation. Load and clear instead.
    log("reloading existing {}".format(MAP_PATH))
    if not level_subsystem.load_level(MAP_PATH):
        raise RuntimeError("load_level failed for {}".format(MAP_PATH))

    removed = 0
    for actor in actor_subsystem.get_all_level_actors():
        if isinstance(actor, OWNED_ACTOR_CLASSES):
            actor_subsystem.destroy_actor(actor)
            removed += 1
    log("cleared {} previously spawned actor(s)".format(removed))
else:
    if not level_subsystem.new_level(MAP_PATH):
        raise RuntimeError("new_level failed for {}".format(MAP_PATH))
    log("created level {}".format(MAP_PATH))

# ---------------------------------------------------------------------------------------
# Floor
# ---------------------------------------------------------------------------------------
floor_mesh = load_asset("/Game/LevelPrototyping/Meshes/SM_Plane")
floor_material = load_asset("/Game/LevelPrototyping/Materials/MI_PrototypeGrid_Gray")

if floor_mesh is not None:
    floor = spawn(unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, 0.0), label="KB_LobbyFloor")
    if floor is not None:
        floor_component = floor.static_mesh_component
        floor_component.set_static_mesh(floor_mesh)
        if floor_material is not None:
            floor_component.set_material(0, floor_material)
        floor_component.set_mobility(unreal.ComponentMobility.STATIC)
        floor_component.set_collision_profile_name("BlockAll")
        scale_to_size(floor, floor_mesh, unreal.Vector(FLOOR_HALF_SIZE * 2.0,
                                                       FLOOR_HALF_SIZE * 2.0,
                                                       0.0))
        log("floor spawned")

# ---------------------------------------------------------------------------------------
# Lighting
#
# The same reasoning as the arena: Lumen is off project-wide, so the SkyLight is the only
# ambient and it needs a SkyAtmosphere to capture, or it resolves to a black cubemap.
# ---------------------------------------------------------------------------------------
spawn(unreal.SkyAtmosphere, unreal.Vector(0.0, 0.0, 0.0), label="KB_LobbySkyAtmosphere")

sun = spawn(unreal.DirectionalLight, unreal.Vector(0.0, 0.0, 2000.0),
            unreal.Rotator(-55.0, 0.0, 0.0), label="KB_LobbySun")
if sun is not None:
    sun_component = sun.light_component
    sun_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sun_component.set_intensity(6.0)
    _try_set(sun_component, "atmosphere_sun_light", True)

sky = spawn(unreal.SkyLight, unreal.Vector(0.0, 0.0, 1000.0), label="KB_LobbySkyLight")
if sky is not None:
    sky_component = sky.light_component
    sky_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    _try_set(sky_component, "real_time_capture", True)
    _try_set(sky_component, "intensity", 1.0)

# ---------------------------------------------------------------------------------------
# PlayerStart
#
# Not strictly required today - the lobby spawns no pawn (see AKBLobbyGameMode's constructor),
# so a player's view comes from the controller itself. It is here so that "a player is at this
# spot" is stated by the level rather than by an absent default, and so that setting a pawn
# class later does not need a map edit.
# ---------------------------------------------------------------------------------------
spawn(unreal.PlayerStart, unreal.Vector(0.0, 0.0, 200.0), label="KB_LobbyPlayerStart")

# ---------------------------------------------------------------------------------------
# GameMode override - the whole point of this level
#
# Without it the lobby would load the project default, AKBGameMode, which spawns the swarm and
# starts the wave clock the moment the map opens.
# ---------------------------------------------------------------------------------------
lobby_game_mode = getattr(unreal, LOBBY_GAME_MODE_NAME, None)
if lobby_game_mode is None:
    raise RuntimeError(
        "{} is not visible to Python. The C++ has to be compiled before this script runs - "
        "build KillBugsEditor first, then re-run.".format(LOBBY_GAME_MODE_PATH))

# NOT via get_all_level_actors(): the WorldSettings actor is not in that list. A probe of
# this build returned the floor, walls, lights and PlayerStart but no WorldSettings, so
# scanning for one silently finds nothing and the override is never applied. The editor
# world's own accessor is the route that works.
editor_subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
editor_world = editor_subsystem.get_editor_world() if editor_subsystem else None
if editor_world is None:
    raise RuntimeError("could not reach the editor world - cannot set the GameMode override")

world_settings = editor_world.get_world_settings()
if world_settings is None:
    raise RuntimeError("no WorldSettings on {} - cannot set the GameMode override"
                       .format(MAP_PATH))

# AWorldSettings::DefaultGameMode, shown in the editor as "GameMode Override".
if not _try_set(world_settings, "default_game_mode", lobby_game_mode):
    raise RuntimeError("could not set default_game_mode on the WorldSettings")

log("GameMode override set to {}".format(LOBBY_GAME_MODE_PATH))

# ---------------------------------------------------------------------------------------
# Save
# ---------------------------------------------------------------------------------------
if not level_subsystem.save_current_level():
    raise RuntimeError("failed to save {}".format(MAP_PATH))

log("DONE - lobby saved to {}".format(MAP_PATH))

# Summary, so a run is verifiable from the log alone rather than by opening the editor. The
# GameMode override is the one thing that has to be right, so it is read back and printed.
log("--- spawned actors ---")
for actor in actor_subsystem.get_all_level_actors():
    log("  {} at {}".format(actor.get_actor_label(), actor.get_actor_location()))

resolved = world_settings.get_editor_property("default_game_mode")
log("--- world settings ---")
log("  default_game_mode = {}".format(resolved.get_path_name() if resolved else "None"))

if resolved is None or LOBBY_GAME_MODE_NAME not in resolved.get_path_name():
    raise RuntimeError("GameMode override did not stick; the lobby would run the arena's mode")
