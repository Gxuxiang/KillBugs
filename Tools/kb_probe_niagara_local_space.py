"""
Read-only: is the projectile effect's emitter in LOCAL space?

A Niagara system attached to a moving component only travels with it if its particles are
simulated in LOCAL space. In world space the particles are born at the location the component
happened to be at that instant and then stay there - so a bullet effect drawn this way appears as
a puff at the muzzle and nothing else, while the component itself is verifiably moving.

Which accessor carries the flag has moved between UE versions (it lives on the versioned emitter
data in UE5, not on UNiagaraEmitter directly), so this tries each in turn and reports which one
answered rather than throwing on the first miss.
"""

import unreal

PREFIX = "[KBNiagaraSpace] "

PATH = "/Game/KillBugs/VFX/FX_bullet"


def report(label, value):
    unreal.log("{}{:<34} {}".format(PREFIX, label, value))


system = unreal.EditorAssetLibrary.load_asset(PATH)
if system is None:
    raise RuntimeError("could not load {}".format(PATH))

report("system", system.get_path_name())

# DEAD END, recorded so nobody repeats it: on UE 5.8 neither the emitter list nor the flag is
# reachable from Python. UNiagaraSystem has no "emitter_handles" property in the binding, and the
# flag itself lives on the versioned emitter data rather than on UNiagaraEmitter, which is not
# exposed either. (Same shape as the failure documented in kb_probe_death_vfx_params.py.)
#
# So the space of an emitter cannot be read from a script here. It has to be either looked at in
# the editor (Emitter Properties -> Local Space) or inferred at runtime, which is what settled it:
# log the projectile's logical location next to the component's, and they match frame for frame
# while the effect visibly stays put. A component that tracks correctly plus an effect that does
# not move means world-space particles, full stop.
try:
    handles = system.get_editor_property("emitter_handles")
except Exception as error:  # noqa: BLE001
    report("emitter_handles", "<unavailable: {}>".format(error))
    unreal.log_warning(PREFIX + "cannot read the emitters from Python - see the note in this "
                                "file's docstring for the runtime check that works instead")
    unreal.log(PREFIX + "DONE")
    raise SystemExit(0)

report("emitter count", len(handles))

for index, handle in enumerate(handles):
    report("handle[{}] name".format(index), handle.get_editor_property("name"))

    # Candidate accessors, newest first. Each is tried in isolation so one missing property does
    # not stop the rest.
    for accessor in ("versioned_emitter", "emitter"):
        try:
            emitter = handle.get_editor_property(accessor)
        except Exception as error:  # noqa: BLE001
            report("  {} ->".format(accessor), "<{}>".format(error))
            continue

        if emitter is None:
            report("  {} ->".format(accessor), "None")
            continue

        report("  {} ->".format(accessor), emitter)

        for flag in ("b_local_space", "bLocalSpace", "local_space"):
            try:
                value = emitter.get_editor_property(flag)
                report("    {} =".format(flag), "{}   <-- THIS IS THE FLAG".format(value))
            except Exception as error:  # noqa: BLE001
                report("    {} ->".format(flag), "<unavailable>")

unreal.log(PREFIX + "DONE")
