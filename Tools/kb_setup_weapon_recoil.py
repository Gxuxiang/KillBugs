"""
Sets the per-weapon recoil distance.

Run headlessly with the EDITOR CLOSED:
  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

Verify afterwards with Tools/kb_verify_weapon_recoil.py in a SEPARATE process.

NOT Tools/kb_setup_weapons.py: that one CREATES the weapon assets and raises if they already
exist, so it cannot be used to change one field on assets that are already in the project (and
re-running it would throw away the hand-tuned values).

The edit is in place, which this project has been burned by before. Two precautions, both
required rather than belt-and-braces:

  * asset.modify() before the set - without it save_asset(only_if_is_dirty=False) returns True
    and writes nothing.
  * a readback after each set. The readback is load-bearing, not logging: setting several
    properties back to back and saving once silently dropped all but the first.

Neither failure is visible in-process, which is why the verification is a separate process.
"""

import unreal

PREFIX = "[KBWeaponRecoil] "

# weapon asset -> recoil distance in cm (0 turns recoil off for that weapon)
#
# Only the shotgun is set, and on purpose. Recoil is a property of the weapon's weight, so a
# value shared across the loadout would make a shotgun and a rifle feel the same - the field is
# per weapon precisely so that stays tunable one gun at a time.
#
# The rifle and the shockwave are written explicitly at 0 rather than skipped, so that "no
# recoil" is a recorded decision on the asset instead of a default nobody has looked at.
ASSIGNMENTS = {
    "/Game/KillBugs/Weapons/DA_Weapon_Shotgun": 300.0,
    "/Game/KillBugs/Weapons/DA_Weapon_AutoRifle": 0.0,
    "/Game/KillBugs/Weapons/DA_Weapon_Shockwave": 0.0,
}

for path, distance in ASSIGNMENTS.items():
    weapon = unreal.EditorAssetLibrary.load_asset(path)
    if weapon is None:
        unreal.log_warning(PREFIX + "MISSING " + path)
        continue

    weapon.set_editor_property("recoil_distance", distance)

    # Load-bearing readback; see the note above.
    readback = weapon.get_editor_property("recoil_distance")
    unreal.log("{}  {} -> recoil_distance={}".format(
        PREFIX, path.split("/")[-1], readback))

    weapon.modify()

    if not unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        unreal.log_warning(PREFIX + "save_asset returned False for " + path)
        continue

    unreal.log(PREFIX + "wrote " + path)

unreal.log(PREFIX + "DONE - verify in a separate process with Tools/kb_verify_weapon_recoil.py")

try:
    unreal.SystemLibrary.quit_editor()
except Exception as error:  # noqa: BLE001 - shutdown is best-effort
    unreal.log_warning(PREFIX + "quit_editor failed: {}".format(error))
