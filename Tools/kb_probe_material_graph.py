"""
Probe: is M_FlatCol's "Base Color" parameter actually wired to the material output?

KBEnemyVisualizerComponent creates a dynamic material instance per archetype and sets a
vector parameter on it. If the parameter exists but is not connected to the Base Color
output, the bugs render identically no matter what value we push - which is exactly the
symptom. This asks the material graph directly instead of guessing.
"""

import unreal

PREFIX = "[KBMatGraph] "

CANDIDATES = [
    "/Game/LevelPrototyping/Materials/M_FlatCol",
    "/Game/LevelPrototyping/Materials/MI_DefaultColorway",
]


def describe(node):
    if node is None:
        return "NONE"
    name = node.get_name()
    cls = node.get_class().get_name()
    detail = ""
    try:
        param = node.get_editor_property("parameter_name")
        detail = " parameter_name='{}'".format(param)
    except Exception:  # noqa: BLE001 - not every expression has one
        pass
    return "{} ({}){}".format(name, cls, detail)


for path in CANDIDATES:
    material = unreal.EditorAssetLibrary.load_asset(path)
    if material is None:
        unreal.log_warning(PREFIX + "missing " + path)
        continue

    unreal.log(PREFIX + path)

    for prop_name in ("MP_BASE_COLOR", "MP_EMISSIVE_COLOR", "MP_METALLIC", "MP_ROUGHNESS"):
        prop = getattr(unreal.MaterialProperty, prop_name, None)
        if prop is None:
            continue
        try:
            node = unreal.MaterialEditingLibrary.get_material_property_input_node(material, prop)
        except Exception as error:  # noqa: BLE001
            unreal.log(PREFIX + "    {} -> <error {}>".format(prop_name, error))
            continue
        unreal.log(PREFIX + "    {} -> {}".format(prop_name, describe(node)))

    try:
        expressions = unreal.MaterialEditingLibrary.get_material_expressions(material)
        for expr in expressions:
            cls = expr.get_class().get_name()
            if "Parameter" in cls:
                unreal.log(PREFIX + "    expr: " + describe(expr))
    except Exception as error:  # noqa: BLE001
        unreal.log_warning(PREFIX + "    expressions error: {}".format(error))

unreal.log(PREFIX + "DONE")
