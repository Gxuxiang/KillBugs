"""
Generates the starting weapons.

Run headlessly, with the EDITOR CLOSED:
  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

Must not be run while an editor holds the assets open, and the assets must not already
exist - see the notes in kb_setup_enemies.py for why both matter. Verify the result in a
separate process with kb_verify_enemies.py.
"""

import unreal

PACKAGE_PATH = "/Game/KillBugs/Weapons"
PREFIX = "[KBWeapons] "


def log(message):
    unreal.log(PREFIX + str(message))


def warn(message):
    unreal.log_warning(PREFIX + str(message))


def enum_member(candidates, members):
    """Best-effort enum lookup; UE Python strips the E prefix from reflected enums."""
    for enum_name in candidates:
        enum_type = getattr(unreal, enum_name, None)
        if enum_type is None:
            continue
        for member in members:
            if hasattr(enum_type, member):
                return getattr(enum_type, member)
        warn("none of {} on {}; available: {}".format(
            members, enum_name, [n for n in dir(enum_type) if not n.startswith("_")]))
        return None
    warn("no enum exposed for {}".format(candidates))
    return None


BEHAVIOR_AUTO = enum_member(("KBWeaponBehavior", "EKBWeaponBehavior"), ("AUTO",))
BEHAVIOR_MANUAL = enum_member(("KBWeaponBehavior", "EKBWeaponBehavior"), ("MANUAL",))
DELIVERY_HITSCAN = enum_member(("KBWeaponDelivery", "EKBWeaponDelivery"), ("HITSCAN",))
DELIVERY_PROJECTILE = enum_member(("KBWeaponDelivery", "EKBWeaponDelivery"), ("PROJECTILE",))
DELIVERY_RADIAL = enum_member(("KBWeaponDelivery", "EKBWeaponDelivery"), ("RADIAL",))

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def make_weapon(asset_name, **properties):
    full_path = "{}/{}".format(PACKAGE_PATH, asset_name)
    if unreal.EditorAssetLibrary.does_asset_exist(full_path):
        raise RuntimeError(
            "{} already exists. Remove it from outside the editor first: "
            "rm Content/KillBugs/Weapons/*.uasset".format(full_path))

    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.KBWeaponDefinition)

    asset = asset_tools.create_asset(asset_name, PACKAGE_PATH, unreal.KBWeaponDefinition, factory)
    if asset is None:
        raise RuntimeError("create_asset failed for {}".format(full_path))

    for key, value in properties.items():
        if value is None:
            continue
        try:
            asset.set_editor_property(key, value)
        except Exception as error:  # noqa: BLE001 - report and keep going
            warn("could not set {} on {}: {}".format(key, asset_name, error))

    # Force the write. A plain save_loaded_asset only writes when the package is dirty, and
    # a Python property change does not reliably mark it so.
    unreal.EditorAssetLibrary.save_asset(full_path, only_if_is_dirty=False)
    log("wrote {}".format(full_path))
    return asset


# --- Auto rifle: the workhorse. Fires itself, so the player only has to position. --------
make_weapon(
    "DA_Weapon_AutoRifle",
    display_name=unreal.Text("自动步枪"),
    description=unreal.Text("自动向最近的虫子开火。"),
    behavior=BEHAVIOR_AUTO,
    delivery=DELIVERY_PROJECTILE,
    base_damage=12.0, damage_per_level=6.0,
    base_cooldown=0.55, cooldown_per_level=-0.04,
    max_level=8,
    # A small round that tests against fat targets. The hit radius is drawn at its true size,
    # so it has to stay small enough to read as a bullet.
    range=1300.0, hit_radius=18.0,
    projectile_speed=2600.0,
    projectiles_per_shot=1,
    tracer_color=unreal.LinearColor(1.0, 0.85, 0.3, 1.0),
    # The two starters are owned from the first launch, so their price is never paid - but a
    # price is still set, because a weapon with no price is a weapon the shop refuses to show,
    # and "can I buy this back if I lose it" should have an answer.
    buy_price_gold=250, base_upgrade_cost=8, upgrade_cost_per_level=4,
)

# --- Shockwave: auto, but area. Rewards letting the swarm bunch up. ----------------------
make_weapon(
    "DA_Weapon_Shockwave",
    display_name=unreal.Text("冲击波"),
    description=unreal.Text("周期性在最近的虫子处引发范围爆发。"),
    behavior=BEHAVIOR_AUTO,
    delivery=DELIVERY_RADIAL,
    base_damage=18.0, damage_per_level=7.0,
    base_cooldown=1.7, cooldown_per_level=-0.09,
    max_level=8,
    range=800.0, radial_radius=260.0,
    tracer_color=unreal.LinearColor(0.4, 0.7, 1.0, 1.0),
    buy_price_gold=600, base_upgrade_cost=15, upgrade_cost_per_level=8,
)

# --- Shotgun: the manual half of the hybrid. Player aims it and holds the trigger. -------
make_weapon(
    "DA_Weapon_Shotgun",
    display_name=unreal.Text("霰弹枪"),
    description=unreal.Text("按住开火，轰击准星方向的目标。"),
    behavior=BEHAVIOR_MANUAL,
    delivery=DELIVERY_PROJECTILE,
    # PER PELLET. Six pellets landing is 84, which is the point: the shotgun has to out-damage
    # the rifle by a wide margin, because the rifle needs no attention at all while the
    # shotgun costs aim and timing. A manual weapon that is merely equal is not worth using.
    base_damage=14.0, damage_per_level=5.0,
    base_cooldown=0.9, cooldown_per_level=-0.05,
    max_level=8,
    range=900.0, hit_radius=22.0,
    projectiles_per_shot=6,
    spread_degrees=24.0,
    projectile_speed=1700.0,
    tracer_color=unreal.LinearColor(1.0, 0.55, 0.2, 1.0),
    buy_price_gold=250, base_upgrade_cost=8, upgrade_cost_per_level=4,
)

log("--- weapons ---")
for name in ("DA_Weapon_AutoRifle", "DA_Weapon_Shockwave", "DA_Weapon_Shotgun"):
    path = "{}/{}".format(PACKAGE_PATH, name)
    loaded = unreal.EditorAssetLibrary.load_asset(path)
    if loaded is None:
        warn("MISSING " + path)
        continue
    log("  {:<20} behavior={} delivery={} dmg={} cd={} range={}".format(
        name,
        loaded.get_editor_property("behavior"),
        loaded.get_editor_property("delivery"),
        loaded.get_editor_property("base_damage"),
        loaded.get_editor_property("base_cooldown"),
        loaded.get_editor_property("range")))

log("DONE")

# Said at RUNTIME, not only in the docstring above.
#
# This script CREATES the weapon assets, which means it can only run when they are absent - so
# using it to change one field means deleting them first, and everything this file does not set
# is silently gone. On 2026-10-05 exactly that happened: the assets were deleted and recreated to
# add shop prices, and the shotgun came back with no recoil and no fire sound, because those live
# in kb_setup_weapon_recoil.py and kb_setup_weapon_audio.py. The docstring warned about it; the
# docstring was read afterwards.
warn("REMINDER: this script does not set recoil or audio. If you just recreated the weapons, "
     "run kb_setup_weapon_recoil.py and kb_setup_weapon_audio.py next, or those fields are zero "
     "and the guns will be silent and kickless.")

try:
    unreal.SystemLibrary.quit_editor()
except Exception as error:  # noqa: BLE001 - shutdown is best-effort
    warn("quit_editor failed: {}".format(error))
