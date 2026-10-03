"""
Generates the card pool.

Run headlessly, with the EDITOR CLOSED, and with the assets not already present:

  rm Content/KillBugs/Cards/*.uasset
  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

Verify in a separate process with kb_verify_enemies.py.
"""

import unreal

PACKAGE_PATH = "/Game/KillBugs/Cards"
PREFIX = "[KBCards] "


def log(message):
    unreal.log(PREFIX + str(message))


def warn(message):
    unreal.log_warning(PREFIX + str(message))


def enum_member(candidates, members):
    for enum_name in candidates:
        enum_type = getattr(unreal, enum_name, None)
        if enum_type is None:
            continue
        for member in members:
            if hasattr(enum_type, member):
                return getattr(enum_type, member)
        warn("none of {} on {}".format(members, enum_name))
        return None
    warn("no enum exposed for {}".format(candidates))
    return None


RARITY_COMMON = enum_member(("KBCardRarity", "EKBCardRarity"), ("COMMON",))
RARITY_RARE = enum_member(("KBCardRarity", "EKBCardRarity"), ("RARE",))
RARITY_EPIC = enum_member(("KBCardRarity", "EKBCardRarity"), ("EPIC",))

EFFECT_ADD_WEAPON = enum_member(("KBCardEffect", "EKBCardEffect"), ("ADD_WEAPON",))
EFFECT_UPGRADE_WEAPON = enum_member(("KBCardEffect", "EKBCardEffect"), ("UPGRADE_WEAPON",))
EFFECT_STAT = enum_member(("KBCardEffect", "EKBCardEffect"), ("STAT_BOOST",))
EFFECT_HEAL = enum_member(("KBCardEffect", "EKBCardEffect"), ("HEAL",))

REPEAT_STACKABLE = enum_member(("KBCardRepeatRule", "EKBCardRepeatRule"), ("STACKABLE",))
REPEAT_ONCE_PER_RUN = enum_member(("KBCardRepeatRule", "EKBCardRepeatRule"), ("ONCE_PER_RUN",))

asset_tools = unreal.AssetToolsHelpers.get_asset_tools()


def load_asset(path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        warn("could not load " + path)
    return asset


def mods(**values):
    """Builds an FKBStatMods, leaving every field at its neutral default."""
    stat_mods = unreal.KBStatMods()
    for key, value in values.items():
        try:
            stat_mods.set_editor_property(key, value)
        except Exception as error:  # noqa: BLE001
            warn("could not set stat mod {}: {}".format(key, error))
    return stat_mods


def make_card(asset_name, **properties):
    full_path = "{}/{}".format(PACKAGE_PATH, asset_name)
    if unreal.EditorAssetLibrary.does_asset_exist(full_path):
        raise RuntimeError(
            "{} already exists. Remove it outside the editor first: "
            "rm Content/KillBugs/Cards/*.uasset".format(full_path))

    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.KBCardDefinition)

    asset = asset_tools.create_asset(asset_name, PACKAGE_PATH, unreal.KBCardDefinition, factory)
    if asset is None:
        raise RuntimeError("create_asset failed for " + full_path)

    for key, value in properties.items():
        if value is None:
            continue
        try:
            asset.set_editor_property(key, value)
        except Exception as error:  # noqa: BLE001
            warn("could not set {} on {}: {}".format(key, asset_name, error))

    unreal.EditorAssetLibrary.save_asset(full_path, only_if_is_dirty=False)
    return asset


AUTO_RIFLE = "/Game/KillBugs/Weapons/DA_Weapon_AutoRifle.DA_Weapon_AutoRifle"
SHOTGUN = "/Game/KillBugs/Weapons/DA_Weapon_Shotgun.DA_Weapon_Shotgun"
SHOCKWAVE = "/Game/KillBugs/Weapons/DA_Weapon_Shockwave.DA_Weapon_Shockwave"

rifle = load_asset(AUTO_RIFLE)
shotgun = load_asset(SHOTGUN)
shockwave = load_asset(SHOCKWAVE)

# --- Weapon cards -----------------------------------------------------------------------
make_card(
    "DA_Card_AddShockwave",
    title=unreal.Text("冲击波"),
    description=unreal.Text("周期性地在目标处引发范围爆发，命中范围内所有虫子。"),
    rarity=RARITY_RARE, weight=1.0,
    effect=EFFECT_ADD_WEAPON, repeat_rule=REPEAT_ONCE_PER_RUN,
    weapon=shockwave,
)

make_card(
    "DA_Card_UpgradeAutoRifle",
    title=unreal.Text("步枪强化"),
    description=unreal.Text("自动步枪伤害提高，射速略微加快。"),
    rarity=RARITY_COMMON, weight=1.0,
    effect=EFFECT_UPGRADE_WEAPON, repeat_rule=REPEAT_STACKABLE,
    requires_weapon=rifle,
)

