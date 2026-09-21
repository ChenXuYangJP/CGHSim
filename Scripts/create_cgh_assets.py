"""Create the CGH starter Blueprints and workbench map inside Unreal Editor.

Run with the project's compiled Editor target and PythonScriptPlugin enabled:
    UnrealEditor-Cmd <project.uproject> -run=pythonscript \
        -script=<absolute-path-to-this-file> -unattended -nullrhi -nosplash

Existing assets and maps are never overwritten. New maps are saved only after
all actors, references, and scene validation have succeeded. A commandlet has
no editor viewport: select the CGH actors in the World Outliner and press F to
frame them after opening the generated map.
"""

from pathlib import Path

import unreal


CONTENT_ROOT = "/Game/CGHSim"
BLUEPRINT_FOLDER = CONTENT_ROOT + "/Blueprints"
MAP_PATH = CONTENT_ROOT + "/Maps/L_CGHWorkbench"
BLUEPRINTS = {
    "BP_CGHTargetPoint": "CGHTargetActor",
    "BP_CGHSLM": "CGHSLMActor",
    "BP_CGHCamera": "CGHCameraActor",
    "BP_CGHReconstructionLight": "CGHReconstructionLightActor",
    "BP_CGHWorkbench": "CGHWorkbenchActor",
    "BP_CGHSolver": "CGHSolverActor",
}


def asset_exists(asset_subsystem, asset_path):
    """Check disk as well as the registry to preserve unindexed user assets."""
    relative_path = asset_path.removeprefix("/Game/")
    content_path = Path(unreal.Paths.project_content_dir()) / relative_path
    return asset_subsystem.does_asset_exist(asset_path) or any(
        content_path.with_suffix(extension).exists()
        for extension in (".uasset", ".umap")
    )


def ensure_blueprint(asset_subsystem, asset_tools, name, parent_class):
    asset_path = BLUEPRINT_FOLDER + "/" + name
    if asset_exists(asset_subsystem, asset_path):
        blueprint = asset_subsystem.load_asset(asset_path)
        if not isinstance(blueprint, unreal.Blueprint):
            raise RuntimeError(f"Existing asset is not a Blueprint: {asset_path}")
        unreal.log(f"CGHSim: preserving {asset_path}")
    else:
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", parent_class)
        blueprint = asset_tools.create_asset(
            name, BLUEPRINT_FOLDER, unreal.Blueprint, factory
        )
        if blueprint is None:
            raise RuntimeError(f"Failed to create {asset_path}")
        if not asset_subsystem.save_loaded_asset(blueprint):
            raise RuntimeError(f"Failed to save {asset_path}")
        unreal.log(f"CGHSim: created {asset_path}")

    generated_class = asset_subsystem.load_blueprint_class(asset_path)
    if generated_class is None:
        raise RuntimeError(f"Blueprint has no generated class: {asset_path}")
    # Blueprint.ParentClass is not exposed to Python; use the reflected UClass
    # inheritance utility instead. Compatible user-created subclasses are valid.
    if not unreal.MathLibrary.class_is_child_of(generated_class, parent_class):
        raise RuntimeError(f"Blueprint does not derive from the required CGH class: {asset_path}")
    return generated_class


def spawn(actor_subsystem, actor_class, label, position, yaw=0.0, folder="CGH"):
    actor = actor_subsystem.spawn_actor_from_class(
        actor_class, unreal.Vector(*position), unreal.Rotator(yaw=yaw)
    )
    if actor is None:
        raise RuntimeError(f"Failed to spawn {label}")
    actor.set_actor_label(label)
    actor.set_folder_path(folder)
    return actor


def validate_workbench(workbench):
    if not workbench.validate_scene():
        messages = workbench.get_editor_property("validation_messages")
        raise RuntimeError("CGH workbench validation failed: " + "; ".join(messages))


