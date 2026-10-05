"""
Read-only probe: which call actually clears a TSoftObjectPtr property?

kb_clear_weapon_audio.py documents that `set_editor_property(field, None)` clears one. It does not
any more - running that script today reports `fire_sound -> <the old path>` straight after the
set, and a separate process confirms the old value is still on disk. The notes were true when they
were written and the engine binding has moved.

This tries the plausible spellings and prints what each one leaves behind, WITHOUT SAVING, so
running it cannot damage anything. Whichever one reads back as unset is the one to use.

Run with the editor CLOSED:
  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" -unattended -nosplash
"""

import unreal

PREFIX = "[KBClearProbe] "

ASSET = "/Game/KillBugs/Weapons/DA_Weapon_Shockwave"
FIELD = "impact_sound"


def describe(weapon):
    value = weapon.get_editor_property(FIELD)
    return value.get_path_name() if value else "UNSET"


def attempt(label, value_factory):
    weapon = unreal.EditorAssetLibrary.load_asset(ASSET)
    if weapon is None:
        unreal.log_warning(PREFIX + "missing " + ASSET)
        return

    before = describe(weapon)
    try:
        weapon.set_editor_property(FIELD, value_factory())
    except Exception as error:  # noqa: BLE001 - a rejected spelling is a result, not a failure
        unreal.log("{}  {:<28} -> raised: {}".format(PREFIX, label, error))
        return

    unreal.log("{}  {:<28} -> {}".format(PREFIX, label, describe(weapon)))
    if before == describe(weapon):
        unreal.log("{}      (unchanged from {})".format(PREFIX, before.split(".")[-1]))


attempt("None", lambda: None)
attempt("empty string", lambda: "")
attempt("SoftObjectPath()", lambda: unreal.SoftObjectPath())
attempt("SoftObjectPath('')", lambda: unreal.SoftObjectPath(""))

# A TSoftObjectPtr set to a real-but-empty instance is a DIFFERENT thing from a null one: it
# would serialise a reference to a transient object. Tried only to see how it reads back.
attempt("new SoundBase", lambda: unreal.new_object(unreal.SoundBase))

unreal.log(PREFIX + "DONE (nothing was saved)")
