"""Integration smoke checks for the generated CGH starter scene.

Run after create_cgh_assets.py, in a fresh UnrealEditor-Cmd process:
    UnrealEditor-Cmd <project.uproject> -run=pythonscript \
        -script=<absolute-path-to-this-file> -unattended -nullrhi -nosplash

The saved starter map is inspected without changing its actors. All mutation
checks use a separate unsaved world. This script never saves any assets/maps.
"""

import math

import unreal


MAP_PATH = "/Game/CGHSim/Maps/L_CGHWorkbench"
BLUEPRINTS = {
    "BP_CGHTargetPoint": "CGHTargetActor",
    "BP_CGHSLM": "CGHSLMActor",
    "BP_CGHCamera": "CGHCameraActor",
    "BP_CGHReconstructionLight": "CGHReconstructionLightActor",
    "BP_CGHWorkbench": "CGHWorkbenchActor",
}


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def near(actual, expected, message):
    check(math.isclose(actual, expected, rel_tol=1.0e-6, abs_tol=1.0e-6),
          f"{message}: expected {expected}, got {actual}")


def vector_near(actual, expected, message):
    for axis, value in zip(("x", "y", "z"), expected):
        near(getattr(actual, axis), value, f"{message}.{axis}")


def set_parameter(actor, name, value):
    parameters = actor.get_editor_property("parameters")
    previous = parameters.get_editor_property(name)
    parameters.set_editor_property(name, value)
    actor.set_editor_property("parameters", parameters)
    return previous


def expect_validation(workbench, valid, diagnostic=""):
    workbench.validate_scene()
    messages = list(workbench.get_editor_property("validation_messages"))
    check(workbench.get_editor_property("scene_valid") == valid,
          f"Expected scene_valid={valid}; diagnostics: {messages}")
    if diagnostic:
        check(any(diagnostic.lower() in message.lower() for message in messages),
              f"Missing diagnostic {diagnostic!r}: {messages}")


def check_defaults(slm, target, camera, light):
    defaults = (
        (slm, {"resolution_x": 1024, "resolution_y": 1024,
               "pixel_pitch_x_um": 8.0, "pixel_pitch_y_um": 8.0}),
        (target, {"amplitude": 1.0, "initial_phase_rad": 0.0}),
        (camera, {"focal_length_mm": 50.0, "f_number": 4.0,
                  "focus_distance_mm": 1000.0, "sensor_width_mm": 36.0,
                  "sensor_height_mm": 24.0, "output_resolution_x": 1920,
                  "output_resolution_y": 1080}),
        (light, {"wavelength_nm": 532.0, "amplitude": 1.0,
                 "initial_phase_rad": 0.0, "polarization_angle_deg": 0.0}),
    )
    for actor, expected in defaults:
        parameters = actor.get_editor_property("parameters")
        for name, value in expected.items():
            near(parameters.get_editor_property(name), value,
                 f"{actor.get_name()}.{name}")
    near(slm.get_active_width_mm(), 8.192, "Default SLM width in mm")
    near(slm.get_active_height_mm(), 8.192, "Default SLM height in mm")
    check(slm.get_editor_property("generation_state") ==
          unreal.CGHGenerationState.NOT_IMPLEMENTED, "SLM must remain unimplemented")
    check(not slm.get_editor_property("has_phase_data"), "SLM must have no phase data")
    check(target.get_editor_property("parameters").target_type ==
          unreal.CGHTargetType.POINT, "Default target must be a mathematical point")
    check(light.get_editor_property("parameters").source_type ==
          unreal.CGHSourceType.PLANE_WAVE, "Default light must be a plane wave")
    check(slm.get_editor_property("parameters").modulation_type ==
          unreal.CGHSLMModulationType.PHASE_ONLY, "Default modulation must be phase only")


