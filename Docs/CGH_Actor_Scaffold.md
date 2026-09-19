# CGH actor scaffold

This implements the editable scene structure from the [shared design](https://chatgpt.com/share/6aad4cb3-0f30-83e8-9ea8-b099461b757d). Optical generation, propagation, mesh sampling, sensor simulation and external solver communication remain unimplemented.

## Files and responsibilities

| Native class | Blueprint | Role |
| --- | --- | --- |
| `ACGHTargetActor` | `BP_CGHTargetPoint` | Mathematical point parameters and a separate visual marker |
| `ACGHSLMActor` | `BP_CGHSLM` | Pixel resolution/pitch, derived active area and explicit placeholder state |
| `ACGHCameraActor` | `BP_CGHCamera` | Double-precision optical parameters driving a Cine Camera geometry preview |
| `ACGHReconstructionLightActor` | `BP_CGHReconstructionLight` | Wavelength, amplitude, phase, polarization and propagation direction |
| `ACGHWorkbenchActor` | `BP_CGHWorkbench` | Explicit actor references, validation and refresh buttons |

Sources are under `Source/CGHSim/CGH/Actors`. Shared enums and parameter structs are in `Types/CGHTypes.h`; conversions are in `Utils/CGHUnitConversion.h`. `FCGHSceneDescription` is only a parameter snapshot, not a complete solver input: it does not yet contain optical poses or perform SI conversion.

The Blueprints and `Maps/L_CGHWorkbench.umap` live under `Content/CGHSim`. `Materials` and `Meshes` are reserved directories; this first scaffold uses built-in Engine meshes. Blueprint graphs contain no optical algorithm. Native default mesh components can be styled in Blueprint children.

## Editing the scene

The editor startup map and game default map are `Content/CGHSim/Maps/L_CGHWorkbench`. Open that map explicitly if the editor was already running. Select the actors in the World Outliner and press **F** to frame them; headless creation cannot persist a live viewport position. Edit each actor's **CGH** parameters in Details. Construction updates dimensions, camera preview and labels. After runtime Blueprint parameter changes, call **Refresh Visualization** explicitly.

Select **CGH Workbench** to use **Validate Scene** or **Refresh Visualization**. Validation checks references, distinct point targets, finite physical inputs, positive dimensions and unit actor scales. The Details status/messages describe the last explicit validation; validate again after changing a linked actor. Successful validation means the configuration is valid, not that a hologram was computed.

The starter map places the SLM at `(0,0,0)` cm, the target at `(50,0,0)` cm, the camera at `(100,0,0)` cm looking toward the target, and the reconstruction light at `(-30,0,0)` cm pointing along +X. Floor and ordinary UE lighting are in a separate **Presentation** folder. These are illustrative scene positions, not a calibrated optical setup. The default camera focus is 1000 mm as in the design; adjust it for the desired subject.

## Units and coordinates

- Scene positions use Unreal centimeters. Optical data uses explicit mm, um, nm and rad field suffixes; scalar optical parameters are `double`.
- SLM local **+X** is optical forward/normal, **+Y** horizontal, **+Z** vertical. Its active plane is YZ.
- Default `1024 x 1024` pixels at `8 um` give `8.192 x 8.192 mm`, or `0.8192 x 0.8192 cm`. The active mesh is physically small; frame the SLM separately for close inspection.
- Keep optical actor scales at `(1,1,1)`. Style child meshes instead. The SLM child scale is derived from its parameters; do not use it as an independent optical setting.
- `GetOpticalPositionMeters(SLM)` subtracts the SLM origin, applies its inverse rotation and converts cm to m. It intentionally ignores reference scale. Passing null returns world position in meters; the workbench requires an assigned SLM.
- Target `MarkerRadiusCm` affects only the visual sphere, never the mathematical point. Mesh targets are reserved and currently rejected by validation.
- Light +X is its propagation direction. The reconstruction-light actor contains no UE illumination component.
- Camera optical parameters flow one way into `PreviewCamera`. `OpticalReference` identifies its future optical pose. Output resolution is independent of both filmback aspect and SLM resolution, and is currently stored only.
- SLM `GenerationState` remains `NotImplemented`, `HasPhaseData` remains false, and the label identifies a placeholder generator.

## Recreate assets and verify

`CinematicCamera` is a runtime module dependency. The editor-only `PythonScriptPlugin` enables the scripts below; no solver or GPU module is required.

From the project root, build:

```bash
/bin/bash /home/cxy/opt/UnrealEngine/UE5/Engine/Build/BatchFiles/Linux/Build.sh \
  CGHSimEditor Linux Development \
  "-Project=$PWD/CGHSim.uproject" -WaitMutex
```

Create missing assets:

```bash
/home/cxy/opt/UnrealEngine/UE5/Engine/Binaries/Linux/UnrealEditor-Cmd \
  "$PWD/CGHSim.uproject" -run=pythonscript \
  "-script=$PWD/Scripts/create_cgh_assets.py" -unattended -nullrhi -nosplash
```

The generator preserves existing Blueprints/maps and rejects conflicting Blueprint parents. It saves a new map only after populating and validating it, then reloads it to check references. It never updates an existing map, so subsequent scene edits remain yours. In an interactive editor it refuses to replace an unsaved world. Assets can also be created through **Tools > Execute Python Script** after building the native classes.

Run the integration checks:

```bash
/home/cxy/opt/UnrealEngine/UE5/Engine/Binaries/Linux/UnrealEditor-Cmd \
  "$PWD/CGHSim.uproject" -run=pythonscript \
  "-script=$PWD/Scripts/verify_cgh_scaffold.py" -unattended -nullrhi -nosplash
```

The check script verifies the initial saved fixture and exercises optical conversions and invalid inputs in an unsaved temporary world. Run it headlessly; it does not write changes to assets. Since the saved map check expects the starter defaults, intentional later changes to that fixture may require updating those expectations. Headless checks do not assess viewport appearance or packaged rendering.

## Verified in this workspace

Both `CGHSimEditor Linux Development` and `CGHSim Linux Development` build successfully against installed UE 5.8.2. Asset creation and map reload completed with zero errors/warnings. The integration script returned `CGH_SCAFFOLD_SMOKE_OK` with zero errors; its 24 warnings are expected rejection diagnostics for intentionally invalid configurations. No graphical inspection, cook, packaging or optical calculation is claimed.
