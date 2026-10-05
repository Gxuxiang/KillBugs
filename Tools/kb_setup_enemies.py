"""
Generates the placeholder bug archetypes.

Run headlessly:
  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

The three archetypes are deliberately different in size, speed and health so the swarm reads
as a swarm rather than as one repeated blob, and so the steering behaviours are visibly
distinct while there is no art. AKBEnemyDirector loads them by path in its constructor.

Phase 6 replaces the meshes and materials with real bug art; the tuning here is throwaway.
"""

import unreal

PACKAGE_PATH = "/Game/KillBugs/Enemies"
PREFIX = "[KBEnemies] "


def log(message):
    unreal.log(PREFIX + str(message))


def warn(message):
    unreal.log_warning(PREFIX + str(message))


def load_asset(path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        warn("could not load {}".format(path))
    return asset


def make_archetype(asset_name, mesh_path, material_path, **properties):
    """
    Creates the archetype. It must NOT already exist.

    Regenerating means removing the old assets from OUTSIDE the editor first:

        rm Content/KillBugs/Enemies/DA_Bug_*.uasset

    Both alternatives were tried and both fail:
      * update-in-place - setting a property from Python does not mark the package dirty, so
        the save writes nothing and the edit is lost when the editor closes. It looks like it
        worked, because re-loading in the same process returns the still-modified in-memory
        object. That is how a material change stayed invisible on disk for several rounds.
      * delete-then-create in-editor - delete_asset drops the registry entry but leaves the
        package resident, so the following create_asset fails even after polling and a GC.
    """
    full_path = "{}/{}".format(PACKAGE_PATH, asset_name)

    if unreal.EditorAssetLibrary.does_asset_exist(full_path):
        raise RuntimeError(
            "{} already exists. Remove it from outside the editor first: "
            "rm Content/KillBugs/Enemies/*.uasset".format(full_path))

    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.KBEnemyArchetype)

    asset = asset_tools.create_asset(asset_name, PACKAGE_PATH, unreal.KBEnemyArchetype, factory)
    if asset is None:
        raise RuntimeError("create_asset failed for {}".format(full_path))

    mesh = load_asset(mesh_path)
    material = load_asset(material_path)

    if mesh is not None:
        asset.set_editor_property("mesh", mesh)
    if material is not None:
        asset.set_editor_property("material", material)

    # The baked vertex animation, for the same reason as mesh and material: set as an OBJECT,
    # not as a path string. It is popped out of the property bag here rather than left to the
    # generic loop below, which would hand set_editor_property a string where a TSoftObjectPtr
    # is wanted.
    anim_data_path = properties.pop("anim_data_path", None)
    if anim_data_path:
        anim_data = load_asset(anim_data_path)
        if anim_data is not None:
            asset.set_editor_property("anim_data", anim_data)
        else:
            warn("could not load anim data {}".format(anim_data_path))

    # Same reason as anim_data_path: the generic loop below would hand set_editor_property a
    # path string where a TSoftObjectPtr is wanted.
    death_effect_path = properties.pop("death_effect_path", None)
    if death_effect_path:
        death_effect = load_asset(death_effect_path)
        if death_effect is not None:
            asset.set_editor_property("death_effect", death_effect)
        else:
            warn("could not load death effect {}".format(death_effect_path))

    for key, value in properties.items():
        if value is None:
            # An enum that could not be resolved; leave the property at its C++ default
            # rather than failing the whole asset over a cosmetic difference.
            continue
        try:
            asset.set_editor_property(key, value)
        except Exception as error:  # noqa: BLE001 - report and keep going
            warn("could not set {} on {}: {}".format(key, asset_name, error))

    # save_asset with only_if_is_dirty=False, NOT save_loaded_asset.
    #
    # Setting a property from Python does not reliably mark an existing package dirty, so
    # save_loaded_asset silently writes nothing and every edit is lost when the editor
    # closes. That failure is invisible in-process: re-loading immediately returns the
    # in-memory object, which still looks correct.
    unreal.EditorAssetLibrary.save_asset(full_path, only_if_is_dirty=False)
    log("wrote {}".format(full_path))
    return asset


asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

