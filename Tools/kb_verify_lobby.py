"""
Reads Lvl_Lobby back off disk and checks it is what kb_setup_lobby.py claims it wrote.

Run in a SEPARATE process from the generator, with the editor closed:

  UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
      -unattended -nosplash -nullrhi

Separate process is the point. A script that writes an asset and then reads it back in the
same process reads its own in-memory object, which is this project's most-repeated failure
mode: an edit that reports success without ever reaching disk. Only a fresh process proves it.
"""

import unreal

MAP_PATH = "/Game/KillBugs/Maps/Lvl_Lobby"
EXPECTED_GAME_MODE = "/Script/KillBugs.KBLobbyGameMode"
EXPECTED_LABELS = (
    "KB_LobbyFloor",
    "KB_LobbySkyAtmosphere",
    "KB_LobbySun",
    "KB_LobbySkyLight",
    "KB_LobbyPlayerStart",
)

PREFIX = "[KBLobbyVerify] "
problems = []


def log(message):
    unreal.log(PREFIX + str(message))


def check(condition, description):
    log("{} {}".format("OK  " if condition else "FAIL", description))
    if not condition:
        problems.append(description)


if not unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
    raise RuntimeError("{} does not exist on disk - the generator did not save".format(MAP_PATH))

log("asset exists: {}".format(MAP_PATH))

level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
if not level_subsystem.load_level(MAP_PATH):
    raise RuntimeError("load_level failed for {}".format(MAP_PATH))

# The GameMode override is the one thing that has to be right: without it this map silently
# runs the arena's mode, which spawns the swarm on load and starts a wave before anyone is
# ready - a failure that looks like a gameplay bug rather than a missing level setting.
editor_subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
editor_world = editor_subsystem.get_editor_world() if editor_subsystem else None
world_settings = editor_world.get_world_settings() if editor_world else None

check(world_settings is not None, "WorldSettings reachable")

if world_settings is not None:
    resolved = world_settings.get_editor_property("default_game_mode")
    path = resolved.get_path_name() if resolved else "None"
    log("default_game_mode = {}".format(path))
    check(path == EXPECTED_GAME_MODE,
          "GameMode override is {} (got {})".format(EXPECTED_GAME_MODE, path))

actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
labels = {actor.get_actor_label() for actor in actor_subsystem.get_all_level_actors()}
log("actor labels on disk: {}".format(sorted(labels)))

for expected in EXPECTED_LABELS:
    check(expected in labels, "actor present: {}".format(expected))

if problems:
    raise RuntimeError("{} check(s) failed: {}".format(len(problems), "; ".join(problems)))

log("ALL CHECKS PASSED")
