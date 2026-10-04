"""
Points the bug archetypes' DeathEffect at the hand-authored explosion.

Run headlessly, with the editor CLOSED:
  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

Why this is a separate script from kb_setup_enemies.py: that one CREATES the archetypes and
refuses to run if they already exist, so it cannot be used to set one field on assets that are
already in the project. The archetypes are also hand-tuned in the editor between runs, so
regenerating them would throw that away. This script only ever touches DeathEffect.

The edit is in place, which this project has been burned by before (see the note in
kb_setup_enemies.py about set_editor_property not marking a package dirty). Two precautions,
both required rather than belt-and-braces:

  * asset.modify() before the set - without it save_asset(only_if_is_dirty=False) returns True
    and writes nothing.
  * one property per asset, no back-to-back sets - a run of several sets in one session has
    silently dropped all but the first.

Neither failure is visible in-process, so the write is checked by Tools/kb_verify_death_vfx.py
in a SEPARATE process. Do not trust this script's own readback.

What the runtime does with the field, for reference - UKBGoreComponent::OnBugDied spawns it at
the bug's last drawn position, on every machine, with no replication. It is budgeted to
MaxSplatsPerFrame per frame, and a bug whose archetype has no DeathEffect still leaves its slime
decal. So a bug here degrades the effect, not the game.
"""

import unreal

PREFIX = "[KBDeathVFX] "

ARCHETYPE_FOLDER = "/Game/KillBugs/Enemies"
ARCHETYPE_NAMES = ("DA_Bug_Grunt", "DA_Bug_Runner", "DA_Bug_Brute")

# Authored by hand in the Niagara editor. One system for every archetype: the colour and the
# size come from UKBGoreComponent writing User.SplatColor and User.SplatScale at spawn, so
# three archetypes do not need three systems.
DEATH_EFFECT = "/Game/KillBugs/VFX/FX_BugBlast"


def log(message):
    unreal.log(PREFIX + str(message))


def warn(message):
    unreal.log_warning(PREFIX + str(message))


effect = unreal.EditorAssetLibrary.load_asset(DEATH_EFFECT)
if effect is None:
    raise RuntimeError("could not load {}".format(DEATH_EFFECT))

log("effect: {} ({})".format(effect.get_path_name(), effect.get_class().get_name()))

for name in ARCHETYPE_NAMES:
    path = "{}/{}".format(ARCHETYPE_FOLDER, name)

    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        warn("MISSING {}".format(path))
        continue

    asset.modify()
    asset.set_editor_property("death_effect", effect)

    if not unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        warn("save_asset returned False for {}".format(path))
        continue

    log("wrote {} -> {}".format(name, DEATH_EFFECT))

log("DONE - verify in a separate process with Tools/kb_verify_death_vfx.py")

try:
    unreal.SystemLibrary.quit_editor()
except Exception as error:  # noqa: BLE001 - shutdown is best-effort
    warn("quit_editor failed: {}".format(error))
