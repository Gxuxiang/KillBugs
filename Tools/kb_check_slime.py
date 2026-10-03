"""
Read-only: is the decal material set up to blend, or does it paint an opaque square?

A deferred decal needs more than the right domain. Blend mode decides whether the decal
CONTRIBUTES to what is under it or REPLACES it, and a material left on the default Opaque blend
draws its full square regardless of what the opacity input is doing - which looks exactly like
"the decal is a glowing square" and sends you hunting in the opacity graph for nothing.

The previous version of this check looked at the domain and the parameter names and stopped,
which is why it passed while the decal still drew a square.
"""

import time

import unreal

MATERIAL_PATH = "/Game/KillBugs/Enemies/M_KBSlime.M_KBSlime"

PREFIX = "[KBslimeCheck] "
problems = []


def log(message):
    unreal.log(PREFIX + str(message))


def read(obj, prop):
    try:
        return obj.get_editor_property(prop)
    except Exception as error:  # noqa: BLE001
        return "<{}>".format(error)


for _ in range(20):
    if unreal.AssetRegistryHelpers.get_asset_registry().is_loading_assets():
        time.sleep(1.0)
    else:
        break

material = unreal.EditorAssetLibrary.load_asset(MATERIAL_PATH)
if material is None:
    raise RuntimeError("could not load {}".format(MATERIAL_PATH))

domain = read(material, "material_domain")
blend = read(material, "blend_mode")
shading = read(material, "shading_model")

log("material_domain = {}".format(domain))
log("blend_mode      = {}".format(blend))
log("shading_model   = {}".format(shading))

if "MD_DEFERRED_DECAL" not in str(domain).upper():
    problems.append("domain is not Deferred Decal")

# THE one that was missed. A decal that is not blending paints its whole projection box.
if "TRANSLUCENT" not in str(blend).upper() and "ALPHA" not in str(blend).upper():
    problems.append(
        "blend_mode is {} - a deferred decal must blend (Translucent / DBuffer), or it paints "
        "an opaque square no matter what the opacity input is".format(blend))

# What feeds opacity, and through how many nodes. Reported rather than judged: the shape of the
# graph is the author's business, but "opacity comes straight off a texture sample" is worth
# seeing because it usually means the texture's ALPHA is what should be connected, not its RGB.
opacity_node = unreal.MaterialEditingLibrary.get_material_property_input_node(
    material, unreal.MaterialProperty.MP_OPACITY)
base_node = unreal.MaterialEditingLibrary.get_material_property_input_node(
    material, unreal.MaterialProperty.MP_BASE_COLOR)

log("opacity driven by    : {}".format(
    opacity_node.get_class().get_name() if opacity_node else "NOTHING"))
log("base color driven by : {}".format(
    base_node.get_class().get_name() if base_node else "NOTHING"))

if opacity_node is None:
    problems.append("opacity is not connected at all")

log("every expression in the graph:")
for expression in unreal.MaterialEditingLibrary.get_material_expressions(material):
    name = expression.get_class().get_name()
    detail = ""
    try:
        detail = " '{}'".format(expression.get_editor_property("parameter_name"))
    except Exception:  # noqa: BLE001
        pass
    log("    {}{}".format(name, detail))

if problems:
    raise RuntimeError("{} problem(s): {}".format(len(problems), "; ".join(problems)))

log("ALL CHECKS PASSED")