def create_map(asset_subsystem, classes):
    if asset_exists(asset_subsystem, MAP_PATH):
        unreal.log(f"CGHSim: preserving existing map {MAP_PATH}")
        return

    # NewBlankMap closes the current world. Do not discard an editor user's work.
    if unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages():
        raise RuntimeError("Save the current dirty map before creating the CGH workbench.")

    world = unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
    if world is None:
        raise RuntimeError("Could not create a blank editor world.")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

    slm = spawn(actors, classes["BP_CGHSLM"], "CGH SLM", (0.0, 0.0, 0.0))
    target = spawn(actors, classes["BP_CGHTargetPoint"], "CGH Target Point", (50.0, 0.0, 0.0))
    camera = spawn(actors, classes["BP_CGHCamera"], "CGH Camera", (100.0, 0.0, 0.0), 180.0)
    light = spawn(actors, classes["BP_CGHReconstructionLight"], "CGH Reconstruction Light", (-30.0, 0.0, 0.0))
    workbench = spawn(actors, classes["BP_CGHWorkbench"], "CGH Workbench", (0.0, 30.0, 25.0))
    solver = spawn(actors, classes["BP_CGHSolver"], "CGH Solver", (0.0, 50.0, 25.0))
    solver.set_editor_property("workbench", workbench)
    workbench.set_editor_property("solver", solver)
    workbench.set_editor_property("slm", slm)
    workbench.set_editor_property("camera", camera)
    workbench.set_editor_property("reconstruction_light", light)
    workbench.set_editor_property("targets", [target])
    workbench.refresh_visualization()
    validate_workbench(workbench)

    # Presentation geometry uses built-in engine assets and has no optical role.
    floor = spawn(actors, unreal.StaticMeshActor, "Workbench Floor", (35.0, 0.0, -8.0), folder="Presentation")
    floor_mesh = unreal.load_asset("/Engine/BasicShapes/Cube.Cube")
    if floor_mesh is None:
        raise RuntimeError("Built-in Engine cube mesh is unavailable.")
    floor.static_mesh_component.set_static_mesh(floor_mesh)
    floor.set_actor_scale3d(unreal.Vector(1.7, 0.8, 0.02))
    preview_light = spawn(actors, unreal.DirectionalLight, "Preview Lighting", (0.0, 0.0, 100.0), folder="Presentation")
    preview_light.set_actor_rotation(unreal.Rotator(pitch=-45.0, yaw=-30.0), False)
    preview_light.get_component_by_class(unreal.DirectionalLightComponent).set_mobility(unreal.ComponentMobility.MOVABLE)

    viewport_position = unreal.Vector(135.0, -145.0, 90.0)
    viewport_rotation = unreal.MathLibrary.find_look_at_rotation(
        viewport_position, unreal.Vector(35.0, 0.0, 0.0)
    )
    # Commandlets have no viewport; frame the actors with F when opening the map.
    viewport_key = level_editor.get_active_viewport_config_key()
    if viewport_key != unreal.Name("None"):
        level_editor.set_level_viewport_camera_info(
            viewport_position, viewport_rotation, viewport_key
        )
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, MAP_PATH):
        raise RuntimeError(f"Failed to save {MAP_PATH}")

    # Reload once to verify that references and validation survive serialization.
    if not level_editor.load_level(MAP_PATH):
        raise RuntimeError(f"Failed to reload {MAP_PATH}")
    persisted = [actor for actor in actors.get_all_level_actors()
                 if actor.get_class() == classes["BP_CGHWorkbench"]]
    if len(persisted) != 1:
        raise RuntimeError("Saved workbench map does not contain exactly one workbench.")
    validate_workbench(persisted[0])
    if len(persisted[0].get_editor_property("targets")) != 1:
        raise RuntimeError("Saved workbench target reference was not preserved.")
    persisted_solver = persisted[0].get_editor_property("solver")
    if persisted_solver is None or persisted_solver.get_class() != classes["BP_CGHSolver"]:
        raise RuntimeError("Saved workbench solver reference was not preserved.")
    if persisted_solver.get_editor_property("workbench") != persisted[0]:
        raise RuntimeError("Saved solver workbench reference was not preserved.")
    unreal.log(f"CGHSim: created and verified {MAP_PATH}")


def main():
    asset_subsystem = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
    unreal.AssetRegistryHelpers.get_asset_registry().wait_for_completion()

    native_classes = {}
    for name, native_name in BLUEPRINTS.items():
        native_class = unreal.load_class(None, "/Script/CGHSim." + native_name)
        if native_class is None:
            raise RuntimeError(f"Build CGHSimEditor before running: missing {native_name}")
        native_classes[name] = native_class

    for folder in ("Blueprints", "Materials", "Meshes", "Maps"):
        folder_path = CONTENT_ROOT + "/" + folder
        if not asset_subsystem.does_directory_exist(folder_path):
            if not asset_subsystem.make_directory(folder_path):
                raise RuntimeError(f"Could not create Content/CGHSim/{folder}")
    classes = {
        name: ensure_blueprint(asset_subsystem, asset_tools, name, native_class)
        for name, native_class in native_classes.items()
    }
    create_map(asset_subsystem, classes)
    unreal.log("CGHSim: starter asset setup complete.")


if __name__ == "__main__":
    main()
