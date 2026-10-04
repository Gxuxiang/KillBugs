"""
Read-only check of what the archetype audio fields contain ON DISK.

Run in its own editor process, separate from kb_setup_enemy_audio.py. A script that modifies an
asset and then re-loads it in the same process reads back its own in-memory object, which cannot
tell you whether the save ever reached the file.

Reports both halves:
  * the two sound soft pointers, which are what a setup script or a hand edit fills in;
  * the numeric fields, which have C++ defaults and so read back as the class default until
    something changes them. An `unset` sound with sensible numbers beside it is a perfectly
    healthy archetype - it just has no audio yet.

A sound that resolves but is a one-shot rather than a looping source will show up in game as a
stuttering movement bed rather than as a load error; nothing here can detect that, because it is
a property of the asset's own looping flag.
"""

import unreal

PREFIX = "[KBVerifyEnemyAudio] "

ARCHETYPES = {
    "DA_Bug_Grunt": "/Game/KillBugs/Enemies/DA_Bug_Grunt",
    "DA_Bug_Runner": "/Game/KillBugs/Enemies/DA_Bug_Runner",
    "DA_Bug_Brute": "/Game/KillBugs/Enemies/DA_Bug_Brute",
}

FLOAT_FIELDS = (
    "move_sound_volume",
    "move_sound_radius",
    "move_sound_pitch_min",
    "move_sound_pitch_max",
    "death_sound_volume",
    "death_sound_pitch_min",
    "death_sound_pitch_max",
)

unset = []

for name, path in ARCHETYPES.items():
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        unreal.log_warning(PREFIX + "MISSING " + path)
        unset.append(name)
        continue

    move = asset.get_editor_property("move_sound")
    death = asset.get_editor_property("death_sound")

    unreal.log("{}  {}  move={}  death={}".format(
        PREFIX, name,
        move.get_path_name() if move else "unset",
        death.get_path_name() if death else "unset"))

    numbers = []
    for field in FLOAT_FIELDS:
        try:
            numbers.append("{}={:.2f}".format(field, asset.get_editor_property(field)))
        except Exception as error:  # noqa: BLE001 - field may not exist yet
            numbers.append("{}<{}>".format(field, error))
    unreal.log("{}      {}".format(PREFIX, "  ".join(numbers)))

    if not move and not death:
        unset.append(name)

unreal.log("{}{}".format(
    PREFIX,
    "OK - every archetype has both sounds" if not unset
    else "SILENT (fields still unset): " + ", ".join(unset)))
unreal.log(PREFIX + "DONE")
