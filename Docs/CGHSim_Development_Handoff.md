# CGHSim development handoff and plan

Last updated: **2026-09-21**

Milestone: **Versioned mesh targets, responsive point-cloud selection, and SLM phase preview**

Environment verified for this milestone: Ubuntu, installed Unreal Engine **5.8.2**, Linux Development targets.

This is the continuation record for the actor scaffold, scene descriptions, target resources, and SLM phase preview. It records completed work separately from proposed next steps. Detailed usage and repeatable commands are in [CGH_Actor_Scaffold.md](CGH_Actor_Scaffold.md); earlier environment setup and the dated implementation record are in [CGHSim_开发进度记录_2026-09-19.md](CGHSim_开发进度记录_2026-09-19.md).

## 1. Current state

The project contains five native C++ actors, their Blueprint children, shared optical parameter definitions, unit helpers, and a saved workbench map with linked actor references. Parameters can be edited, checked, and used to update scene visualization. The workbench now maintains `FCGHSceneDescription` schema version 2 automatically in the editor and at runtime, with optical poses and SI-valued parameters.

Mesh targets now maintain versioned geometry and sampled contour-point resources, with a point-cloud debug view. Selecting a target reuses its cached resources without building bulk-array Details rows. SLM actors can load the bundled saved `DA_SLMPreviewPattern` through **Load Stored Phase Pattern**, store validated active phase grids, and display their current pattern in a scaled, grayscale editor inset using Unreal's native selected-actor preview.

Hologram generation and optical reconstruction are **not implemented**. The camera provides a normal Unreal geometry preview. The SLM preview displays supplied phase samples or an explicitly generated test ramp. Scene validation confirms configuration consistency only.

## 2. Work completed

| Area | Implementation |
| --- | --- |
| Target | `ACGHTargetActor`: point or static-mesh targets; mesh component, slice count and contour spacing; SLM-relative sampling; shared actor `ResourceId` and matching revisions in description/mesh/point-cloud resources; automatic lifecycle, transform, mesh and parameter updates; cached point-cloud debug rendering. Point targets ignore geometry resources. |
| SLM | `ACGHSLMActor`: resolution/pitch and physical dimensions, plus transient `FCGHSLMPhasePattern` storage, validated publication, revision tracking, clear/test-ramp controls, and `UCGHSLMPreviewComponent`. Selection shows an exact-resolution grayscale texture scaled independently of physical pitch. Starts empty/`NotImplemented`; accepted phase data sets `Ready`. |
| Camera | `ACGHCameraActor`: double-precision optical parameters, one-way Cine Camera preview synchronization, and optical pose export from `OpticalReference`, including its component offset and rotation. |
| Reconstruction light | `ACGHReconstructionLightActor`: source type, wavelength, amplitude, phase, polarization, and local +X propagation direction. Presentation lighting is separate. |
| Workbench | `ACGHWorkbenchActor`: explicit references, transient read-only `SceneDescription`, reference completeness, automatic editor/runtime updates, and explicit validation/visualization controls. `UpdateSceneDescription()` allows immediate consumption after same-frame parameter writes. |
| Shared definitions | `FCGHSceneDescription` has `SchemaVersion = 2` and dedicated SLM, reconstruction-light, target, and camera descriptions. Snapshot distances use meters, angles use radians, and poses use the SLM actor frame. Editable actor parameters retain their existing units. `FCGHMeshGeometryResource` and `FCGHPointCloudResource` hold target resources; `FCGHObjectPoint` carries point attributes. |
| Assets | Five real Blueprint `.uasset` files and `L_CGHWorkbench.umap`, with floor and ordinary lighting under `Presentation`. Starter visuals use Engine meshes; mesh targets also accept assigned static-mesh assets. |
| Project setup | `CinematicCamera`, `RenderCore`, and `RHI` support camera/geometry rendering; editor-only `UnrealEd`, `PropertyEditor`, `Slate`, and `SlateCore` support editor hooks, UI, and checks. `PythonScriptPlugin` remains editor-only. Startup maps point to the workbench. |
| Tooling | Asset creation preserves existing maps. The C++ suite now has 26 headless tests covering scene descriptions, mesh sampling, resource lifecycle/selection, and phase storage/preview caching, plus one real-RHI Slate render test. The existing SLM Blueprint also passed a no-save phase API/component smoke check. |

