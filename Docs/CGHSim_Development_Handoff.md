# CGHSim development handoff and plan

Last updated: **2026-09-19**  
Milestone: **Editable CGH actor scaffold and starter workbench**  
Environment verified for this milestone: Ubuntu, installed Unreal Engine **5.8.2**, Linux Development targets.

This is the continuation record for the actor-structure implementation. It records completed work separately from proposed next steps. Detailed usage and repeatable commands are in [CGH_Actor_Scaffold.md](CGH_Actor_Scaffold.md); earlier environment setup and the dated implementation record are in [CGHSim_开发进度记录_2026-09-19.md](CGHSim_开发进度记录_2026-09-19.md).

## 1. Current state

The project contains five native C++ actors, their Blueprint children, shared optical parameter definitions, unit helpers, and a saved workbench map with linked actor references. Parameters can be edited, checked, and used to update scene visualization.

Hologram generation and optical reconstruction are **not implemented**. The camera provides a normal Unreal geometry preview. Scene validation confirms configuration consistency only.

## 2. Work completed

| Area | Implementation |
| --- | --- |
| Target | `ACGHTargetActor`: point amplitude/phase, visual marker with independent radius, and reference-local position export in meters. Mesh sampling remains reserved. |
| SLM | `ACGHSLMActor`: resolution, pixel pitch, derived physical width/height, active-area mesh, normal arrow, and placeholder label. Generation state stays `NotImplemented`; phase data stays unavailable. |
| Camera | `ACGHCameraActor`: double-precision optical parameters, `OpticalReference`, and one-way synchronization into `UCineCameraComponent`, including focus distance conversion. |
| Reconstruction light | `ACGHReconstructionLightActor`: source type, wavelength, amplitude, phase, polarization, and local +X propagation direction. Presentation lighting is separate. |
| Workbench | `ACGHWorkbenchActor`: explicit SLM/camera/light/target references, configuration diagnostics, and Details-panel validation/refresh buttons. |
| Shared definitions | `CGHTypes.h` contains enums and parameter structs; `CGHUnitConversion.h` centralizes unit conversions. `FCGHSceneDescription` is currently a parameter snapshot, without optical poses or complete solver input conversion. |
| Assets | Five real Blueprint `.uasset` files and `L_CGHWorkbench.umap`, with floor and ordinary lighting under `Presentation`. Materials/Meshes directories are reserved; visuals currently use Engine meshes. |
| Project setup | Added the `CinematicCamera` module and editor-only `PythonScriptPlugin`. Editor startup and game default maps both point to the new workbench. |
| Tooling | `Scripts/create_cgh_assets.py` creates missing assets and preserves existing ones. `Scripts/verify_cgh_scaffold.py` checks the saved fixture and exercises behavior in an unsaved temporary world. |

Main locations:

- Native classes: `Source/CGHSim/CGH/Actors/`.
- Shared definitions: `Source/CGHSim/CGH/Types/` and `Source/CGHSim/CGH/Utils/`.
- Blueprint children: `Content/CGHSim/Blueprints/`.
- Starter level: `Content/CGHSim/Maps/L_CGHWorkbench.umap`.
- Startup configuration: `Config/DefaultEngine.ini`.

The implementation is present in the working tree; no Git commit or remote publication was performed during this work.

## 3. Verification completed

| Check | Recorded result |
| --- | --- |
| `CGHSimEditor Linux Development` | Passed, including reflected class generation and linking. |
| `CGHSim Linux Development` | Passed, checking runtime compilation independently of the editor target. |
| Headless asset creation and map reload | Passed with zero errors/warnings; linked references survived serialization. |
| Integration smoke checks | Passed: `CGH_SCAFFOLD_SMOKE_OK`, zero errors. The 24 warnings are expected diagnostics from deliberately invalid test configurations. |
| Repeat asset setup | Passed with zero errors/warnings. SHA-256 comparisons confirmed all six Blueprint/map files were unchanged by verification and repeated setup. |
| Repository whitespace check | `git diff --check` passed. |

The smoke checks cover saved Blueprint ancestry and defaults, SLM physical dimensions, target coordinates after a common rigid transform, marker-size independence, camera preview synchronization, missing/duplicate references, invalid physical values, and non-unit optical actor scales.

