"""
Read-only probe: what does each bug archetype actually point at right now?

Answers "is the swarm drawing my model yet?" with the data the archetype assets hold, rather
than with what the generator script was supposed to have written. The two diverge the moment a
generator run is skipped or half-applied.

Prints, per archetype: the mesh, the material, whether the baked animation is wired up, and the
scale/tint that distinguish the three from one another.
"""

import time

import unreal

FOLDER = "/Game/KillBugs/Enemies"
PREFIX = "[KBEnemyProbe] "


def log(message):
    unreal.log(PREFIX + str(message))


for _ in range(20):
    if unreal.AssetRegistryHelpers.get_asset_registry().is_loading_assets():
        time.sleep(1.0)
    else:
        break

paths = unreal.EditorAssetLibrary.list_assets(FOLDER, recursive=True, include_folder=False)

log("--- archetypes ---")
for path in paths:
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if not isinstance(asset, unreal.KBEnemyArchetype):
        continue

    def read(prop):
        try:
            return asset.get_editor_property(prop)
        except Exception:  # noqa: BLE001
            return "<missing>"

    mesh = read("mesh")
    material = read("material")
    anim = read("anim_data")

    log("{}".format(asset.get_name()))
    log("    mesh      = {}".format(
        mesh.get_path_name() if hasattr(mesh, "get_path_name") else mesh))
    log("    material  = {}".format(
        material.get_path_name() if hasattr(material, "get_path_name") else material))
    log("    anim_data = {}".format(
        anim.get_path_name() if hasattr(anim, "get_path_name") else anim))
    log("    scale     = {}   tint = {}".format(read("mesh_scale"), read("tint")))

log("--- baked assets available to point at ---")
for path in paths:
    name = str(path).split("/")[-1].split(".")[0]
    if name.startswith(("SM_Bug", "DA_Bug_Vertex", "MI_Bug")):
        log("    {}".format(path))

log("PROBE DONE")
