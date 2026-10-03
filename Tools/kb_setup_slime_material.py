"""
Generates M_KBSlime, the ground decal a dead bug leaves behind.

Run headlessly with the editor CLOSED:

  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

The material is a Deferred Decal: it projects downward and paints what it hits, which is the
only way to get slime that follows the arena floor rather than floating on a quad above it.

It exposes exactly two parameters, and UKBGoreComponent writes both:

  Base Color  (vector) - the archetype's SlimeColor, so a Runner and a Brute leave visibly
                         different puddles from one shared material
  Opacity     (scalar) - the decal pool drives this down over the puddle's last few seconds

The radial fade is built from nodes rather than left flat, because a decal projects as a
SQUARE: without a mask every puddle would be a hard-edged square of slime. Everything is
wired through MaterialEditingLibrary, which is the one part of this project's Python material
tooling that had never been exercised before - see the failure notes at the bottom.
"""

import os

import unreal

PACKAGE_PATH = "/Game/KillBugs/Enemies"
ASSET_NAME = "M_KBSlime"
FULL_PATH = "{}.{}".format(PACKAGE_PATH, ASSET_NAME)

PREFIX = "[KBSlimeMat] "

problems = []


def log(message):
    unreal.log(PREFIX + str(message))


def warn(message):
    unreal.log_warning(PREFIX + str(message))
    problems.append(str(message))


def enum_member(enum_name, *candidates):
    """
    Resolve a reflected enum value, trying each spelling.

    UE's Python binding strips the type prefix from reflected enums (EBlendMode -> BlendMode),
    and which spelling survives has varied between versions, so probing both is cheaper than
    guessing and getting an AttributeError three lines later.
    """
    enum = getattr(unreal, enum_name, None)
    if enum is None:
        return None
    for candidate in candidates:
        value = getattr(enum, candidate, None)
        if value is not None:
            return value
    return None


def make(expression_class, x, y):
    node = unreal.MaterialEditingLibrary.create_material_expression(material, expression_class, x, y)
    if node is None:
        warn("could not create {}".format(expression_class))
    return node


def connect(from_node, from_output, to_node, to_input):
    if from_node is None or to_node is None:
        return False
    ok = unreal.MaterialEditingLibrary.connect_material_expressions(
        from_node, from_output, to_node, to_input)
    if not ok:
        warn("connect {}:{} -> {}:{} failed".format(from_node, from_output, to_node, to_input))
    return ok


# ---------------------------------------------------------------------------------------
# Asset
# ---------------------------------------------------------------------------------------
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

# Regeneration is done by deleting the .uasset OUTSIDE the editor and re-running, exactly as
# kb_setup_enemies.py requires. delete_asset + create_asset in the same process does NOT work:
# the delete leaves a stale entry in the Asset Registry, create_asset then builds an object that
# never registers, and the only symptom is `SaveAsset failed: could not be found in the Asset
# Registry` - with no file on disk and no error pointing at the delete.
disk_path = unreal.Paths.convert_relative_path_to_full(
    unreal.Paths.project_content_dir()) + "KillBugs/Enemies/{}.uasset".format(ASSET_NAME)

if os.path.exists(disk_path):
    raise RuntimeError(
        "{} already exists on disk. Delete it from OUTSIDE the editor and re-run:\n"
        "    rm \"{}\"".format(disk_path, disk_path))

log("creating a fresh {}".format(FULL_PATH))

material = asset_tools.create_asset(ASSET_NAME, PACKAGE_PATH, unreal.Material, unreal.MaterialFactoryNew())
if material is None:
    raise RuntimeError("create_asset failed for {}".format(FULL_PATH))

# ---------------------------------------------------------------------------------------
# Domain: a decal, not a surface
# ---------------------------------------------------------------------------------------
domain = enum_member("MaterialDomain", "MD_DEFERRED_DECAL", "DEFERRED_DECAL")
if domain is None:
    raise RuntimeError("no deferred-decal MaterialDomain value found - cannot continue")
material.set_editor_property("material_domain", domain)
log("material_domain = MD_DEFERRED_DECAL")

blend = enum_member("BlendMode", "BLEND_TRANSLUCENT", "TRANSLUCENT")
if blend is not None:
    material.set_editor_property("blend_mode", blend)
    log("blend_mode = BLEND_TRANSLUCENT")

# The shading model is deliberately LEFT ALONE. An earlier version set it to Unlit, which a
# deferred decal may not be: the material then failed to compile, and the only symptom was
# save_asset returning false with no error anywhere - the graph was fine, the asset just could
# not be written. The factory default (Default Lit) is what a decal wants.

# ---------------------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------------------
base_colour = make(unreal.MaterialExpressionVectorParameter, -600, -200)
if base_colour is not None:
    base_colour.set_editor_property("parameter_name", unreal.Name("Base Color"))
    base_colour.set_editor_property("default_value", unreal.LinearColor(0.36, 0.72, 0.18, 1.0))

