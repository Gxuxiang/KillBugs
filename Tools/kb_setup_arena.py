"""
Generates the KillBugs arena level.

Run headlessly:
  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

This is a script rather than a hand-authored .umap so the arena is reproducible: change a
number here and re-run, instead of re-placing actors by hand. It only ever writes to
/Game/KillBugs/Maps, so nothing existing is at risk.

Phase 1 content: a flat floor, four boundary walls, a movable sun, a sky light and a
PlayerStart. Bugs, spawners and the real art land in later phases.
"""

import unreal

MAP_PATH = "/Game/KillBugs/Maps/Lvl_Arena"
FLOOR_HALF_SIZE = 5500.0   # 110m across: the camera sees ~2000 units, so this leaves room to spawn off-screen
WALL_HEIGHT = 400.0
WALL_THICKNESS = 100.0

PREFIX = "[KBSetup] "


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
    missing one should degrade the lighting rather than abort the whole arena build.
    """
    try:
        obj.set_editor_property(property_name, value)
        return True
    except Exception as error:  # noqa: BLE001 - report and carry on
        warn("could not set {}: {}".format(property_name, error))
        return False


def scale_to_size(actor, mesh, target_size, centre):
    """
    Scale a static mesh actor to target_size and put its SCALED BOUNDS centred on `centre`.

    Any target component of 0 means "leave that axis at scale 1". This matters for flat
    meshes: SM_Plane has zero Z extent, so a single degenerate axis must not abandon the
    whole scale - that silently leaves the floor 1 metre across.

    The repositioning is not a nicety, and the reason is worth keeping:

    A mesh is scaled about its PIVOT, and a mesh's pivot is wherever its author put it. These
    prototyping meshes put it on the minimum corner - SM_Cube's bounds origin is (50,50,50),
    its geometry occupying [0,100] locally. So scaling grows the cube in +X/+Y/+Z only, and
    placing the actor at the centre you want is only correct for a centred pivot.

    The four walls were built that way: each was spawned where its centre belonged, grew a
    full wall-length in +Y (or +X), and floated 200 units up. Four bars in a pinwheel, with the
    arena open on every side - and nothing ever said so, because nothing in the game had ever
    collided with them: the swarm is clamped by coordinates, bullets do not test the world, and
    no camera looks at the arena from the side. The flow field's obstacle bake was the first
    thing to ask what is physically there, and it found a gap under every wall.

    Compensating here rather than at each call site means it holds for any mesh, pivot and all.
    """
    bounds = mesh.get_bounds()
    extent = bounds.box_extent

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

    # Where the mesh's bounds centre ends up if the actor sits at `centre`, and the move that
    # puts it back: scaled_bounds_centre = actor_location + scale * bounds_origin.
    offset = unreal.Vector(scale.x * bounds.origin.x,
                           scale.y * bounds.origin.y,
                           scale.z * bounds.origin.z)
    actor.set_actor_location(unreal.Vector(centre.x - offset.x,
                                           centre.y - offset.y,
                                           centre.z - offset.z), False, False)


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
# ---------------------------------------------------------------------------------------
# Actor classes this script owns. Re-running clears exactly these and nothing else, so the
# level's own scaffolding (WorldSettings, World Partition data layers, level script) survives.
OWNED_ACTOR_CLASSES = (
    unreal.StaticMeshActor,
    unreal.DirectionalLight,
    unreal.SkyLight,
    unreal.SkyAtmosphere,
    unreal.PlayerStart,
)

if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
    # Re-running must be idempotent. Delete-then-create does NOT work here: delete_asset
    # returns before the deletion has propagated, so an immediate new_level fails validation
    # with "an asset already exists at this location". Load and clear instead.
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
    floor = spawn(unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, 0.0), label="KB_ArenaFloor")
    if floor is not None:
        floor_component = floor.static_mesh_component
        floor_component.set_static_mesh(floor_mesh)
        if floor_material is not None:
            floor_component.set_material(0, floor_material)
        floor_component.set_mobility(unreal.ComponentMobility.STATIC)
        # A walkable floor is the one thing that must not be got wrong, so force collision
        # rather than trusting the mesh's authored setup.
        floor_component.set_collision_profile_name("BlockAll")
        # Z target 0 = leave the plane's own scale alone; it has no thickness to size.
        scale_to_size(floor, floor_mesh, unreal.Vector(FLOOR_HALF_SIZE * 2.0,
                                                       FLOOR_HALF_SIZE * 2.0,
                                                       0.0),
                      unreal.Vector(0.0, 0.0, 0.0))
        log("floor spawned")

# ---------------------------------------------------------------------------------------
# Boundary walls, so the player cannot walk off the arena edge
# ---------------------------------------------------------------------------------------
box_mesh = load_asset("/Game/LevelPrototyping/Meshes/SM_Cube")

if box_mesh is not None:
    wall_length = FLOOR_HALF_SIZE * 2.0
    walls = [
        ("KB_Wall_North", unreal.Vector(0.0, FLOOR_HALF_SIZE, WALL_HEIGHT * 0.5)),
        ("KB_Wall_South", unreal.Vector(0.0, -FLOOR_HALF_SIZE, WALL_HEIGHT * 0.5)),
        ("KB_Wall_East", unreal.Vector(FLOOR_HALF_SIZE, 0.0, WALL_HEIGHT * 0.5)),
        ("KB_Wall_West", unreal.Vector(-FLOOR_HALF_SIZE, 0.0, WALL_HEIGHT * 0.5)),
    ]
    for label, location in walls:
        wall = spawn(unreal.StaticMeshActor, location, label=label)
        if wall is None:
            continue
        wall_component = wall.static_mesh_component
        wall_component.set_static_mesh(box_mesh)
        wall_component.set_mobility(unreal.ComponentMobility.STATIC)
        wall_component.set_collision_profile_name("BlockAll")

        # Long axis follows the wall's direction.
        along_x = abs(location.y) > abs(location.x)
        size = unreal.Vector(
            wall_length if along_x else WALL_THICKNESS,
            WALL_THICKNESS if along_x else wall_length,
            WALL_HEIGHT,
        )
        # `location` is the wall's intended CENTRE: half a wall-height up, and half a wall out
        # along the axis its thickness runs on. scale_to_size moves the actor so the scaled
        # bounds actually land there, whatever the mesh's pivot is.
        scale_to_size(wall, box_mesh, size, location)
    log("boundary walls spawned")

# ---------------------------------------------------------------------------------------
# Lighting
#
# Lumen is disabled project-wide and the arena is a flat plane, so one movable directional
# light plus a sky light is the whole lighting story. The light is movable rather than
# static because static lighting is off until Phase 4 (see DefaultEngine.ini).
# ---------------------------------------------------------------------------------------
# A SkyAtmosphere is NOT optional here. With Lumen disabled there is no GI to fill unlit
# surfaces, so the only ambient comes from the SkyLight - and a SkyLight with no sky to
# capture resolves to a black cubemap, i.e. zero ambient, i.e. a nearly black arena.
# Adding an atmosphere gives the skylight something to capture and lets the sun drive the
# sky colour.
spawn(unreal.SkyAtmosphere, unreal.Vector(0.0, 0.0, 0.0), label="KB_SkyAtmosphere")

sun = spawn(unreal.DirectionalLight, unreal.Vector(0.0, 0.0, 2000.0),
            unreal.Rotator(-55.0, 0.0, 0.0), label="KB_Sun")
if sun is not None:
    sun_component = sun.light_component
    sun_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    sun_component.set_intensity(6.0)
    # Route the sun through the atmosphere so the sky lights up and the skylight captures it.
    _try_set(sun_component, "atmosphere_sun_light", True)

sky = spawn(unreal.SkyLight, unreal.Vector(0.0, 0.0, 1000.0), label="KB_SkyLight")
if sky is not None:
    sky_component = sky.light_component
    sky_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    # Real-time capture so the ambient tracks the sky without relying on a one-shot capture
    # that may happen before the atmosphere has initialised.
    _try_set(sky_component, "real_time_capture", True)
    _try_set(sky_component, "intensity", 1.0)

# ---------------------------------------------------------------------------------------
# PlayerStart, above the floor so the pawn has room to spawn
# ---------------------------------------------------------------------------------------
spawn(unreal.PlayerStart, unreal.Vector(0.0, 0.0, 200.0), label="KB_PlayerStart")

# ---------------------------------------------------------------------------------------
# Save
# ---------------------------------------------------------------------------------------
if not level_subsystem.save_current_level():
    raise RuntimeError("failed to save {}".format(MAP_PATH))

log("DONE - arena saved to {}".format(MAP_PATH))

# Summary, so a run is verifiable from the log alone rather than by opening the editor.
# A floor left at scale 1 is the specific failure this catches.
log("--- spawned actors ---")
for actor in actor_subsystem.get_all_level_actors():
    if isinstance(actor, OWNED_ACTOR_CLASSES):
        location = actor.get_actor_location()
        scale = actor.get_actor_scale3d()
        log("  {:<16} loc=({:>7.0f},{:>7.0f},{:>7.0f})  scale=({:.1f},{:.1f},{:.1f})".format(
            actor.get_actor_label(), location.x, location.y, location.z,
            scale.x, scale.y, scale.z))

try:
    unreal.SystemLibrary.quit_editor()
except Exception as error:  # noqa: BLE001 - shutdown is best-effort
    warn("quit_editor failed: {}".format(error))
