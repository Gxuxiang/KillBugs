"""
Probe: which LevelPrototyping materials expose a colour parameter we can drive per archetype?

Each archetype owns its own instanced mesh component, so setting a vector parameter on that
component tints every bug of that archetype - no material authoring required, as long as the
parent material actually has a parameter to drive.
"""

import unreal

PREFIX = "[KBProbe] "

CANDIDATES = [
    "/Game/LevelPrototyping/Materials/M_PrototypeGrid",
    "/Game/LevelPrototyping/Materials/MI_PrototypeGrid_Gray",
    "/Game/LevelPrototyping/Materials/MI_PrototypeGrid_Gray_02",
    "/Game/LevelPrototyping/Materials/MI_PrototypeGrid_Gray_Round",
    "/Game/LevelPrototyping/Materials/MI_PrototypeGrid_TopDark",
    "/Game/LevelPrototyping/Materials/M_FlatCol",
    "/Game/LevelPrototyping/Materials/MI_DefaultColorway",
]

for path in CANDIDATES:
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        unreal.log_warning(PREFIX + "missing " + path)
        continue

    try:
        vectors = unreal.MaterialEditingLibrary.get_vector_parameter_names(asset)
    except Exception as error:  # noqa: BLE001
        vectors = ["<error: {}>".format(error)]

    try:
        scalars = unreal.MaterialEditingLibrary.get_scalar_parameter_names(asset)
    except Exception as error:  # noqa: BLE001
        scalars = ["<error: {}>".format(error)]

    unreal.log("{}{}\n    vectors={}\n    scalars={}".format(
        PREFIX, path, list(vectors), list(scalars)))

unreal.log(PREFIX + "DONE")
