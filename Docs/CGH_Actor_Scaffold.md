# CGH actor scaffold

This implements the editable scene structure from the [shared design](https://chatgpt.com/share/6aad4cb3-0f30-83e8-9ea8-b099461b757d). Optical generation, propagation, mesh sampling, sensor simulation and external solver communication remain unimplemented.

## Files and responsibilities

| Native class | Blueprint | Role |
| --- | --- | --- |
| `ACGHTargetActor` | `BP_CGHTargetPoint` | Mathematical point parameters and a separate visual marker |
| `ACGHSLMActor` | `BP_CGHSLM` | Pixel resolution/pitch, derived active area and explicit placeholder state |
| `ACGHCameraActor` | `BP_CGHCamera` | Double-precision optical parameters driving a Cine Camera geometry preview |
| `ACGHReconstructionLightActor` | `BP_CGHReconstructionLight` | Wavelength, amplitude, phase, polarization and propagation direction |
| `ACGHWorkbenchActor` | `BP_CGHWorkbench` | Explicit actor references, automatic SI snapshot updates, validation and refresh buttons |

Sources are under `Source/CGHSim/CGH/Actors`. Shared enums, editable parameter structs, and read-only description structs are in `Types/CGHTypes.h`; conversions are in `Utils/CGHUnitConversion.h`. `FCGHSceneDescription` exports optical poses and parameters in SI units with `SchemaVersion = 1`.

The Blueprints and `Maps/L_CGHWorkbench.umap` live under `Content/CGHSim`. `Materials` and `Meshes` are reserved directories; this first scaffold uses built-in Engine meshes. Blueprint graphs contain no optical algorithm. Native default mesh components can be styled in Blueprint children.

## Editing the scene

The editor startup map and game default map are `Content/CGHSim/Maps/L_CGHWorkbench`. Open that map explicitly if the editor was already running. Select the actors in the World Outliner and press **F** to frame them; headless creation cannot persist a live viewport position. Edit each actor's **CGH** parameters in Details. Construction updates dimensions, camera preview and labels. After runtime Blueprint parameter changes, call **Refresh Visualization** explicitly.

Select **CGH Workbench** to inspect **Scene Description**, or use **Validate Scene** or **Refresh Visualization**. The snapshot updates automatically; validation remains explicit. Validation checks references, distinct point targets, finite physical inputs, positive dimensions and unit actor scales. The Details status/messages describe the last explicit validation; validate again after changing a linked actor. Successful validation means the configuration is valid, not that a hologram was computed.

The starter map places the SLM at `(0,0,0)` cm, the target at `(50,0,0)` cm, the camera at `(100,0,0)` cm looking toward the target, and the reconstruction light at `(-30,0,0)` cm pointing along +X. Floor and ordinary UE lighting are in a separate **Presentation** folder. These are illustrative scene positions, not a calibrated optical setup. The native default camera focus is 500 mm; adjust it for the desired subject.

## Units and coordinates

- Scene positions use Unreal centimeters. Editable optical parameters use explicit mm, um, nm, rad, and degree field suffixes; scalar optical parameters are `double`. Scene-description lengths use meters and angles use radians.
- SLM local **+X** is optical forward/normal, **+Y** horizontal, **+Z** vertical. Its active plane is YZ.
- Default `4096 x 4096` pixels at `8 um` give `32.768 x 32.768 mm`, or `3.2768 x 3.2768 cm`. The active mesh is physically small; frame the SLM separately for close inspection.
- Keep optical actor scales at `(1,1,1)`. Style child meshes instead. The SLM child scale is derived from its parameters; do not use it as an independent optical setting.
- `GetOpticalPositionMeters(SLM)` subtracts the SLM origin, applies its inverse rotation and converts cm to m. It intentionally ignores reference scale. Passing null returns world position in meters; the workbench requires an assigned SLM.
- Target `MarkerRadiusCm` affects only the visual sphere, never the mathematical point. Mesh targets are reserved and currently rejected by validation.
- Light +X is its propagation direction. The reconstruction-light actor contains no UE illumination component.
- Camera optical parameters flow one way into `PreviewCamera`. `OpticalReference` supplies the exported optical position and forward direction, including component offsets. Output resolution is independent of both filmback aspect and SLM resolution, and is currently stored only.
- SLM `GenerationState` remains `NotImplemented`, `HasPhaseData` remains false, and the label identifies a placeholder generator.

## Scene description and updates

The workbench exposes a transient, Blueprint-read-only `SceneDescription`. Its schema contains:

| Block | Fields |
| --- | --- |
| Root | `SchemaVersion = 1` |
| `SLM` | `ResolutionX/Y`, `PixelPitchXM/YM`, `ActiveWidthM`, `ActiveHeightM`, `ModulationType` |
| `ReconstructionLight` | `WavelengthM`, `Amplitude`, `InitialPhaseRad`, `DirectionSLM`, `SourceType`, `PositionSLMM`, `PolarizationAngleRad` |
| `Targets[]` | `PositionSLMM`, `Amplitude`, `PhaseRad`, `TargetType` |
| `Camera` | `OpticalPositionSLMM`, `ForwardDirectionSLM`, `FocalLengthM`, `FNumber`, `FocusDistanceM`, `SensorWidthM`, `SensorHeightM`, `OutputResolutionX/Y` |

All positions are relative to the assigned SLM actor's origin and rotation. Export ignores SLM scale so distances retain their physical size; forward and propagation vectors are unit directions in the same frame. Active width and height come from resolution multiplied by pixel pitch.

Position coordinates retain their signs. With the actor's local +X as its optical normal, `X = dot(WorldPositionCm - SLMOriginCm, SLMNormal) * 0.01`: points on the normal side have positive X, points on the opposite side have negative X, and points in the SLM plane have zero X. Y and Z are signed projections onto the actor's local horizontal and vertical axes. Rotating the SLM updates this entire frame; reversing its normal swaps the sign of X for fixed world-space points. The `OpticalNormal` arrow visualizes the actor's +X axis; rotate the SLM actor to change the optical frame.

Editor property and transaction notifications, transform observers, and runtime post-update polling keep the snapshot current when linked actors, their references, or the camera optical component change. Undo/redo and actor deletion also refresh it. C++ and Blueprint code that writes public parameters and immediately consumes the snapshot in the same frame should call `UpdateSceneDescription()` first.

`bSceneDescriptionComplete` reports whether all required references are present, including at least one target. It does not certify physical validity; call `ValidateScene()` for that. Missing actors produce zero-valued description blocks, and missing targets retain their array indices as zero-valued entries. Without an SLM, the entire description is empty/zero-valued except for its schema version. Updates do not run validation, optical calculations, or runtime visualization refresh.

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

Run the C++ scene-description automation tests after building:

```bash
/home/cxy/opt/UnrealEngine/UE5/Engine/Binaries/Linux/UnrealEditor-Cmd \
  "$PWD/CGHSim.uproject" \
  -ExecCmds="Automation RunTests CGH.SceneDescription; Quit" \
  -unattended -nullrhi -nosplash
```

## Verified in this workspace

On **2026-09-20**, the SI scene-description implementation passed `CGHSimEditor Linux Development` and `CGHSim Linux Development` builds against installed UE 5.8.2. The final Editor build and all four `CGH.SceneDescription` automation tests passed after the signed-normal regression was added. Tests cover SI conversion, optical camera offsets, editor notifications, runtime writes and reference changes, and positive/negative/zero positions when the SLM normal changes. The test runner exited with code 0.

The Python integration script passed with `CGH_SCAFFOLD_SMOKE_OK` and exit code 0, including automatic snapshot initialization on map load. Its expectations were aligned with the existing 4096-pixel SLM and 500 mm focus defaults; saved target position checks use the actual optical pose. No assets or maps were saved during these checks.

Asset creation, map reload, and byte-for-byte preservation on repeated setup were verified on **2026-09-19**. Local execution logs are listed in the [development handoff](CGHSim_Development_Handoff.md#3-verification-completed). Graphical acceptance, interactive Simulation and undo/redo, cook/packaging, and optical calculations remain unverified or unimplemented; transaction callbacks are covered by automation.
