"""
Read-only probe: which existing material can tint a dropped item?

Drops need two colours (material vs medkit) and there is no loot art yet. Creating a Material
from a headless run does not work in this project - see the note in Docs/Status.md - so the
question is whether something already on disk exposes a colour parameter we can drive with a
MID.

Reports the vector parameters of a few candidates, plus every material under /Game that has one.

Run with the editor CLOSED:
  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" -unattended -nosplash
"""

import unreal

PREFIX = "[KBLootProbe] "

CANDIDATES = [
    "/Engine/BasicShapes/BasicShapeMaterial",
    "/Engine/EngineMaterials/WorldGridMaterial",
    "/Game/LevelPrototyping/Materials/MI_PrototypeGrid_Gray",
    "/Game/KillBugs/Enemies/M_KBEnemy",
]


def report(path):
    material = unreal.load_asset(path)
    if material is None:
        print(PREFIX + "{}: NOT FOUND".format(path))
        return

    try:
        vectors = unreal.MaterialEditingLibrary.get_vector_parameter_names(material)
    except Exception as error:
        print(PREFIX + "{}: vector params unreadable ({})".format(path, error))
        return

    scalars = unreal.MaterialEditingLibrary.get_scalar_parameter_names(material)
    domain = "?"
    try:
        domain = material.get_editor_property("material_domain")
    except Exception:
        pass

    print(PREFIX + "{}".format(path))
    print(PREFIX + "    domain={}  vectors={}  scalars={}".format(domain, list(vectors), list(scalars)))


def scan_project():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_path("/Game", recursive=True)
    found = 0
    for asset in assets:
        if asset.asset_class_path.asset_name != "Material":
            continue
        material = asset.get_asset()
        if material is None:
            continue
        try:
            vectors = unreal.MaterialEditingLibrary.get_vector_parameter_names(material)
        except Exception:
            continue
        if vectors:
            found += 1
            print(PREFIX + "    candidate: {} -> {}".format(asset.package_name, list(vectors)))
    print(PREFIX + "materials under /Game with a vector parameter: {}".format(found))


print(PREFIX + "--- candidates ---")
for path in CANDIDATES:
    report(path)

print(PREFIX + "--- project scan ---")
scan_project()
print(PREFIX + "done")
