"""
Read-only verification: did the weapon fire/impact sounds actually reach disk?

kb_setup_weapon_audio.py's own docstring says to verify in a SEPARATE process, because "an
in-place edit that appears to succeed but never reaches disk is this project's most-repeated
failure" - and the file it names did not exist. This is it.

Run with the editor CLOSED:
  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" -unattended -nosplash
"""

import unreal

PREFIX = "[KBVerifyWeaponAudio] "

PACKAGE_PATH = "/Game/KillBugs/Weapons"
WEAPONS = ("DA_Weapon_AutoRifle", "DA_Weapon_Shotgun", "DA_Weapon_Shockwave")


def describe(asset_path):
    weapon = unreal.load_asset(asset_path)
    if weapon is None:
        unreal.log_warning(PREFIX + "MISSING " + asset_path)
        return

    fire = weapon.get_editor_property("fire_sound")
    impact = weapon.get_editor_property("impact_sound")
    pitch = weapon.get_editor_property("fire_sound_pitch")

    fire_name = fire.get_name() if fire else "<none>"
    impact_name = impact.get_name() if impact else "<none>"

    unreal.log("{}  {}  fire={} pitch={} impact={}".format(
        PREFIX, asset_path.split("/")[-1], fire_name, pitch, impact_name))

    if fire is None:
        unreal.log_warning(PREFIX + "  ^ NO FIRE SOUND - the weapon will be silent")


for name in WEAPONS:
    describe("{}/{}".format(PACKAGE_PATH, name))

unreal.log(PREFIX + "DONE")
