"""
Read-only: what property names does UKBGameSettings actually expose to Python right now?

Written to settle a specific puzzle: the UHT-generated KBGameSettings.gen.cpp contains
ProjectileEffect, but get_editor_property("projectile_effect") raised "Failed to find property".
Either the name the binding wants differs, or something older is loaded.
"""

import unreal

PREFIX = "[KBSettingsProps] "

settings_class = unreal.load_class(None, "/Script/KillBugs.KBGameSettings")
unreal.log("{}class loaded: {}".format(PREFIX, settings_class))

if settings_class is None:
    unreal.log_warning(PREFIX + "could not load the class at all")
else:
    settings = unreal.get_default_object(settings_class)
    unreal.log(PREFIX + "default object: {}".format(settings))

    # The definitive list: every property the binding knows about, filtered to the ones we care
    # about so the output stays readable.
    names = [name for name in dir(settings) if not name.startswith("_")]
    interesting = [n for n in names if "projectile" in n.lower() or "slime" in n.lower()]
    unreal.log(PREFIX + "projectile/slime-ish attributes: {}".format(sorted(interesting)))

    for candidate in ("projectile_effect", "ProjectileEffect", "projectile_tint",
                      "ProjectileTint"):
        try:
            value = settings.get_editor_property(candidate)
            unreal.log("{}  {} -> {}".format(PREFIX, candidate, value))
        except Exception as error:  # noqa: BLE001 - the whole point is to report the failure
            unreal.log_warning("{}  {} -> {}".format(PREFIX, candidate, error))

unreal.log(PREFIX + "DONE")
