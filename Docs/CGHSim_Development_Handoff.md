# CGHSim development handoff and plan

Last updated: **2026-09-20**

Milestone: **Automatic SI scene description with signed SLM-local coordinates**

Environment verified for this milestone: Ubuntu, installed Unreal Engine **5.8.2**, Linux Development targets.

This is the continuation record for the actor scaffold and scene-description implementation. It records completed work separately from proposed next steps. Detailed usage and repeatable commands are in [CGH_Actor_Scaffold.md](CGH_Actor_Scaffold.md); earlier environment setup and the dated implementation record are in [CGHSim_开发进度记录_2026-09-19.md](CGHSim_开发进度记录_2026-09-19.md).

## 1. Current state

The project contains five native C++ actors, their Blueprint children, shared optical parameter definitions, unit helpers, and a saved workbench map with linked actor references. Parameters can be edited, checked, and used to update scene visualization. The workbench now maintains `FCGHSceneDescription` schema version 1 automatically in the editor and at runtime, with optical poses and SI-valued parameters.

Hologram generation and optical reconstruction are **not implemented**. The camera provides a normal Unreal geometry preview. Scene validation confirms configuration consistency only.

## 2. Work completed

| Area | Implementation |
| --- | --- |
| Target | `ACGHTargetActor`: point amplitude/phase, visual marker with independent radius, and reference-local position export in meters. Mesh sampling remains reserved. |
| SLM | `ACGHSLMActor`: resolution, pixel pitch, derived physical width/height, active-area mesh, normal arrow, and placeholder label. Generation state stays `NotImplemented`; phase data stays unavailable. |
| Camera | `ACGHCameraActor`: double-precision optical parameters, one-way Cine Camera preview synchronization, and optical pose export from `OpticalReference`, including its component offset and rotation. |
| Reconstruction light | `ACGHReconstructionLightActor`: source type, wavelength, amplitude, phase, polarization, and local +X propagation direction. Presentation lighting is separate. |
| Workbench | `ACGHWorkbenchActor`: explicit references, transient read-only `SceneDescription`, reference completeness, automatic editor/runtime updates, and explicit validation/visualization controls. `UpdateSceneDescription()` allows immediate consumption after same-frame parameter writes. |
| Shared definitions | `FCGHSceneDescription` has `SchemaVersion = 1` and dedicated SLM, reconstruction-light, target, and camera descriptions. Snapshot distances use meters, angles use radians, and poses use the SLM actor frame. Editable actor parameters retain their existing units. |
| Assets | Five real Blueprint `.uasset` files and `L_CGHWorkbench.umap`, with floor and ordinary lighting under `Presentation`. Materials/Meshes directories are reserved; visuals currently use Engine meshes. |
| Project setup | Added the `CinematicCamera` module and editor-only `PythonScriptPlugin`. Editor startup and game default maps both point to the new workbench. |
| Tooling | Asset creation remains unchanged. The Python smoke script also checks automatic snapshot initialization on map load. Four `CGH.SceneDescription` C++ automation tests cover SI conversion, signed positions, and editor/runtime updates in temporary worlds. |

Main locations:

- Native classes: `Source/CGHSim/CGH/Actors/`.
- Shared definitions: `Source/CGHSim/CGH/Types/` and `Source/CGHSim/CGH/Utils/`.
- Blueprint children: `Content/CGHSim/Blueprints/`.
- Starter level: `Content/CGHSim/Maps/L_CGHWorkbench.umap`.
- Startup configuration: `Config/DefaultEngine.ini`.
- Scene-description regression tests: `Source/CGHSim/CGH/Tests/CGHSceneDescriptionTests.cpp`.

The implementation is present in the working tree; no Git commit or remote publication was performed during this work.

## 3. Verification completed

The actor-scaffold asset checks below were recorded on 2026-09-19. Builds, snapshot tests, and smoke checks were verified again on 2026-09-20 as noted.

