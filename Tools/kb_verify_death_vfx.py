"""
Read-only check of what DeathEffect the archetype assets contain ON DISK.

Run in its own editor process, separate from kb_setup_death_vfx.py. The setter's own readback
returns its in-memory object and cannot tell you whether the save reached the file - the exact
trap that hid a lost edit for several rounds on this project.

A line reading `NONE` means UKBGoreComponent will skip the burst for that archetype and leave
only the slime decal. A line reading a path that does not end in FX_BugBlast means something
overwrote the field after the last setup run.
"""

import unreal

PREFIX = "[KBVerifyDeathVFX] "
PATH = "/Game/KillBugs/Enemies/{}"
EXPECTED = "/Game/KillBugs/VFX/FX_BugBlast"

missing = []

for name in ("DA_Bug_Grunt", "DA_Bug_Runner", "DA_Bug_Brute"):
    asset = unreal.EditorAssetLibrary.load_asset(PATH.format(name))
    if asset is None:
        unreal.log_warning(PREFIX + "MISSING " + name)
        missing.append(name)
        continue

    effect = asset.get_editor_property("death_effect")

    # get_path_name on a soft pointer resolves the asset; the package path is what is stored,
    # so compare on the object path minus the trailing ".FX_BugBlast".
    if effect is None:
        unreal.log_warning("{}{} death_effect=NONE".format(PREFIX, name))
        missing.append(name)
        continue

    object_path = effect.get_path_name()
    resolved = object_path.split(".")[0]

    line = "{}{} death_effect={}".format(PREFIX, name, object_path)
    if resolved != EXPECTED:
        unreal.log_warning(line + "  <-- not {}".format(EXPECTED))
        missing.append(name)
    else:
        unreal.log(line)

unreal.log("{}{}".format(PREFIX, "OK" if not missing else "INCOMPLETE: " + ", ".join(missing)))
unreal.log(PREFIX + "DONE")
