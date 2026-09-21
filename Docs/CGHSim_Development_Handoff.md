# CGHSim development handoff and plan

Last updated: **2026-09-21**

Milestone: **CPU complex superposition for multiple Point and Mesh targets**

Environment verified for this milestone: Ubuntu, installed Unreal Engine **5.8.2**, Linux Development targets.

This is the continuation record for the actor scaffold, scene descriptions, target resources, SLM phase preview, and CPU PointFocus solver. It records completed work separately from proposed next steps. The latest multi-target/mesh-cloud verification is tracked separately from earlier single-point and persistence milestones. Detailed usage and repeatable commands are in [CGH_Actor_Scaffold.md](CGH_Actor_Scaffold.md); earlier environment setup and the dated implementation record are in [CGHSim_开发进度记录_2026-09-19.md](CGHSim_开发进度记录_2026-09-19.md).

## 1. Current state

The project contains six native C++ actors, shared optical parameter definitions, unit helpers, and a saved workbench map with the five scene Blueprint actors and linked references. The new solver can be added as a native Actor; no existing map or Blueprint has been saved for this milestone. Parameters can be edited, checked, and used to update scene visualization. The workbench now maintains `FCGHSceneDescription` schema version 2 automatically in the editor and at runtime, with optical poses and SI-valued parameters.

Mesh targets now maintain versioned geometry and sampled contour-point resources, with a point-cloud debug view. Selecting a target reuses its cached resources without building bulk-array Details rows. SLM actors can load the bundled saved `DA_SLMPreviewPattern` through **Load Stored Phase Pattern**, store validated active phase grids, and display their current pattern in a scaled, grayscale editor inset using Unreal's native selected-actor preview.

The CPU **PointFocus** reference now generates a phase-only SLM pattern from one or more Point/Mesh targets. It sums `F(p) = sum_j a_j*exp(i*(phi_j-k*r_j))`, then outputs `phase_slm = wrap(arg(F(p)) - phase_incident(p))` under `exp(+i*k*r)` propagation. Mesh emitters come from the existing sampled point clouds; the algorithm name remains `PointFocus` for settings compatibility. PlaneWave incident phase is `InitialPhaseRad + k*dot(DirectionSLM, pixel_position)` and is referenced to the SLM origin, independently of light position. A solver Actor snapshots the workbench, runs the numerical work asynchronously, and publishes accepted results to the existing SLM preview. Generate is manual by default, with optional automatic updates. Optical reconstruction images and sensor simulation remain unimplemented; the camera provides a normal Unreal geometry preview. See the [solver contract](CGH_PointFocus_Solver.md).

An explicit **Save Phase Pattern** button now persists the current phase as a reusable Unreal DataAsset, exact float64 numerical files with JSON metadata, and a grayscale PNG with one image pixel per SLM pixel. Solver Save requires its `Ready` result to remain the current buffer on the published SLM; SLM Save also accepts manual/preview buffers. Folder settings live on the SLM. Neither Generate nor Auto Solve writes files. Active phase buffers remain transient; saved assets are reactivated through **Stored Phase Pattern** and **Load Stored Phase Pattern**.

## 2. Work completed

| Area | Implementation |
| --- | --- |
| Target | `ACGHTargetActor`: point or static-mesh targets; mesh component, slice count and contour spacing; SLM-relative sampling; shared actor `ResourceId` and matching revisions in description/mesh/point-cloud resources; automatic lifecycle, transform, mesh and parameter updates; cached point-cloud debug rendering. Point targets ignore geometry resources. |
| SLM | `ACGHSLMActor`: resolution/pitch and physical dimensions, plus transient `FCGHSLMPhasePattern` storage, validated publication, revision tracking, clear/test-ramp controls, and `UCGHSLMPreviewComponent`. Selection shows an exact-resolution grayscale texture scaled independently of physical pitch. Starts empty/`NotImplemented`; accepted phase data sets `Ready`. |
| Phase persistence | Explicit editor-only SLM/solver Save buttons; unique matching `.uasset`, little-endian float64 row-major radians `.bin`, UTF-8 metadata `.json`, and exact-resolution 8-bit grayscale `.png`. Defaults are `/Game/CGHSim/PhasePatterns/Generated` and project-relative `Saved/CGHSim/PhasePatterns`. Save preserves the active buffer, phase revision, stored-asset selection, and solver job state. |
| Camera | `ACGHCameraActor`: double-precision optical parameters, one-way Cine Camera preview synchronization, and optical pose export from `OpticalReference`, including its component offset and rotation. |
| Reconstruction light | `ACGHReconstructionLightActor`: source type, wavelength, amplitude, phase, polarization, and local +X propagation direction. Presentation lighting is separate. |
| Workbench | `ACGHWorkbenchActor`: explicit references, transient read-only `SceneDescription`, reference completeness, automatic editor/runtime updates, and validation/visualization controls. Optional `Solver` forwards Solve/Cancel and mirrors status; the solver owns jobs. `UpdateSceneDescription()` allows immediate consumption after same-frame parameter writes. |
| Solver | `ACGHSolverActor`, `UCGHSolverBackend`, and `UCGHCPUSolverBackend`: immutable SI requests with owned mesh point clouds, thread-pool complex Point/Mesh superposition, one running/one latest queued request, cooperative cancellation, stale-result checks, and game-thread publication by move through `SetPhasePattern`. `UCGHDockerSolverBackend` adds asynchronous TCP transport to the independent `Backend/V100` CUDA PointFocus server using CGHV protocol 1.1, with explicit dummy mode retained for transport tests; see [setup and verification](CGH_Docker_Backend.md). |
| Shared definitions | `FCGHSceneDescription` has `SchemaVersion = 2` and dedicated SLM, reconstruction-light, target, and camera descriptions. Snapshot distances use meters, angles use radians, and poses use the SLM actor frame. Editable actor parameters retain their existing units. `FCGHMeshGeometryResource` and `FCGHPointCloudResource` hold target resources; `FCGHObjectPoint` carries point attributes. |
| Assets | The five scene Blueprint `.uasset` files and `L_CGHWorkbench.umap` are preserved. The generator supports a missing `BP_CGHSolver` and connects it in new maps only; it was not executed for this milestone. Add the native solver directly to an existing map. |
| Project setup | `CinematicCamera`, `RenderCore`, and `RHI` support camera/geometry rendering; editor-only `UnrealEd`, `PropertyEditor`, `Slate`, and `SlateCore` support editor hooks, UI, and checks. `PythonScriptPlugin` remains editor-only. Startup maps point to the workbench. |
| Tooling | Editor/Game builds and all 58 headless CGH tests pass for the multi-target/mesh-cloud extension. Independent mixed-scene complex-field checks and cancellation checks pass; all 15 existing assets/maps retain their hashes. Earlier milestones retain independent PNG decoding, fresh-process asset reload, the 4096×4096 single-point solve/cancel, Blueprint activation, and a separate real-RHI Slate render check. Asset creation preserves existing maps. |