| Check | Recorded result |
| --- | --- |
| `CGHSimEditor Linux Development` | Passed on 2026-09-20 after signed-normal regression coverage was added, including reflected code generation and linking. |
| `CGHSim Linux Development` | Passed on 2026-09-20 for the scene-description implementation, checking runtime compilation without editor-only APIs. |
| Headless asset creation and map reload | Passed with zero errors/warnings; linked references survived serialization. |
| Integration smoke checks | Passed on 2026-09-20: `CGH_SCAFFOLD_SMOKE_OK`, exit code 0. Checks include automatic snapshot initialization from saved references; warnings come from deliberately invalid inputs. |
| Scene-description automation | All four `CGH.SceneDescription` tests passed on 2026-09-20; `TEST COMPLETE. EXIT CODE: 0`. |
| Repeat asset setup | Passed with zero errors/warnings. SHA-256 comparisons confirmed all six Blueprint/map files were unchanged by verification and repeated setup. |
| Repository whitespace check | `git diff --check` passed. |

The smoke checks cover saved Blueprint ancestry and defaults, automatic snapshot initialization, SLM physical dimensions, target coordinates after a common rigid transform, marker-size independence, camera preview synchronization, missing/duplicate references, invalid physical values, and non-unit optical actor scales. Expectations now match the existing 4096-pixel SLM and 500 mm focus defaults; the saved target pose is compared with the optical export rather than a hard-coded starter location.

The C++ automation suite contains:

- `SIUnitsAndOpticalCoordinates`: double-precision conversions, rotated/translated SLM coordinates, scale independence, target order, and the camera optical component pose.
- `RuntimeChangesAndReferences`: actual game-world BeginPlay/ticks, direct public parameter writes, transform changes, reference removal/restoration, and target deletion.
- `EditorChanges`: property notifications, component/actor transforms, transaction notifications, and viewport-only ticks.
- `SignedPositionsFollowSLMNormal`: positive/negative depth, zero depth in the SLM plane, reversed normals with stationary world-space actors, and a tilted SLM. Runs in both game and editor worlds and checks automatic updates without an explicit refresh after SLM rotation.

Local execution evidence is under the ignored `Saved/Logs/` directory:

- `CGHAssetSetup.log`: successful asset creation and reload.
- `CGHScaffoldChecks.log`: integration checks and expected rejection diagnostics.
- `CGHAssetSetupRepeat.log`: preservation of existing assets on a repeat run.
- `CGHSceneDescriptionEditorBuild_2026-09-20.log`: final Editor build.
- `CGHSceneDescriptionGameBuild_2026-09-20.log`: Game build for the scene-description implementation.
- `CGHSceneDescriptionTests_2026-09-20.log`: four passing automation tests and exit code 0.
- `CGHSceneDescriptionSmoke_2026-09-20.log`: passing Python smoke checks.