opacity = make(unreal.MaterialExpressionScalarParameter, -600, 60)
if opacity is not None:
    opacity.set_editor_property("parameter_name", unreal.Name("Opacity"))
    opacity.set_editor_property("default_value", 1.0)

# ---------------------------------------------------------------------------------------
# Radial falloff
#
# UV (0..1 across the projection) -> SphereMask against the centre -> OneMinus -> * Opacity.
# SphereMask rather than a length-and-divide chain because it takes the radius and the edge
# softness as inputs, which is exactly the two things worth being able to tune later.
# ---------------------------------------------------------------------------------------
# MaterialExpressionTextureCoordinate, NOT "TexCoord": the class is named after the node's
# full title even though the pin is labelled UV.
uv = make(unreal.MaterialExpressionTextureCoordinate, -600, 320)
centre = make(unreal.MaterialExpressionConstant2Vector, -600, 460)
if centre is not None:
    centre.set_editor_property("r", 0.5)
    centre.set_editor_property("g", 0.5)

radius = make(unreal.MaterialExpressionConstant, -600, 560)
if radius is not None:
    radius.set_editor_property("r", 0.5)

hardness = make(unreal.MaterialExpressionScalarParameter, -600, 660)
if hardness is not None:
    hardness.set_editor_property("parameter_name", unreal.Name("Edge Softness"))
    hardness.set_editor_property("default_value", 0.7)

mask = make(unreal.MaterialExpressionSphereMask, -320, 400)
connect(uv, "", mask, "A")
connect(centre, "", mask, "B")
connect(radius, "", mask, "Radius")
connect(hardness, "", mask, "Hardness")

# No OneMinus here, and that is not an oversight: SphereMask already returns 1 near the centre
# and 0 past the radius, which is the shape a puddle wants. An earlier version inverted it and
# then discovered that OneMinus's input pin will not accept a connection from Python at all -
# see Tools/kb_probe_material_wiring.py, which tests that against every node in this graph.
fade = make(unreal.MaterialExpressionMultiply, 120, 300)
connect(mask, "", fade, "A")
connect(opacity, "", fade, "B")

# ---------------------------------------------------------------------------------------
# Outputs
# ---------------------------------------------------------------------------------------
if base_colour is not None:
    unreal.MaterialEditingLibrary.connect_material_property(
        base_colour, "", unreal.MaterialProperty.MP_BASE_COLOR)

# The falloff chain, or the bare Opacity parameter if building it failed. A flat square decal
# is a worse-looking puddle but a working one, and a material with no opacity connected at all
# would leave the gore system running with nothing visible - the exact silent failure this
# project has been bitten by before.
opacity_source = fade if fade is not None else opacity
if opacity_source is not None:
    unreal.MaterialEditingLibrary.connect_material_property(
        opacity_source, "", unreal.MaterialProperty.MP_OPACITY)

# Note: NOT used_with_instanced_static_meshes - that flag is for the swarm's instanced meshes.
# A decal is drawn by the deferred decal pass and does not need it.
unreal.MaterialEditingLibrary.recompile_material(material)

# ---------------------------------------------------------------------------------------
# Save and read back
#
# save_asset with only_if_is_dirty=False, NOT save_loaded_asset: property writes from Python
# do not reliably dirty a package, and save_loaded_asset then writes nothing at all. The
# read-back here only proves the graph is in memory - the real check is a SEPARATE process.
# ---------------------------------------------------------------------------------------
# save_loaded_asset, NOT save_asset(path).
#
# save_asset resolves the path through the ASSET REGISTRY first, and a package created moments
# ago in this process has not been registered yet - so it fails with "The AssetData ... could
# not be found in the Asset Registry" and writes nothing, while the in-memory object is
# perfectly valid. save_loaded_asset takes the object itself and skips the lookup.
#
# This is the opposite of the rule for EDITING an existing asset, where save_asset(path,
# only_if_is_dirty=False) is required because Python property writes do not dirty the package.
# Freshly created here -> save_loaded_asset. Editing something on disk -> save_asset.
if not unreal.EditorAssetLibrary.save_loaded_asset(material):
    raise RuntimeError("failed to save {}".format(FULL_PATH))

log("DONE - {}".format(FULL_PATH))

# What this process CAN report is the graph it just built.
log("  domain = {}".format(material.get_editor_property("material_domain")))
log("  vector params: {}".format(unreal.MaterialEditingLibrary.get_vector_parameter_names(material)))
log("  scalar params: {}".format(unreal.MaterialEditingLibrary.get_scalar_parameter_names(material)))

# What it CANNOT report is whether any of that reached disk.
#
# Two reasons, both of which cost real time to learn here:
#   - load_asset in this process returns the in-memory object, so it always "succeeds"
#   - and the asset registry does not know about a package created moments ago, so a lookup
#     through it fails even though the file is written
# The only honest check is Tools/kb_verify_slime_material.py, in a separate process.
log("  (verify on disk with kb_verify_slime_material.py, NOT from here)")

if problems:
    raise RuntimeError("{} problem(s): {}".format(len(problems), "; ".join(problems)))