make_card(
    "DA_Card_UpgradeShotgun",
    title=unreal.Text("重型弹丸"),
    description=unreal.Text("霰弹枪伤害提高，射速略微加快。"),
    rarity=RARITY_COMMON, weight=1.0,
    effect=EFFECT_UPGRADE_WEAPON, repeat_rule=REPEAT_STACKABLE,
    requires_weapon=shotgun,
)

make_card(
    "DA_Card_UpgradeShockwave",
    title=unreal.Text("扩散冲击"),
    description=unreal.Text("冲击波伤害更高，触发更频繁。"),
    rarity=RARITY_COMMON, weight=0.85,
    effect=EFFECT_UPGRADE_WEAPON, repeat_rule=REPEAT_STACKABLE,
    requires_weapon=shockwave,
)

# --- Stat cards -------------------------------------------------------------------------
make_card(
    "DA_Card_DamageUp",
    title=unreal.Text("开刃"),
    description=unreal.Text("所有武器伤害提高 15%。"),
    rarity=RARITY_COMMON, weight=1.0,
    effect=EFFECT_STAT, repeat_rule=REPEAT_STACKABLE,
    stat_mods=mods(damage_mult=1.15),
)

make_card(
    "DA_Card_Swift",
    title=unreal.Text("疾步"),
    description=unreal.Text("移动速度提高 12%。走位是你唯一的防御。"),
    rarity=RARITY_COMMON, weight=1.0,
    effect=EFFECT_STAT, repeat_rule=REPEAT_STACKABLE,
    stat_mods=mods(move_speed_mult=1.12),
)

make_card(
    "DA_Card_RapidFire",
    title=unreal.Text("速射"),
    description=unreal.Text("所有武器射速提高 15%。"),
    rarity=RARITY_RARE, weight=0.8,
    effect=EFFECT_STAT, repeat_rule=REPEAT_STACKABLE,
    stat_mods=mods(cooldown_mult=0.85),
)

make_card(
    "DA_Card_LongSight",
    title=unreal.Text("远见"),
    description=unreal.Text("经验拾取范围大幅扩大，经验获取提高 15%。"),
    rarity=RARITY_RARE, weight=0.7,
    effect=EFFECT_STAT, repeat_rule=REPEAT_STACKABLE,
    stat_mods=mods(pickup_radius_mult=1.5, xp_gain_mult=1.15),
)

make_card(
    "DA_Card_Toughness",
    title=unreal.Text("坚韧"),
    description=unreal.Text("生命上限提高 30，并立即补足这部分生命。"),
    rarity=RARITY_COMMON, weight=1.0,
    effect=EFFECT_STAT, repeat_rule=REPEAT_STACKABLE,
    stat_mods=mods(max_health_add=30.0),
)

make_card(
    "DA_Card_FieldRepair",
    title=unreal.Text("战地急救"),
    description=unreal.Text("立即将生命值恢复至满。"),
    rarity=RARITY_COMMON, weight=0.9,
    effect=EFFECT_HEAL, repeat_rule=REPEAT_STACKABLE,
)

make_card(
    "DA_Card_Veteran",
    title=unreal.Text("老兵"),
    description=unreal.Text("伤害提高 30%，射速提高 10%。"),
    rarity=RARITY_EPIC, weight=0.6, min_wave=2,
    effect=EFFECT_STAT, repeat_rule=REPEAT_ONCE_PER_RUN,
    stat_mods=mods(damage_mult=1.30, cooldown_mult=0.90),
)

# --- Summary ----------------------------------------------------------------------------
log("--- cards ---")
for name in ("DA_Card_AddShockwave", "DA_Card_UpgradeAutoRifle", "DA_Card_UpgradeShotgun",
             "DA_Card_UpgradeShockwave", "DA_Card_DamageUp", "DA_Card_Swift",
             "DA_Card_RapidFire", "DA_Card_LongSight", "DA_Card_Toughness",
             "DA_Card_FieldRepair", "DA_Card_Veteran"):
    path = "{}/{}".format(PACKAGE_PATH, name)
    loaded = unreal.EditorAssetLibrary.load_asset(path)
    if loaded is None:
        warn("MISSING " + path)
        continue
    log("  {:<26} {:<14} effect={} weight={}".format(
        name,
        str(loaded.get_editor_property("rarity")).split(".")[-1],
        str(loaded.get_editor_property("effect")).split(".")[-1],
        loaded.get_editor_property("weight")))

log("DONE")

try:
    unreal.SystemLibrary.quit_editor()
except Exception as error:  # noqa: BLE001
    warn("quit_editor failed: {}".format(error))