# ---------------------------------------------------------------------------------------
# The real bug
#
# All three archetypes now share one baked mesh and one animation, distinguished by scale and
# by BodyRadius. One model is what the project has; three would be three meshes, three baked
# animation texture sets and three times the video memory for a difference the player reads at
# 50-130 pixels.
#
# These three paths come out of Tools/kb_setup_bug_vat.py, and the MATERIAL IS NOT OPTIONAL:
# MI_Bug_VertexAnimation is the instance that knows how to read the baked textures. Pointing
# the archetype back at the mesh's own M_bug would draw the bug in its rest pose forever, with
# no error anywhere.
# ---------------------------------------------------------------------------------------
# Authored by hand in the editor, in the plugin's own example workflow (BP_AnimToTexture), using
# BONE mode rather than the vertex mode kb_setup_bug_vat.py attempted. Bone mode is the plugin's
# default and the one its example material is built for; the scripted attempt had to force Vertex
# because Bone mode writes a bone-weight texture that only the editor's property-change path ever
# creates, and asserts on the null when driven from a script.
BUG_MESH = "/Game/KillBugs/Mesh/bug/VAT/SM_bug"
BUG_ANIM_DATA = "/Game/KillBugs/Mesh/bug/VAT/DA_BoneAnimation_bug"
BUG_MATERIAL = "/Game/KillBugs/Mesh/bug/VAT/M_bug_bone_Inst"

# The model is authored looking down -X, while the swarm turns each instance to face the way the
# bug is travelling - which assumes +X. 180 corrects it; without it the bugs run tail-first.
BUG_MESH_YAW_OFFSET = 180.0

# Uniform, and derived rather than guessed: the baked mesh measures 53.8 x 45.4 units half-extent,
# and BodyRadius is a radius, so scaling by BodyRadius / 53.8 makes the drawn bug's half-length
# exactly the circle it is hit in. The placeholder scales these replace were tuned for a
# 100-unit cube and would have rendered the bug at the wrong size.
#
#   Runner  28 / 53.81 = 0.52
#   Grunt   42 / 53.81 = 0.78
#   Brute   85 / 53.81 = 1.58

# Our own material, NOT any of the LevelPrototyping ones.
#
# Every prototype material has bUsedWithInstancedStaticMeshes = False, which means an
# InstancedStaticMeshComponent cannot render it and the renderer silently falls back to the
# engine default material - so all archetypes draw in the same grey regardless of the Base
# Color we set on their dynamic material instances. M_KBEnemy is generated by
# Tools/kb_setup_enemy_material.py with that flag enabled.
ENEMY_MAT = "/Game/KillBugs/Enemies/M_KBEnemy"
TINT_PARAM = "Base Color"

# The burst every archetype plays where it died, spawned by UKBGoreComponent::OnBugDied. One
# system covers all three: the per-archetype difference is User.SplatColor and User.SplatScale,
# which the gore component writes at spawn rather than needing a system per bug type.
DEATH_EFFECT = "/Game/KillBugs/VFX/FX_BugBlast"

# --- Resolve the reflected types defensively -------------------------------------------
# The exact Python names for game-module enums are not guaranteed, and a missing attribute
# here is a hard crash at import time. Resolve them, report what was found, and carry on
# with defaults for anything missing.
log("KBEnemyArchetype exposed: {}".format(hasattr(unreal, "KBEnemyArchetype")))
log("KB-named attributes: {}".format(sorted(
    name for name in dir(unreal) if "KB" in name or "Steering" in name or "RenderTier" in name)))


def enum_member(*candidate_enum_names, members):
    """Best-effort lookup across the naming conventions UE Python may use."""
    for enum_name in candidate_enum_names:
        enum_type = getattr(unreal, enum_name, None)
        if enum_type is None:
            continue
        for member in members:
            if hasattr(enum_type, member):
                return getattr(enum_type, member)
        warn("none of {} on {}; available: {}".format(
            members, enum_name, [n for n in dir(enum_type) if not n.startswith("_")]))
        return None

    warn("no enum exposed for {}; leaving that property at its default".format(candidate_enum_names))
    return None


# UE Python strips the type prefix from reflected enums: EKBSteeringBehavior becomes
# KBSteeringBehavior. The E-prefixed spellings are kept as fallbacks in case that changes.
STEER_SEEK = enum_member("KBSteeringBehavior", "EKBSteeringBehavior", members=("SEEK_NEAREST_PLAYER",))
STEER_CHARGE = enum_member("KBSteeringBehavior", "EKBSteeringBehavior", members=("CHARGE",))
STEER_ORBIT = enum_member("KBSteeringBehavior", "EKBSteeringBehavior", members=("ORBIT",))
TIER_INSTANCED = enum_member("KBRenderTier", "EKBRenderTier", members=("INSTANCED",))

