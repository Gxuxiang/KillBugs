"""
Enables the InstancedStaticMesh usage flag on the VAT material.

"the bugs have no material" is this project's most-repeated rendering failure, and it looks like
the mesh simply failed to load rather than like a material problem: an
InstancedStaticMeshComponent cannot render a material whose bUsedWithInstancedStaticMeshes is
false, and the renderer falls back to the engine default WITHOUT logging anything. The bugs then
draw in default grey (or, on a build without the checker, appear untextured).

The flag lives on the BASE material, not on the material instance, so it goes on M_bug_bone -
the parent the user built the VAT workflow around. M_KBEnemy needed exactly this treatment when
it was made.

The flag is a compile-time shader permutation, hence recompile_material afterwards: setting it
does nothing until the material is rebuilt.
"""

import time

import unreal

# The base material, NOT the _Inst instance: usage flags are a property of the material itself.
MATERIALS = [
    "/Game/KillBugs/Mesh/bug/VAT/M_bug_bone.M_bug_bone",
    "/Game/KillBugs/Mesh/bug/VAT/M_bug_bone_Inst.M_bug_bone_Inst",
]

FLAGS = ("used_with_instanced_static_meshes", "used_with_static_meshes")

PREFIX = "[KBVatIsm] "


def log(message):
    unreal.log(PREFIX + str(message))


for _ in range(20):
    if unreal.AssetRegistryHelpers.get_asset_registry().is_loading_assets():
        time.sleep(1.0)
    else:
        break

touched_base = False

for path in MATERIALS:
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        log("could not load {}".format(path))
        continue

    log("--- {}".format(path.split("/")[-1]))

    # An instance's flags come from its parent; setting them here is harmless but reports False,
    # which is worth seeing so the log is not mistaken for a failed write.
    if not isinstance(asset, unreal.Material):
        log("    not a base material, skipping (flags come from the parent)")
        continue

    for flag in FLAGS:
        try:
            asset.set_editor_property(flag, True)
            log("    {} = True".format(flag))
        except Exception as error:  # noqa: BLE001 - flag spellings vary between versions
            log("    {} : {}".format(flag, error))

    # A usage flag selects a shader permutation, so it does nothing until the material is rebuilt.
    try:
        unreal.MaterialEditingLibrary.recompile_material(asset)
        log("    recompiled")
    except Exception as error:  # noqa: BLE001
        log("    recompile failed: {}".format(error))

    if unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        log("    saved")
        touched_base = True
    else:
        log("    SAVE FAILED")

if not touched_base:
    raise RuntimeError("no base material was updated - check the paths")

log("DONE")
