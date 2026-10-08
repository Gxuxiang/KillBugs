"""
List every actor in Lvl_Arena with its class, location, scale and collision profile.

Read-only, and it does NOT save. Written because the flow field's obstacle bake found the arena's
walls nowhere near where the arena script puts them, and "the query is wrong" and "the geometry is
not there" look identical from the bake's own log line.

    UnrealEditor-Cmd.exe <project>.uproject -ExecutePythonScript="<this file>" \
        -unattended -nosplash -nullrhi
"""

import unreal

MAP_PATH = "/Game/KillBugs/Maps/Lvl_Arena"


def describe(actor):
    location = actor.get_actor_location()
    scale = actor.get_actor_scale3d()

    line = "{:<16} {:<22} loc=({:>8.0f},{:>8.0f},{:>8.0f}) scale=({:.2f},{:.2f},{:.2f})".format(
        actor.get_class().get_name(),
        actor.get_actor_label()[:22],
        location.x, location.y, location.z,
        scale.x, scale.y, scale.z,
    )

    # Static meshes carry the collision the bake queries, so name the profile and the bounds.
    component = None
    if isinstance(actor, unreal.StaticMeshActor):
        component = actor.static_mesh_component

    if component:
        profile = component.get_collision_profile_name()
        mesh = component.get_editor_property("static_mesh")
        # get_local_bounds() hands back a plain tuple of (box_extent, box_origin), not a struct.
        bounds = component.get_local_bounds()
        extent = bounds[0] if isinstance(bounds, tuple) else bounds.box_extent
        line += "  collision={} mesh={} extent=({:.0f},{:.0f},{:.0f})".format(
            profile,
            mesh.get_name() if mesh else "None",
            extent.x, extent.y, extent.z,
        )

    return line


def describe_mesh_collision(mesh):
    """
    The question the bake's log line cannot answer: does this mesh have SIMPLE collision?

    A mesh with only complex collision is invisible to a query that does not ask for complex
    collision - the component reports a BlockAll profile and still blocks nothing.
    """
    bounds = mesh.get_bounds()
    parts = ["{}: bounds origin=({:.0f},{:.0f},{:.0f}) extent=({:.0f},{:.0f},{:.0f})".format(
        mesh.get_name(),
        bounds.origin.x, bounds.origin.y, bounds.origin.z,
        bounds.box_extent.x, bounds.box_extent.y, bounds.box_extent.z)]

    body = mesh.get_editor_property("body_setup")
    if not body:
        parts.append("NO body setup")
        return " | ".join(parts)

    parts.append("collision_trace_flag={}".format(
        body.get_editor_property("collision_trace_flag")))

    for field in ("agg_geom",):
        try:
            geom = body.get_editor_property(field)
        except Exception as error:  # noqa: BLE001 - the point is to report, not to handle
            parts.append("{}=unreadable ({})".format(field, error))
            continue

        for element in ("box_elems", "convex_elems", "sphere_elems", "sphyl_elems"):
            try:
                parts.append("{}={}".format(element, len(geom.get_editor_property(element))))
            except Exception as error:  # noqa: BLE001
                parts.append("{}=unreadable".format(element))

    return " | ".join(parts)


def main():
    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    if not subsystem.load_level(MAP_PATH):
        unreal.log_error("could not load {}".format(MAP_PATH))
        return

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
    unreal.log("kb_probe_arena_actors: {} actor(s) in {}".format(len(actors), MAP_PATH))
    for actor in actors:
        unreal.log("  " + describe(actor))

    seen = set()
    for actor in actors:
        if not isinstance(actor, unreal.StaticMeshActor):
            continue
        mesh = actor.static_mesh_component.get_editor_property("static_mesh")
        if not mesh or mesh.get_name() in seen:
            continue
        seen.add(mesh.get_name())
        unreal.log("  mesh collision - " + describe_mesh_collision(mesh))


main()
