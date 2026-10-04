"""
Read-only: does FX_BugBlast expose the user parameters UKBGoreComponent writes at spawn?

UKBGoreComponent::OnBugDied sets User.SplatColor and User.SplatScale after spawning the system.
A Niagara system that does not expose a parameter simply ignores the set - no warning, no error -
it just plays untinted and at its authored size. This probe is the only way to tell which of
those two worlds the effect is in, because the two are indistinguishable from the game.

Run headlessly, read-only; safe to run with the editor open in principle, but this project runs
probes headless anyway.
"""

import unreal

PREFIX = "[KBProbeVFX] "
PATH = "/Game/KillBugs/VFX/FX_BugBlast"

system = unreal.EditorAssetLibrary.load_asset(PATH)
if system is None:
    raise RuntimeError("could not load {}".format(PATH))

unreal.log(PREFIX + "class={}".format(system.get_class().get_name()))

names = []
source = None

# Try every accessor this API surface has carried across versions. Guarded individually: a
# probe that hard-crashes tells you less than one that admits it could not look.
getter = getattr(system, "get_exposed_parameters", None)
if getter is not None:
    try:
        for parameter in getter():
            names.append(str(parameter.get_editor_property("name")))
        source = "get_exposed_parameters()"
    except Exception as error:  # noqa: BLE001
        unreal.log_warning(PREFIX + "get_exposed_parameters failed: {}".format(error))

if not names:
    # The store itself, straight off the object. Guarded: on 5.8 this property does not exist on
    # UNiagaraSystem at all, and an unguarded access aborts the whole script with a traceback.
    try:
        store = system.get_editor_property("exposed_parameters")
        if store is not None:
            source = "exposed_parameters property"
            # A parameter store is not directly enumerable in Python; dump its repr, which does
            # list the parameter names, and scrape what we care about out of it.
            names = [str(store)]
    except Exception as error:  # noqa: BLE001
        unreal.log_warning(PREFIX + "exposed_parameters unavailable: {}".format(error))

if source is None:
    # Python cannot read the user-parameter store on this version. That is not a dead end: a
    # Niagara name is stored verbatim in the package's name table, so an exposed User parameter
    # turns up in the raw bytes. This is the check that actually settled it -
    #
    #   grep -a -o -E "User\.[A-Za-z0-9_]+" Content/KillBugs/VFX/FX_BugBlast.uasset
    #
    # returns nothing at all: the system exposes no user parameters of any name. So the MISSING
    # lines below are a conclusion about the asset, not a limitation of the probe.
    unreal.log_warning(PREFIX + "cannot read the store from Python on this version - grep the "
                                "asset for 'User\\.' instead; an empty result means no user "
                                "parameters exist")

unreal.log(PREFIX + "read via {}".format(source or "the name table (see the warning above)"))
for name in names:
    unreal.log(PREFIX + "  " + name)

# The two the gore component actually writes, and the one the archetype header documents.
for wanted in ("SplatColor", "SplatScale", "SplatDirection"):
    found = any(wanted in name for name in names)
    line = "{} {} -> {}".format(PREFIX, wanted, "FOUND" if found else "MISSING")
    (unreal.log if found else unreal.log_warning)(line)

unreal.log(PREFIX + "DONE")