def inspect_saved_scene(assets, actors, level_editor):
    classes = {}
    for blueprint_name, native_name in BLUEPRINTS.items():
        native = unreal.load_class(None, "/Script/CGHSim." + native_name)
        check(native is not None, f"Missing native class {native_name}")
        path = "/Game/CGHSim/Blueprints/" + blueprint_name
        blueprint = assets.load_asset(path)
        check(isinstance(blueprint, unreal.Blueprint), f"Missing Blueprint {path}")
        classes[blueprint_name] = assets.load_blueprint_class(path)
        check(classes[blueprint_name] is not None, f"Missing generated class {path}")
        check(unreal.MathLibrary.class_is_child_of(classes[blueprint_name], native),
              f"Incorrect native parent for {path}")

    check(level_editor.load_level(MAP_PATH), "Could not load saved workbench map")
    saved_actors = actors.get_all_level_actors()
    workbenches = [actor for actor in saved_actors
                  if actor.get_class() == classes["BP_CGHWorkbench"]]
    check(len(workbenches) == 1, "Saved map must have exactly one workbench")
    workbench = workbenches[0]
    refs = {}
    for field, blueprint in (("slm", "BP_CGHSLM"), ("camera", "BP_CGHCamera"),
                             ("reconstruction_light", "BP_CGHReconstructionLight")):
        refs[field] = workbench.get_editor_property(field)
        check(refs[field] in saved_actors and refs[field].get_class() == classes[blueprint],
              f"Persisted {field} reference must point to the expected actor in the map")
    targets = list(workbench.get_editor_property("targets"))
    check(len(targets) == 1 and targets[0] in saved_actors and
          targets[0].get_class() == classes["BP_CGHTargetPoint"],
          "Persisted target reference must point to the target Blueprint in the map")
    check_defaults(refs["slm"], targets[0], refs["camera"], refs["reconstruction_light"])
    vector_near(targets[0].get_optical_position_meters(refs["slm"]),
                (0.5, 0.0, 0.0), "Persisted target position in SLM-local meters")
    unreal.log("CGH smoke: saved Blueprint parents, references, and defaults passed")
    return classes


