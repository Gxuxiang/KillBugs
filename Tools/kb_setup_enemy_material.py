"""
Creates M_KBEnemy, the base material for swarm instanced rendering.

WHY THIS EXISTS AT ALL: the LevelPrototyping materials (M_FlatCol, MI_PrototypeGrid_*) all
have bUsedWithInstancedStaticMeshes = False. An InstancedStaticMeshComponent cannot render a
material without that flag - the renderer silently substitutes the engine default material,
so every archetype draws in the same grey no matter what Base Color its dynamic material
instance holds. That failure is invisible from the code side: the MID is created, the
parameter reads back correctly, and the bugs still all look identical.

Exposes:
  Base Color (vector)  - driven per archetype by KBEnemyVisualizerComponent
  Roughness  (scalar)
  Metallic   (scalar)
"""

import unreal

PREFIX = "[KBEnemyMat] "
PACKAGE_PATH = "/Game/KillBugs/Enemies"
ASSET_NAME = "M_KBEnemy"


def log(message):
    unreal.log(PREFIX + str(message))


def warn(message):
    unreal.log_warning(PREFIX + str(message))


asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
full_path = "{}/{}".format(PACKAGE_PATH, ASSET_NAME)

if unreal.EditorAssetLibrary.does_asset_exist(full_path):
    unreal.EditorAssetLibrary.delete_asset(full_path)

factory = unreal.MaterialFactoryNew()
material = asset_tools.create_asset(ASSET_NAME, PACKAGE_PATH, unreal.Material, factory)
if material is None:
    raise RuntimeError("could not create " + full_path)

# --- Graph: parameters wired straight to the material outputs ---------------------------
def add_parameter(expression_class, parameter_name, default_value, property_enum, pos_x, pos_y):
    expression = unreal.MaterialEditingLibrary.create_material_expression(
        material, expression_class, pos_x, pos_y)
    if expression is None:
        raise RuntimeError("could not create expression " + parameter_name)
    expression.set_editor_property("parameter_name", unreal.Name(parameter_name))
    expression.set_editor_property("default_value", default_value)
    unreal.MaterialEditingLibrary.connect_material_property(expression, "", property_enum)
    return expression


add_parameter(unreal.MaterialExpressionVectorParameter, "Base Color",
              unreal.LinearColor(0.7, 0.7, 0.7, 1.0),
              unreal.MaterialProperty.MP_BASE_COLOR, -400, 0)

add_parameter(unreal.MaterialExpressionScalarParameter, "Roughness", 0.9,
              unreal.MaterialProperty.MP_ROUGHNESS, -400, 200)

add_parameter(unreal.MaterialExpressionScalarParameter, "Metallic", 0.0,
              unreal.MaterialProperty.MP_METALLIC, -400, 300)

# --- Usage flags: this is the whole point of the asset --------------------------------
for flag in ("used_with_instanced_static_meshes", "used_with_static_meshes"):
    try:
        material.set_editor_property(flag, True)
        log("set {} = True".format(flag))
    except Exception as error:  # noqa: BLE001 - flag names vary between versions
        warn("could not set {}: {}".format(flag, error))

unreal.MaterialEditingLibrary.recompile_material(material)
unreal.EditorAssetLibrary.save_loaded_asset(material)

# --- Verify by reading the flag back off the saved asset ------------------------------
reloaded = unreal.EditorAssetLibrary.load_asset(full_path)
if reloaded is None:
    raise RuntimeError("asset vanished after save: " + full_path)

log("{} -> used_with_instanced_static_meshes={}".format(
    full_path, reloaded.get_editor_property("used_with_instanced_static_meshes")))

for name in ("Base Color", "Roughness", "Metallic"):
    log("  parameter: {}".format(name))

log("DONE")
