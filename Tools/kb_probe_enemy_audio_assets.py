"""
Read-only: what are the bug sound assets, and are they usable for their slot?

The one thing this exists to catch is the LOOPING flag on a movement sound. An imported
SoundWave defaults to non-looping, and UKBSwarmAudioComponent responds to a voice that has
stopped by playing it again - so a non-looping movement sound is not silent, it retriggers, and
the symptom is a repeated chirp rather than a continuous bed. That reads as a bug in the swarm
audio code and is in fact a checkbox on the asset.

Death sounds do NOT need to loop; they are one-shots by definition.

Run headlessly. Read-only, so it is safe against a live editor.
"""

import unreal

PREFIX = "[KBEnemyAudioAssets] "

# (label, path, must_loop)
CANDIDATES = [
    ("bugMove", "/Game/KillBugs/Sound/bugMove", True),
    ("MS_Fire", "/Game/KillBugs/Sound/MS_Fire", False),
]


def describe(path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        return None, "MISSING"

    class_name = asset.get_class().get_name()

    if class_name == "SoundWave":
        looping = asset.get_editor_property("looping")
        duration = asset.get_editor_property("duration")
        return asset, "{}  looping={}  duration={:.2f}s".format(class_name, looping, duration)

    # MetaSounds and cues expose no single looping flag; their sustain is part of the graph, so
    # there is nothing here to check and nothing to warn about.
    return asset, "{}  (looping is a property of the graph, not checkable here)".format(class_name)


problems = []

for label, path, must_loop in CANDIDATES:
    asset, text = describe(path)
    line = "{}{:<10} {}".format(PREFIX, label, text)

    if asset is None:
        unreal.log_warning(line)
        if must_loop:
            problems.append(label)
        continue

    if must_loop and asset.get_class().get_name() == "SoundWave":
        if not asset.get_editor_property("looping"):
            unreal.log_warning(line + "   <-- needs Looping=true; it will retrigger")
            problems.append(label)
            continue

    unreal.log(line)

unreal.log("{}{}".format(
    PREFIX,
    "OK" if not problems
    else "PROBLEMS: " + ", ".join(problems) + " (see the note in this file's docstring)"))
unreal.log(PREFIX + "DONE")
