"""
Read-only probe: how big is the baked bug mesh, and what scale would each archetype need?

The three archetypes' mesh_scale values were tuned for placeholder primitives (a 100-unit cube,
a cylinder) and are meaningless for a real model. This derives them instead from the thing the
gameplay actually cares about: BodyRadius, which is what separation, hit tests and the blob
shadow all use. A bug drawn at a different size from the circle it is hit in reads as a hit
that should have missed.

Also reports which way the mesh is longest, because the swarm yaws each instance with
FRotator(0, Yaw, 0) - a model authored facing +Y instead of +X would appear to walk sideways,
and nothing in the code can tell you that.
"""

import time

import unreal

STATIC_MESH_PATH = "/Game/KillBugs/Mesh/bug/VAT/SM_bug.SM_bug"
SKELETAL_MESH_PATH = "/Game/KillBugs/Mesh/bug/bug.bug"

# From Tools/kb_setup_enemies.py - the gameplay size each archetype is balanced around.
ARCHETYPES = [
    ("Runner", 28.0),
    ("Grunt", 42.0),
    ("Brute", 85.0),
]

PREFIX = "[KBBoundsProbe] "


def log(message):
    unreal.log(PREFIX + str(message))


for _ in range(20):
    if unreal.AssetRegistryHelpers.get_asset_registry().is_loading_assets():
        time.sleep(1.0)
    else:
        break


def report(label, path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        log("{}: COULD NOT LOAD {}".format(label, path))
        return None

    try:
        bounds = asset.get_bounds()
    except Exception as error:  # noqa: BLE001
        log("{}: no get_bounds ({})".format(label, error))
        return None

    extent = bounds.box_extent
    origin = bounds.origin
    log("{}: origin=({:.1f}, {:.1f}, {:.1f}) extent=({:.2f}, {:.2f}, {:.2f})".format(
        label, origin.x, origin.y, origin.z, extent.x, extent.y, extent.z))
    return extent


log("--- source skeletal mesh ---")
skel_extent = report("bug (skeletal)", SKELETAL_MESH_PATH)

log("--- baked static mesh ---")
baked_extent = report("SM_Bug_VertexAnimation", STATIC_MESH_PATH)

extent = baked_extent or skel_extent
if extent is None:
    raise RuntimeError("could not measure either mesh")

# The footprint the swarm moves and hits on is the horizontal one; height is cosmetic and is
# left proportional so the bug does not come out squashed.
footprint_half = max(abs(extent.x), abs(extent.y))
if footprint_half <= 0.0:
    raise RuntimeError("mesh has a zero horizontal extent")

log("--- suggested mesh_scale (so the drawn bug matches its BodyRadius) ---")
for name, body_radius in ARCHETYPES:
    uniform = body_radius / footprint_half
    log("  {:<7} BodyRadius {:>5.1f}  ->  uniform scale {:.4f}  "
        "(delivered as x=y={:.3f}, z={:.3f} to keep the current squat proportions)".format(
            name, body_radius, uniform, uniform * 0.9, uniform * 0.9))

log("PROBE DONE")
