"""
Probe: what are the pack's assets, and which are loopable enough to serve as BGM?

BGM needs a bed that sustains for minutes. A one-shot plays its envelope and stops, so the
class and loop behaviour decide whether an asset can be a track as-is or has to be wrapped in
a MetaSound that loops it.
"""

import unreal

PREFIX = "[KBBgm] "

# (label, path) - grouped by the role each group would play.
CANDIDATES = [
    ("AMBIENT", "/Game/SoundMorphMetaSounds/Weather/Presets/Thunder_Rain_Wind_1"),
    ("AMBIENT", "/Game/SoundMorphMetaSounds/Weather/Presets/Thunder_Rain_Wind_3"),
    ("AMBIENT", "/Game/SoundMorphMetaSounds/Weather/Rain/Rain"),
    ("DRONE",   "/Game/SoundMorphMetaSounds/Wooshes/WooshMaker/Samples/1_drone_after_life"),
    ("DRONE",   "/Game/SoundMorphMetaSounds/Wooshes/WooshMaker/Samples/1_drone_a_ghost_choir"),
    ("DRONE",   "/Game/SoundMorphMetaSounds/Wooshes/WooshMaker/Samples/1_drone_air_stratos"),
    ("IMPACT",  "/Game/SoundMorphMetaSounds/Wooshes/WooshMaker/Samples/3_impact_blasts_rubblerebel"),
    ("IMPACT",  "/Game/SoundMorphMetaSounds/Wooshes/WooshMaker/Samples/3_impact_complextechhits"),
    ("GUN",     "/Game/SoundMorphMetaSounds/Gun/Presets/Gun_Preset_1"),
    ("GUN",     "/Game/SoundMorphMetaSounds/Gun/Presets/Gun_Preset_5"),
    ("EXPLODE", "/Game/SoundMorphMetaSounds/Explosion/Presets/Explosion_Preset_1"),
    ("UI",      "/Game/SoundMorphMetaSounds/UI/Presets/UI_Buttons_Preset_1"),
    ("UI",      "/Game/SoundMorphMetaSounds/UI/Presets/UI_Buttons_Preset_6"),
    ("PICKUP",  "/Game/SoundMorphMetaSounds/Retro/Presets/CoinRandom_Preset_1"),
    ("HEAL",    "/Game/SoundMorphMetaSounds/Heal/Presets/Heal_Preset_1"),
    ("BUG",     "/Game/SoundMorphMetaSounds/SmallBot/Presets/Beeper_Preset_1"),
]


def describe(path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        return "MISSING"

    cls = asset.get_class().get_name()
    extra = ""

    # SoundWave carries an explicit looping flag; a MetaSoundSource does not.
    if "SoundWave" in cls:
        try:
            extra = "  looping={}".format(asset.get_editor_property("looping"))
        except Exception as error:  # noqa: BLE001
            extra = "  <{}>".format(error)

    # MetaSoundSource reports whether it is one-shot or looping.
    duration = ""
    try:
        if asset.get_editor_property("is_one_shot") is not None:
            duration = "  oneShot={}".format(asset.get_editor_property("is_one_shot"))
    except Exception:  # noqa: BLE001 - not every sound exposes it
        pass

    return "{}{}{}".format(cls, extra, duration)


for role, path in CANDIDATES:
    unreal.log("{}{:<9} {:<34} {}".format(PREFIX, role, path.split("/")[-1], describe(path)))

unreal.log(PREFIX + "DONE")
