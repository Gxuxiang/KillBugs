"""
Bakes the bug's skeletal animation into vertex-animation textures, so 600 bugs can play it
while still drawn as instanced static meshes.

The swarm is instanced - one draw call per archetype - and an instanced static mesh cannot play
a skeletal animation. AnimToTexture's answer is to bake the animation into textures and drive
the vertices from the material, which is what this script produces:

  SM_...   a static mesh carrying the bug's shape
  T_...    vertex position / normal textures, one texel per vertex per frame
  DA_...   the data asset tying them together, plus the frame range per animation
  MI_...   a material instance whose parameters point at those textures

The last one is what the swarm's instanced mesh draws with.

WHY THE STATIC MESH AND THE MATERIAL INSTANCE MIGHT FAIL HERE: this project has no working
headless path for saving some asset types - see the notes at the bottom, and the memory entry
about materials in particular. The BAKE itself is attempted first and reported separately, so a
failure in the asset plumbing does not hide whether the bake worked.
"""

import time

import unreal

# ---- Inputs: what the user imported -----------------------------------------------------
SKELETAL_MESH_PATH = "/Game/KillBugs/Mesh/bug/bug.bug"
ANIM_SEQUENCE_PATH = "/Game/KillBugs/Mesh/bug/bug_Anim.bug_Anim"

# ---- Outputs ----------------------------------------------------------------------------
OUT_PACKAGE = "/Game/KillBugs/Enemies"
STATIC_MESH_NAME = "SM_Bug_VertexAnimation"
DATA_ASSET_NAME = "DA_Bug_VertexAnimation"
MATERIAL_INSTANCE_NAME = "MI_Bug_VertexAnimation"

# The plugin ships this; it is the material that knows how to read the baked textures. Copying
# its parameter contract is far safer than rebuilding the graph by hand.
PLUGIN_BASE_MATERIAL = ("/AnimToTexture/Characters/Mannequin/Materials/VertexAnimation/"
                        "M_Body_VertexAnimation.M_Body_VertexAnimation")

PREFIX = "[KBBugVat] "
problems = []


def log(message):
    unreal.log(PREFIX + str(message))


def warn(message):
    unreal.log_warning(PREFIX + str(message))
    problems.append(str(message))


def save_asset(asset):
    """save_loaded_asset, because the asset registry will not know a fresh package yet."""
    if asset is None:
        return False
    return unreal.EditorAssetLibrary.save_loaded_asset(asset)


# The registry is not ready the moment -ExecutePythonScript runs.
registry = unreal.AssetRegistryHelpers.get_asset_registry()
for _ in range(20):
    if registry.is_loading_assets():
        time.sleep(1.0)
    else:
        break

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

mesh = unreal.EditorAssetLibrary.load_asset(SKELETAL_MESH_PATH)
animation = unreal.EditorAssetLibrary.load_asset(ANIM_SEQUENCE_PATH)

if mesh is None:
    raise RuntimeError("could not load {}".format(SKELETAL_MESH_PATH))
if animation is None:
    raise RuntimeError("could not load {}".format(ANIM_SEQUENCE_PATH))

log("source mesh : {} ({})".format(mesh.get_name(), mesh.get_class().get_name()))
log("source anim : {} ({}) length={:.3f}s".format(
    animation.get_name(), animation.get_class().get_name(), animation.get_play_length()))

# ---------------------------------------------------------------------------------------
# 1. The static mesh that will carry the shape
#
# ConvertSkeletalMeshToStaticMesh, not a plain copy: the result has to be a static mesh whose
# vertices the material can move, which is a different asset from the original.
# ---------------------------------------------------------------------------------------
static_mesh = None
try:
    static_mesh = unreal.AnimToTextureBPLibrary.convert_skeletal_mesh_to_static_mesh(
        mesh, "{}/{}".format(OUT_PACKAGE, STATIC_MESH_NAME), 0)
    log("converted to static mesh: {}".format(
        static_mesh.get_path_name() if static_mesh else "None"))
except Exception as error:  # noqa: BLE001 - report and carry on
    warn("ConvertSkeletalMeshToStaticMesh failed: {}".format(error))

if static_mesh is not None:
    # The baked vertices move outside the rest pose and the renderer culls against the static
    # bounds, so those bounds have to be inflated or the swarm pops out of existence near the
    # screen edge.
    #
    # NOT done here: UAnimToTextureBPLibrary::SetBoundsExtensions is a plain static C++ function
    # with no UFUNCTION macro, so Python cannot see it at all. The swarm inflates its instanced
    # component's bounds instead (UPrimitiveComponent::SetBoundsScale), which works per component
    # and needs no asset edit - see KBEnemyVisualizerComponent.
    if not save_asset(static_mesh):
        warn("could not save the converted static mesh")

# ---------------------------------------------------------------------------------------
# 2. The data asset describing the bake
# ---------------------------------------------------------------------------------------
factory = unreal.DataAssetFactory()
factory.set_editor_property("data_asset_class", unreal.AnimToTextureDataAsset)

data_asset = asset_tools.create_asset(
    DATA_ASSET_NAME, OUT_PACKAGE, unreal.AnimToTextureDataAsset, factory)
if data_asset is None:
    raise RuntimeError("could not create the AnimToTexture data asset")

