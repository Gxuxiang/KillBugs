"""
Assigns the bug movement and death sounds to the three archetype assets.

Run headlessly with the EDITOR CLOSED:
  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

Verify afterwards with Tools/kb_verify_enemy_audio.py in a SEPARATE process.

NOTHING IS SET until SOUND PATHS below is filled in. You can equally drag the assets onto the
archetype in the editor - this script exists so the wiring is reproducible and reviewable rather
than a series of clicks nobody can see afterwards.

The numeric fields (volume, radius, pitch spread) are deliberately NOT written here: they are
new UPROPERTYs with working defaults, so every archetype already has them at their class-default
values, and the asset only serialises a value once something changes it. Tuning them is a job
for the editor, per archetype, by ear.
"""

import unreal

PREFIX = "[KBEnemyAudio] "

ARCHETYPES = {
    "DA_Bug_Grunt": "/Game/KillBugs/Enemies/DA_Bug_Grunt",
    "DA_Bug_Runner": "/Game/KillBugs/Enemies/DA_Bug_Runner",
    "DA_Bug_Brute": "/Game/KillBugs/Enemies/DA_Bug_Brute",
}

# (move_sound, death_sound) per archetype, as full object paths, or None to leave that field
# alone. Both may be None - an archetype with no sounds is silent and does not break anything.
#
# A movement sound should be LOOPING; a one-shot will be restarted whenever the swarm audio
# component picks the voice back up, which reads as a stutter rather than a bed.
#
# Example, using the SoundMorph pack that ships in Content/ - replace with the real assets:
#     "/Game/SoundMorphMetaSounds/SmallBot/Talker",
#     "/Game/SoundMorphMetaSounds/Retro/ExplosionRandom",
SOUND_PATHS = {
    "DA_Bug_Grunt": (None, None),
    "DA_Bug_Runner": (None, None),
    "DA_Bug_Brute": (None, None),
}

# The two soft pointers the archetype exposes. Named as UE Python spells the C++ fields.
SOUND_FIELDS = ("move_sound", "death_sound")


def readback(asset, field):
    """Resolving readback - see note 3 below. Not decoration."""
    try:
        value = asset.get_editor_property(field)
    except Exception as error:  # noqa: BLE001 - field may not exist yet
        return "<{}>".format(error)
    return value.get_path_name() if value else "unset"


assigned = 0

for name, asset_path in ARCHETYPES.items():
    move_path, death_path = SOUND_PATHS[name]
    if move_path is None and death_path is None:
        unreal.log("{}skip {} - no paths given".format(PREFIX, name))
        continue

    asset = unreal.EditorAssetLibrary.load_asset(asset_path)
    if asset is None:
        unreal.log_warning("{}MISSING {}".format(PREFIX, asset_path))
        continue

    for field, path in zip(SOUND_FIELDS, (move_path, death_path)):
        if path is None:
            continue

        sound = unreal.EditorAssetLibrary.load_asset(path)
        if sound is None:
            unreal.log_warning("{}could not load {} for {}".format(PREFIX, path, name))
            continue

        asset.set_editor_property(field, sound)

        # LOAD-BEARING, not logging. Setting several soft pointers back to back and then saving
        # silently drops all but the first; reading the value between the sets is what makes
        # each one stick. This is the failure that cost four attempts on the weapon sounds.
        unreal.log("{}  {}.{} -> {}".format(PREFIX, name, field, readback(asset, field)))

    # modify() is required before saving: set_editor_property does not mark the package dirty, so
    # without it save_asset(only_if_is_dirty=False) returns True and writes the OLD value.
    asset.modify()

    # save_asset, NOT save_loaded_asset, and only_if_is_dirty=False for the same reason.
    if not unreal.EditorAssetLibrary.save_asset(asset_path, only_if_is_dirty=False):
        unreal.log_warning("{}save_asset returned False for {}".format(PREFIX, asset_path))
        continue

    assigned += 1
    unreal.log("{}wrote {}  move={}  death={}".format(
        PREFIX, name, readback(asset, "move_sound"), readback(asset, "death_sound")))

unreal.log("{}{} archetype(s) updated".format(PREFIX, assigned))
unreal.log(PREFIX + "DONE - verify in a separate process with Tools/kb_verify_enemy_audio.py")

try:
    unreal.SystemLibrary.quit_editor()
except Exception as error:  # noqa: BLE001 - shutdown is best-effort
    unreal.log_warning(PREFIX + "quit_editor failed: {}".format(error))
