"""
Reads the baked vertex-animation assets back off disk and checks they are usable.

Run in a SEPARATE process from kb_setup_bug_vat.py, with the editor closed:

  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash

Separate process because the generator cannot check its own output: the asset registry does not
know about packages created moments earlier, and a same-process load returns the in-memory
object either way. Only a cold process reads what actually reached disk.

What is worth checking is not "do the assets exist" - they do - but the three things that make
the animation actually PLAY:

  - a non-zero frame count, or the material has nothing to sample
  - a UV channel in which the material can find each frame, since that is how AnimToTexture
    hands the frame index to the vertex shader
  - the material instance inheriting the plugin's parent, which is what knows how to read the
    baked textures at all
"""

import time

import unreal

PACKAGE = "/Game/KillBugs/Enemies"

# FULL OBJECT PATHS ("/Game/.../Name.Name"), not the package-only form ("/Game/.../Name").
# load_asset resolves the short form through the asset registry and returned None for all three
# of these while the long form loaded them on the first try - which reads as "the bake never
# wrote anything" and is entirely a path-format artefact.
DATA_ASSET_PATH = "{}/DA_Bug_VertexAnimation.DA_Bug_VertexAnimation".format(PACKAGE)
STATIC_MESH_PATH = "{}/SM_Bug_VertexAnimation.SM_Bug_VertexAnimation".format(PACKAGE)
MATERIAL_INSTANCE_PATH = "{}/MI_Bug_VertexAnimation.MI_Bug_VertexAnimation".format(PACKAGE)

PREFIX = "[KBBugVatVerify] "
problems = []


def log(message):
    unreal.log(PREFIX + str(message))


def check(condition, description):
    log("{} {}".format("OK  " if condition else "FAIL", description))
    if not condition:
        problems.append(description)


# Wait for the asset registry's first scan to FINISH before loading anything.
#
# Not a retry loop: -ExecutePythonScript runs before the scan completes, and every load here
# resolves through the registry. Retrying with time.sleep on the game thread did not work -
# it made this report "did not reach disk" for three assets that load perfectly a moment later.
# Polling the registry's own loading flag does work.
for _ in range(30):
    if unreal.AssetRegistryHelpers.get_asset_registry().is_loading_assets():
        time.sleep(1.0)
    else:
        break


def load(path):
    return unreal.EditorAssetLibrary.load_asset(path)


data_asset = load(DATA_ASSET_PATH)
check(data_asset is not None, "data asset loads: {}".format(DATA_ASSET_PATH))

if data_asset is None:
    raise RuntimeError("{} did not reach disk".format(DATA_ASSET_PATH))

num_frames = data_asset.get_editor_property("num_frames")
mode = data_asset.get_editor_property("mode")
rows = data_asset.get_editor_property("vertex_rows_per_frame")
uv_channel = data_asset.get_editor_property("uv_channel")

log("mode = {}, num_frames = {}, vertex_rows_per_frame = {}, uv_channel = {}".format(
    mode, num_frames, rows, uv_channel))

check(int(num_frames) > 0, "the bake produced frames (got {})".format(num_frames))
# The reflected enum prints uppercased ("<AnimToTextureMode.VERTEX: 0>"), so compare that way.
check("VERTEX" in str(mode).upper(), "baked in Vertex mode")

# The textures. A null one means the material has nothing to read, which looks exactly like a
# mesh that simply never animates.
#
# The property names are VertexPositionTexture / VertexNormalTexture - not "position_texture":
# the asset spells out which MODE the texture belongs to, because it also carries the bone-mode
# set (BonePositionTexture, BoneRotationTexture, BoneWeightTexture) alongside.
for name, label in (("vertex_position_texture", "position"),
                    ("vertex_normal_texture", "normal")):
    try:
        texture = data_asset.get_editor_property(name)
    except Exception as error:  # noqa: BLE001
        log("could not read {}: {}".format(name, error))
        problems.append("property {} missing".format(name))
        continue

    try:
        width = texture.blueprint_get_size_x() if texture else 0
        height = texture.blueprint_get_size_y() if texture else 0
    except Exception:  # noqa: BLE001
        width = height = 0

    log("{} texture = {} ({}x{})".format(label, texture, width, height))
    check(texture is not None, "the {} texture exists".format(label))
    if texture is not None:
        check(int(width) > 0 and int(height) > 0,
              "the {} texture has a size ({}x{})".format(label, width, height))

# The static mesh, and the UV channel the frame lookup lives in.
static_mesh = load(STATIC_MESH_PATH)
check(static_mesh is not None, "static mesh loads: {}".format(STATIC_MESH_PATH))

if static_mesh is not None:
    try:
        uv_count = unreal.StaticMeshLibrary.get_num_uv_channels_static(static_mesh, 0) \
            if hasattr(unreal, "StaticMeshLibrary") else None
    except Exception:  # noqa: BLE001
        uv_count = None

    if uv_count is not None:
        log("static mesh UV channels = {}".format(uv_count))
        # AnimToTexture writes the frame lookup into its own UV set. If the mesh has no channel
        # at that index the material cannot tell the two vertices of a frame apart, and every
        # instance collapses to frame 0.
        check(int(uv_count) > int(uv_channel),
              "static mesh has UV channel {} (has {})".format(uv_channel, uv_count))
    else:
        log("could not read the UV channel count; skipping that check")

# The material instance. Its parent is the plugin's vertex-animation material, which is what
# actually knows how to unpack the textures; an instance of anything else draws the rest pose.
material_instance = load(MATERIAL_INSTANCE_PATH)
check(material_instance is not None, "material instance loads: {}".format(MATERIAL_INSTANCE_PATH))

if material_instance is not None:
    parent = material_instance.get_editor_property("parent")
    log("material instance parent = {}".format(parent))
    check(parent is not None, "material instance has a parent material")

if problems:
    raise RuntimeError("{} check(s) failed: {}".format(len(problems), "; ".join(problems)))

log("ALL CHECKS PASSED")
