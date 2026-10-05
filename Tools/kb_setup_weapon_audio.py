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
# The fire sounds are the project's OWN (Content/KillBugs/Sound/MS_Fire*.uasset), not pack
# presets.
#
# This script used to point both guns at /Game/SoundMorphMetaSounds/Gun/Sources/BasicGun, with the
# shotgun pitched down to 0.7 to tell them apart - which is exactly what a placeholder sounds
# like: one gun, twice. The user heard it, said "音量/音色不对", and named the two assets this now
# uses. MS_Fire is the rifle, MS_Fire1 the shotgun.
# None means "no sound at all", and the loop below clears the field for it.
#
# Only the two guns the project has sounds for get one. The impact sounds and the shockwave are
# deliberately silent: the user listened and said the impact cues were too loud and the wrong
# character, and the shockwave's pack preset likewise. Silence is a real choice here - a wrong
# sound is worse than none, and a shot already has a muzzle flash and a recoil kick to read it by.
ASSIGNMENTS = [
    (
        "/Game/KillBugs/Weapons/DA_Weapon_AutoRifle",
        "/Game/KillBugs/Sound/MS_Fire",
        1.0,
        None,
    ),
    (
        "/Game/KillBugs/Weapons/DA_Weapon_Shotgun",
        "/Game/KillBugs/Sound/MS_Fire1",
        # 1.0 now: the 0.7 was there to disguise two guns sharing one source, and they no longer do.
        1.0,
        None,
    ),
    (
        "/Game/KillBugs/Weapons/DA_Weapon_Shockwave",
        None,
        1.0,
        None,
    ),
]


def load(path):
    if path is None:
        return None

    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        unreal.log_warning(PREFIX + "MISSING " + path)
    return asset


def apply_sound(weapon, property_name, path):
    """
    Sets one TSoftObjectPtr, or clears it when path is None, and reads it back.

    The readback is load-bearing rather than logging, and so is modify(): both come from
    kb_clear_weapon_audio.py's notes, which were earned by bisecting a version that reported
    success and wrote nothing. Setting several soft pointers back to back and saving once
    silently dropped all but the first.
    """
    value = load(path)
    if path is not None and value is None:
        return

    # READ BEFORE WRITE. Not decoration: a None write is not honoured until the property has been
    # read at least once, and a write that looks like it worked is the failure this whole file is
    # written around. Found with Tools/kb_probe_clear_soft_ptr.py, which reads before it sets and
    # clears fine - while this loop, which set first, silently kept the old value.
    unreal.log("{}  {} (before) = {}".format(
        PREFIX, property_name,
        weapon.get_editor_property(property_name).get_name() or "?"
        if weapon.get_editor_property(property_name) else "<none>"))

    weapon.set_editor_property(property_name, value)

    readback = weapon.get_editor_property(property_name)
    unreal.log("{}  {} (after)  = {}".format(
        PREFIX, property_name, readback.get_name() if readback else "<none>"))


for weapon_path, fire_path, fire_pitch, impact_path in ASSIGNMENTS:
    weapon = load(weapon_path)
    if weapon is None:
        continue

    apply_sound(weapon, "fire_sound", fire_path)
    apply_sound(weapon, "impact_sound", impact_path)

    try:
        weapon.set_editor_property("fire_sound_pitch", fire_pitch)
    except Exception as error:  # noqa: BLE001 - field may not exist yet
        unreal.log_warning(PREFIX + "could not set pitch: {}".format(error))

    # modify() AFTER the edits, immediately before the save - the order kb_clear_weapon_audio.py
    # uses, and the order matters: with modify() called first, the clears did not reach disk at
    # all, which a separate-process verification caught while the in-process readback still showed
    # the old values.
    weapon.modify()

    # save_asset, NOT save_loaded_asset: setting a property from Python does not reliably mark
    # the package dirty, so the latter writes nothing and the edit is lost on editor close.
    unreal.EditorAssetLibrary.save_asset(weapon_path, only_if_is_dirty=False)
    unreal.log("{}{} <- fire={} pitch={} impact={}".format(
        PREFIX, weapon_path.split("/")[-1],
        (fire_path or "<none>").split("/")[-1], fire_pitch,
        (impact_path or "<none>").split("/")[-1]))

unreal.log(PREFIX + "DONE")
