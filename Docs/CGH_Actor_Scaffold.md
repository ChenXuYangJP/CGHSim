# CGH actor scaffold

This implements the editable scene structure from the [shared design](https://chatgpt.com/share/6aad4cb3-0f30-83e8-9ea8-b099461b757d). Point and static-mesh targets are supported, including CPU surface-contour sampling. The [CPU PointFocus solver](CGH_PointFocus_Solver.md) generates an SLM phase pattern from one or more Point/Mesh targets using coherent complex-field superposition. Reconstruction images, sensor simulation and external solver communication remain unimplemented.

## Files and responsibilities

| Native class | Blueprint | Role |
| --- | --- | --- |
| `ACGHTargetActor` | `BP_CGHTargetPoint` | Point or static-mesh target, versioned geometry/point cloud, and debug preview |
| `ACGHSLMActor` | `BP_CGHSLM` | Pixel resolution/pitch, derived active area, versioned phase storage, selected-actor phase preview, and explicit asset/raw-file saving |
| `ACGHCameraActor` | `BP_CGHCamera` | Thin-lens sensor reconstruction, sensor resolution/pitch, complex field preview/save/load, and selectable Cine Camera geometry preview |
| `ACGHReconstructionLightActor` | `BP_CGHReconstructionLight` | Wavelength, amplitude, phase, polarization and propagation direction |
| `ACGHWorkbenchActor` | `BP_CGHWorkbench` | Explicit actor references, automatic SI snapshots, validation, refresh, and optional solver buttons/status |
| `ACGHSolverActor` | Optional generated `BP_CGHSolver` | Asynchronous CPU PointFocus jobs for Point/Mesh target lists, input/output buffers, cancellation, opt-in auto updates, and validated SLM publication |

Sources are under `Source/CGHSim/CGH/Actors`. Shared enums, editable parameter structs, and read-only description structs are in `Types/CGHTypes.h`; conversions are in `Utils/CGHUnitConversion.h`. `FCGHSceneDescription` exports optical poses and parameters in SI units with `SchemaVersion = 2`.

The scene Blueprints and `Maps/L_CGHWorkbench.umap` live under `Content/CGHSim`. The solver can be placed directly as a native C++ Actor; the asset generator creates a missing `BP_CGHSolver` and connects it only in newly created maps. `Materials` and `Meshes` are reserved directories; this first scaffold uses built-in Engine meshes. Blueprint graphs contain no optical algorithm. Native default mesh components can be styled in Blueprint children.

## Editing the scene

The editor startup map and game default map are `Content/CGHSim/Maps/L_CGHWorkbench`. Open that map explicitly if the editor was already running. Select the actors in the World Outliner and press **F** to frame them; headless creation cannot persist a live viewport position. Edit each actor's **CGH** parameters in Details. Construction updates dimensions, camera preview and labels. Target parameters and mesh changes refresh automatically. For other actor previews, call **Refresh Visualization** after runtime Blueprint parameter changes.

Select **CGH Workbench** to inspect **Scene Description**, or use **Validate Scene** or **Refresh Visualization**. The snapshot updates automatically; validation remains explicit. Validation checks references, distinct targets, finite physical inputs, positive dimensions, valid mesh resources, and unit scales on non-target optical actors. The Details status/messages describe the last explicit validation; validate again after changing a linked actor. Successful validation means the configuration is valid, not that a hologram was computed.

The starter map places the SLM at `(0,0,0)` cm, the target at `(50,0,0)` cm, the camera at `(100,0,0)` cm looking toward the target, and the reconstruction light at `(-30,0,0)` cm pointing along +X. Floor and ordinary UE lighting are in a separate **Presentation** folder. These are illustrative scene positions, not a calibrated optical setup. The native default camera focus is 500 mm; adjust it for the desired subject.

## Units and coordinates

- Scene positions use Unreal centimeters. Editable optical parameters use explicit mm, um, nm, rad, and degree field suffixes; scalar optical parameters are `double`. Scene-description lengths use meters and angles use radians.
- SLM local **+X** is optical forward/normal, **+Y** horizontal, **+Z** vertical. Its active plane is YZ.
- The [canonical SLM pixel convention](SLM_Pixel_Coordinates.md) defines `PhaseRad[row * ResolutionX + column]`, column steps along local +Y, and row steps along -Z. Pixel centers are `X = 0`, `Y = (column - (ResolutionX - 1) / 2.0) * PixelPitchXM`, `Z = ((ResolutionY - 1) / 2.0 - row) * PixelPitchYM`. Resolution/pitch X means grid horizontal/local Y; resolution/pitch Y means grid vertical/local Z.
- The canonical front **image** is defined from the +X side toward -X, with +Y right and +Z up. An ordinary upright Unreal camera on that side shows +Y left, so matching that image requires a horizontal display mirror. The SLM inset directly draws the canonical 2D grid, independent of any camera pose.
- The current native default `256 x 256` pixels at `8 um` gives `2.048 x 2.048 mm`, or `0.2048 x 0.2048 cm`; existing Blueprint/level overrides may differ. The active mesh is physically small; frame the SLM separately for close inspection.
- Keep SLM, camera and light actor scales at `(1,1,1)`. Mesh targets support actor and component scale, including nonuniform and mirrored scale. The SLM child scale is derived from its parameters; do not use it as an independent optical setting.
- `GetOpticalPositionMeters(SLM)` subtracts the SLM origin, applies its inverse rotation and converts cm to m. It intentionally ignores reference scale. Passing null returns world position in meters; the workbench requires an assigned SLM.
- Target `MarkerRadiusCm` affects only the visual sphere, never the mathematical point. Point targets ignore mesh and sampling settings and keep both geometry resources empty.
- Light +X is its propagation direction. The reconstruction-light actor contains no UE illumination component.
- Camera `Parameters` drive `PreviewCamera`; on placed actors, editing the component’s Current Focal Length, Current Aperture, or Manual Focus Distance also updates the matching optical parameter. Component manual focus uses centimeters; the optical focus parameter uses millimeters. `OpticalReference` supplies the exported optical position and forward direction, including component offsets. Output resolution and effective sensor pitch define the reconstructed sensor. **Sensor Size** retains legacy width/height behavior; **Pixel Pitch** derives physical extent from resolution and pitch. The full optical quaternion also preserves sensor roll. Use **Camera (Thin Lens)** on the existing reconstructor; see [camera reconstruction](CGH_Camera_Reconstruction.md).
- An SLM starts with `GenerationState = NotImplemented` and `HasPhaseData = false`. Publishing valid phase data changes the state to `Ready`; this indicates stored data. The legacy empty-state enum name does not describe whether a solver implementation exists; solver job state is separate. Clearing the data restores the initial state.

## Scene description and updates

The workbench exposes a transient, Blueprint-read-only `SceneDescription`. Its schema contains:

| Block | Fields |
| --- | --- |
| Root | `SchemaVersion = 2` |
| `SLM` | `ResolutionX/Y`, `PixelPitchXM/YM`, `ActiveWidthM`, `ActiveHeightM`, `ModulationType` |
| `ReconstructionLight` | `WavelengthM`, `Amplitude`, `InitialPhaseRad`, `DirectionSLM`, `SourceType`, `PositionSLMM`, `PolarizationAngleRad` |
| `Targets[]` | `ResourceId`, `Revision`, `PositionSLMM`, `RotationSLM`, `Amplitude`, `PhaseRad`, `TargetType`, compatibility aliases |
| `Camera` | `OpticalPositionSLMM`, `ForwardDirectionSLM`, `FocalLengthM`, `FNumber`, `FocusDistanceM`, `SensorWidthM`, `SensorHeightM`, `OutputResolutionX/Y` |

All positions are relative to the assigned SLM actor's origin and rotation. Export ignores SLM scale so distances retain their physical size; forward and propagation vectors are unit directions in the same frame. Active width and height come from resolution multiplied by pixel pitch.

Position coordinates retain their signs. With the actor's local +X as its optical normal, `X = dot(WorldPositionCm - SLMOriginCm, SLMNormal) * 0.01`: points on the normal side have positive X, points on the opposite side have negative X, and points in the SLM plane have zero X. Y and Z are signed projections onto the actor's local horizontal and vertical axes. Rotating the SLM updates this entire frame; reversing its normal swaps the sign of X for fixed world-space points. The `OpticalNormal` arrow visualizes the actor's +X axis; rotate the SLM actor to change the optical frame.

Editor property and transaction notifications, transform observers, and runtime post-update polling keep the snapshot current when linked actors, their references, or the camera optical component change. Undo/redo and actor deletion also refresh it. C++ and Blueprint code that writes public parameters and immediately consumes the snapshot in the same frame should call `UpdateSceneDescription()` first.

`bSceneDescriptionComplete` reports whether all required references are present, including at least one target. It does not certify physical validity; call `ValidateScene()` for that. Missing actors produce zero-valued description blocks, and missing targets retain their array indices as zero-valued entries. Without an SLM, the entire description is empty/zero-valued except for its schema version. Updates do not run validation or optical calculations. Target visualizations update automatically.

## Mesh targets and point-cloud preview

1. Set the target's **Parameters > Target Type** to **Mesh**.
2. Select its **GeometryMesh** component and assign a Static Mesh. This component supplies the geometry; the original MarkerMesh is only the point-target selection aid.
3. Assign the target's **SLM**, or add it to a workbench's **Targets** list so it inherits that workbench's SLM. An explicit target SLM wins; a workbench reports an error if the references disagree. A target can use only one SLM frame at a time.
4. Set **Slice Count** (default 32) and **Point Spacing Mm** (default 5 mm). Spacing is measured after applying actor and component scale. **Max Point Count** defaults to 100,000; exceeding it reports an error and clears the result instead of exporting a partial cloud.
5. Enable **Show Point Cloud** to hide GeometryMesh and render the sampled points. **Debug Point Size** is in screen pixels and **Debug Point Color** sets their color. This works in editor viewports and play mode. Disabling the option restores the mesh preview.

Sampling uses LOD 0 triangle data (the fallback render mesh for Nanite assets), interpolated vertex normals, and UV channel 0. Cross-section planes are perpendicular to the direction from the target actor origin to the SLM origin. Coincident origins use the SLM's normal as the axis. Each plane lies at the midpoint of one equally sized depth interval; one slice therefore samples the center section. Samples follow the triangle/plane intersection contours, including endpoints, with gaps no larger than the requested spacing. Coincident points merge. Flat coplanar meshes use boundary edges and omit internal triangulation diagonals; degenerate triangles are ignored. This samples contour edges, not the filled interior of each cross-section. It does not simulate occlusion or material-dependent optical response.

`TargetDescription`, `MeshGeometryResource`, and `PointCloudResource` are transient actor-owned caches. Their `ResourceId` identifies the actor for the current process/session; each actor instance (including duplicates) receives its own ID. Their `Revision` values match and advance together after changes to optical inputs, target/component transforms, mesh assignment or asset rebuild, sampling settings, or the SLM pose/reference. Repeated updates with unchanged inputs retain the revision. Selecting an actor also retains the cached data and revision. The Details panel shows `MeshVertexCount`, `MeshTriangleCount`, and `PointCount`, plus description IDs/revisions and resource status. Bulk mesh/point arrays remain available to C++, Blueprint and Python but are excluded from Details and undo transactions, avoiding expensive per-element property rows on every selection. Debug appearance alone does not change the optical revision. The original `TargetId`, `GeometryResourceId`, `GeometryRevision`, and `PositionSLM` fields are retained as compatibility aliases; geometry aliases are zero for point targets. Unsigned 64-bit identifiers remain available to C++ and reflection, but are not Blueprint pins.

Resource vertices and points use meters in the target actor's rigid local frame. Actor/component scale and mesh-component offsets are already baked in. To obtain SLM-local meters, use `TargetDescription.PositionSLMM + TargetDescription.RotationSLM.RotateVector(Point.PositionLocalM)`; do not apply actor scale again. Normals use the inverse transpose for nonuniform scale, and mirrored geometry reverses triangle winding. Every point inherits target amplitude and phase and carries an interpolated normal and UV. The CPU solver uses each sampled amplitude and phase once; it does not apply the target values again. Normals and UVs are unused by the scalar reference solver.

Construction, BeginPlay, transform/property/transaction callbacks, and an input comparison each tick keep resources current. Direct C++/Blueprint writes and mesh swaps are caught on the next tick; call **Update Target Resources** before an immediate same-frame read. **Rebuild Target Resources** forces an update after custom runtime edits to mesh data. The workbench refreshes targets before publishing its snapshot and retains their IDs/revisions; it does not copy large geometry arrays into the scene description. `bResourcesValid` and `ResourceError` expose failures. Failed mesh updates stamp the new revision and clear both resource arrays so a consumer cannot reuse stale points.

For cooked runtime sampling, enable **Allow CPU Access** on each source mesh before cooking and keep LOD 0 resident (disable mesh LOD streaming if necessary). Enabling CPU access after buffers have been discarded cannot recover them. Missing CPU data, unavailable LOD 0, zero scale, invalid sampling inputs, and excessive point/work counts produce an explicit error. Async mesh compilation retries when completed. Cooked runtime rendering still needs a separate packaging check; headless tests do not verify viewport appearance.

## SLM phase data and selected-actor preview

Select an SLM actor to show its phase pattern in the editor's native picture-in-picture panel, like the camera preview. The panel is supplied by `UCGHSLMPreviewComponent`; no camera or SceneCapture is needed. If selected-actor previews are disabled, enable **Preview Selected Cameras** in the level-editor viewport preferences. **Camera Preview Size** controls the inset size; the displayed image retains its pixel-grid aspect ratio. A new SLM shows **No phase pattern** against a dark background, without allocating or pretending to have a zero-phase hologram.

A saved sample is included at **`/Game/CGHSim/PhasePatterns/DA_SLMPreviewPattern`**. After rebuilding and restarting the editor, select the SLM and click **CGH > Phase > Load Stored Phase Pattern**. The actor's **Stored Phase Pattern** field selects this asset by default. It contains a 256-by-256 grid with horizontal/vertical ramps, rings, a checkerboard, and a bright top-left orientation marker. Loading uses nearest-neighbor sampling to produce exactly the SLM's current resolution, without changing resolution, pixel pitch, or physical dimensions. **Clear Phase Pattern** removes active data while keeping the saved asset available for another load. The sample persists as a DataAsset; active actor phase data remains transient.

`UCGHPhasePatternAsset` serializes its phase samples outside the Details property tree. Use `SetPattern()` to populate an asset and `GetPattern()` for C++ read access. Invalid payloads are rejected, and failed activation leaves the previous valid actor pattern intact. Asset labels and the preview flag identify the loaded source. Existing assets are preserved by `Scripts/create_cgh_phase_sample.py`; run it inside Unreal like the other setup scripts if the bundled sample needs to be recreated. Only a newly created sample asset is saved by that script.

To persist the current buffer, click **Save Phase Pattern** on the SLM under **CGH > Phase**. One explicit save writes a `UCGHPhasePatternAsset` in **Phase Asset Save Folder** (default `/Game/CGHSim/PhasePatterns/Generated`) and full-precision `.bin`, UTF-8 `.json`, and grayscale `.png` files in **Phase Raw Save Directory** (default project-relative `Saved/CGHSim/PhasePatterns`, with absolute folders also accepted). Matching timestamp-and-GUID names avoid replacing earlier saves. The binary preserves row-major little-endian float64 radians; the sidecar records resolution, pitches in meters, axes, source label, and preview flag. The PNG uses one 8-bit grayscale image pixel per SLM pixel at exactly the current resolution, with row zero at the top and no flip, resampling, borders, or gamma transformation. It uses the preview's wrapped-phase mapping; the binary and asset preserve full precision. The export does not archive the complete solver request. Save does not change the active phase revision or **Stored Phase Pattern**. Read the save status and `LastSavedPhaseAsset` / `LastSavedPhaseBinaryFile` / `LastSavedPhaseMetadataFile` / `LastSavedPhaseImageFile` for results. To reload later, select that asset in **Stored Phase Pattern** and click **Load Stored Phase Pattern**; nearest-neighbor resizing still applies when grids differ. Saving is synchronous and editor-only. See the [full save format and behavior](CGH_PointFocus_Solver.md#saving-and-reloading-phase-patterns).

For a separate generated display check, click **Generate Preview Phase Ramp** under **CGH > Phase**. It creates a horizontal ramp with sample `2*pi*column/ResolutionX`, increasing along local +Y and repeated on every row, and labels the result **Preview test ramp**. This button does not run a CGH solver. **Clear Phase Pattern** removes the samples and returns the panel to its empty state.

To publish solver or imported data, construct `FCGHSLMPhasePattern` from `CGH/Types/CGHSLMPhasePattern.h` and call `SetPhasePattern` in C++ or Blueprint. Set `ResolutionX`, `ResolutionY`, and exactly `ResolutionX * ResolutionY` finite `double` phase values in radians. Storage is row-major: `PhaseRad[row * ResolutionX + column]`, with row zero at the top (maximum local Z) and column zero at the left (minimum local Y) of the canonical image. Dimensions must match the SLM parameters. Input is limited to 16,384 pixels per axis and 67,108,864 total pixels; the graphics device may impose a smaller preview-texture limit. Invalid input returns false, sets `PhasePatternError`, and preserves the previous valid pattern. The actor owns accepted data; the ordinary C++/Blueprint setter copies it, while the C++ rvalue overload moves a completed solver buffer after validation. The incoming revision is ignored.

C++ consumers can read `GetPhasePattern()` without copying the array and compare `GetPhasePatternRevision()` to their cached revision. Blueprint readers use **Get Phase Pattern Copy**, which returns an independent value; editing that copy cannot change the published buffer or bypass `SetPhasePattern` validation and revision updates. Revisions advance when phase samples change, when existing data is cleared, or when resolution changes. Publishing identical samples and repeatedly selecting the actor retain the revision. Publishing external data clears the demo label even when its samples match the preview ramp. Phase storage is transient and is not saved with the map.

Changing pixel pitch preserves phase samples, because it changes physical size rather than the pixel grid. Changing either resolution clears mismatched samples; editor/runtime ticks catch direct parameter writes, and C++ code can call `SynchronizePhasePattern()` before consuming data in the same frame. **Refresh Visualization** preserves a valid pattern. `HasValidPhasePattern()` confirms that the current samples still match the current grid.

The preview texture has exactly one texel per SLM pixel and uses nearest-neighbor display scaling, preserving the pixel-grid aspect ratio independently of physical pitch. Phase wraps into `[0, 2*pi)` and maps linearly to grayscale: zero and whole cycles are black, pi is mid-gray, and values approaching 2*pi are white. Negative phases wrap the same way. Texture bytes use linear BGRA8 with gamma conversion disabled. The texture is updated only after its phase revision or grid changes; unchanged selection reuses it. The full phase array stays out of the Details tree and undo transactions, avoiding per-pixel property rows. The preview component and its texture are editor-only; runtime phase storage is available to the CPU solver and future output devices.

## CPU PointFocus controls

Place a native **CGH Solver Actor** in the existing level and set its **Workbench**. With one or more distinct Point/Mesh targets in the workbench’s **Targets** list, a phase-only SLM, and a valid PlaneWave reconstruction light, choose **CPU / PointFocus** and click **Generate Phase Pattern**. **Auto Solve** is disabled by default; enabling it recomputes after changes to consumed optical inputs. **Cancel Solve** retains the last published pattern. Select the SLM to inspect the output after the solver reaches `Ready`. Click the solver's **Save Phase Pattern** to persist that result using the SLM save folders; this requires the same published buffer and destination to remain current. Generate and Auto Solve never save automatically. The SLM's separate Save button also accepts valid manually supplied or preview patterns.

Optionally assign the workbench's **Solver** reference for its **Solve Phase Pattern**, **Cancel Solve**, and mirrored status. Its Solve button binds an unassigned solver to that workbench. Jobs and buffers remain owned by the solver actor. Camera is optional for PointFocus; general scene validation still checks the full scene.

The numerical core runs on a worker using an immutable SI snapshot. For `U(r) proportional to exp(+i k r)`, it forms `F(p) = sum_j a_j * exp(i*(phi_j-k*r_j))` and outputs `wrap(arg(F(p)) - phase_incident(p))`: complex fields are added before taking the phase. Point targets contribute their position/amplitude/phase; Mesh targets contribute their cached samples after rigid transformation into SLM-local meters. Sample weights already include target amplitude and phase, and scale is already baked into sampled positions. No extra target factor, `1/r`, point-count normalization, normal, or UV weighting is applied. All positive-amplitude emitters must lie off the SLM plane. Zero-amplitude emitters are skipped; all-zero input fails, and numerically cancelled pixel fields receive phase zero. PlaneWave illumination uses `phase_incident(p) = InitialPhaseRad + k*dot(DirectionSLM, p)`, with a finite unit direction and phase referenced to the SLM origin; light position is ignored. Light amplitude must be finite/nonnegative and polarization angle finite, but neither alters the scalar phase-only result. PointSource is unsupported.

Auto Solve responds to target list/position/amplitude/phase changes, mesh transforms/sampling/resource revisions, and light wavelength/phase/direction/source changes. Valid light position/amplitude/polarization changes do not trigger a new pattern; invalid light fields fail validation, including before publication. Missing/duplicate targets, invalid or mismatched point-cloud resources, and obsolete results are rejected. Cloud arrays are copied once at job launch; routine polling compares their descriptions and revisions. The CPU cost is proportional to SLM pixels times contributing emitters, so use a small grid and coarse mesh sampling initially. Resource refresh/copying, publication, validation, grayscale conversion and texture upload occur on the game thread and can still take frame time. Docker remains an explicitly unimplemented backend. Read the [solver contract and usage](CGH_PointFocus_Solver.md) for numerical limits and lifecycle details.

## Recreate assets and verify

`CinematicCamera` is a runtime module dependency; `RenderCore` and `RHI` support mesh-buffer access and the point-cloud debug component. The editor-only `PythonScriptPlugin` enables the scripts below; no solver plugin is required.

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

The generator preserves existing Blueprints/maps and rejects conflicting Blueprint parents. It saves a new map only after populating and validating it, then reloads it to check references. It never updates an existing map, so subsequent scene edits remain yours. Newly created maps include `BP_CGHSolver` with both workbench/solver references; existing levels need a solver placed explicitly. The updated generator has not been run for this milestone. In an interactive editor it refuses to replace an unsaved world. Assets can also be created through **Tools > Execute Python Script** after building the native classes.

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
  -ExecCmds="Automation RunTests CGH; Quit" \
  -unattended -nullrhi -nosplash
```

The grayscale render check requires a real graphics device and can run without opening an editor window:

```bash
/home/cxy/opt/UnrealEngine/UE5/Engine/Binaries/Linux/UnrealEditor \
  "$PWD/CGHSim.uproject" -RenderOffscreen -unattended -nosplash -nosound \
  -ExecCmds="Automation RunTests CGH.SLMPhasePattern.RenderedPreviewPreservesGrayscale; Quit"
```

It renders the actual phase widget in an isolated Slate window, checks displayed grayscale and row order, and saves `Saved/Automation/CGHSLMPreview/SLMPhasePreview.png`. The test is excluded under NullRHI. The native selection/discovery and texture-cache tests run headlessly.

## Verified in this workspace

The complex multi-target/mesh-cloud extension on **2026-09-21** passed Editor/Game Linux Development builds and all **58 headless CGH tests**, exit code 0 with zero warnings, failures, or not-run tests. Added coverage checks weighted complex fields, transformed mesh samples, destructive interference, input/resource validation, owned snapshots, cancellation, stale target/cloud rejection, and automatic updates. A no-save 256×256 mixed-scene smoke check independently verified 81 pixels from 78 emitters; all 15 existing assets/maps retained their hashes. Evidence is under `Saved/Automation/CGHMultiTarget` and `Saved/Logs/CGHMultiTarget*2026-09-21.log`; see the [verification record](CGH_PointFocus_Solver.md#verification-record). Earlier records below retain their original milestone scope.

The PNG follow-up on **2026-09-21** passed Editor/Game Linux Development builds and all **49 headless CGH tests**, exit code 0 with zero warnings, failures, or not-run tests. An independent Pillow decoder matched every pixel in two 257×129 CPU-generated PNGs, including dimensions, grayscale mode, and row order; raw phase precision was preserved. Reports are under `Saved/Automation/CGHPhasePng` and `Saved/Automation/CGHPhasePngSmoke`; details and limitations are in the [verification record](CGH_PointFocus_Solver.md#verification-record).

The explicit-save follow-up on **2026-09-21** passed Editor/Game Linux Development builds and all **48 headless CGH tests**, exit code 0 with zero warnings, failures, or not-run tests. Separate writer and fresh-process reload checks confirmed exact float64 bytes, unique repeated saves, and no automatic saving; all 11 original assets/maps retained their hashes, and temporary save fixtures were removed. Evidence is in `Saved/Automation/CGHPhaseSave`, `Saved/Automation/CGHPhaseSaveSmoke`, and `Saved/Logs/CGHPhaseSave*2026-09-21.log`; see the [save verification record](CGH_PointFocus_Solver.md#verification-record). Interactive save responsiveness and Windows behavior remain unverified. The following entries retain earlier milestone results.

The CPU PointFocus milestone passed Editor/Game builds and all **42 headless CGH tests** on **2026-09-21** (exit code 0, zero test failures/warnings/not-run). Coverage includes incident plane-wave compensation, automatic updates, publication, cancellation, stale inputs, duplication and teardown. The no-save 4096×4096 oblique-illumination solve, cancellation, and existing Blueprint 32×16 activation also passed; all 11 existing assets/maps were unchanged. See the [verification record](CGH_PointFocus_Solver.md#verification-record) for timings and limitations. The following entries describe earlier milestones.

The stored-sample follow-up on **2026-09-21** passed both Editor and Game builds and all **26 headless CGH tests** (exit code 0). Added tests cover saved payload ownership/serialization, nearest-neighbor resizing, activation/reload, invalid-load preservation, and unchanged SLM physical settings. The new `DA_SLMPreviewPattern` asset was saved, reloaded in a fresh Unreal process, and activated through the existing `BP_CGHSLM` at 32-by-16 and through the native actor at its default 4096-by-4096 resolution. Repeat setup preserved the asset bytes. Hashes confirmed all 10 pre-existing assets/maps were unchanged. Reports are in `Saved/Automation/CGHStoredPhasePattern`; build/setup/smoke logs are `Saved/Logs/CGHStoredPhase*2026-09-21.log`. The earlier real-RHI render check below remains the evidence for the unchanged grayscale drawing path.

On **2026-09-21**, the SLM phase preview passed both Editor and Game Development builds, all **21 headless `CGH` automation tests**, and the separate **Vulkan/Slate render test**, each with exit code 0. The rendered 4-by-2 grid retained gray levels 0, 64, 128, and 255 in the correct row order. A no-save smoke check also verified that the existing `BP_CGHSLM` inherits the active preview component and supports phase publication, readback, demo generation, and clearing. Reports are in `Saved/Automation/CGHSLMPreview` and `Saved/Automation/CGHSLMPreviewRender`; the rendered screenshot is alongside the headless report. No project assets or maps were saved. Restart the editor after rebuilding to load the new native preview component.

On **2026-09-21**, the selection-performance fix passed both Editor and Game Development builds and all **14 `CGH` automation tests** (exit code 0). The new selection test uses Unreal's editor selection and Details property-row generator with 3,264 cached points, verifies that bulk arrays produce no Details trees, checks the displayed counts, and confirms that repeated selection/deselection preserves resource revisions and buffer storage. Full resources remain accessible through reflection and Blueprint. The report is in `Saved/Automation/CGHTargetSelection`.

The mesh-target update on **2026-09-20** passed both `CGHSimEditor Linux Development` and `CGHSim Linux Development` builds. All **13 `CGH` automation tests** passed, including contour sampling/attributes, coplanar and degenerate geometry, invalid-input limits, target lifecycle and revisions, deleted SLM references, mesh swaps, mirrored/nonuniform transforms and normals, workbench bindings, and debug visibility. The no-save Python smoke check also passed with `CGH_SCAFFOLD_SMOKE_OK`. The automation report is in `Saved/Automation/CGHTargetResources`. These headless checks do not verify viewport appearance or cooking/packaging.

On **2026-09-20**, the SI scene-description implementation passed `CGHSimEditor Linux Development` and `CGHSim Linux Development` builds against installed UE 5.8.2. The final Editor build and all four `CGH.SceneDescription` automation tests passed after the signed-normal regression was added. Tests cover SI conversion, optical camera offsets, editor notifications, runtime writes and reference changes, and positive/negative/zero positions when the SLM normal changes. The test runner exited with code 0.

The Python integration script passed with `CGH_SCAFFOLD_SMOKE_OK` and exit code 0, including automatic snapshot initialization on map load. Its expectations were aligned with the existing 4096-pixel SLM and 500 mm focus defaults; saved target position checks use the actual optical pose. No assets or maps were saved during these checks.

Asset creation, map reload, and byte-for-byte preservation on repeated setup were verified on **2026-09-19**. Local execution logs are listed in the [development handoff](CGHSim_Development_Handoff.md#3-verification-completed). Full graphical acceptance, interactive Simulation and undo/redo, cook/packaging, and optical reconstruction remain unverified or unimplemented; transaction callbacks are covered by automation.
