"""
Assigns SoundMorph pack sounds to the existing weapon assets.

Run headlessly with the EDITOR CLOSED. Verify afterwards with kb_verify_weapons_audio.py in a
separate process - an in-place edit that appears to succeed but never reaches disk is this
project's most-repeated failure.

The mapping is a guess by role, not by ear: nothing here has been listened to. Each weapon
gets a different preset so they are at least distinguishable, and the summary below prints
what landed where so any of it can be swapped in the editor in seconds.
"""

import unreal

PREFIX = "[KBWeaponAudio] "

# (weapon asset, fire sound, fire pitch, impact sound)
#
# The fire sounds come from Gun/Sources/, NOT Gun/Presets/. The presets are mostly BURSTS -
# a single trigger pull plays "rat-tat-tat" - which is wrong for a weapon that fires one
# bullet at a time: the audible rhythm stops matching the visible one, and on a fast weapon
# the bursts overlap into noise. Sources/BasicGun is the single-shot primitive the presets
# are built from.
#
# Pitch is what tells the two guns apart while sharing one asset: lower reads as heavier.
ASSIGNMENTS = [
    (
        "/Game/KillBugs/Weapons/DA_Weapon_AutoRifle",
        "/Game/SoundMorphMetaSounds/Gun/Sources/BasicGun",
        1.0,
        "/Game/SoundMorphMetaSounds/Wooshes/WooshMaker/Samples/3_impact_complextechhits",
    ),
    (
        "/Game/KillBugs/Weapons/DA_Weapon_Shotgun",
        "/Game/SoundMorphMetaSounds/Gun/Sources/BasicGun",
        0.7,
        "/Game/SoundMorphMetaSounds/Wooshes/WooshMaker/Samples/3_impact_blasts_rubblerebel",
    ),
    (
        "/Game/KillBugs/Weapons/DA_Weapon_Shockwave",
        "/Game/SoundMorphMetaSounds/Explosion/Presets/Explosion_Preset_1",
        1.0,
        "/Game/SoundMorphMetaSounds/Explosion/Presets/Explosion_Preset_3",
    ),
]


def load(path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        unreal.log_warning(PREFIX + "MISSING " + path)
    return asset


for weapon_path, fire_path, fire_pitch, impact_path in ASSIGNMENTS:
    weapon = load(weapon_path)
    fire = load(fire_path)
    impact = load(impact_path)

    if weapon is None:
        continue

    if fire is not None:
        weapon.set_editor_property("fire_sound", fire)
    if impact is not None:
        weapon.set_editor_property("impact_sound", impact)

    try:
        weapon.set_editor_property("fire_sound_pitch", fire_pitch)
    except Exception as error:  # noqa: BLE001 - field may not exist yet
        unreal.log_warning(PREFIX + "could not set pitch: {}".format(error))

    # save_asset, NOT save_loaded_asset: setting a property from Python does not reliably mark
    # the package dirty, so the latter writes nothing and the edit is lost on editor close.
    unreal.EditorAssetLibrary.save_asset(weapon_path, only_if_is_dirty=False)
    unreal.log("{}{} <- fire={} pitch={} impact={}".format(
        PREFIX, weapon_path.split("/")[-1],
        fire_path.split("/")[-1], fire_pitch, impact_path.split("/")[-1]))

unreal.log(PREFIX + "DONE")
