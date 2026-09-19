# CGHSim

**A computer-generated holography (CGH) scene workbench built with Unreal Engine 5.8 and C++.**

CGHSim provides an editable scene for arranging a target, spatial light modulator (SLM), camera, and reconstruction light. Native C++ actors define optical parameters and validation; Blueprint children provide scene presentation.

**Current milestone:** actor scaffolding and a saved workbench scene. Hologram generation, wave propagation, and optical reconstruction are not yet implemented. The camera currently shows a standard Unreal geometry preview.

## What works today

| Component | Current capabilities |
| --- | --- |
| **Target** | Mathematical point parameters, an independently sized visual marker, and position export in SLM-local meters. |
| **SLM** | Editable pixel resolution and pitch, derived physical active area, a normal arrow, and explicit `NotImplemented` generation status. |
| **Camera** | Double-precision optical parameters synchronized to a Cine Camera preview, including focal length, aperture, sensor dimensions, and focus distance. |
| **Reconstruction light** | Source type, wavelength, amplitude, phase, polarization, and propagation direction. |
| **Workbench** | Explicit actor references, configuration validation, and visualization refresh controls in the editor. |

The starter level includes all five Blueprint actors with connected references, plus separate presentation geometry and lighting. Python scripts create missing assets and verify the scaffold.

## Requirements

- **Unreal Engine 5.8** with a working C++ build environment. The current implementation was built and checked with **UE 5.8.2 on Ubuntu/Linux**.
- **Git LFS** for Unreal `.uasset` and `.umap` files, as configured in [.gitattributes](.gitattributes).
- A graphical desktop session for interactive editor use. Asset generation and integration checks can run headlessly.

The project enables the editor-only **PythonScriptPlugin** and depends on the **CinematicCamera** runtime module. The scaffold does not require an external optical solver or CUDA integration. Other platforms have not been verified.

## Quick start on Linux

Clone this repository, open a terminal in its root directory, and retrieve the Unreal assets:

```bash
git lfs install
git lfs pull
```

Set your engine path and build the Editor target. Replace the example path below with your Unreal Engine installation; use the same shell for the following commands.

```bash
CGHSIM_UE_ROOT="/path/to/UnrealEngine/UE5"
CGHSIM_PROJECT_ROOT="$PWD"

bash "$CGHSIM_UE_ROOT/Engine/Build/BatchFiles/Linux/Build.sh" \
  CGHSimEditor Linux Development \
  "-Project=$CGHSIM_PROJECT_ROOT/CGHSim.uproject" \
  -WaitMutex
```

Launch the editor from a graphical desktop session:

```bash
"$CGHSIM_UE_ROOT/Engine/Binaries/Linux/UnrealEditor" \
  "$CGHSIM_PROJECT_ROOT/CGHSim.uproject" -log
```

The editor startup map and game default map are both set to:

```text
/Game/CGHSim/Maps/L_CGHWorkbench
```

In the editor:

1. Select the CGH actors in the World Outliner and press **F** to frame them.
2. Edit each actor's optical parameters in the **CGH** sections of the Details panel.
3. Select **CGH Workbench** and click **Validate Scene** or **Refresh Visualization**.
4. Save the level to preserve parameter and placement changes.

The SLM is shown at its physical dimensions: the default `1024 × 1024` pixels at `8 µm` pitch produce an active area of **8.192 × 8.192 mm**. Frame the SLM separately for a close view. The target sphere is a selection marker, not the physical extent of the mathematical point.

The checked-in [VS Code workspace](CGHSim.Dev.code-workspace) contains paths for the original development machine. Adjust its project/engine paths and desktop environment configuration before using it elsewhere. The [environment setup notes](Docs/CGHSim_开发进度记录_2026-09-19.md) describe the Remote SSH and Moonlight workflow.

## Project structure