Main locations:

- Native classes: `Source/CGHSim/CGH/Actors/`.
- Shared definitions: `Source/CGHSim/CGH/Types/` and `Source/CGHSim/CGH/Utils/`.
- Blueprint children: `Content/CGHSim/Blueprints/`.
- Starter level: `Content/CGHSim/Maps/L_CGHWorkbench.umap`.
- Startup configuration: `Config/DefaultEngine.ini`.
- Native point-cloud and SLM preview components: `Source/CGHSim/CGH/Components/`.
- Automation tests: `Source/CGHSim/CGH/Tests/`.
- Phase data/API: `Types/CGHSLMPhasePattern.h`, `Actors/CGHSLMActor.h`, and `Utils/CGHPhasePreview.h` under `Source/CGHSim/CGH/`.

The SLM preview changes are present in the working tree. No Git commit or remote publication was performed for this milestone or this record update.

## 3. Verification completed

The latest checks below were completed on **2026-09-21** against UE 5.8.2. Earlier scaffold and scene-description evidence is retained separately.

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

Current automation groups are `CGH.MeshSampling`, `CGH.TargetResources`, `CGH.SceneDescription`, `CGH.PhasePreview`, `CGH.SLMPhasePattern`, `CGH.PhasePatternAsset`, and `CGH.StoredPhasePattern`. Tests cover contour sampling and degenerate geometry, mirrored/nonuniform transforms, resource IDs/revisions and mesh swaps, SLM references, compact Details, signed SI coordinates, atomic phase validation, independent readback, resolution invalidation, demo labels, grayscale mapping and unchanged-selection caching. The rendered test requires a real RHI and is excluded from NullRHI runs.

Local evidence:

- `Saved/Automation/CGHStoredPhasePattern/index.json`: latest 26 passing headless tests.
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
- SLM local +X is the optical normal, +Y horizontal, +Z vertical. The active area lies in YZ. Signed depth is `X = dot(WorldPositionCm - SLMOriginCm, SLMNormal) * 0.01`: positive along the normal, negative behind the plane, zero on the plane. Y/Z retain their signs in the rotated actor frame. Reversing the normal reverses X for fixed world-space positions. Rotate the SLM actor to change this frame; its normal arrow is a visualization.
- Keep SLM, camera and light actor scales at `(1,1,1)`. Mesh targets support nonuniform and mirrored actor/component scale. Mesh vertices and sampled points already include that scale in target-local meters; do not apply actor scale a second time when drawing/exporting them. SLM physical dimensions derive from resolution and pitch.
- Current native SLM defaults are `4096 × 4096` pixels at `8 um`, giving `32.768 × 32.768 mm`. The existing user edits to these defaults and the 500 mm camera focus were preserved.
- All scene-description positions use the SLM actor origin and rotation and ignore reference scale. Camera position and forward direction come from `OpticalReference`; light propagation follows its actor +X. Directions are unit vectors in the same SLM-local frame.
- Optical camera parameters drive the Cine Camera preview, not the reverse. Output image resolution currently remains configuration data only.
- Do not interpret target-marker geometry, ordinary scene lighting, camera preview pixels, or the SLM test ramp as a computed optical result.
- Mesh sampling uses LOD 0 (Nanite fallback render geometry), with slice planes normal to the target-to-SLM direction and spacing along intersection contours. It does not fill slice interiors or model optical occlusion. Cooked sampling requires CPU mesh access and resident LOD 0.
- Description/mesh/point-cloud resources share the target actor's session-local ID and current revision. Unchanged selection and debug appearance changes do not advance optical revisions. Bulk arrays stay outside Details and undo transactions.
- SLM phase storage is transient, row-major `PhaseRad[Y * ResolutionX + X]`, in radians, with row zero at the top. Publish through `SetPhasePattern`; C++ reads use `GetPhasePattern`, Blueprint reads use `Get Phase Pattern Copy`. Invalid input preserves the previous valid pattern. Dimensions must match the SLM; resolution changes clear samples, while pitch changes preserve them.
- The phase preview uses one texel per SLM pixel, nearest-neighbor scaling and a pixel-grid aspect ratio independent of physical pitch. Phase wraps to `[0, 2*pi)` and maps to linear grayscale; the texture is cached by revision/grid and released on clear. The preview component is editor-only; storage/API remain available at runtime.

