# CGHSim

**A computer-generated holography (CGH) scene workbench built with Unreal Engine 5.8 and C++.**

CGHSim provides an editable scene for arranging a target, spatial light modulator (SLM), camera, and reconstruction light. Native C++ actors define optical parameters and validation; Blueprint children provide scene presentation.

**Current milestone:** an asynchronous CPU reference solver generates a phase-only SLM pattern for one point target, compensating incident PlaneWave phase under the explicit `exp(+i k r)` propagation convention. The workbench supplies SI scene descriptions; the solver publishes to the existing SLM preview. Optical reconstruction images and sensor simulation remain future work. The camera shows a standard Unreal geometry preview.

## What works today

| Component | Current capabilities |
| --- | --- |
| **Target** | Point or static-mesh targets, configurable contour slicing and point spacing, versioned geometry/point-cloud resources, and a cached debug point-cloud view. |
| **SLM** | Editable resolution/pitch and physical active area, validated transient phase storage, and an exact-resolution grayscale preview on selection. Includes explicit test-ramp and clear controls. |
| **Camera** | Double-precision optical parameters synchronized to a Cine Camera preview, including focal length, aperture, sensor dimensions, and focus distance. |
| **Reconstruction light** | Editable source/optical parameters. PointFocus uses PlaneWave wavelength, initial phase, and direction, with phase referenced to the SLM origin; amplitude/polarization are validated but unused by the scalar phase model. |
| **Workbench** | Explicit actor references, an automatically updated SI scene description, configuration validation, and optional solver command/status controls. |
| **Solver** | CPU PointFocus for exactly one point target, asynchronous jobs, cancellation, rejection of obsolete results, Generate button, and optional automatic updates. A replaceable backend interface reserves Docker/TCP integration for later. |

The checked-in starter level includes the five scene Blueprint actors with connected references, plus separate presentation geometry and lighting. Add the native **CGH Solver Actor** to an existing level and assign its **Workbench**. The asset generator also supports `BP_CGHSolver` and connects it when creating a new map; it preserves existing maps.

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
3. Select **CGH Workbench** to inspect the automatically updated **Scene Description**, or click **Validate Scene** or **Refresh Visualization**.
4. Select the SLM for its phase inset; click **CGH > Phase > Load Stored Phase Pattern** to activate the bundled saved sample, or **Clear Phase Pattern** to remove it. **Generate Preview Phase Ramp** provides another display check. These samples do not run a solver.
5. For a computed point-focus pattern, add **CGH Solver Actor**, set its **Workbench**, and use exactly one **Point** target off the SLM plane with a **PlaneWave** reconstruction light. Click **Generate Phase Pattern** with **CPU / PointFocus** selected; enable **Auto Solve** only if wanted. The [solver guide](Docs/CGH_PointFocus_Solver.md) covers workbench buttons, assumptions, and status.
6. Save the level to preserve parameter and placement changes. Phase data is transient and must be supplied or generated again after reopening.

The SLM is shown at its physical dimensions: the native default `4096 × 4096` pixels at `8 µm` pitch produce an active area of **32.768 × 32.768 mm**. Frame the SLM separately for a close view. Its phase inset scales to a readable size independently of physical pitch; **Camera Preview Size** controls the inset size. For point targets, the sphere is a selection marker rather than the mathematical point's physical extent. Mesh targets use their assigned static mesh and support **Show Point Cloud**.

The checked-in [VS Code workspace](CGHSim.Dev.code-workspace) contains paths for the original development machine. Adjust its project/engine paths and desktop environment configuration before using it elsewhere. The [environment setup notes](Docs/CGHSim_开发进度记录_2026-09-19.md) describe the Remote SSH and Moonlight workflow.

## Project structure

