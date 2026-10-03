"""
Read-only: can the code actually see the slime decal material?

Reading the ini file is not evidence - the value has to survive parsing into the settings object
the game uses. An earlier attempt put the line in the wrong ini section, so the file plainly
contained `SlimeDecalMaterial=...` while `UKBGoreComponent` logged that it was unset and drew the
engine's default decal, which is a glowing square.

This asks the same object the component asks: the UKBGameSettings CDO.
"""

import unreal

PREFIX = "[KBSettings] "


def log(message):
    unreal.log(PREFIX + str(message))


# Not unreal.KBGameSettings: the settings class is not exposed under a Python name, so it is
# resolved by path instead. load_class needs the full script path.
settings_class = unreal.load_class(None, "/Script/KillBugs.KBGameSettings")
if settings_class is None:
    raise RuntimeError("could not resolve /Script/KillBugs.KBGameSettings")

settings = unreal.get_default_object(settings_class)
if settings is None:
    raise RuntimeError("could not get the UKBGameSettings CDO")

value = settings.get_editor_property("slime_decal_material")
log("slime_decal_material = {}".format(value))

for name in ("max_players",):
    try:
        log("{} = {}".format(name, settings.get_editor_property(name)))
    except Exception as error:  # noqa: BLE001
        log("{} : {}".format(name, error))

# LoadSynchronous is what the gore component does; if this returns something, so will it.
if value is None:
    log("FAIL: the setting is empty - the decal will draw with the engine default")
    raise RuntimeError("SlimeDecalMaterial is not set")

loaded = value.load_synchronous() if hasattr(value, "load_synchronous") else value
if loaded is None:
    log("FAIL: the path is set but nothing loads from it - check the path form")
    raise RuntimeError("SlimeDecalMaterial does not resolve")

log("OK: resolves to a {}".format(loaded.get_class().get_name()))