Local execution evidence is under the ignored `Saved/Logs/` directory:

- `CGHAssetSetup.log`: successful asset creation and reload.
- `CGHScaffoldChecks.log`: integration checks and expected rejection diagnostics.
- `CGHAssetSetupRepeat.log`: preservation of existing assets on a repeat run.

These were headless checks. Visual appearance in Moonlight, interactive Simulation, cooking, packaging, and standalone application behavior remain unverified for this scene.

## 4. Conventions to preserve

- Unreal positions use centimeters. Optical fields carry explicit unit suffixes; optical scalar parameters use `double`.
- SLM local +X is optical forward, +Y horizontal, +Z vertical. The active area lies in YZ.
- Optical actor scale stays `(1,1,1)`; visual marker/body sizing belongs to child components. SLM active dimensions derive from resolution and pitch.
- Default SLM dimensions are `1024 × 8 um = 8.192 mm` on each side. Its small appearance relative to the target marker is intentional.
- Target export uses the reference actor's origin and rotation, ignores reference scale, and converts cm to m. Workbench validation requires an assigned SLM.
- Optical camera parameters drive the Cine Camera preview, not the reverse. Output image resolution currently remains configuration data only.
- Do not interpret target-marker geometry, ordinary scene lighting, or camera preview pixels as a computed optical result.

The sample target is 50 cm from the camera, while the inherited design's default focus distance is 1000 mm. Choose the intended focus during visual setup; the initial layout is illustrative rather than a calibrated experiment.

## 5. Proposed next steps

The following work is planned, not completed. The next concrete task is **editor visual acceptance**.

| Order | Work | Completion evidence |
| --- | --- | --- |
| 1 | Open the workbench in the graphical editor; inspect actor placement, labels, selection, SLM panel, camera preview, and Details buttons. Change parameters, save, and reopen. | Visible parameter updates and persisted edits; `Validate Scene` reports a valid configuration. |
| 2 | Verify the existing Simulation launch workflow. Decide how the runtime observer should navigate or view the scene, adding a presentation camera/player setup if needed. | Predictable initial viewpoint and usable simulation window through Moonlight. The optical camera should not be assumed to become the runtime view automatically. |
| 3 | Establish a Linux cook/package checkpoint for the small scene. | A packaged build launches with the workbench and assets outside the editor workflow. Record packaging separately from C++ build success. |
| 4 | Define complete scene export from the workbench: SLM-local poses, directions, SI-valued optical parameters, and an explicit schema/version. | Deterministic exports for a known scene, invariant under a common rigid transform, with invalid configurations rejected. No transport dependency is required at this stage. |
| 5 | Agree on a first optical calculation and its assumptions, then implement a minimal numerical reference case. | A documented expected result and numerical comparison, including phase convention, sampling, and units. Algorithm/backend selection remains open. |
| 6 | Add phase/result visualization and explicit job states, then introduce the external solver interface when its data contract is stable. | Results originate from an actual calculation; errors and unavailable data remain visible. Geometry preview and reconstruction output are clearly identified. |

GS/FFT propagation, mesh sampling, sensor simulation, V100/container communication, sockets/shared memory, and a runtime parameter UI remain future scope. Their order should follow the chosen optical case and deployment needs.

## 6. Next-session checklist

1. Review this handoff and the current working tree before editing.
2. Use the existing build/start workflow; rebuild the Editor target after native class changes.
3. Open `/Game/CGHSim/Maps/L_CGHWorkbench`. Select CGH actors in the World Outliner and press **F** to frame them; inspect the physically small SLM separately.
4. Run **Validate Scene** on the workbench, make one parameter change, and verify the update visually.
5. Record the observed result and any issues here. After implementation changes, run relevant checks and update their evidence.

Asset generation deliberately skips an existing map; rerunning it does not reset scene edits. The smoke script expects the starter fixture's defaults, so intentional changes to that fixture need corresponding test expectations or a separate test level. Run the smoke script headlessly because it opens a temporary world.

## 7. Future log entries

For each follow-up milestone, append its date, implemented changes, validation commands/results, remaining issues, and the next concrete task. Keep proposed work in the plan until it has execution evidence.
