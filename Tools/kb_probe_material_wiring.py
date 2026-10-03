"""
Read-only probe: which node-to-node wiring actually works from Python in this build?

Creates a scratch material in the TRANSIENT package (never saved), builds a few nodes, and
tries connecting them with each candidate output name. Prints what succeeds.

This exists because the output side of ConnectMaterialExpressions is the one thing our material
tooling had never exercised: a parameter node answers to an empty output name, but a node whose
output is computed rather than declared may not.
"""

import unreal

PREFIX = "[KBWiringProbe] "


def log(message):
    unreal.log(PREFIX + str(message))


asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
material = asset_tools.create_asset(
    "M_ProbeWiring", "/Game/KillBugs/Enemies", unreal.Material, unreal.MaterialFactoryNew())

if material is None:
    raise RuntimeError("could not create the scratch material")


def make(expression_class):
    node = unreal.MaterialEditingLibrary.create_material_expression(material, expression_class, 0, 0)
    if node is None:
        log("CREATE FAILED: {}".format(expression_class))
    return node


uv = make(unreal.MaterialExpressionTextureCoordinate)
constant = make(unreal.MaterialExpressionConstant2Vector)
sphere = make(unreal.MaterialExpressionSphereMask)
distance = make(unreal.MaterialExpressionDistance)
one_minus = make(unreal.MaterialExpressionOneMinus)
multiply = make(unreal.MaterialExpressionMultiply)

log("nodes created: uv={} const={} sphere={} dist={} oneminus={} mul={}".format(
    uv is not None, constant is not None, sphere is not None, distance is not None,
    one_minus is not None, multiply is not None))


def try_connect(from_node, from_output, to_node, to_input, label):
    if from_node is None or to_node is None:
        log("SKIP {} (missing node)".format(label))
        return
    ok = unreal.MaterialEditingLibrary.connect_material_expressions(
        from_node, from_output, to_node, to_input)
    log("{} from_output='{}' to_input='{}' -> {}".format(label, from_output, to_input, ok))


# Inputs: do they work at all?
try_connect(uv, "", sphere, "A", "UV -> SphereMask.A")
try_connect(constant, "", sphere, "B", "Const2V -> SphereMask.B")

# The unknown: what is SphereMask's output called?
for name in ("", "Result", "Output", "RGBA", "R"):
    try_connect(sphere, name, one_minus, "Input", "SphereMask -> OneMinus.Input")

# And Distance, in case SphereMask is the awkward one.
try_connect(uv, "", distance, "A", "UV -> Distance.A")
try_connect(constant, "", distance, "B", "Const2V -> Distance.B")
try_connect(distance, "", one_minus, "Input", "Distance -> OneMinus.Input")

# Multiply's input pin names, in case they are not A/B.
try_connect(one_minus, "", multiply, "A", "OneMinus -> Multiply.A")
try_connect(one_minus, "", multiply, "B", "OneMinus -> Multiply.B")

log("PROBE DONE (nothing saved; the scratch material is transient)")
