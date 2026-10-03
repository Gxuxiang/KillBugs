"""
Read-only probe of the bug assets the user imported.

Nothing is written. It reports what the files actually contain, because every number here
decides whether the vertex-animation bake is possible and how big its textures will be:

  - vertex count      -> the VAT texture is (vertices x frames); over 4096 vertices needs rows
  - animation frames  -> the other half of that product
  - root motion       -> a walk cycle with root motion fights the code that moves the swarm
  - UV channel 1      -> AnimToTexture writes its frame lookup into a spare UV set

Defensive on purpose: the Python surface for meshes moves between versions, so each accessor is
tried on its own and a missing one is reported rather than aborting the probe.
"""

import time

import unreal

FOLDER = "/Game/KillBugs/Mesh/bug"
PREFIX = "[KBBugProbe] "


def log(message):
    unreal.log(PREFIX + str(message))


def attempt(label, fn):
    try:
        value = fn()
        log("  {} = {}".format(label, value))
        return value
    except Exception as error:  # noqa: BLE001 - report and carry on
        log("  {} : unavailable ({})".format(label, error))
        return None


# The asset registry is not ready the instant -ExecutePythonScript runs.
registry = unreal.AssetRegistryHelpers.get_asset_registry()
for _ in range(15):
    if registry.is_loading_assets():
        time.sleep(1.0)
    else:
        break

assets = unreal.EditorAssetLibrary.list_assets(FOLDER, recursive=True, include_folder=False)
log("assets under {}: {}".format(FOLDER, len(assets)))

for path in assets:
    short = str(path).split(".")[-1]
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        log("--- {} : COULD NOT LOAD".format(short))
        continue

    log("--- {} : {}".format(short, asset.get_class().get_name()))

    if isinstance(asset, unreal.SkeletalMesh):
        attempt("lod_count", lambda: asset.get_lod_num())
        attempt("num_vertices(0)", lambda: asset.get_num_vertices(0))
        attempt("num_triangles(0)", lambda: asset.get_num_triangles(0))
        attempt("skeleton", lambda: asset.get_editor_property("skeleton"))
        attempt("materials", lambda: [str(m) for m in asset.get_editor_property("materials")])

    if isinstance(asset, unreal.AnimSequence):
        attempt("sequence_length", lambda: asset.get_play_length())
        attempt("num_frames", lambda: asset.get_number_of_frames())
        attempt("sampling_frame_rate", lambda: asset.get_editor_property("sampling_frame_rate"))
        attempt("has_root_motion", lambda: asset.get_editor_property("b_has_root_motion"))
        attempt("enable_root_motion", lambda: asset.get_editor_property("b_enable_root_motion"))
        attempt("skeleton", lambda: asset.get_editor_property("skeleton"))

    if isinstance(asset, unreal.Skeleton):
        attempt("bone_count", lambda: len(asset.get_editor_property("bone_tree")))

    if isinstance(asset, unreal.StaticMesh):
        attempt("num_vertices(0)", lambda: asset.get_num_vertices(0))

log("PROBE DONE")
