"""
Read-only probe of the SoundMorph pack and the current weapon audio wiring.

Answers two questions before anything is changed:
  1. What class are the pack's assets? A MetaSound Preset is directly playable and can be
     referenced from a weapon; a bare MetaSound graph may need a preset or parameters.
  2. What has already been assigned on the three weapon assets, so a script does not
     silently overwrite hand-authored work.
"""

import unreal

PREFIX = "[KBSounds] "

WEAPONS = [
    "/Game/KillBugs/Weapons/DA_Weapon_AutoRifle",
    "/Game/KillBugs/Weapons/DA_Weapon_Shockwave",
    "/Game/KillBugs/Weapons/DA_Weapon_Shotgun",
]

FX_FIELDS = ["fire_sound", "muzzle_effect", "impact_sound", "impact_effect"]

SAMPLE_ASSETS = [
    "/Game/KillBugs/Sound/MS_Fire",
    "/Game/SoundMorphMetaSounds/Gun/Gun",
    "/Game/SoundMorphMetaSounds/Gun/Presets/Gun_Preset_1",
    "/Game/SoundMorphMetaSounds/Gun/Presets/Gun_Preset_2",
    "/Game/SoundMorphMetaSounds/Gun/Presets/Gun_Preset_3",
    "/Game/SoundMorphMetaSounds/Gun/Presets/Gun_Preset_4",
    "/Game/SoundMorphMetaSounds/Gun/Presets/Gun_Preset_5",
    "/Game/SoundMorphMetaSounds/Explosion/Explosion",
    "/Game/SoundMorphMetaSounds/Explosion/Presets/Explosion_Preset_1",
]


def describe(path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        return "MISSING"
    return "{}  ({})".format(asset.get_class().get_name(), asset.get_path_name())


unreal.log(PREFIX + "--- sound pack assets ---")
for path in SAMPLE_ASSETS:
    unreal.log(PREFIX + "  {:<62} {}".format(path.split("/")[-1], describe(path)))

unreal.log(PREFIX + "--- current weapon wiring ---")
for path in WEAPONS:
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        unreal.log_warning(PREFIX + "  MISSING " + path)
        continue

    unreal.log(PREFIX + "  " + path.split("/")[-1])
    for field in FX_FIELDS:
        try:
            value = asset.get_editor_property(field)
        except Exception as error:  # noqa: BLE001 - field may not exist yet
            unreal.log(PREFIX + "      {:<16} <{}>".format(field, error))
            continue

        # TSoftObjectPtr reads back as the resolved object, or None when unset.
        text = value.get_path_name() if value else "unset"
        unreal.log(PREFIX + "      {:<16} {}".format(field, text))

unreal.log(PREFIX + "DONE")
