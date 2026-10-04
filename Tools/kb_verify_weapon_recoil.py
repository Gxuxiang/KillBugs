"""
Read-only check of the weapon recoil values ON DISK, plus the projectile effect field.

Run in its own editor process, separate from kb_setup_weapon_recoil.py. A script that modifies an
asset and then re-loads it in the same process reads back its own in-memory object, which cannot
tell you whether the save ever reached the file.
"""

import unreal

PREFIX = "[KBVerifyWeaponRecoil] "

WEAPONS = [
    "/Game/KillBugs/Weapons/DA_Weapon_AutoRifle",
    "/Game/KillBugs/Weapons/DA_Weapon_Shotgun",
    "/Game/KillBugs/Weapons/DA_Weapon_Shockwave",
]

for path in WEAPONS:
    weapon = unreal.EditorAssetLibrary.load_asset(path)
    if weapon is None:
        unreal.log_warning(PREFIX + "MISSING " + path)
        continue

    distance = weapon.get_editor_property("recoil_distance")
    behavior = weapon.get_editor_property("behavior")
    delivery = weapon.get_editor_property("delivery")

    unreal.log("{}  {:<18} recoil={:.0f}cm  behavior={}  delivery={}".format(
        PREFIX, path.split("/")[-1], distance, behavior, delivery))

# The bullet visual is a PROJECT SETTING, not a property on the weapons or on the director -
# see the note in KBProjectileDirector.h on why. Read it from the settings object, which is where
# /Script/KillBugs.KBGameSettings in DefaultGame.ini actually lands.
settings_class = unreal.load_class(None, "/Script/KillBugs.KBGameSettings")
if settings_class is None:
    unreal.log_warning(PREFIX + "could not load UKBGameSettings")
else:
    settings = unreal.get_default_object(settings_class)

    # CAMEL CASE here, NOT the snake_case every other asset in this project uses.
    #
    # UKBGameSettings is a UDeveloperSettings and its binding exposes the raw property names;
    # UKBWeaponDefinition and UKBEnemyArchetype above are DataAssets and use the usual
    # set_editor_property("recoil_distance") spelling. Getting this wrong does not look like a
    # naming mistake - it raises "Failed to find property", which reads as "the field was never
    # added". See Tools/kb_probe_settings_props.py, which is what settled it.
    effect = settings.get_editor_property("ProjectileEffect")
    tint = settings.get_editor_property("ProjectileTint")

    unreal.log(PREFIX + "  KBGameSettings projectile_effect={}".format(
        effect.get_path_name() if effect else "unset -> bullets draw as spheres"))
    unreal.log(PREFIX + "  KBGameSettings projectile_tint=({:.2f},{:.2f},{:.2f})".format(
        tint.r, tint.g, tint.b))

unreal.log(PREFIX + "DONE")
