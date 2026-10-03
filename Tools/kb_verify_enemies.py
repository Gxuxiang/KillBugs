"""
Read-only check of what the enemy archetype assets actually contain ON DISK.

Run this in its own editor process, separate from kb_setup_enemies.py. A script that
modifies an asset and then re-loads it in the same process reads back its own in-memory
object, which cannot tell you whether the save ever reached the file.
"""

import unreal

PREFIX = "[KBVerify] "
PATH = "/Game/KillBugs/Enemies/{}"

for name in ("DA_Bug_Grunt", "DA_Bug_Runner", "DA_Bug_Brute"):
    asset = unreal.EditorAssetLibrary.load_asset(PATH.format(name))
    if asset is None:
        unreal.log_warning(PREFIX + "MISSING " + name)
        continue

    material = asset.get_editor_property("material")
    mesh = asset.get_editor_property("mesh")
    tint = asset.get_editor_property("tint")

    unreal.log("{}{}  mesh={}  material={}  tint=({:.2f},{:.2f},{:.2f})".format(
        PREFIX, name,
        mesh.get_path_name() if mesh else "NONE",
        material.get_path_name() if material else "NONE",
        tint.r, tint.g, tint.b))

material = unreal.EditorAssetLibrary.load_asset("/Game/KillBugs/Enemies/M_KBEnemy")
unreal.log("{}M_KBEnemy ism_flag={}".format(
    PREFIX,
    material.get_editor_property("used_with_instanced_static_meshes") if material else "MISSING"))

# --- Weapons and cards -------------------------------------------------------------------
# Also checks that the localised text survived the asset round trip; a mojibake or empty
# string here means the encoding was lost on write.
for kind, folder, names in (
    ("weapon", "/Game/KillBugs/Weapons", ("DA_Weapon_AutoRifle", "DA_Weapon_Shockwave", "DA_Weapon_Shotgun")),
    ("card", "/Game/KillBugs/Cards", ("DA_Card_AddShockwave", "DA_Card_DamageUp", "DA_Card_Veteran")),
):
    for name in names:
        asset = unreal.EditorAssetLibrary.load_asset("{}/{}".format(folder, name))
        if asset is None:
            unreal.log_warning("{}MISSING {}/{}".format(PREFIX, folder, name))
            continue
        if kind == "weapon":
            unreal.log("{}{} {} title='{}'".format(
                PREFIX, kind, name, asset.get_editor_property("display_name")))
        else:
            # Card -> weapon references are hard pointers. Recreating a weapon asset at the
            # same path should re-resolve them, but "should" is what this line checks: a
            # dangling reference here means every card that grants or upgrades a weapon is
            # silently dead.
            weapon = asset.get_editor_property("weapon")
            requires = asset.get_editor_property("requires_weapon")
            unreal.log("{}{} {} title='{}' weapon={} requiresWeapon={}".format(
                PREFIX, kind, name, asset.get_editor_property("title"),
                weapon.get_name() if weapon else "-",
                requires.get_name() if requires else "-"))

unreal.log(PREFIX + "DONE")
