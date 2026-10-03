"""
Probe: do the prototype materials carry the instanced-static-mesh usage flag?

A material without bUsedWithInstancedStaticMeshes cannot be rendered by an
InstancedStaticMeshComponent; the renderer silently substitutes the default material, so
every archetype draws in the same grey no matter what Base Color we set on its MID.
"""

import unreal

PREFIX = "[KBIsmUsage] "

CANDIDATES = [
    "/Game/LevelPrototyping/Materials/M_FlatCol",
    "/Game/LevelPrototyping/Materials/MI_DefaultColorway",
    "/Game/LevelPrototyping/Materials/MI_PrototypeGrid_Gray",
    "/Game/LevelPrototyping/Materials/MI_PrototypeGrid_TopDark",
]

FLAGS = [
    "used_with_instanced_static_meshes",
    "used_with_static_meshes",
    "used_with_skeletal_mesh",
    "used_with_particle_sprites",
    "wireframe",
    "shading_model",
    "blend_mode",
]

for path in CANDIDATES:
    material = unreal.EditorAssetLibrary.load_asset(path)
    if material is None:
        unreal.log_warning(PREFIX + "missing " + path)
        continue

    parts = []
    for flag in FLAGS:
        try:
            parts.append("{}={}".format(flag, material.get_editor_property(flag)))
        except Exception as error:  # noqa: BLE001
            parts.append("{}<{}>".format(flag, error))

    unreal.log("{}{}\n    {}".format(PREFIX, path, "\n    ".join(parts)))

unreal.log(PREFIX + "DONE")