Editor property/transaction callbacks, actor/component transform observers, and actor destruction refresh the snapshot. The workbench post-update tick catches direct C++/Blueprint parameter writes and reference changes. Call `UpdateSceneDescription()` when consuming a parameter write immediately in the same frame. Target resources/debug visualization update automatically. Phase data follows its setter and dimension checks; call `SynchronizePhasePattern()` for immediate consumption after a same-frame resolution write. Other runtime presentation changes can use `RefreshVisualization()`.

`bSceneDescriptionComplete` means all referenced actors are available in the same world and at least one target is present; it does not certify physical validity. `ValidateScene()` remains explicit. Missing actor descriptions are zeroed, missing target references retain zero-valued array entries, and a missing SLM clears the whole snapshot except `SchemaVersion`. Invalid physical inputs remain available for validation and must be checked before future solver use.

## 5. Proposed next steps

The following work remains planned. The next concrete task is **full workbench visual acceptance**, including the new mesh-target and SLM previews.

| Order | Work | Completion evidence |
| --- | --- | --- |
| 1 | Open the workbench in the graphical editor; inspect mesh/point-cloud alignment, responsive target selection, the selected-SLM inset/test-ramp/clear controls, camera preview and Details buttons. Verify resolution versus pitch edits, then save and reopen editable parameters. | Visible parameter updates and persisted editable settings; `Validate Scene` reports a valid configuration. Transient phase/resources are regenerated or republished rather than persisted as map data. |
| 2 | Verify the existing Simulation launch workflow. Decide how the runtime observer should navigate or view the scene, adding a presentation camera/player setup if needed. | Predictable initial viewpoint and usable simulation window through Moonlight. The optical camera should not be assumed to become the runtime view automatically. |
| 3 | Establish a Linux cook/package checkpoint for the small scene. | A packaged build launches with the workbench and assets outside the editor workflow. Record packaging separately from C++ build success. |
| 4 | Agree on a first optical calculation and its assumptions, then implement a minimal numerical reference case. | A documented expected result and numerical comparison, including phase convention, sampling, and units. Algorithm/backend selection remains open. |
| 5 | Connect a validated optical calculation to `SetPhasePattern`, add solver job states and reconstruction-result visualization, then introduce an external solver interface when its contract is stable. | The existing phase preview displays computed data; numerical results and job errors are verified. Reconstruction output remains distinct from camera geometry preview. |

GS/FFT propagation, sensor simulation, V100/container communication, sockets/shared memory, and a runtime parameter UI remain future scope. Their order should follow the chosen optical case and deployment needs.

## 6. Next-session checklist

1. Review this handoff and the current working tree before editing.
2. Use the existing build/start workflow; rebuild and restart the editor after native class/component changes.
3. Open `/Game/CGHSim/Maps/L_CGHWorkbench`. Select CGH actors in the World Outliner and press **F** to frame them; inspect the physically small SLM separately.
4. Inspect **Scene Description**, edit a linked actor parameter, and rotate the SLM to verify signed positions update. Run **Validate Scene** separately and use **Refresh Visualization** for runtime presentation changes.
5. On a mesh target, compare **GeometryMesh** and **Show Point Cloud** at the same transform; check repeated selection remains responsive.
6. Select the SLM and click **CGH > Phase > Load Stored Phase Pattern** to load the bundled saved sample. **Generate Preview Phase Ramp** is an additional generated display check. Check the inset, then **Clear Phase Pattern**. Enable **Preview Selected Cameras** if needed; **Camera Preview Size** controls display size. A new/reopened SLM has no phase data until supplied.
7. Record the observed result and any issues here. After implementation changes, run relevant checks and update their evidence.

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

For each follow-up milestone, append its date, implementation, validation evidence, remaining issues, and next task. Keep proposed work in the plan until it has execution evidence.