```text
CGHSim/
├── CGHSim.uproject
├── Source/CGHSim/
│   ├── CGHSim.Build.cs
│   └── CGH/
│       ├── Actors/                  # Target, SLM, camera, light, workbench
│       ├── Types/CGHTypes.h         # Reflected enums and optical parameters
│       └── Utils/CGHUnitConversion.h
├── Content/CGHSim/
│   ├── Blueprints/                 # Five Blueprint children
│   ├── Materials/                  # Reserved for custom materials
│   ├── Meshes/                     # Reserved for custom meshes
│   └── Maps/L_CGHWorkbench.umap
├── Config/
├── Scripts/
│   ├── create_cgh_assets.py
│   └── verify_cgh_scaffold.py
└── Docs/
```

## Coordinates and data model

- Unreal scene positions use **centimeters**; optical fields have explicit unit suffixes, and optical scalar parameters use `double`.
- SLM local **+X** is optical forward, **+Y** is horizontal, and **+Z** is vertical. The active area lies in the local **YZ plane**.
- Keep optical actor scales at **(1, 1, 1)**. Resize visual child components instead; SLM active dimensions are driven by resolution and pixel pitch.
- Target position export applies the reference actor's inverse translation and rotation, then converts centimeters to meters. Reference scale is ignored and non-unit actor scales are rejected by workbench validation.
- Camera parameters drive the Cine Camera preview in one direction. Output resolution is currently stored as configuration data; it does not produce a sensor image.

`FCGHSceneDescription` is currently a parameter snapshot. Complete optical pose export, a solver data contract, and solver communication remain future work. Successful scene validation does not indicate that a hologram has been calculated.

## Asset generation and checks

Use the engine/project variables from Quick start and build `CGHSimEditor` before running either script. These scripts run inside Unreal, not system Python.

The Blueprint assets and starter map are included in the project. To create missing assets:

```bash
"$CGHSIM_UE_ROOT/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$CGHSIM_PROJECT_ROOT/CGHSim.uproject" \
  -run=pythonscript \
  "-script=$CGHSIM_PROJECT_ROOT/Scripts/create_cgh_assets.py" \
  -unattended -nullrhi -nosplash
```

The generator preserves existing Blueprints and maps, verifies compatible native classes, and reloads a newly saved map to check its actor references. It does not reset an existing workbench.

Run the integration checks:

```bash
"$CGHSIM_UE_ROOT/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$CGHSIM_PROJECT_ROOT/CGHSim.uproject" \
  -run=pythonscript \
  "-script=$CGHSIM_PROJECT_ROOT/Scripts/verify_cgh_scaffold.py" \
  -unattended -nullrhi -nosplash
```

A successful run logs `CGH_SCAFFOLD_SMOKE_OK`. Checks cover saved references, parameter defaults, coordinate invariance, SLM dimensions, camera synchronization, and rejection of invalid configurations. Warnings from deliberately invalid test inputs are expected. Mutation checks use an unsaved temporary world and do not write assets.

The saved-scene checks expect the starter defaults. If you intentionally modify that fixture, update the expectations or maintain a separate test map.

### Recorded verification

As of **2026-09-19**:

- Editor and Game Linux Development builds passed.
- Headless asset creation, map reload, and integration checks passed.
- Repeated asset generation preserved all six Blueprint/map files byte for byte.

Graphical acceptance of this scene, interactive Simulation, cooking, packaging, and standalone deployment remain to be verified.

## Roadmap

1. Complete visual acceptance of the workbench and verify interactive Simulation.
2. Establish a Linux cook/package checkpoint.
3. Export a complete scene description with optical poses and SI units.
4. Choose a first optical calculation and implement a verified numerical reference case.
5. Add phase/result visualization and an external solver interface.

GS/FFT propagation, mesh sampling, camera sensor simulation, GPU solver integration, and runtime parameter controls remain future scope. See the handoff for proposed ordering and acceptance criteria.

## Documentation

- [Actor scaffold and usage](Docs/CGH_Actor_Scaffold.md) — class responsibilities, units, editor workflow, and scripts.
- [Development handoff and plan](Docs/CGHSim_Development_Handoff.md) — completed work, validation evidence, and next steps.
- [Development progress and environment setup (中文)](Docs/CGHSim_开发进度记录_2026-09-19.md) — environment history, build setup, and remote desktop workflow.
