"""
Reads M_KBSlime back off disk and checks it is what kb_setup_slime_material.py claims it wrote.

Run in a SEPARATE process from the generator, with the editor closed:

  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

Separate process is the point, and it is not a formality here. The generator cannot check its
own output at all: a fresh package is not in the asset registry yet, so a lookup fails even
though the file was written, and load_asset inside that same process would return the in-memory
object regardless. Only a cold process reads what actually reached disk.
"""

import os
import time

import unreal

PACKAGE_PATH = "/Game/KillBugs/Enemies"
ASSET_NAME = "M_KBSlime"
FULL_PATH = "{}.{}".format(PACKAGE_PATH, ASSET_NAME)

PREFIX = "[KBSlimeVerify] "
problems = []


def log(message):
    unreal.log(PREFIX + str(message))


def check(condition, description):
    log("{} {}".format("OK  " if condition else "FAIL", description))
    if not condition:
        problems.append(description)


disk_path = unreal.Paths.convert_relative_path_to_full(
    unreal.Paths.project_content_dir()) + "KillBugs/Enemies/{}.uasset".format(ASSET_NAME)
check(os.path.exists(disk_path), "file exists on disk: {}".format(disk_path))

# -ExecutePythonScript runs EARLY - before the asset registry has finished its initial scan,
# which takes a few seconds on this project. load_asset resolves through that registry, so
# without this wait the verifier reports "could not be loaded" for an asset that is plainly on
# disk and perfectly fine. Retry rather than sleep a fixed amount: on a warm cache it is ready
# immediately, and on a cold one the scan is what we are actually waiting for.
material = None
for attempt in range(15):
    material = unreal.EditorAssetLibrary.load_asset(FULL_PATH)
    if material is not None:
        log("loaded on attempt {}".format(attempt + 1))
        break
    time.sleep(1.0)

# If the registry cannot see it, try the raw loader before concluding the FILE is broken. The
# two failures look identical from the outside but mean opposite things: a blind registry means
# the asset is fine and only this headless path cannot find it.
if material is None:
    raw = unreal.load_asset(FULL_PATH)
    log("EditorAssetLibrary could not find it; raw load_asset -> {}".format(raw))
    if raw is not None:
        log("  ...so the file is VALID and the asset registry is what cannot see it")
        material = raw

check(material is not None, "loads from disk")

if material is None:
    raise RuntimeError("{} could not be loaded - the generator did not save".format(FULL_PATH))

# The domain is the whole point: a surface material on a decal component draws nothing at all.
domain = material.get_editor_property("material_domain")
log("material_domain = {}".format(domain))
check(str(domain).endswith("MD_DEFERRED_DECAL"), "domain is Deferred Decal")

# UKBGoreComponent writes both of these by name, so a rename here silently stops the tint or
# the fade without any error anywhere.
vector_params = unreal.MaterialEditingLibrary.get_vector_parameter_names(material)
scalar_params = unreal.MaterialEditingLibrary.get_scalar_parameter_names(material)
log("vector params: {}".format(vector_params))
log("scalar params: {}".format(scalar_params))

check("Base Color" in [str(p) for p in vector_params], "has a 'Base Color' vector parameter")
check("Opacity" in [str(p) for p in scalar_params], "has an 'Opacity' scalar parameter")

# Connected, not merely present: a parameter node floating unconnected looks identical in the
# parameter list and does nothing.
base_colour_node = unreal.MaterialEditingLibrary.get_material_property_input_node(
    material, unreal.MaterialProperty.MP_BASE_COLOR)
opacity_node = unreal.MaterialEditingLibrary.get_material_property_input_node(
    material, unreal.MaterialProperty.MP_OPACITY)

log("BaseColor driven by: {}".format(base_colour_node.get_class().get_name() if base_colour_node else "nothing"))
log("Opacity driven by:   {}".format(opacity_node.get_class().get_name() if opacity_node else "nothing"))

check(base_colour_node is not None, "Base Color is connected to something")
check(opacity_node is not None, "Opacity is connected to something")

# The radial falloff, so a puddle is not a hard-edged square. Multiply is the node the mask
# feeds; if the falloff failed to build the generator fell back to the bare Opacity parameter.
if opacity_node is not None:
    opacity_class = str(opacity_node.get_class().get_name())
    check("Multiply" in opacity_class,
          "Opacity runs through the radial falloff (got {})".format(opacity_class))

if problems:
    raise RuntimeError("{} check(s) failed: {}".format(len(problems), "; ".join(problems)))

log("ALL CHECKS PASSED")