# VERTEX mode, and this is NOT a preference - the data asset DEFAULTS to Bone and Bone mode
# crashes the editor here.
#
# In Bone mode AnimationToTexture writes a bone-weight texture (AnimToTextureBPLibrary.cpp:334),
# but that texture is only ever populated through the editor's own property-change path, which a
# script never goes through. It is null, and the plugin asserts on it:
#   Assertion failed: Texture [AnimToTextureUtils.h:161]
# taking the whole editor process down mid-bake.
#
# Vertex mode stores per-vertex position and normal instead, needs no weight texture, and no
# secondary mesh sharing this skeleton exists to make the bone format worth its trouble anyway.
mode = None
for candidate in ("VERTEX", "Vertex"):
    mode = getattr(unreal.AnimToTextureMode, candidate, None)
    if mode is not None:
        break

if mode is None:
    warn("could not resolve AnimToTextureMode::Vertex - the bake will run in Bone mode and crash")
else:
    data_asset.set_editor_property("mode", mode)
    log("mode = Vertex")

data_asset.set_editor_property("skeletal_mesh", mesh)
data_asset.set_editor_property("skeletal_lod_index", 0)
if static_mesh is not None:
    data_asset.set_editor_property("static_mesh", static_mesh)
    data_asset.set_editor_property("static_lod_index", 0)

# UV2, not UV1, and the reason is not cosmetic: ConvertSkeletalMeshToStaticMesh produces a mesh
# whose build settings GENERATE LIGHTMAP UVs, and those land in UV1. CheckDataAsset refuses
# outright if the channel the bake wants is the lightmap one -
# "Invalid StaticMesh UVChannel: 1. Already used by LightMap" - and the bake returns false with
# nothing else to go on.
data_asset.set_editor_property("uv_channel", 2)

# One entry per animation. Only the walk cycle exists today; the field is a list so an attack or
# a death animation can be added later without changing anything else.
anim_info = unreal.AnimToTextureAnimSequenceInfo()
anim_info.set_editor_property("enabled", True)
anim_info.set_editor_property("anim_sequence", animation)
anim_info.set_editor_property("use_custom_range", False)
data_asset.set_editor_property("anim_sequences", [anim_info])

log("data asset configured (uv_channel=1, 1 animation)")

# ---------------------------------------------------------------------------------------
# 3. Bake
# ---------------------------------------------------------------------------------------
baked = False
try:
    baked = unreal.AnimToTextureBPLibrary.animation_to_texture(data_asset)
    log("AnimationToTexture -> {}".format(baked))
except Exception as error:  # noqa: BLE001
    warn("AnimationToTexture raised: {}".format(error))

# What the bake actually decided. These numbers are the whole reason to do it this way: the
# position texture is (vertices x frames), and knowing them says whether the mesh is a sane size.
for prop in ("num_frames", "vertex_rows_per_frame", "num_bones", "texture_width", "texture_height"):
    try:
        log("  baked {} = {}".format(prop, data_asset.get_editor_property(prop)))
    except Exception:  # noqa: BLE001 - property names vary, and a missing one is not fatal
        pass

if not save_asset(data_asset):
    warn("could not save the data asset")

# ---------------------------------------------------------------------------------------
# 4. The material instance the swarm draws with
# ---------------------------------------------------------------------------------------
base_material = unreal.EditorAssetLibrary.load_asset(PLUGIN_BASE_MATERIAL)
if base_material is None:
    warn("could not load the plugin's base material at {} - the material instance was skipped, "
         "and the swarm will keep drawing with whatever material the archetype already has"
         .format(PLUGIN_BASE_MATERIAL))
else:
    log("base material: {}".format(base_material.get_path_name()))
    try:
        mi = asset_tools.create_asset(
            MATERIAL_INSTANCE_NAME, OUT_PACKAGE, unreal.MaterialInstanceConstant,
            unreal.MaterialInstanceConstantFactoryNew())

        if mi is not None:
            mi.set_editor_property("parent", base_material)

            # The plugin's own helper wires the frame count and the textures into the instance,
            # which is why the base material does not have to be rebuilt by hand here.
            #
            # The third parameter is an EMaterialParameterAssociation with a DEFAULT. Passing a
            # bool for it fails at the reflection boundary ("Failed to convert parameter
            # 'material_parameter_association'"), so it is left off entirely.
            unreal.AnimToTextureBPLibrary.update_material_instance_from_data_asset(data_asset, mi)

            if save_asset(mi):
                log("material instance saved: {}".format(mi.get_path_name()))
            else:
                warn("could not save the material instance")
        else:
            warn("could not create the material instance")
    except Exception as error:  # noqa: BLE001
        warn("material instance step failed: {}".format(error))

# ---------------------------------------------------------------------------------------
# Report
#
# Everything above is checked as it goes rather than at the end, because the failure modes here
# are all silent: a bake that produces nothing, and assets that are created in memory but never
# reach disk. The real check is Tools/kb_verify_bug_vat.py in a SEPARATE process.
# ---------------------------------------------------------------------------------------
log("DONE (baked={})".format(baked))
log("  verify on disk with kb_verify_bug_vat.py, NOT from here")

if problems:
    raise RuntimeError("{} problem(s): {}".format(len(problems), "; ".join(problems)))
