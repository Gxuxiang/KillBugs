"""
Diagnostic: at which step does the cleared value come back?

Earlier runs showed the set working in memory but the value present again after the save, so
this prints the readback after EVERY step to pin down which call reverts it.
"""

import unreal

PREFIX = "[KBDiag] "

PATH = "/Game/KillBugs/Weapons/DA_Weapon_AutoRifle"


def read(asset):
    value = asset.get_editor_property("fire_sound")
    return value.get_path_name() if value else "unset"


weapon = unreal.EditorAssetLibrary.load_asset(PATH)
if weapon is None:
    raise RuntimeError("missing " + PATH)

unreal.log("{}1. loaded                   fire={}".format(PREFIX, read(weapon)))

weapon.set_editor_property("fire_sound", None)
unreal.log("{}2. after set None           fire={}".format(PREFIX, read(weapon)))

weapon.modify()
unreal.log("{}3. after modify()           fire={}".format(PREFIX, read(weapon)))

ok = unreal.EditorAssetLibrary.save_asset(PATH, only_if_is_dirty=False)
unreal.log("{}4. save returned {}      fire={}".format(PREFIX, ok, read(weapon)))

# Re-load from disk to see what was actually written, without trusting the in-memory copy.
unreal.EditorAssetLibrary.unload_asset(PATH)
fresh = unreal.EditorAssetLibrary.load_asset(PATH)
unreal.log("{}5. reloaded from disk       fire={}".format(PREFIX, read(fresh)))

unreal.log(PREFIX + "DONE")