```text
CGHSim/
├── CGHSim.uproject
├── Source/CGHSim/
│   ├── CGHSim.Build.cs
│   └── CGH/
│       ├── Actors/                  # Target, SLM, camera, light, workbench, solver
│       ├── Components/              # Point-cloud rendering and SLM phase preview
│       ├── Solver/                  # Backend interface and CPU PointFocus
│       ├── Types/                   # Optical descriptions, resources, phase pattern, jobs
│       ├── Utils/                   # Unit conversion, mesh sampling, phase grayscale
│       └── Tests/                   # Headless and Slate-render automation
├── Content/CGHSim/
│   ├── Blueprints/                 # Scene Blueprint children; optional generated solver
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
- Phase storage is `PhaseRad[row * ResolutionX + column]`: columns increase toward local **+Y**, rows toward **-Z**. `ResolutionX`/`PixelPitchXM` name the horizontal grid axis (local Y); `ResolutionY`/`PixelPitchYM` name the vertical grid axis (local Z). The [canonical pixel-coordinate rules](Docs/SLM_Pixel_Coordinates.md) define centered pixel positions and the requested +X-side front image (+Y right, +Z up), including the horizontal-mirror requirement for an ordinary Unreal camera on that side.
- Keep SLM, camera and light actor scales at **(1, 1, 1)**. Mesh targets support nonuniform/mirrored scale; their resource coordinates already include it. SLM physical dimensions derive from resolution and pixel pitch.
- Scene-description positions apply the SLM actor's inverse translation and rotation, then convert centimeters to meters. SLM reference scale is ignored. Mesh-target scale is baked into target-local geometry and point resources once.
- Camera parameters drive the Cine Camera preview in one direction. Output resolution is currently stored as configuration data; it does not produce a sensor image.

`FCGHSceneDescription` has `SchemaVersion = 2` and separate read-only SLM, reconstruction-light, target, and camera descriptions. All lengths and positions use meters; phases and polarization use radians. Positions and unit directions use the SLM-local frame, and the camera pose comes from its `OpticalReference` component. Editable actor parameters keep their existing mm, µm, nm, and degree units.

The workbench's transient **Scene Description** updates after editor property changes, undo/redo, transforms, reference changes, and actor deletion. Runtime polling runs in the post-update tick; call **Update Scene Description** when code needs the snapshot immediately after a same-frame parameter write. **Scene Description Complete** means all required actor references are present; use **Validate Scene** separately to check physical validity. Target resources/debug display update automatically. SLM phase publication and resolution changes update its preview; other runtime presentation edits can use **Refresh Visualization**. Scene-description updates alone do not run the solver. Generate explicitly, or enable **Auto Solve** on a linked solver actor; the default is manual.

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

The generator preserves existing Blueprints and maps, verifies compatible native classes, and reloads a newly saved map to check its actor references. It does not reset an existing workbench or add the new solver actor to an existing map. No asset generation was run for the CPU-solver milestone.

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

Run the headless CGH automation tests after building the Editor target:

```bash
"$CGHSIM_UE_ROOT/Engine/Binaries/Linux/UnrealEditor-Cmd" \
  "$CGHSIM_PROJECT_ROOT/CGHSim.uproject" \
  -ExecCmds="Automation RunTests CGH; Quit" \
  -unattended -nullrhi -nosplash
```

### Recorded verification

**2026-09-21 — CPU PointFocus solver:** Editor/Game builds and all **42 headless CGH tests passed**, including plane-wave phase compensation and asynchronous job lifecycle checks. A no-save 4096×4096 solve, cancellation, and existing SLM Blueprint activation also passed; all 11 existing assets/maps were preserved. See the [solver verification record](Docs/CGH_PointFocus_Solver.md#verification-record) for coverage and timings.

**2026-09-21 — mesh targets and SLM phase preview:**

- Editor and Game Linux Development builds passed.
- All 26 headless CGH tests passed, including saved-pattern serialization and activation. A separate earlier Vulkan/Slate test verified the rendered phase grid's grayscale values and row order.
- The bundled 256-by-256 sample asset was saved and reloaded from disk. The existing `BP_CGHSLM` inherited its reference and passed load/clear/reload checks; native default 4096-by-4096 activation also passed.
- Reports, screenshot, usage, and the real-RHI test command are documented in [SLM preview usage and verification](Docs/CGH_Actor_Scaffold.md#slm-phase-data-and-selected-actor-preview). Only the new phase sample asset was saved; all pre-existing assets/maps were preserved.

**2026-09-20 — SI scene description:**

- Editor and Game Linux Development builds passed.
- All four `CGH.SceneDescription` automation tests passed with exit code 0, covering SI conversions, editor/runtime updates, reference changes, and signed coordinates when the SLM normal is reversed.
- The Python smoke checks passed, including automatic snapshot initialization on map load. Current default expectations are 4096-pixel SLM resolution and 500 mm camera focus.
- No Blueprint or map assets were saved. Local build/test logs are listed in the [development handoff](Docs/CGHSim_Development_Handoff.md#3-verification-completed).

**2026-09-19 — actor scaffold:** headless asset creation and map reload passed; repeated asset generation preserved all six Blueprint/map files byte for byte.

Full workbench graphical acceptance, interactive Simulation and undo/redo, cooking, packaging, and standalone deployment remain to be verified. The SLM widget itself has passed the separate render check. Transaction callbacks and game-world ticks are covered by the headless tests.

## Roadmap

1. Complete visual acceptance of the workbench and verify interactive Simulation.
2. Establish a Linux cook/package checkpoint.
3. Extend the CPU PointFocus reference with a numerically checked reconstruction view.
4. Implement the Docker/TCP backend behind the existing solver interface, preserving the explicit propagation convention and job lifecycle.

GS/FFT propagation, camera sensor simulation, GPU solver integration, and runtime parameter controls remain future scope. See the handoff for proposed ordering and acceptance criteria.

## Documentation

- [CPU PointFocus solver](Docs/CGH_PointFocus_Solver.md) — propagation sign, numerical assumptions, asynchronous jobs, controls, and backend contract.
- [SLM pixel coordinates and code audit](Docs/SLM_Pixel_Coordinates.md) — indexing, physical positions, grid-axis names, and canonical front-view versus Unreal camera orientation.
- [Actor scaffold and usage](Docs/CGH_Actor_Scaffold.md) — class responsibilities, units, editor workflow, and scripts.
- [Development handoff and plan](Docs/CGHSim_Development_Handoff.md) — completed work, validation evidence, and next steps.
- [Development progress and environment setup (中文)](Docs/CGHSim_开发进度记录_2026-09-19.md) — environment history, build setup, and remote desktop workflow.
