"""
Read-only probe: how does this engine build let Python reach the WorldSettings actor?

Run headlessly with the editor closed. Writes nothing.
"""

import unreal

PREFIX = "[KBLobbyProbe] "


def log(message):
    unreal.log(PREFIX + str(message))


log("unreal.WorldSettings exists: {}".format(hasattr(unreal, "WorldSettings")))
log("unreal.UnrealEditorSubsystem exists: {}".format(hasattr(unreal, "UnrealEditorSubsystem")))

actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors = actor_subsystem.get_all_level_actors() if actor_subsystem else []
log("get_all_level_actors returned {} actor(s)".format(len(actors)))
for actor in actors[:15]:
    log("  actor: {} -> {}".format(actor.get_actor_label(), type(actor)))

# Route A: the editor world's own accessor.
if hasattr(unreal, "UnrealEditorSubsystem"):
    editor_subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    log("UnrealEditorSubsystem instance: {}".format(editor_subsystem))
    if editor_subsystem:
        log("  has get_editor_world: {}".format(hasattr(editor_subsystem, "get_editor_world")))
        world = editor_subsystem.get_editor_world()
        log("  editor world: {}".format(world))
        if world:
            log("  has get_world_settings: {}".format(hasattr(world, "get_world_settings")))
            settings = world.get_world_settings()
            log("  world settings: {}".format(settings))
            if settings:
                log("  type: {}".format(type(settings)))
                log("  has default_game_mode: {}".format(
                    hasattr(settings, "get_editor_property")))
                log("  default_game_mode = {}".format(
                    settings.get_editor_property("default_game_mode")))

log("PROBE DONE")