These are local, ignored evidence files; they are not committed artifacts. Repeatable build and test commands are in [CGH_Actor_Scaffold.md](CGH_Actor_Scaffold.md#recreate-assets-and-verify). The automation command uses `-ExecCmds="Automation RunTests CGH.SceneDescription; Quit"` so its exit status reflects test results.

These were headless checks. Visual appearance in Moonlight, interactive Simulation, interactive undo/redo through the editor UI, cooking, packaging, and standalone application behavior remain unverified for this scene. The automation suite verifies transaction callbacks, not a full interactive undo session.

## 4. Conventions to preserve

- Unreal positions use centimeters. Editable optical fields carry explicit unit suffixes; optical scalar parameters use `double`. Scene-description lengths and positions are meters, and phases/polarization are radians.
- SLM local +X is the optical normal, +Y horizontal, +Z vertical. The active area lies in YZ. Signed depth is `X = dot(WorldPositionCm - SLMOriginCm, SLMNormal) * 0.01`: positive along the normal, negative behind the plane, zero on the plane. Y/Z retain their signs in the rotated actor frame. Reversing the normal reverses X for fixed world-space positions. Rotate the SLM actor to change this frame; its normal arrow is a visualization.
- Optical actor scale stays `(1,1,1)`; visual marker/body sizing belongs to child components. SLM active dimensions derive from resolution and pitch.
- Current native SLM defaults are `4096 × 4096` pixels at `8 um`, giving `32.768 × 32.768 mm`. The existing user edits to these defaults and the 500 mm camera focus were preserved.
- All scene-description positions use the SLM actor origin and rotation and ignore reference scale. Camera position and forward direction come from `OpticalReference`; light propagation follows its actor +X. Directions are unit vectors in the same SLM-local frame.
- Optical camera parameters drive the Cine Camera preview, not the reverse. Output image resolution currently remains configuration data only.
- Do not interpret target-marker geometry, ordinary scene lighting, or camera preview pixels as a computed optical result.

Editor property/transaction callbacks, actor/component transform observers, and actor destruction refresh the snapshot. The workbench post-update tick catches direct C++/Blueprint parameter writes and reference changes. Call `UpdateSceneDescription()` when consuming a parameter write immediately in the same frame. Runtime visualization still requires `RefreshVisualization()`.

`bSceneDescriptionComplete` means all referenced actors are available in the same world and at least one target is present; it does not certify physical validity. `ValidateScene()` remains explicit. Missing actor descriptions are zeroed, missing target references retain zero-valued array entries, and a missing SLM clears the whole snapshot except `SchemaVersion`. Invalid physical inputs remain available for validation and must be checked before future solver use.

## 5. Proposed next steps

The following work is planned, not completed. The next concrete task is **editor visual acceptance**.

| Order | Work | Completion evidence |
| --- | --- | --- |
| 1 | Open the workbench in the graphical editor; inspect actor placement, labels, selection, SLM panel, camera preview, and Details buttons. Change parameters, save, and reopen. | Visible parameter updates and persisted edits; `Validate Scene` reports a valid configuration. |
| 2 | Verify the existing Simulation launch workflow. Decide how the runtime observer should navigate or view the scene, adding a presentation camera/player setup if needed. | Predictable initial viewpoint and usable simulation window through Moonlight. The optical camera should not be assumed to become the runtime view automatically. |
| 3 | Establish a Linux cook/package checkpoint for the small scene. | A packaged build launches with the workbench and assets outside the editor workflow. Record packaging separately from C++ build success. |
| 4 | Agree on a first optical calculation and its assumptions, then implement a minimal numerical reference case. | A documented expected result and numerical comparison, including phase convention, sampling, and units. Algorithm/backend selection remains open. |
| 5 | Add phase/result visualization and explicit job states, then introduce the external solver interface when its data contract is stable. | Results originate from an actual calculation; errors and unavailable data remain visible. Geometry preview and reconstruction output are clearly identified. |

GS/FFT propagation, mesh sampling, sensor simulation, V100/container communication, sockets/shared memory, and a runtime parameter UI remain future scope. Their order should follow the chosen optical case and deployment needs.

## 6. Next-session checklist

1. Review this handoff and the current working tree before editing.
2. Use the existing build/start workflow; rebuild the Editor target after native class changes.
3. Open `/Game/CGHSim/Maps/L_CGHWorkbench`. Select CGH actors in the World Outliner and press **F** to frame them; inspect the physically small SLM separately.
4. Inspect **Scene Description**, edit a linked actor parameter, and rotate the SLM to verify signed positions update. Run **Validate Scene** separately and use **Refresh Visualization** for runtime presentation changes.
5. Record the observed result and any issues here. After implementation changes, run relevant checks and update their evidence.

Asset generation deliberately skips an existing map; rerunning it does not reset scene edits. The smoke script expects the starter fixture's defaults, so intentional changes to that fixture need corresponding test expectations or a separate test level. Run the smoke script headlessly because it opens a temporary world.

## 7. Milestone log

### 2026-09-20 — SI scene description and signed optical coordinates

Completed schema version 1 with SLM resolution/pitch/active dimensions; reconstruction-light wavelength, amplitude, phase, direction, position, source type and polarization; ordered target positions/amplitudes/phases/types; and camera optical pose, lens, sensor and output-resolution fields. Added automatic workbench updates and reference completeness without changing actor-editing units or introducing a solver.

Confirmed that the existing inverse-rotation conversion already preserves coordinate signs relative to the SLM normal. Added explicit convention documentation and a regression test for front/back positions, the SLM plane, normal reversal, and tilted orientation. No absolute-value conversion or extra sign inversion is applied.

Verification: Editor and Game builds passed, the four automation tests passed, and the Python smoke test passed. Only source, tests, and documentation changed; Blueprint/map assets were not saved. Next concrete task remains graphical acceptance of the workbench, including its automatic snapshot display and signed coordinates; optical algorithm selection and implementation remain future work.

For each follow-up milestone, append its date, implementation, validation evidence, remaining issues, and next task. Keep proposed work in the plan until it has execution evidence.
