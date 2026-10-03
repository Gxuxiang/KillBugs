"""
Read-only: is the slime decal material built the way UKBGoreComponent expects?

The pool writes two parameters BY NAME, so a material that is otherwise perfect but calls them
something else draws nothing and reports nothing. And a material that is not a Deferred Decal
cannot be placed on a decal component at all - it simply does not appear.

Full object paths ("/Game/.../Name.Name"), not the package-only form: load_asset resolves the
short form through the asset registry and returns None for assets that are plainly on disk.
"""

import time

import unreal

MATERIAL_PATH = "/Game/KillBugs/Enemies/M_KBSlime.M_KBSlime"

PREFIX = "[KBslimeCheck] "
problems = []


def log(message):
    unreal.log(PREFIX + str(message))


for _ in range(20):
    if unreal.AssetRegistryHelpers.get_asset_registry().is_loading_assets():
        time.sleep(1.0)
    else:
        break

material = unreal.EditorAssetLibrary.load_asset(MATERIAL_PATH)
if material is None:
    raise RuntimeError("could not load {}".format(MATERIAL_PATH))

log("class = {}".format(material.get_class().get_name()))

domain = material.get_editor_property("material_domain")
log("material_domain = {}".format(domain))
if "MD_DEFERRED_DECAL" not in str(domain).upper():
    problems.append("domain is {} - a decal component needs Deferred Decal".format(domain))

# The names UKBGoreComponent writes. Both are load-bearing: without Opacity the puddle never
# fades (and is drawn at full strength forever), without Base Color every archetype's slime is
# the same colour.
vector_names = [str(n) for n in unreal.MaterialEditingLibrary.get_vector_parameter_names(material)]
scalar_names = [str(n) for n in unreal.MaterialEditingLibrary.get_scalar_parameter_names(material)]

log("vector params = {}".format(vector_names))
log("scalar params = {}".format(scalar_names))

if "Base Color" not in vector_names:
    problems.append("no 'Base Color' vector parameter")
if "Opacity" not in scalar_names:
    problems.append("no 'Opacity' scalar parameter")

# Connected, not merely present: a parameter node sitting unconnected shows up in both lists
# above and does nothing at all.
for label, prop in (("Base Color", unreal.MaterialProperty.MP_BASE_COLOR),
                    ("Opacity", unreal.MaterialProperty.MP_OPACITY)):
    node = unreal.MaterialEditingLibrary.get_material_property_input_node(material, prop)
    log("{} is driven by {}".format(
        label, node.get_class().get_name() if node else "NOTHING"))
    if node is None:
        problems.append("{} is not connected".format(label))

if problems:
    raise RuntimeError("{} problem(s): {}".format(len(problems), "; ".join(problems)))

log("ALL CHECKS PASSED")
