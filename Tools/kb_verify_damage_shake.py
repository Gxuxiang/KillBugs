"""
Read-only: is a camera shake hooked up to player damage?

The shake is off until someone assigns one - the project ships no camera shake asset - so this
reports "unset" as a normal state, not a failure. What it is here to catch is the case where the
asset was assigned in the editor and did NOT reach DefaultGame.ini, or was assigned to a class
that is not a UCameraShakeBase at all (which ClientStartCameraShake would silently ignore).

Note the CAMEL CASE property names: UKBGameSettings is a UDeveloperSettings and its Python binding
exposes the raw names, unlike the DataAssets elsewhere in this project which use snake_case.
"""

import unreal

PREFIX = "[KBVerifyDamageShake] "

settings_class = unreal.load_class(None, "/Script/KillBugs.KBGameSettings")
if settings_class is None:
    raise RuntimeError("could not load UKBGameSettings")

settings = unreal.get_default_object(settings_class)

shake = settings.get_editor_property("PlayerDamageCameraShake")
scale = settings.get_editor_property("PlayerDamageShakeScale")

if shake is None:
    unreal.log_warning(
        "{}PlayerDamageCameraShake = UNSET -> taking damage will not shake the camera.\n"
        "{}  Set it in Project Settings > Game > KillBugs > 战斗|受伤反馈, or add\n"
        "{}  PlayerDamageCameraShake=/Game/.../YourShake.YourShake_C to DefaultGame.ini".format(
            PREFIX, PREFIX, PREFIX))
else:
    # A class here resolves to the generated class object, whose name ends in _C for a Blueprint
    # and is the plain name for a native one. Reporting the parent chain is what distinguishes
    # "a real UCameraShakeBase" from "some class that happens to be assigned".
    unreal.log("{}PlayerDamageCameraShake = {}".format(PREFIX, shake.get_path_name()))
    unreal.log("{}  parent = {}".format(
        PREFIX, shake.get_super_class().get_name() if shake.get_super_class() else "none"))

unreal.log("{}PlayerDamageShakeScale = {}".format(PREFIX, scale))

if shake is not None and scale <= 0.0:
    unreal.log_warning(
        "{}  scale is {} -> the shake is configured but multiplied by zero, so it will never "
        "be visible. 0 means off.".format(PREFIX, scale))

unreal.log(PREFIX + "DONE")