def inspect_temporary_scene(classes, actors):
    check(unreal.EditorLoadingAndSavingUtils.new_blank_map(False) is not None,
          "Could not create unsaved test world")
    spawned = {}
    for name, actor_class in classes.items():
        spawned[name] = actors.spawn_actor_from_class(actor_class, unreal.Vector())
        check(spawned[name] is not None, f"Could not spawn {name}")
    slm = spawned["BP_CGHSLM"]
    target = spawned["BP_CGHTargetPoint"]
    camera = spawned["BP_CGHCamera"]
    light = spawned["BP_CGHReconstructionLight"]
    workbench = spawned["BP_CGHWorkbench"]
    for field, actor in (("slm", slm), ("camera", camera), ("reconstruction_light", light)):
        workbench.set_editor_property(field, actor)
    workbench.set_editor_property("targets", [target])
    workbench.refresh_visualization()
    expect_validation(workbench, True)

    target.set_actor_location(unreal.Vector(50.0, 12.0, -7.0), False, False)
    expected_position = (0.5, 0.12, -0.07)
    vector_near(target.get_optical_position_meters(slm), expected_position, "SI export")
    target.set_editor_property("marker_radius_cm", 17.0)
    target.refresh_visualization()
    vector_near(target.get_optical_position_meters(slm), expected_position,
                "Marker size must not change optical position")
    near(target.get_editor_property("marker_mesh").get_editor_property("relative_scale3d").x,
         17.0 / 50.0, "Marker visualization updates independently")

    # Apply the same yaw=90 degree rotation and translation to both actor poses.
    slm.set_actor_location(unreal.Vector(170.0, -240.0, 90.0), False, False)
    slm.set_actor_rotation(unreal.Rotator(yaw=90.0), False)
    target.set_actor_location(unreal.Vector(158.0, -190.0, 83.0), False, False)
    target.set_actor_rotation(unreal.Rotator(yaw=90.0), False)
    vector_near(target.get_optical_position_meters(slm), expected_position,
                "Common rigid transform must preserve optical coordinates")
    vector_near(target.get_optical_position_meters(None), (1.58, -1.90, 0.83),
                "Null reference exports world meters")
    light.set_actor_rotation(unreal.Rotator(yaw=90.0), False)
    vector_near(light.get_propagation_direction(), (0.0, 1.0, 0.0),
                "Propagation follows the light's local +X axis")

    for name, value in (("resolution_x", 2048), ("resolution_y", 512),
                        ("pixel_pitch_x_um", 6.4), ("pixel_pitch_y_um", 10.0)):
        set_parameter(slm, name, value)
    slm.refresh_visualization()
    near(slm.get_active_width_mm(), 13.1072, "Edited SLM width in mm")
    near(slm.get_active_height_mm(), 5.12, "Edited SLM height in mm")
    active = slm.get_editor_property("active_area_root")
    mesh = slm.get_editor_property("active_area_mesh")
    low, high = mesh.get_local_bounds()
    parent_scale = active.get_editor_property("relative_scale3d")
    mesh_scale = mesh.get_editor_property("relative_scale3d")
    near((high.y - low.y) * mesh_scale.y * parent_scale.y, 1.31072,
         "Displayed active width in cm")
    near((high.z - low.z) * mesh_scale.z * parent_scale.z, 0.512,
         "Displayed active height in cm")
    vector_near(slm.get_actor_scale3d(), (1.0, 1.0, 1.0), "SLM root remains unscaled")

    preview = camera.get_editor_property("preview_camera")
    near(preview.get_editor_property("focus_settings").manual_focus_distance,
         100.0, "Default camera focus converts mm to cm")
    for name, value in (("focal_length_mm", 8.0), ("f_number", 0.7),
                        ("focus_distance_mm", 250.0), ("sensor_width_mm", 12.0),
                        ("sensor_height_mm", 8.0)):
        set_parameter(camera, name, value)
    camera.refresh_visualization()
    near(preview.get_editor_property("current_focal_length"), 8.0,
         "Custom focal length must survive Cine Camera lens limits")
    near(preview.get_editor_property("current_aperture"), 0.7,
         "Custom aperture must survive Cine Camera lens limits")
    near(preview.get_editor_property("focus_settings").manual_focus_distance,
         25.0, "Edited camera focus converts mm to cm")
    near(preview.get_editor_property("filmback").sensor_width, 12.0, "Preview sensor width")
    near(preview.get_editor_property("filmback").sensor_height, 8.0, "Preview sensor height")
    expect_validation(workbench, True)

    for field in ("slm", "camera", "reconstruction_light"):
        actor = workbench.get_editor_property(field)
        workbench.set_editor_property(field, None)
        expect_validation(workbench, False, "valid actor")
        workbench.set_editor_property(field, actor)
    for targets, diagnostic in (([], "at least one"), ([None], "valid actor"),
                                ([target, target], "duplicates")):
        workbench.set_editor_property("targets", targets)
        expect_validation(workbench, False, diagnostic)
    workbench.set_editor_property("targets", [target])

    invalid_parameters = (
        (slm, "resolution_x", 0, "resolutions"),
        (slm, "pixel_pitch_y_um", -1.0, "pixel pitch"),
        (light, "wavelength_nm", 0.0, "wavelength"),
        (light, "amplitude", -1.0, "amplitude"),
        (light, "polarization_angle_deg", float("inf"), "polarization"),
        (camera, "focal_length_mm", 0.0, "focal length"),
        (camera, "f_number", -1.0, "f-number"),
        (camera, "focus_distance_mm", 0.0, "focus distance"),
        (camera, "sensor_width_mm", 0.0, "sensor width"),
        (camera, "output_resolution_y", 0, "output resolutions"),
        (target, "amplitude", -1.0, "amplitude"),
        (target, "initial_phase_rad", float("nan"), "phase"),
        (target, "target_type", unreal.CGHTargetType.MESH, "not implemented"),
    )
    for actor, name, value, diagnostic in invalid_parameters:
        previous = set_parameter(actor, name, value)
        expect_validation(workbench, False, diagnostic)
        set_parameter(actor, name, previous)
    for actor in (slm, camera, light, target):
        actor.set_actor_scale3d(unreal.Vector(2.0, 3.0, 4.0))
        expect_validation(workbench, False, "actor scale")
        if actor == slm:
            vector_near(target.get_optical_position_meters(slm), expected_position,
                        "Reference scale must not distort exported distances")
        actor.set_actor_scale3d(unreal.Vector(1.0, 1.0, 1.0))
    workbench.refresh_visualization()
    expect_validation(workbench, True)
    check(slm.get_editor_property("generation_state") ==
          unreal.CGHGenerationState.NOT_IMPLEMENTED and
          not slm.get_editor_property("has_phase_data"),
          "Refreshing and validating must never fabricate an optical result")
    unreal.log("CGH smoke: coordinate, visualization, preview, and validation checks passed")


def main():
    check(not unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages(),
          "Run in a fresh commandlet; do not discard a user's dirty map")
    assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    classes = inspect_saved_scene(assets, actors, level_editor)
    inspect_temporary_scene(classes, actors)
    unreal.log("CGH_SCAFFOLD_SMOKE_OK (no assets or maps saved)")


if __name__ == "__main__":
    main()