# --- Grunt: the baseline. Slow, cheap, arrives in numbers. -----------------------------
make_archetype(
    "DA_Bug_Grunt", BUG_MESH, BUG_MATERIAL,
    anim_data_path=BUG_ANIM_DATA,
    death_effect_path=DEATH_EFFECT,
    mesh_yaw_offset=BUG_MESH_YAW_OFFSET,
    display_name=unreal.Text("Grunt"),
    mesh_scale=unreal.Vector(0.78, 0.78, 0.78),
    tint=unreal.LinearColor(0.42, 0.50, 0.62, 1.0),
    tint_parameter_name=unreal.Name(TINT_PARAM),
    base_health=10.0, health_per_wave=4.0,
    move_speed=270.0, speed_per_wave=8.0,
    contact_damage=5.0, damage_per_wave=1.0,
    body_radius=42.0,
    steering=STEER_SEEK,
    acceleration=900.0, turn_rate_degrees=420.0,
    xp_value=1, gold_value=0,
    # Loot. These match the C++ defaults, so loot works before this script is re-run - a field
    # missing from an existing asset reads as its default. Re-running is what makes the numbers
    # editable per archetype in the asset instead of merely equal to the default.
    material_drop_chance=0.15, material_drop_min=1, material_drop_max=1,
    medkit_drop_chance=0.03,
    render_tier=TIER_INSTANCED,
)

# --- Runner: fast and fragile, flattens the front line. ---------------------------------
make_archetype(
    "DA_Bug_Runner", BUG_MESH, BUG_MATERIAL,
    anim_data_path=BUG_ANIM_DATA,
    death_effect_path=DEATH_EFFECT,
    mesh_yaw_offset=BUG_MESH_YAW_OFFSET,
    display_name=unreal.Text("Runner"),
    mesh_scale=unreal.Vector(0.52, 0.52, 0.52),
    tint=unreal.LinearColor(0.95, 0.48, 0.10, 1.0),
    tint_parameter_name=unreal.Name(TINT_PARAM),
    base_health=6.0, health_per_wave=2.0,
    move_speed=430.0, speed_per_wave=12.0,
    contact_damage=4.0, damage_per_wave=1.0,
    body_radius=28.0,
    steering=STEER_CHARGE,
    acceleration=1600.0, turn_rate_degrees=200.0,
    xp_value=1, gold_value=0,
    # Drops slightly more often than a Grunt, because there are far fewer of them.
    material_drop_chance=0.20, material_drop_min=1, material_drop_max=1,
    medkit_drop_chance=0.05,
    render_tier=TIER_INSTANCED,
)

# --- Brute: slow, tanky, orbits so it is not simply a bigger grunt. ---------------------
make_archetype(
    "DA_Bug_Brute", BUG_MESH, BUG_MATERIAL,
    anim_data_path=BUG_ANIM_DATA,
    death_effect_path=DEATH_EFFECT,
    mesh_yaw_offset=BUG_MESH_YAW_OFFSET,
    display_name=unreal.Text("Brute"),
    mesh_scale=unreal.Vector(1.58, 1.58, 1.58),
    tint=unreal.LinearColor(0.62, 0.13, 0.13, 1.0),
    tint_parameter_name=unreal.Name(TINT_PARAM),
    # Tanky, but not a slog: 40 HP is roughly two shotgun blasts, so it reads as heavy
    # without stalling the wave.
    base_health=40.0, health_per_wave=10.0,
    move_speed=170.0, speed_per_wave=5.0,
    contact_damage=12.0, damage_per_wave=2.0,
    body_radius=85.0,
    steering=STEER_ORBIT,
    preferred_distance=320.0,
    acceleration=500.0, turn_rate_degrees=180.0,
    xp_value=5, gold_value=2,
    # The payday: rare enough that finding one is worth the trip, and dropping 2-3 at a time.
    material_drop_chance=0.60, material_drop_min=2, material_drop_max=3,
    medkit_drop_chance=0.10,
    render_tier=TIER_INSTANCED,
)

# --- Summary, so a run is verifiable from the log alone. --------------------------------
log("--- archetypes ---")
for name in ("DA_Bug_Grunt", "DA_Bug_Runner", "DA_Bug_Brute"):
    path = "{}/{}".format(PACKAGE_PATH, name)
    loaded = unreal.EditorAssetLibrary.load_asset(path)
    if loaded is None:
        warn("MISSING {}".format(path))
        continue
    tint = loaded.get_editor_property("tint")
    material = loaded.get_editor_property("material")
    mesh = loaded.get_editor_property("mesh")
    log("  {:<14} hp={:<6} speed={:<6} radius={:<6} tint=({:.2f},{:.2f},{:.2f})\n"
        "      mesh={} material={}".format(
            name,
            loaded.get_editor_property("base_health"),
            loaded.get_editor_property("move_speed"),
            loaded.get_editor_property("body_radius"),
            tint.r, tint.g, tint.b,
            mesh.get_path_name() if mesh else "NONE",
            material.get_path_name() if material else "NONE"))

log("DONE")

try:
    unreal.SystemLibrary.quit_editor()
except Exception as error:  # noqa: BLE001 - shutdown is best-effort
    warn("quit_editor failed: {}".format(error))
