"""
Read-only probe: how big is every texture the bug actually uses?

Texture budget is not one number, because these are three different kinds of texture:

  VAT bone textures (TX_bone*)  - sized by bones x frames. A tiny texture holding transforms.
  the bug's PBR set             - a normal/base-colour map, sized by how close the camera gets.
  a baked render target         - whatever the workflow that made it needed.

The second kind matters most here: the swarm draws up to 600 bugs, but they are all ONE mesh
sharing ONE material, so a texture is on the GPU once no matter how many bugs are on screen.
That is the opposite of the vertex-count problem, where every vertex is multiplied by 600.
"""

import time

import unreal

PREFIX = "[KBTextureSizes] "

GROUPS = {
    "VAT (bone mode)": [
        "/Game/KillBugs/Mesh/bug/VAT/TX_bonePosition",
        "/Game/KillBugs/Mesh/bug/VAT/TX_boneRotation",
        "/Game/KillBugs/Mesh/bug/VAT/TX_boneWeight",
    ],
    "bug PBR set": [
        "/Game/KillBugs/Mesh/bug/texture_pbr_20250901",
        "/Game/KillBugs/Mesh/bug/texture_pbr_20250901_normal",
        "/Game/KillBugs/Mesh/bug/texture_pbr_20250901_metallic",
        "/Game/KillBugs/Mesh/bug/texture_pbr_20250901_roughness",
    ],
    "misc": [
        "/Game/KillBugs/Mesh/bug/VAT/NewTextureRenderTarget2D",
    ],
}


def log(message):
    unreal.log(PREFIX + str(message))


for _ in range(20):
    if unreal.AssetRegistryHelpers.get_asset_registry().is_loading_assets():
        time.sleep(1.0)
    else:
        break

for group, paths in GROUPS.items():
    log("--- {}".format(group))
    for path in paths:
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if asset is None:
            log("    {} : could not load".format(path.split("/")[-1]))
            continue

        try:
            width = asset.blueprint_get_size_x()
            height = asset.blueprint_get_size_y()
        except Exception:  # noqa: BLE001 - render targets expose a different accessor
            try:
                width = asset.get_editor_property("size_x")
                height = asset.get_editor_property("size_y")
            except Exception as error:  # noqa: BLE001
                log("    {} : no size ({})".format(path.split("/")[-1], error))
                continue

        # Mips multiply the memory by ~4/3, and the count is what decides whether a texture is
        # wasteful rather than just large.
        try:
            mips = asset.get_editor_property("mip_gen_settings")
        except Exception:  # noqa: BLE001
            mips = "?"

        log("    {:<28} {} x {}   mips={}".format(path.split("/")[-1], width, height, mips))

log("PROBE DONE")
