"""Create the stored CGH SLM preview pattern, preserving any existing asset.

Run with the compiled CGHSimEditor target and PythonScriptPlugin enabled:
    UnrealEditor-Cmd <project.uproject> -run=pythonscript \
        -script=<absolute-path-to-this-file> -unattended -nullrhi -nosplash

Creates /Game/CGHSim/PhasePatterns/DA_SLMPreviewPattern as a 256x256 phase
DataAsset. Its quadrants contain horizontal/vertical ramps, concentric rings,
and a checkerboard; a bright L marks the top-left corner. This is a display
sample, not a computed hologram. Only this newly created asset is saved.
Existing assets are validated and preserved, including user-edited patterns
with different dimensions. No maps, Blueprints, or actor instances are edited.
"""

import math
from pathlib import Path

import unreal


ASSET_FOLDER = "/Game/CGHSim/PhasePatterns"
ASSET_NAME = "DA_SLMPreviewPattern"
ASSET_PATH = ASSET_FOLDER + "/" + ASSET_NAME
SAMPLE_WIDTH = 256
SAMPLE_HEIGHT = 256
FULL_CYCLE = 2.0 * math.pi


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def asset_exists(asset_subsystem, asset_path):
    """Preserve packages on disk even when the asset registry has not indexed them."""
    content_path = (Path(unreal.Paths.project_content_dir()) /
                    asset_path.removeprefix("/Game/"))
    return asset_subsystem.does_asset_exist(asset_path) or any(
        content_path.with_suffix(extension).exists()
        for extension in (".uasset", ".umap")
    )


def make_preview_pattern():
    """Finite radians in row-major order, with visually distinct orientation cues."""
    phases = []
    half_width = SAMPLE_WIDTH // 2
    half_height = SAMPLE_HEIGHT // 2
    for y in range(SAMPLE_HEIGHT):
        for x in range(SAMPLE_WIDTH):
            if y < half_height:
                if x < half_width:
                    fraction = x / half_width  # Top-left: horizontal ramp.
                else:
                    fraction = y / half_height  # Top-right: vertical ramp.
            elif x < half_width:
                u = (x + 0.5) / half_width - 0.5
                v = (y - half_height + 0.5) / half_height - 0.5
                fraction = (5.0 * math.hypot(u, v)) % 1.0  # Bottom-left: rings.
            else:
                cell_x = (x - half_width) // 16
                cell_y = (y - half_height) // 16
                fraction = 0.9 if (cell_x + cell_y) % 2 else 0.1
            if x < 24 and y < 24 and (x < 8 or y < 8):
                fraction = 0.97  # Asymmetric top-left L, visible after downsampling.
            phases.append((fraction % 1.0) * FULL_CYCLE)

    check(len(phases) == SAMPLE_WIDTH * SAMPLE_HEIGHT, "Incorrect sample-grid size")
    check(all(math.isfinite(value) and 0.0 <= value < FULL_CYCLE for value in phases),
          "Sample generation produced an invalid phase")
    pattern = unreal.CGHSLMPhasePattern()
    pattern.set_editor_property("resolution_x", SAMPLE_WIDTH)
    pattern.set_editor_property("resolution_y", SAMPLE_HEIGHT)
    pattern.set_editor_property("phase_rad", phases)
    return pattern


def validate_asset(asset):
    check(isinstance(asset, unreal.CGHPhasePatternAsset),
          f"Existing package must contain a CGHPhasePatternAsset: {ASSET_PATH}")
    width = asset.get_resolution_x()
    height = asset.get_resolution_y()
    count = asset.get_sample_count()
    error = asset.get_validation_error()
    check(width > 0 and height > 0 and count == width * height and asset.has_valid_pattern(),
          f"Stored phase pattern is invalid; asset preserved: {ASSET_PATH}: {error}")
    return width, height, count


def main():
    native_class = unreal.load_class(None, "/Script/CGHSim.CGHPhasePatternAsset")
    check(native_class is not None,
          "Build CGHSimEditor before running: missing CGHPhasePatternAsset")
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    unreal.AssetRegistryHelpers.get_asset_registry().wait_for_completion()

    if asset_exists(assets, ASSET_PATH):
        asset = assets.load_asset(ASSET_PATH)
        check(asset is not None, f"Existing package cannot be loaded; preserved: {ASSET_PATH}")
        width, height, count = validate_asset(asset)
        unreal.log(f"CGH_PHASE_SAMPLE_READY preserved {ASSET_PATH} ({width}x{height}, {count} samples)")
        return

    pattern = make_preview_pattern()
    if not assets.does_directory_exist(ASSET_FOLDER):
        check(assets.make_directory(ASSET_FOLDER), f"Could not create {ASSET_FOLDER}")
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", native_class)
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        ASSET_NAME, ASSET_FOLDER, native_class, factory
    )
    check(asset is not None, f"Could not create {ASSET_PATH}")
    check(asset.set_pattern(pattern),
          f"Could not initialize {ASSET_PATH}: {asset.get_validation_error()}")
    asset.set_editor_property("pattern_label", unreal.Text("Stored preview pattern"))
    asset.set_editor_property("is_preview_pattern", True)
    width, height, count = validate_asset(asset)
    check((width, height, count) ==
          (SAMPLE_WIDTH, SAMPLE_HEIGHT, SAMPLE_WIDTH * SAMPLE_HEIGHT),
          "New sample asset does not match the generated grid")
    check(assets.save_loaded_asset(asset), f"Could not save new sample asset {ASSET_PATH}")
    unreal.log(f"CGH_PHASE_SAMPLE_READY created {ASSET_PATH} ({width}x{height}, {count} samples)")


if __name__ == "__main__":
    main()