Main locations:

- Native classes: `Source/CGHSim/CGH/Actors/`.
- Solver contract/core/backends: `Source/CGHSim/CGH/Types/CGHSolverTypes.h` and `Source/CGHSim/CGH/Solver/`; usage and assumptions: [CGH_PointFocus_Solver.md](CGH_PointFocus_Solver.md).
- Shared definitions: `Source/CGHSim/CGH/Types/` and `Source/CGHSim/CGH/Utils/`.
- Blueprint children: `Content/CGHSim/Blueprints/`.
- Starter level: `Content/CGHSim/Maps/L_CGHWorkbench.umap`.
- Startup configuration: `Config/DefaultEngine.ini`.
- Native point-cloud and SLM preview components: `Source/CGHSim/CGH/Components/`.
- Automation tests: `Source/CGHSim/CGH/Tests/`.
- Phase data/API: `Types/CGHSLMPhasePattern.h`, `Actors/CGHSLMActor.h`, `Utils/CGHPhasePreview.h`, and `Utils/CGHPhasePatternIO.h` under `Source/CGHSim/CGH/`.

The CPU solver changes are present in the working tree. No Git commit or remote publication was performed for this milestone or this record update.

## 3. Verification completed

**Complex multi-target/mesh-cloud extension — 2026-09-21:** Editor/Game Linux Development builds and all **58 headless CGH tests passed**, exit code 0 with zero warnings/failures/not-run. Nine added tests cover complex weighting, mesh transforms/sample ownership, numerical cancellation, resource/input rejection, immutable clouds, job cancellation/publication, stale target/cloud edits, and automatic updates/recovery. The no-save 256×256 mixed-scene smoke check passed, and all **15 existing assets/maps** retained their hashes. See the [solver verification record](CGH_PointFocus_Solver.md#verification-record) for numerical errors, timings, and limits. Earlier test counts and single-point timings below retain their original scope.

The PNG follow-up passed Editor/Game Linux Development builds and all **49 headless CGH tests** on **2026-09-21**, exit code 0 with zero warnings/failures/not-run. Asymmetric 3×2 and 1×1 fixtures check pixel accuracy. Independent Pillow decoding of two 257×129 CPU-generated exports matched all 66,306 pixels, dimensions, single-channel mode, and row order while preserving full-precision raw phase. Interactive save responsiveness and Windows behavior remain unverified.

The explicit-save follow-up passed Editor and Game Linux Development builds and all **48 headless CGH tests** on **2026-09-21**, exit code 0 with zero warnings/failures/not-run. Separate writer and fresh-process reload smoke checks passed, verifying exact numeric bytes, unique repeated saves, and no automatic saving. All 11 original assets/maps retained their hashes; temporary save fixtures were removed. This verifies persistence and its guards, not interactive save responsiveness or Windows behavior. The following records preserve earlier milestones.

The CPU PointFocus milestone passed Editor and Game Linux Development builds and all **42 headless CGH tests** on **2026-09-21** against UE 5.8.2, with exit code 0 and zero test failures/warnings/not-run. A no-save 4096×4096 oblique-plane-wave solve, nonblocking cancellation, and existing SLM Blueprint activation also passed; all 11 saved assets/maps were unchanged. See the [solver verification record](CGH_PointFocus_Solver.md#verification-record) for numerical/lifecycle coverage and single-run timing measurements. The table below retains the earlier stored-phase/preview results.

| Check | Recorded result |
| --- | --- |
| `CGHSimEditor Linux Development` | Passed, exit code 0, including reflected phase types, native preview component, Slate widget and tests. |
| `CGHSim Linux Development` | Passed, exit code 0; editor UI is excluded from the runtime build. |
| Headless `CGH` automation | 26 passed, zero failures/warnings/not-run, exit code 0. Stored-asset serialization, resizing and activation are included. Includes the prior target/scene tests and new phase validation, storage, editor/runtime resolution changes, Details edits, native preview discovery, exact texture bytes and cache reuse. |
| Vulkan/Slate render test | `CGH.SLMPhasePattern.RenderedPreviewPreservesGrayscale` passed, exit code 0. Rendered the actual custom preview widget in an isolated Slate window; all eight cells retained correct row order and gray levels 0, 64, 128 and 255. The captured image was also inspected. |
| Existing SLM Blueprint smoke check | `CGH_SLM_BLUEPRINT_PHASE_OK`, exit code 0. Existing `BP_CGHSLM` inherited the active preview component and supported publication, copied readback, ramp generation, visualization refresh and clearing in an unsaved world. |
| Earlier target milestones | 13 CGH tests passed for mesh resources on 2026-09-20; 14 passed after the selection-performance fix on 2026-09-21. Both milestones passed Editor and Game builds. |
| Earlier scene-description milestone | Four scene-description tests and `CGH_SCAFFOLD_SMOKE_OK` passed on 2026-09-20, including automatic snapshot initialization. |
| Original asset creation/repeat setup | On 2026-09-19, headless creation and reload passed; repeated setup preserved all six Blueprint/map files byte for byte. |
| Repository whitespace check | `git diff --check` passed for the implementation and progress-record changes. |

Current automation groups are `CGH.MeshSampling`, `CGH.TargetResources`, `CGH.SceneDescription`, `CGH.PhasePreview`, `CGH.SLMPhasePattern`, `CGH.PhasePatternAsset`, `CGH.StoredPhasePattern`, `CGH.Solver.PointFocus`, `CGH.SolverActor`, `CGH.PhasePatternSave`, and `CGH.PhaseSaveActor`. Tests cover contour sampling and degenerate geometry, mirrored/nonuniform transforms, resource IDs/revisions and mesh swaps, SLM references, compact Details, signed SI coordinates, atomic phase validation, independent readback, resolution invalidation, demo labels, grayscale mapping and unchanged-selection caching. The rendered test requires a real RHI and is excluded from NullRHI runs.

Local evidence:

- `Saved/Automation/CGHMultiTarget/index.json`: 58 passing headless tests, including multi-target/mesh-cloud coverage.
- `Saved/Automation/CGHMultiTarget/verify_mixed_scene.py` and `smoke_metrics.json`: no-save 256×256 mixed Point/Mesh smoke script and numerical/timing evidence.
- `Saved/Logs/CGHMultiTarget*2026-09-21.log`: Editor/Game builds, tests, and mixed-scene smoke check.
- `Saved/Automation/CGHPhasePng/index.json`: earlier 49 passing headless tests.
- `Saved/Automation/CGHPhasePngSmoke/`: CPU-generated PNG export and independent decoder evidence/scripts.
- `Saved/Logs/CGHPhasePng*2026-09-21.log`: Editor/Game builds, tests, and PNG smoke check.
- `Saved/Automation/CGHPhaseSave/index.json`: earlier 48 passing headless tests.
- `Saved/Automation/CGHPhaseSaveSmoke/`: save/fresh-process reload scripts and manifest; temporary fixture outputs were removed.
- `Saved/Logs/CGHPhaseSave*2026-09-21.log`: Editor/Game builds, full tests, writer and fresh-process reload checks.
- `Saved/Automation/CGHPointFocus/index.json`: earlier 42 passing headless tests; the same folder retains the default-grid smoke script.
- `Saved/Logs/CGHPointFocus*2026-09-21.log`: Editor/Game builds, full tests, and no-save default-grid/cancellation/Blueprint checks.
- `Saved/Automation/CGHStoredPhasePattern/index.json`: earlier 26 passing headless tests.
- `Saved/Logs/CGHStoredPhase*2026-09-21.log`: final stored-sample builds, creation, tests and disk-reload smoke checks.
- `Saved/Automation/CGHSLMPreview/index.json`: earlier 21 passing phase-preview tests.
- `Saved/Automation/CGHSLMPreviewRender/index.json`: one passing rendered test.
- `Saved/Automation/CGHSLMPreview/SLMPhasePreview.png`: captured 480-by-362 preview widget displaying a 4-by-2 phase grid.
- `Saved/Automation/CGHTargetResources/` and `Saved/Automation/CGHTargetSelection/`: prior target reports.
- `Saved/Logs/CGHSLMPreviewEditorBuild_2026-09-21.log` and `CGHSLMPreviewGameBuild_2026-09-21.log`: final builds.
- `Saved/Logs/CGHSLMPreviewTests_2026-09-21.log`, `CGHSLMPreviewRender_2026-09-21.log`, and `CGHSLMPreviewBlueprintSmoke_2026-09-21.log`: latest checks, copied from the session's temporary logs when recording progress.
- Earlier logs in `Saved/Logs/`: `CGHAssetSetup.log`, `CGHScaffoldChecks.log`, `CGHAssetSetupRepeat.log`, and the `CGHSceneDescription*2026-09-20.log` build/test/smoke records.

These are local, ignored evidence files. Repeatable commands are in [CGH_Actor_Scaffold.md](CGH_Actor_Scaffold.md#recreate-assets-and-verify). Run the headless suite with `-ExecCmds="Automation RunTests CGH; Quit"`; use a separate `-RenderOffscreen` run without NullRHI for the rendered test. The stored-sample follow-up saved only the newly created `Content/CGHSim/PhasePatterns/DA_SLMPreviewPattern.uasset`. A fresh-process load passed with the existing Blueprint at 32-by-16 and the native default 4096-by-4096 grid; repeat setup preserved its bytes. Hash comparisons verified all 10 pre-existing assets/maps remained unchanged.

The render check verifies the SLM widget's actual pixels; native selected-actor discovery and selection caching are covered separately by editor automation. Full workbench visual acceptance through Moonlight, interactive Simulation and undo/redo, cooking/packaging, and standalone application behavior still need their own checks. Transaction callbacks and isolated game-world ticks do not establish those results.

## 4. Conventions to preserve

- Unreal positions use centimeters. Editable optical fields carry explicit unit suffixes; optical scalar parameters use `double`. Scene-description lengths and positions are meters, and phases/polarization are radians.
- The authoritative [SLM pixel-coordinate convention](SLM_Pixel_Coordinates.md) defines local +X as the optical normal, +Y as horizontal right in the canonical image, and +Z as up. The active area lies in YZ. Signed depth is `X = dot(WorldPositionCm - SLMOriginCm, SLMNormal) * 0.01`: positive along the normal, negative behind the plane, zero on the plane. Y/Z retain their signs in the rotated actor frame. Reversing the normal reverses X for fixed world-space positions. Rotate the SLM actor to change this frame; its normal arrow is a visualization.
- Preserve public grid-axis names: `ResolutionX` / `PixelPitchXM` (editable `PixelPitchXUm`) describe horizontal columns along local +Y; `ResolutionY` / `PixelPitchYM` (editable `PixelPitchYUm`) describe vertical rows along local Z. Increasing column moves +Y, increasing row moves -Z. Pixel centers in meters are `X = 0`, `Y = (column - (ResolutionX - 1) / 2.0) * PixelPitchXM`, `Z = ((ResolutionY - 1) / 2.0 - row) * PixelPitchYM`; use floating-point division for even resolutions.
- The canonical SLM front image is defined from the +X side toward -X, with +Y displayed right and +Z up. This is a 2D image convention: an ordinary upright Unreal camera at +X looking -X shows +Y left. Such a camera image needs a horizontal mirror to match the canonical image; keep the physical actor frame and optical camera unchanged. The current phase widget draws the stored image directly.
- Keep SLM, camera and light actor scales at `(1,1,1)`. Mesh targets support nonuniform and mirrored actor/component scale. Mesh vertices and sampled points already include that scale in target-local meters; do not apply actor scale a second time when drawing/exporting them. SLM physical dimensions derive from resolution and pitch.
- Current native SLM defaults are `256 × 256` pixels at `8 um`, giving `2.048 × 2.048 mm`; existing Blueprint/level overrides may differ. These user-set defaults and the 500 mm camera focus are preserved. Earlier 4096×4096 results remain historical benchmarks.
- All scene-description positions use the SLM actor origin and rotation and ignore reference scale. Camera position and forward direction come from `OpticalReference`; light propagation follows its actor +X. Directions are unit vectors in the same SLM-local frame.
- PointFocus propagation is explicitly `exp(+i*k*r)`. Sum complex emitter fields `F(p) = sum_j a_j*exp(i*(phi_j-k*r_j))`, then use `phase_slm = wrap(arg(F(p)) - phase_incident(p))`. Never add phase angles. One or more unique Point/Mesh targets are supported; contributing emitters must lie off the SLM plane. Use `/ 2.0` for centered pixels and a phase-only SLM. Only PlaneWave illumination is supported: `phase_incident(p) = InitialPhaseRad + k*dot(DirectionSLM,p)`, with a finite unit direction and phase referenced to the SLM origin; light position is ignored. Light amplitude/polarization are validated but unused; target/sample amplitude sets relative complex weights. No `1/r`, polarization-response or camera model is included. Future FFT/network implementations must preserve or deliberately convert the sign contract.
- Solver worker input is a copied SI snapshot without UObject pointers. Mesh clouds are copied once at launch after ID/revision and count-bound validation; routine capture/polling uses descriptions/revisions. Only the game thread accesses scene resources and publishes accepted phase data. One running and one latest pending request bound the buffers; phase-revision/input/target-list/mesh-revision/reference guards prevent obsolete publication. Target amplitude and mesh transforms/sampling participate in automatic updates. Cancellation is checked within emitter accumulation. Cancel/failure preserves the accepted SLM data, except existing resolution invalidation.
- Optical camera parameters drive the Cine Camera preview, not the reverse. Output image resolution currently remains configuration data only.
- Do not interpret target-marker geometry, ordinary scene lighting, camera preview pixels, or the SLM test ramp as a computed optical result.
- Mesh sampling uses LOD 0 (Nanite fallback render geometry), with slice planes normal to the target-to-SLM direction and spacing along intersection contours. It does not fill slice interiors or model optical occlusion. Cooked sampling requires CPU mesh access and resident LOD 0.
- Description/mesh/point-cloud resources share the target actor's session-local ID and current revision. The solver rejects missing, empty, invalid, or mismatched mesh clouds. Sample amplitude/phase already includes the target values and is used once; mesh positions receive only description rotation and translation. There is no point-count normalization, normal, or UV weighting. Unchanged selection and debug appearance changes do not advance optical revisions. Bulk arrays stay outside Details and undo transactions.
- SLM phase storage is transient, row-major `PhaseRad[row * ResolutionX + column]`, in radians, with `column` in `[0, ResolutionX - 1]`, `row` in `[0, ResolutionY - 1]`, and row zero at the top (+Z edge). Publish through `SetPhasePattern`; C++ reads use `GetPhasePattern`, Blueprint reads use `Get Phase Pattern Copy`. Invalid input preserves the previous valid pattern. Dimensions must match the SLM; resolution changes clear samples, while pitch changes preserve them.
- Save is explicit, editor-only, and synchronous. Both the Unreal asset and raw numerical files use unique matching names; ordinary failure triggers best-effort cleanup of that save's new files. Raw binary preserves exact stored little-endian float64 radians. The separate PNG maps phase to 8-bit grayscale at exactly the SLM resolution, without resampling or gamma transformation. Metadata describes the phase buffer and current pitches; it does not assert target/wavelength provenance. Do not save from generation or automatic-update callbacks, mutate the active buffer/revision, or change **Stored Phase Pattern** during saving.
- The phase preview uses one texel per SLM pixel, nearest-neighbor scaling and a pixel-grid aspect ratio independent of physical pitch. Phase wraps to `[0, 2*pi)` and maps to linear grayscale; the texture is cached by revision/grid and released on clear. The preview component is editor-only; storage/API remain available at runtime.

Editor property/transaction callbacks, actor/component transform observers, and actor destruction refresh the snapshot. The workbench post-update tick catches direct C++/Blueprint parameter writes and reference changes. Call `UpdateSceneDescription()` when consuming a parameter write immediately in the same frame. Target resources/debug visualization update automatically. Phase data follows its setter and dimension checks; call `SynchronizePhasePattern()` for immediate consumption after a same-frame resolution write. Other runtime presentation changes can use `RefreshVisualization()`.

`bSceneDescriptionComplete` means all referenced actors are available in the same world and at least one target is present; it does not certify physical validity. `ValidateScene()` remains explicit. Missing actor descriptions are zeroed, missing target references retain zero-valued array entries, and a missing SLM clears the whole snapshot except `SchemaVersion`. Invalid physical inputs remain available for validation. PointFocus performs its own validation of consumed inputs; camera completeness is not required for this calculation.

## 5. Proposed next steps

The following work remains planned. The next concrete task is **full workbench visual acceptance**, including the new mesh-target and SLM previews.

| Order | Work | Completion evidence |
| --- | --- | --- |
| 1 | Open the workbench in the graphical editor; inspect mesh/point-cloud alignment, responsive target selection, the selected-SLM inset/test-ramp/clear controls, camera preview and Details buttons. Verify resolution versus pitch edits, then save and reopen editable parameters. | Visible parameter updates and persisted editable settings; `Validate Scene` reports a valid configuration. Transient phase/resources are regenerated or republished rather than persisted as map data. |
| 2 | Verify the existing Simulation launch workflow. Decide how the runtime observer should navigate or view the scene, adding a presentation camera/player setup if needed. | Predictable initial viewpoint and usable simulation window through Moonlight. The optical camera should not be assumed to become the runtime view automatically. |
| 3 | Establish a Linux cook/package checkpoint for the small scene. | A packaged build launches with the workbench and assets outside the editor workflow. Record packaging separately from C++ build success. |
| 4 | Add reconstruction-result visualization using the CPU PointFocus pattern as the first reference input. | Propagated fields match independently checked numerical expectations; reconstruction output remains distinct from camera geometry preview. |
| 5 | Extend CUDA/V100 performance and workload coverage, preserving the explicit propagation convention, CPU numerical parity, and job lifecycle. | Protocol, units, errors, cancellation and numerical parity are verified against the CPU reference. |

GS/FFT propagation, sensor simulation, shared memory, and a runtime parameter UI remain future scope. Their order should follow the chosen optical case and deployment needs.

## 6. Next-session checklist

1. Review this handoff and the current working tree before editing.
2. Use the existing build/start workflow; rebuild and restart the editor after native class/component changes.
3. Open `/Game/CGHSim/Maps/L_CGHWorkbench`. Select CGH actors in the World Outliner and press **F** to frame them; inspect the physically small SLM separately.
4. Inspect **Scene Description**, edit a linked actor parameter, and rotate the SLM to verify signed positions update. Run **Validate Scene** separately and use **Refresh Visualization** for runtime presentation changes.
5. On a mesh target, compare **GeometryMesh** and **Show Point Cloud** at the same transform; check repeated selection remains responsive.
6. Select the SLM and click **CGH > Phase > Load Stored Phase Pattern** to load the bundled saved sample. **Generate Preview Phase Ramp** is an additional generated display check. Check the inset, then **Clear Phase Pattern**. Enable **Preview Selected Cameras** if needed; **Camera Preview Size** controls display size. A new/reopened SLM has no phase data until supplied.
7. Add native **CGH Solver Actor**, set **Workbench**, and optionally set the workbench's **Solver** reference. Use one or more distinct Point/Mesh targets, **CPU / PointFocus**, and **Generate Phase Pattern**. Start with a small SLM grid/coarse mesh sampling and check valid point clouds; contributing samples must be off the SLM plane. Check `Ready` and the SLM inset; **Auto Solve** is opt-in. Follow the [solver guide](CGH_PointFocus_Solver.md).
8. At `Ready`, click **Save Phase Pattern** on the solver. Check the asset in `/Game/CGHSim/PhasePatterns/Generated` and matching `.bin`/`.json`/`.png` under project `Saved/CGHSim/PhasePatterns`, or the configured SLM destinations. Clear, select the saved asset in **Stored Phase Pattern**, then **Load Stored Phase Pattern** to check reuse. Use matching resolution/pitch for the original physical plane.
9. Record the observed result and any issues here. After implementation changes, run relevant checks and update their evidence.

Asset generation deliberately skips an existing map; rerunning it does not reset scene edits. The smoke script expects the starter fixture's defaults, so intentional changes to that fixture need corresponding test expectations or a separate test level. Run the smoke script headlessly because it opens a temporary world.

## 7. Milestone log

### 2026-09-20 — SI scene description and signed optical coordinates

Completed schema version 1 with SLM resolution/pitch/active dimensions; reconstruction-light wavelength, amplitude, phase, direction, position, source type and polarization; ordered target positions/amplitudes/phases/types; and camera optical pose, lens, sensor and output-resolution fields. Added automatic workbench updates and reference completeness without changing actor-editing units or introducing a solver.

Confirmed that the existing inverse-rotation conversion already preserves coordinate signs relative to the SLM normal. Added explicit convention documentation and a regression test for front/back positions, the SLM plane, normal reversal, and tilted orientation. No absolute-value conversion or extra sign inversion is applied.

Verification: Editor and Game builds passed, the four automation tests passed, and the Python smoke test passed. Only source, tests, and documentation changed; Blueprint/map assets were not saved. Next concrete task remains graphical acceptance of the workbench, including its automatic snapshot display and signed coordinates; optical algorithm selection and implementation remain future work.

### 2026-09-20 to 2026-09-21 — Mesh resources, point-cloud debug view and selection performance

Advanced the scene schema to version 2 and added static-mesh target resources, target-to-SLM contour slicing, configurable slice count/point spacing, per-point attributes, actor resource identity and synchronized revisions. BeginPlay and editor/runtime changes refresh resources; point targets keep the new resources empty. The debug component renders sampled points in place of the mesh, with scale baked into target-local positions once. Nanite targets use LOD 0 fallback geometry.

The selection slowdown was traced to expanding bulk resource arrays in Details. Those arrays now stay outside Details and undo transactions while compact counts/status remain visible. Selection reuses resource buffers and revisions. The first milestone passed 13 tests and both builds; the selection regression raised the suite to 14 passing tests. Graphical alignment and cooked-runtime mesh access still require their own acceptance checks.

### 2026-09-21 — SLM phase storage and selected-actor preview

Added `FCGHSLMPhasePattern`, validated actor-owned phase publication, revision/copy APIs, resolution invalidation and clear/demo controls. Added an editor-only native selected-actor preview with cached exact-resolution BGRA8 texture, nearest scaling, grayscale over one phase cycle, and visible empty/error/demo states. The explicit ramp is a display aid; a new SLM does not allocate a fabricated full-size phase buffer. Bulk phase data is hidden from Details and cannot be edited through a Blueprint struct reference that bypasses the setter. Clearing releases the cached texture even while unselected.

Verification: both Linux Development builds, all 21 headless tests, the Vulkan/Slate rendered grayscale test, and the existing Blueprint no-save smoke check passed. The screenshot was inspected; phase cell order and grayscale values matched the data. Storage is transient; no solver, optical reconstruction, phase persistence, cook/package result, or full interactive workbench acceptance is claimed. Next steps are the focused manual preview checks above and selection of a first numerical optical case.

### 2026-09-21 — Saved phase sample and editor activation

Added `UCGHPhasePatternAsset` with private serialized phase data, validation, compact metadata and nearest-neighbor resizing. Created `Content/CGHSim/PhasePatterns/DA_SLMPreviewPattern.uasset`: a 256-by-256 diagnostic grid containing horizontal/vertical ramps, rings, checkerboard and a bright top-left marker. SLM actors reference it by default through a soft asset reference; **Load Stored Phase Pattern** explicitly activates it at the current SLM resolution without changing physical settings. **Clear Phase Pattern** clears active data while leaving the saved sample available. Active actor phases remain transient; the sample asset persists across editor restarts. `Scripts/create_cgh_phase_sample.py` creates missing sample data and preserves an existing asset.

Both builds and all 26 headless tests passed. A fresh-process smoke check verified disk persistence, inherited default reference in the existing Blueprint, load/clear/reload, default 4096-by-4096 activation, and byte-preserving repeat setup. All 10 pre-existing assets/maps remained unchanged. The sample is a display test rather than a calculated hologram. After restarting the editor, select the SLM and press the new load button to check it interactively.

### 2026-09-21 — SLM pixel coordinates and code audit

Recorded the authoritative [SLM pixel-coordinate convention](SLM_Pixel_Coordinates.md): row-major storage, columns toward local +Y, rows toward local -Z, centered pixel positions at X = 0, and public X/Y suffixes retained as grid-axis names. Header comments now distinguish pixel indices from the actor's spatial axes. The canonical front image retains the requested +X-side viewpoint with +Y right/+Z up as a 2D display convention; matching it with an upright physical Unreal camera from that side requires a horizontal mirror.

Source inspection found storage, preview upload/drawing, nearest-neighbor resampling, active dimensions and rigid scene-coordinate export compatible with the grid convention. The active mesh width is along local Y and height along local Z. The existing scene camera at +X facing -X is an ordinary geometry preview, so its horizontal orientation differs from the canonical phase image. No pixel-center optical solver or phase-textured world mesh is implemented yet; their future mapping must use the recorded center formula. This update changes records and code comments only, with no runtime behavior or asset changes.

Verification: Editor Linux Development build passed; all **26 headless CGH tests passed**, exit code 0, with zero test failures, warnings or not-run tests. Report: `Saved/Automation/CGHSLMCoordinates/index.json`; build/test logs: `Saved/Logs/CGHSLMCoordinates*2026-09-21.log`. Documentation links and `git diff --check` passed. No new tests were added or real-RHI render check rerun; pixel-center/camera mapping and the symmetric render-fixture gap remain documented on the canonical page.

### 2026-09-21 — CPU PointFocus reference and asynchronous solver jobs

Implemented the single-point phase-only reference with explicit `exp(+i*k*r)` propagation and `wrap(target_phase - k*r - phase_incident)` output. The core uses canonical centered SLM pixels in meters and `double` distances/phases. PlaneWave illumination consumes wavelength, initial phase, and unit direction; its phase reference is the SLM origin, independent of light position. Normal incidence along ±X with zero initial phase reduces to the original formula. PointSource/unknown sources fail. Validation covers finite phase/direction, nonzero unit direction with squared-norm tolerance `1e-6`, finite nonnegative amplitude, finite polarization angle, positive wavelength/pitch/grid, an off-plane point, and phase-only modulation. Signed target X is retained. Amplitude and polarization are validated but unused; no `1/r` weighting, polarization model, reconstruction or sensor image is produced.

Added `ACGHSolverActor` with CPU/Docker backend selection, PointFocus algorithm, `Idle/Queued/Running/Ready/Failed`, Generate/Cancel controls, and manual-by-default optional automatic updates. The abstract backend interface and owned plain-data request/result mailbox separate game-thread scene access from CPU work. Result metadata carries the propagation convention, and mismatches are rejected. The solver permits one running worker and one latest queued request, cooperatively cancels obsolete work, and checks inputs, actor references and SLM phase revision before publication. Accepted output moves through `SetPhasePattern`; cancel/failure leaves the last phase intact. Ending play resets job state; duplicated actors/PIE copies reset backend and job metadata. Auto mode recovers after temporarily missing references and avoids repeated submissions for unchanged invalid values, including NaNs. It responds to light phase/direction/source changes, ignores valid light position/amplitude/polarization changes, and rejects invalid fields again before publication. Preview conversion/upload remains game-thread work and may still affect large-grid frame time.

The workbench optionally links a solver, forwards Solve/Cancel and mirrors status; it does not own jobs. Existing levels use the native solver class with an assigned Workbench. The setup script now supports `BP_CGHSolver` and newly created map references, while preserving existing assets/maps. It has not been run, and no Blueprint or map was saved for this milestone. Docker/TCP was still unimplemented at this historical CPU milestone.

Validation: Editor/Game builds and **42 headless CGH tests passed**, exit code 0 with zero test failures/warnings/not-run. The no-save 4096×4096 oblique-plane-wave solve measured 0.111 ms submission, 0.548120 s worker time and 22.267 ms maximum polling/publication call; cancellation returned in 0.053 ms. Existing `BP_CGHSLM` activation at 32×16 passed. All 11 saved assets/maps were preserved. Report: `Saved/Automation/CGHPointFocus/index.json`; details in the [solver verification record](CGH_PointFocus_Solver.md#verification-record). These headless measurements exclude grayscale/render upload and full GUI responsiveness; interactive Simulation, reconstruction, and packaging remain separate acceptance work.

### 2026-09-21 — Explicit phase-pattern persistence

Added SLM and solver **Save Phase Pattern** buttons after the user requested both a reusable Unreal asset and raw numerical files. `SaveCurrentPhasePattern()` accepts any valid current SLM buffer; `SaveGeneratedPhasePattern()` requires `Ready` and the same SLM/revision as the last accepted solver publication, rejecting busy, cleared or replaced results. The destination folders are configured on the SLM. Each explicit save creates a matching uniquely named `UCGHPhasePatternAsset`, headerless little-endian float64 radians `.bin`, and UTF-8 JSON sidecar. JSON describes grid dimensions, pixel pitches in meters, row/column axes, source label and preview flag; no complete target/wavelength provenance is asserted.

Saving does not modify the current buffer, phase revision, **Stored Phase Pattern**, or solver job state. Generate/Auto Solve never save. Existing outputs are not overwritten; ordinary failure uses best-effort rollback of newly created output files. Save reports paths and status, and uses synchronous editor-only persistence; Game builds return unsupported. Reuse is explicit through the existing stored-asset load control, including its nearest-neighbor resize behavior. See [saving and reloading](CGH_PointFocus_Solver.md#saving-and-reloading-phase-patterns).

Validation: Editor/Game Linux Development builds and **48 headless CGH tests passed**, exit code 0, with zero warnings, failures or not-run tests. The six save tests cover exact export/disk reload, uniqueness, invalid input and directory failures, actor state preservation, publication guards, and explicit-only saving with project-relative paths. A regression check caught and verified the correction for an engine-relative `ProjectDir`; a pre-existing unity-build `TwoPi` name collision in the PointFocus test was also resolved.

Separate writer and fresh-process reload checks passed, confirming exact bytes, unique repeated saves, and no automatic saving. All 11 original assets/maps remained byte-identical, and temporary fixtures were removed. Report: `Saved/Automation/CGHPhaseSave/index.json`; scripts/manifest: `Saved/Automation/CGHPhaseSaveSmoke/`; logs: `Saved/Logs/CGHPhaseSave*2026-09-21.log`. The 42-test records above remain historical CPU milestone evidence. Interactive Save responsiveness, Windows behavior, optical reconstruction and packaging remain separate acceptance work.

### 2026-09-21 — Pixel-accurate grayscale PNG export

The existing Save button also writes a same-name lossless single-channel 8-bit PNG to the raw export directory. Its dimensions exactly match the SLM grid, row zero is at the top, and each phase sample becomes one image pixel without flips, resampling, borders, or gamma transformation. Grayscale matches `PhaseToGray`: `round(255 * wrap_[0,2*pi)(phase_rad) / (2*pi))`. The image quantizes phase to 256 gray levels; the binary and asset retain full precision. `LastSavedPhaseImageFile` exposes the path, and additive version-1 JSON fields describe the PNG filename, format, bit depth, mapping, and row order. One Save action now creates four matching files.

Validation: Editor/Game Linux Development builds and **49 headless CGH tests passed**, exit code 0 with zero warnings/failures/not-run. The added PNG test uses asymmetric 3×2 and 1×1 grids. Independent Pillow decoding matched all 66,306 pixels in two 257×129 native CPU-generated exports, including exact dimensions, grayscale mode `L`, and row order; raw phase retained full precision. Report: `Saved/Automation/CGHPhasePng/index.json`; smoke evidence/scripts: `Saved/Automation/CGHPhasePngSmoke/`; logs: `Saved/Logs/CGHPhasePng*2026-09-21.log`. The previous 48-test record remains historical. Interactive save responsiveness and Windows behavior remain unverified.

### 2026-09-21 — Complex-field superposition for multiple Point and Mesh targets

Expanded the existing CPU `PointFocus` algorithm to accept mixed target lists and sampled mesh point clouds. Every SLM pixel receives the argument of an amplitude-weighted complex field sum, followed by compensation for the incident PlaneWave phase. Point amplitudes now affect relative weights. Mesh points already contain target amplitude/phase and baked scale, so those values are used once and sample positions receive only rigid target rotation/translation. There is no `1/r` weighting, per-mesh point-count normalization, normal/UV response, or reconstruction-intensity guarantee.

Input validation rejects duplicate/missing actors, invalid/mismatched/empty clouds, invalid weights/poses, contributing in-plane emitters, all-zero input, and an aggregate count above 1,000,000 emitters. Zero weights are skipped. Common amplitude scaling prevents overflow; numerically cancelled pixels use deterministic SLM phase zero at a threshold of `32 * double_epsilon * sum(normalized_amplitudes)`. The [solver guide](CGH_PointFocus_Solver.md) gives the exact optical and resource contracts.

Descriptions/references are compared during routine polling; clouds are copied once when launching the worker. Target lists, relative amplitudes, mesh poses and resource revisions now participate in automatic-update and stale-publication checks. Workers remain isolated from UObjects, and dense inner loops check cancellation. Computation grows as pixels times emitters; resource refresh/copying and publication still cost game-thread time. Use small grids/coarse clouds for this CPU reference. Existing Save outputs and the manual/optional-automatic workflow remain available. No Blueprint/map regeneration is required.

Editor/Game Linux Development builds and all **58 headless CGH tests passed**, exit code 0 with zero warnings, failures, or not-run tests. Five new numerical and four new actor/lifecycle tests cover the expanded behavior. All 15 existing assets/maps remained byte-identical. The independent no-save smoke check passed for a 256×256 grid with two Point targets, one Mesh target, and 78 emitters. Its 81 distributed pixel checks had maximum circular phase error 1.8058163e-8 rad. One NullRHI run measured 0.140953 ms submission, 0.298134772 s worker computation, 0.318106 ms maximum completion-poll call, and 0.026991 ms cancellation return. These measurements exclude actual preview rendering and full GUI responsiveness. Evidence is in `Saved/Automation/CGHMultiTarget/` and `Saved/Logs/CGHMultiTarget*2026-09-21.log`. Historical single-point timing and persistence records above remain unchanged.

For each follow-up milestone, append its date, implementation, validation evidence, remaining issues, and next task. Keep proposed work in the plan until it has execution evidence.
