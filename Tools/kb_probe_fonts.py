"""
Probe: can the engine's default Canvas font actually render CJK?

Canvas DrawText takes a UFont. If that font's composite includes a CJK fallback typeface, the
runtime font cache resolves Chinese glyphs on demand and nothing needs building. If it does
not, Chinese text renders as blank boxes and a UFont pointing at DroidSansFallback has to be
authored - which is exactly the failure mode that is invisible without looking at the screen.
"""

import unreal

PREFIX = "[KBFonts] "

CANDIDATES = [
    "/Engine/EngineFonts/Roboto",
    "/Engine/EngineFonts/SmallFont",
    "/Engine/EngineFonts/TinyFont",
    "/Engine/EngineFonts/RobotoDistanceField",
]


def describe_face(font_face):
    if font_face is None:
        return "NONE"
    try:
        return font_face.get_path_name()
    except Exception as error:  # noqa: BLE001
        return "<{}>".format(error)


def describe_typeface(typeface):
    """A typeface holds a list of faces plus scaling for each."""
    try:
        faces = typeface.get_editor_property("fonts")
    except Exception as error:  # noqa: BLE001
        return ["<fonts error: {}>".format(error)]

    described = []
    for entry in faces:
        try:
            face = entry.get_editor_property("font")
        except Exception:  # noqa: BLE001
            face = None
        described.append(describe_face(face))
    return described


for path in CANDIDATES:
    font = unreal.EditorAssetLibrary.load_asset(path)
    if font is None:
        unreal.log_warning(PREFIX + "missing " + path)
        continue

    try:
        cache_type = font.get_editor_property("font_cache_type")
    except Exception as error:  # noqa: BLE001
        cache_type = "<{}>".format(error)

    unreal.log("{}{}  cache_type={}".format(PREFIX, path, cache_type))

    try:
        composite = font.get_editor_property("composite_font")
    except Exception as error:  # noqa: BLE001
        unreal.log(PREFIX + "    no composite_font: {}".format(error))
        continue

    try:
        default_typeface = composite.get_editor_property("default_typeface")
        unreal.log(PREFIX + "    default:  {}".format(describe_typeface(default_typeface)))
    except Exception as error:  # noqa: BLE001
        unreal.log(PREFIX + "    default typeface error: {}".format(error))

    try:
        fallback_typeface = composite.get_editor_property("fallback_typeface")
        unreal.log(PREFIX + "    fallback: {}".format(describe_typeface(fallback_typeface)))
    except Exception as error:  # noqa: BLE001
        unreal.log(PREFIX + "    fallback typeface error: {}".format(error))

unreal.log(PREFIX + "DONE")
