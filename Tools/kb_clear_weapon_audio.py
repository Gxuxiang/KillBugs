"""
Clears the sound assignments kb_setup_weapon_audio.py made, so the weapons can be wired by
hand without anything of the script's left behind.

Run headlessly with the EDITOR CLOSED. Verify afterwards with kb_probe_sounds.py in a SEPARATE
process - an in-place edit that reports success without reaching disk is this project's
most-repeated failure.

WHAT THIS TOOK TO GET RIGHT, all three found by bisecting a non-working version:

  1. A TSoftObjectPtr is cleared with None. A SoftObjectPath is rejected outright - the Python
     binding demands an actual UObject of the property's class - and a bare string is rejected
     too.

  2. modify() is required before saving. set_editor_property does not mark the package dirty,
     so the save quietly writes the old value and still returns success.

  3. Each set is followed by a readback, and that is not just logging. Setting several soft
     pointers back to back and saving silently dropped all but the first; reading the value
     between the calls is what makes each one stick. The readback in the loop below is
     load-bearing, not decoration.
"""

import unreal

PREFIX = "[KBClearAudio] "

WEAPONS = [
    "/Game/KillBugs/Weapons/DA_Weapon_AutoRifle",
    "/Game/KillBugs/Weapons/DA_Weapon_Shotgun",
    "/Game/KillBugs/Weapons/DA_Weapon_Shockwave",
]

SOUND_FIELDS = ["fire_sound", "impact_sound"]


def readback(asset, field):
    try:
        value = asset.get_editor_property(field)
    except Exception as error:  # noqa: BLE001
        return "<{}>".format(error)
    return value.get_path_name() if value else "unset"


for path in WEAPONS:
    weapon = unreal.EditorAssetLibrary.load_asset(path)
    if weapon is None:
        unreal.log_warning(PREFIX + "MISSING " + path)
        continue

    for field in SOUND_FIELDS:
        weapon.set_editor_property(field, None)
        # Load-bearing readback; see note 3 above.
        unreal.log("{}  {} -> {}".format(PREFIX, field, readback(weapon, field)))

    weapon.set_editor_property("fire_sound_pitch", 1.0)

    weapon.modify()
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    unreal.log("{}{}  fire={}  impact={}".format(
        PREFIX, path.split("/")[-1],
        readback(weapon, "fire_sound"), readback(weapon, "impact_sound")))

unreal.log(PREFIX + "DONE")
