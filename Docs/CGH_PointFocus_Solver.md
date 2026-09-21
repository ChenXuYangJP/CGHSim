# CPU PointFocus reference solver

Implemented **2026-09-21** and expanded to multiple Point and Mesh targets. The CPU backend generates one phase-only SLM pattern by coherently summing the complex contribution from every target emitter. `PointFocus` remains the algorithm name for compatibility with existing solver settings; it now accepts one or more targets. A Point target supplies one emitter, and a Mesh target supplies its sampled point cloud. The workbench supplies SI scene descriptions and the solver publishes through the existing phase API and selected-SLM preview.

## Optical contract

The propagation convention is part of the input contract. At SLM pixel position `p`, let emitter `j` have SLM-local position `q_j`, nonnegative amplitude `a_j`, and phase `phi_j`:

```text
U(r) proportional to exp(+i k r)
k = 2*pi / WavelengthM
r_j = length(q_j - p)
F(p) = sum_j a_j * exp(i * (phi_j - k*r_j))
phase_incident(p) = InitialPhaseRad + k * dot(DirectionSLM, p)
phase_slm(p) = wrap_[0, 2*pi)(atan2(Im(F(p)), Re(F(p))) - phase_incident(p))
```

The sum is over **complex fields, not phase angles**. Equivalently, accumulate `a_j*cos(phi_j-k*r_j)` and `a_j*sin(phi_j-k*r_j)`, then take `atan2` once for the resulting field. Numerically, the implementation precomputes each weighted emitter phasor `a_j*exp(i*phi_j)` and rotates it by `exp(-i*k*r_j)`; this preserves small relative target-phase offsets when the propagation phase is large. For one positive-amplitude Point target this reduces to the original `wrap(target_phase - k*r - phase_incident)`. Adding incident phase and `+k*r` then gives that single target's phase modulo `2*pi`. With multiple emitters, the output retains only the summed field's phase; it does not guarantee each target's requested intensity or eliminate interference between reconstructed points.

Each pixel uses the [canonical SLM coordinates](SLM_Pixel_Coordinates.md):

```text
PhaseRad[row * ResolutionX + column]
Xp = 0
Yp = (column - (ResolutionX - 1) / 2.0) * PixelPitchXM
Zp = ((ResolutionY - 1) / 2.0 - row) * PixelPitchYM
r_j = sqrt(q_j.X*q_j.X + (q_j.Y-Yp)*(q_j.Y-Yp) + (q_j.Z-Zp)*(q_j.Z-Zp))
```

Floating-point division preserves half-pixel centering for even resolutions. Columns increase toward local +Y; rows increase toward -Z. The implementation evaluates distance with `std::hypot`, calculates in `double`, and returns phase radians in `[0, 2*pi)`.

Point emitters use the target description's position, amplitude, and phase. Mesh emitters use their already sampled target-local positions and optical attributes:

```text
q_j = TargetDescription.PositionSLMM
    + TargetDescription.RotationSLM.RotateVector(Point.PositionLocalM)
a_j = Point.Amplitude
phi_j = Point.Phase
```

Point-cloud positions are meters. Actor/component scale and mesh-component offsets are already baked into the resource; the solver applies only target rotation and translation. Each sampled point already inherits target amplitude and phase, so the solver uses those point attributes once without multiplying amplitude or adding phase from the description again. There is no per-mesh point-count normalization: denser clouds contribute more emitters at the same point weights. Normals, UVs, materials, occlusion, `1/r` falloff, and obliquity factors do not affect this reference calculation.

Zero-amplitude emitters contribute nothing. An input with no positive-amplitude emitter fails because it defines no field phase. To avoid overflow from large weights, the implementation divides all positive amplitudes by their common maximum before accumulation and uses Neumaier compensated sums for the real and imaginary parts; common scaling leaves the phase unchanged. At an individual pixel, when the magnitude of the normalized complex sum is at most `32 * double_epsilon * sum(normalized_amplitudes)`, destructive interference makes its phase numerically undefined. That pixel receives deterministic **SLM phase zero**, including under oblique illumination; incident-phase compensation is skipped for this fallback. This convention is a numerical choice, not a claim that a phase-only SLM can produce zero transmitted amplitude there.

This reference supports **PlaneWave illumination only**. The reconstruction-light actor supplies `WavelengthM`, `InitialPhaseRad`, and the unit propagation direction `DirectionSLM`. The plane-wave phase reference is the **SLM origin**: `InitialPhaseRad` is the incident phase at `p = (0, 0, 0)`. Light `PositionSLMM` is ignored, so moving the light actor does not translate the phase reference. A nonzero tangential direction component produces a phase ramp across the SLM that the generated pattern compensates.

Normal incidence along either local +X or -X has `dot(DirectionSLM, p) = 0` because every pixel has X=0. With zero initial light phase, the output is the phase of the complex sum itself.

Light amplitude must be finite and nonnegative, and `PolarizationAngleRad` must be finite. They are validated but do not change this scalar phase-only result. Target amplitudes do set relative weights in the complex sum. Point-target rotation and marker geometry, and all camera parameters, are ignored. There is no polarization response, phase quantization, propagation image, or sensor simulation. `PointSource` and unknown light source types are rejected.

`ECGHPropagationConvention::ExpPositiveIKR` is carried explicitly with each numerical request and returned in result metadata. The actor rejects a result whose convention differs from the submitted input. A future CUDA, FFT, or network backend must preserve this convention or convert its input/output signs deliberately. A library's negative-exponent transform convention alone does not change this contract.

## Accepted inputs

- One or more valid targets, each of type `Point` or `Mesh`, and a phase-only SLM, described by scene schema version 2. Mixed lists are supported. Actor references must be unique; listing the same actor twice is rejected.
- Positive resolution within the existing phase-buffer limits: at most 16,384 pixels per axis and 67,108,864 pixels total.
- Finite positive pixel pitches and wavelength, with finite wave number, propagation phase, incident phase, and combined phase.
- `SourceType = PlaneWave`, finite `InitialPhaseRad`, and finite nonzero unit `DirectionSLM`: `abs(DirectionSLM.SizeSquared() - 1) <= 1e-6`.
- Finite nonnegative light `Amplitude` and finite `PolarizationAngleRad`; valid values are accepted but unused by this scalar phase calculation. Light position is not consumed.
- Finite emitter positions and phases, and finite nonnegative amplitudes. At least one amplitude must be positive. Every positive-amplitude emitter must lie off the SLM plane (`q_j.X != 0`); a Mesh actor's origin can be on the plane if its contributing samples are off it. Signed X on either side is accepted; the scalar distance calculation does not choose a hardware emission side.
- A valid, nonempty point-cloud resource for every Mesh target, with nonzero resource ID and revision matching its description. Missing, stale, mismatched, empty, extra, duplicate, or invalid resources fail. The aggregate input limit is **1,000,000 emitters**, counting one per Point target and every Mesh sample, including zero-amplitude entries.
- For actor submission, a Workbench, SLM, reconstruction-light actor, and all targets in the same world; finite SLM/target transforms, unit SLM actor scale, and target resources using that same SLM reference. Mesh rigid rotations must be finite and normalized with squared-norm tolerance `1e-6`; their scale is already baked into the resource.

A camera is optional for PointFocus even though the workbench's general **Scene Description Complete** and **Validate Scene** checks require it. Solver validation checks its consumed optical inputs separately. Unsupported target types, duplicate/missing targets, invalid point clouds, in-plane contributing emitters, all-zero weights, PointSource/unknown light types, invalid numerical inputs, and unsupported backends fail without publishing a partial pattern.

## Ownership, buffering, and asynchronous work

```text
UE scene actors and cached mesh point clouds
    -> Workbench.UpdateSceneDescription()       [game thread]
    -> ACGHSolverActor captures descriptions/references
    -> copy required point clouds at job launch
    -> FCGHSolverInput owned SI value snapshot
    -> UCGHSolverBackend
    -> UCGHCPUSolverBackend / CGHPointFocus      [CPU worker]
    -> FCGHSolverResult / FCGHSLMPhasePattern
    -> ACGHSolverActor.PollSolver()             [game thread]
    -> SLM.SetPhasePattern()
    -> selected-SLM phase preview
```

`ACGHSolverActor` owns job state, request identifiers, cancellation, queued input, and accepted publication. `UCGHSolverBackend::Submit()` is the replaceable backend boundary. `UCGHCPUSolverBackend` schedules the pure numerical calculation on Unreal's thread pool. `UCGHDockerSolverBackend` now implements the same boundary using asynchronous TCP and the standalone `Backend/V100` dummy server. Its output validates transport and publication only; the CPU backend and PointFocus numerical implementation are unchanged. See the [Docker backend guide](CGH_Docker_Backend.md).

The worker receives an owned, immutable `FCGHSolverInput` snapshot containing plain copied scene data and the required point-cloud resources, plus a shared plain-data `FCGHSolverJob` with cancellation/completion flags and output. It never reads Actors or other UObjects. Only the worker writes its result, and the game thread reads it after the atomic completion flag is published. There is no per-pixel scene query or cross-thread Actor communication.

Input comparison and routine polling use descriptions, resource identities/revisions, and actor references; they do not repeatedly copy clouds. Required clouds are copied once when a job launches, after their resource metadata is checked. Each solver allows one running worker and one latest queued request. A newer request replaces the queued input and requests cancellation of the running job. The old worker is reaped before another starts, bounding concurrent output allocation. Cancellation is cooperative and checked within the emitter accumulation as well as around output work, so a dense cloud does not require finishing a whole row before responding. Cancellation and actor destruction never wait for the worker. `EndPlay` cancels/releases pending work and resets state to `Idle`. Backend and job-status metadata are `DuplicateTransient`, so duplicated actors and PIE copies start without inherited in-flight work or stale status.

The actor polls completion in editor and game-world ticks. Before starting queued work and before publishing a result, it checks the currently consumed inputs, ordered target list and references, mesh resource identities/revisions, and destination SLM phase revision against the submission. Changed target positions/amplitudes/phases, mesh poses/sampling/resources, grid/pitch/wavelength, light phase/source type/direction, replaced or deleted references, or intervening SLM phase publication/clear prevent an obsolete result from overwriting the current pattern. Input validation is repeated before publication, so a light amplitude or polarization value that becomes invalid also prevents publication. Valid light position/amplitude/polarization changes and camera/debug-display changes do not change the numerical result.

A successful result is moved once through the validated `SetPhasePattern` overload on the game thread. Failed, cancelled, or rejected jobs retain the last accepted pattern; existing SLM resolution-change behavior still clears a pattern whose dimensions no longer match. Work is **O(SLM pixels × contributing emitters)**. Start with a small SLM grid and coarse mesh sampling; the reference CPU path is not a fast dense-mesh algorithm. Numerical accumulation runs off-thread, but resource refresh, snapshot copying, phase validation, grayscale conversion, and texture upload remain on the game thread and can still cost frame time. The current design does not promise a fully asynchronous preview update.

## Controls and state

The solver defaults to **CPU**, **PointFocus**, and **Auto Solve disabled** (`bAutoSolve = false`). The existing algorithm name also covers mesh clouds and multiple targets.

| Control or state | Behavior |
| --- | --- |
| **Generate Phase Pattern** / `StartSolve()` | Captures current input and submits or replaces the latest request without waiting for numerical computation. |
| **Save Phase Pattern** / `SaveGeneratedPhasePattern()` | Explicit editor save of the last published result, only while `Ready` and the destination SLM still holds that result. Writes an Unreal asset, raw numerical files, and an exact-resolution grayscale PNG using the SLM's configured folders; reports save status separately from job state. |
| **Cancel Solve** | Drops queued work, requests cancellation, suppresses publication, and returns to `Idle`; preserves the last published SLM pattern. |
| **Auto Solve** | Opt-in mode: submits initially and when consumed optical inputs, target list/references, or mesh resource revisions change. Target amplitude participates in the complex sum and now triggers recomputation. Cancelling does not immediately retry unchanged input. Restoring a temporarily missing reference allows automatic recovery; unchanged invalid numerical inputs, including NaNs, do not submit repeated jobs. Valid light position/amplitude/polarization changes, camera/debug appearance, and manual phase clear alone do not trigger recomputation; invalid fields still fail validation. |
| `Idle` | No requested result awaiting publication; also used after cancellation. |
| `Queued` | Request accepted, awaiting worker start or the previous worker's cancellation. |
| `Running` | CPU worker has started the active request. |
| `Ready` | Last accepted result was published to the SLM. With automatic updates disabled, subsequent scene edits require another Generate request. |
| `Failed` | Input, backend, execution, or publication failed, or an obsolete result was rejected. Read `StatusMessage`. |
| `JobId`, `LastComputeSeconds` | Request identifier and compute duration of the last successful computation considered for publication. |

The workbench's optional `Solver` reference enables **Solve Phase Pattern**, **Cancel Solve**, and mirrored job status. **Solve Phase Pattern** binds an unassigned solver's `Workbench` to itself, but does not replace another existing workbench binding. The solver remains the sole owner of jobs and buffers; the workbench supplies scene inputs and forwards user commands.

`Ready` means phase data was published. It does not mean an optical reconstruction image or a measured physical focus has been produced. The existing camera continues to show ordinary Unreal geometry.

## Try it in an existing level

1. Rebuild `CGHSimEditor` and restart the editor after native code changes.
2. Add the native **CGH Solver Actor** (`ACGHSolverActor`) using Place Actors / the C++ class browser. Set its **Workbench** reference to the existing workbench.
3. On the workbench, assign the SLM, a reconstruction light with **Source Type = PlaneWave**, and one or more distinct entries in **Targets**. Each entry can be **Point** or **Mesh**. Point targets need an off-plane position and a positive amplitude to contribute. For Mesh targets, assign a static mesh, set coarse **Slice Count** / **Point Spacing Mm**, and check **Resources Valid** and **Point Count**; **Show Point Cloud** displays the emitters the solver uses. Keep contributing samples off the SLM plane and all targets linked to the workbench's SLM. Set light wavelength, initial phase, and actor orientation to define the incident wave. Optionally set the workbench's **Solver** reference for its buttons and status display.
4. Start with a small SLM grid and keep the solver at **CPU / PointFocus**. Click **Generate Phase Pattern**, or use **Solve Phase Pattern** on the linked workbench. Select the SLM to inspect the generated grayscale pattern after `Ready`.
5. Enable **Auto Solve** if target list/position/amplitude/phase, mesh transform/sampling, SLM grid/pitch, or light wavelength/initial-phase/direction/source-type edits should request another pattern.
6. To keep a result, wait for `Ready`, configure the save folders on the SLM if needed, then click **Save Phase Pattern** on the solver. After reopening the level, choose the saved asset in the SLM's **Stored Phase Pattern** and click **Load Stored Phase Pattern**. Generation and automatic updates never save files themselves.

No existing Blueprint or map needs to be regenerated. `Scripts/create_cgh_assets.py` can create a missing `BP_CGHSolver` and includes both solver/workbench references when creating a new starter map. It preserves existing assets/maps and does not insert a solver into an existing level. No asset generation is needed for this extension.

## Saving and reloading phase patterns

Saving is an explicit editor action. **Save Phase Pattern** on the solver calls `SaveGeneratedPhasePattern()` and saves all four files from the last successfully published result. It requires `Ready`, the same destination SLM, and its recorded phase revision. Queued/running work, a replaced SLM, a cleared pattern, or a later phase publication that changes its revision prevents the solver from saving an unrelated buffer. The SLM's own **Save Phase Pattern** button calls `SaveCurrentPhasePattern()` and can save any valid current pattern, including a loaded pattern or a manually generated preview ramp.

Choose destinations on the **SLM** before saving:

| SLM setting | Default and meaning |
| --- | --- |
| **Phase Asset Save Folder** / `PhaseAssetSaveFolder` | `/Game/CGHSim/PhasePatterns/Generated`, an Unreal Content folder. Saves a reusable `UCGHPhasePatternAsset` `.uasset`. |
| **Phase Raw Save Directory** / `PhaseRawSaveDirectory` | `Saved/CGHSim/PhasePatterns`, relative to the project directory. Also accepts an absolute filesystem folder selected with the folder picker. Saves matching `.bin`, `.json`, and `.png` files. |

Each click creates a unique timestamp-and-GUID name shared by all four files. Existing files/assets are not overwritten. The operation attempts to remove files created by that save if an ordinary failure prevents any output from completing; this is best-effort cleanup, not a crash-safe filesystem transaction. Check **Phase Save Status** (`PhaseSaveStatus`) on the actor for the outcome. The SLM's `LastSavedPhaseAsset`, `LastSavedPhaseBinaryFile`, `LastSavedPhaseMetadataFile`, and `LastSavedPhaseImageFile` report successful output locations. Saving does not publish a new phase buffer, advance its revision, change **Stored Phase Pattern**, or alter the solver's job state.

The raw binary is the exact stored phase array: **headerless little-endian IEEE-754 float64 radians**, with no grayscale conversion or phase quantization. It contains `ResolutionX * ResolutionY` samples in `PhaseRad[row * ResolutionX + column]` order and has a byte count of `8 * ResolutionX * ResolutionY`. Columns increase toward SLM-local +Y; rows increase toward -Z. The UTF-8 JSON sidecar records the dimensions, pixel pitches in meters, layout/axis convention, source label, and preview flag. This is phase-buffer metadata; it does not claim to archive the originating target, wavelength, or complete solver input. In particular, a preview ramp remains identified as preview data.

The PNG is a lossless **single-channel 8-bit grayscale** image with exactly `ResolutionX` columns and `ResolutionY` rows. Each SLM sample maps directly to one image pixel; row zero is the top row, with no flips, resampling, borders, or gamma transformation. Its stored byte value matches the preview's `PhaseToGray` mapping:

```text
gray = round(255 * wrap_[0, 2*pi)(phase_rad) / (2*pi))
```

PNG preserves those byte values losslessly, but phase-to-grayscale conversion is quantized to 256 levels. The `.bin` and Unreal asset retain full-precision phase data. The exported image can therefore be pixel-accurate without preserving every phase value numerically.

The JSON schema retains `format_version = 1` with additive image fields. Its keys are `asset_path`, `binary_filename`, `resolution_x`, `resolution_y`, `phase_unit`, `dtype`, `endianness`, `array_order`, `index`, `pixel_pitch_x_m`, `pixel_pitch_y_m`, `coordinate_frame`, `pixel_center_convention`, `optical_normal_slm`, `pixel_plane_x_m`, `column_direction_slm`, `row_direction_slm`, `pattern_label`, `is_preview_pattern`, `image_filename`, `image_format`, `image_bit_depth`, `image_mapping`, and `image_row_order`. Both `binary_filename` and `image_filename` are relative to the sidecar's directory. Image metadata declares `image_format = "PNG"`, `image_bit_depth = 8`, `image_mapping = "round(255 * wrap_0_2pi(phase_rad) / (2*pi))"`, and `image_row_order = "top-to-bottom"`. Pixel pitches describe the SLM settings at save time; the asset itself preserves the phase grid and its label/preview flag.

The sidecar declares `coordinate_frame = "SLM-local"`, `pixel_center_convention = "centered"`, `optical_normal_slm = "+X"`, and `pixel_plane_x_m = 0`. With the exported grid dimensions and pitches, its sample positions in meters are:

```text
X = 0
Y = (column - (resolution_x - 1) / 2.0) * pixel_pitch_x_m
Z = ((resolution_y - 1) / 2.0 - row) * pixel_pitch_y_m
```

No SLM world transform is exported; these positions use the local optical frame.

For external NumPy consumers:

```python
import json
from pathlib import Path
import numpy as np

metadata_path = Path("/path/to/Phase_...json")
metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
phase_rad = np.fromfile(
    metadata_path.parent / metadata["binary_filename"], dtype="<f8"
).reshape(metadata["resolution_y"], metadata["resolution_x"])
```

To reuse the Unreal asset, select it in **Stored Phase Pattern** on the SLM and click **Load Stored Phase Pattern**. Loading retains the existing nearest-neighbor resize behavior if the saved grid and current SLM resolution differ; it does not restore the saved pitch or scene settings. Use the same resolution and pitch when reproducing the original physical phase plane. Save the level if the selected asset reference should persist. Active phase data itself remains transient.

Persistence is editor-only; Game builds return an unsupported-operation status. The explicit Save call performs synchronous disk and asset work, so large grids can take time. It is independent of the asynchronous CPU solve and is never invoked by **Generate Phase Pattern** or **Auto Solve**.

## Verification record

**Complex point-cloud and multi-target extension — 2026-09-21, UE 5.8.2 on Linux:** Editor and Game Linux Development builds passed, exit code 0. All **58 headless CGH tests passed**, with zero warnings, failures, or not-run tests. The nine added tests cover weighted complex summation, mesh rigid transforms and baked sample fields, amplitude scaling, zero weights/destructive interference, resource/sample validation, owned cloud snapshots, cancellation, mixed-target publication, stale cloud/target-list rejection, and automatic updates/recovery. All **15 existing assets/maps** remained byte-identical. The mixed-scene smoke check did not save a phase pattern.

A no-save mixed-scene smoke check passed using the current native **256×256** grid, two Point targets, one Mesh target, and **78 emitters**. An independent calculation checked 81 distributed pixels, with maximum circular phase error **1.8058163e-8 rad**. One headless run measured **0.140953 ms submission**, **0.298134772 s worker computation**, **0.318106 ms maximum completion-poll call**, and **0.026991 ms cancellation return**. These single-run NullRHI measurements do not include actual grayscale/texture rendering or establish interactive GUI responsiveness; dense-cloud cost still grows with pixels times emitters.

Report: `Saved/Automation/CGHMultiTarget/index.json`; smoke script/metrics: `Saved/Automation/CGHMultiTarget/verify_mixed_scene.py` and `smoke_metrics.json`. Logs under `Saved/Logs`: `CGHMultiTargetEditorBuild_2026-09-21.log`, `CGHMultiTargetGameBuild_2026-09-21.log`, `CGHMultiTargetTests_2026-09-21.log`, and `CGHMultiTargetSmoke_2026-09-21.log`. The records below retain earlier single-point and persistence milestones; their timings do not describe dense-cloud solves.

**Pixel-accurate PNG follow-up — 2026-09-21, UE 5.8.2 on Linux:** Editor/Game Linux Development builds passed, exit code 0, and all **49 headless CGH tests passed**, with zero warnings, failures, or not-run tests. The added `PixelAccurateGrayscalePNG` test checks asymmetric 3×2 and 1×1 grids. A separate native CPU solve at 257×129 was saved twice; an independent Pillow decoder matched all **66,306 pixels** across the two PNGs, including exact dimensions, single-channel mode `L`, and row order. Full-precision raw phase data was preserved.

Report: `Saved/Automation/CGHPhasePng/index.json`; smoke scripts/evidence: `Saved/Automation/CGHPhasePngSmoke/`. Logs under `Saved/Logs`: `CGHPhasePngEditorBuild_2026-09-21.log`, `CGHPhasePngGameBuild_2026-09-21.log`, `CGHPhasePngTests_2026-09-21.log`, and `CGHPhasePngSmoke_2026-09-21.log`. Interactive save responsiveness and Windows behavior remain unverified. The 48-test result below retains the earlier asset/numerical-file milestone.

**Explicit phase saving — 2026-09-21, UE 5.8.2 on Linux:**

- Editor and Game Linux Development builds passed, exit code 0. All **48 headless CGH tests passed**, with zero warnings, failures, or not-run tests. The six added save tests cover exact numerical export and asset reload, unique repeated saves, invalid inputs/directory failures, SLM state preservation, solver publication guards, and explicit-only saving after automatic generation. Project-relative raw paths are covered, including the correction for an engine-relative `ProjectDir`.
- Separate writer and fresh-process reload smoke checks passed, exit code 0, confirming exact bytes, repeated-save uniqueness, and no automatic saving. All **11 original asset/map hashes** remained unchanged. Temporary save fixtures were removed.
- Report: `Saved/Automation/CGHPhaseSave/index.json`. Logs under `Saved/Logs`: `CGHPhaseSaveEditorBuild_2026-09-21.log`, `CGHPhaseSaveGameBuild_2026-09-21.log`, `CGHPhaseSaveTests_2026-09-21.log`, `CGHPhaseSaveWrite_2026-09-21.log`, and `CGHPhaseSaveReload_2026-09-21.log`. Smoke scripts and manifest remain under `Saved/Automation/CGHPhaseSaveSmoke/` as local ignored evidence.

The save feature has not been assessed for interactive GUI responsiveness or Windows behavior. Large saves still perform synchronous work. The following results retain the earlier CPU solver milestone; its performance measurements do not measure saving.

**CPU PointFocus — 2026-09-21, UE 5.8.2:**

- Editor and Game Linux Development builds passed, exit code 0.
- **42 headless CGH tests passed**, with zero test failures, warnings, or not-run tests. This includes six numerical PointFocus tests and ten solver-actor/lifecycle tests. Independent complex propagation verifies incident plane-wave phase + SLM phase + `k*r` equals the requested target phase, including oblique illumination and a rotated SLM frame. Coverage also includes centered grids, invalid inputs, cancellation/replacement, stale-result rejection, automatic updates/recovery, move publication, duplication, and teardown.
- A no-save **4096×4096** solve with oblique PlaneWave illumination and nonzero initial phase passed. One headless run measured **0.111 ms submission**, **0.548120 s worker computation**, and **22.267 ms maximum completion-poll call**. Cancelling another full-grid job returned in **0.053 ms**. These are single-run measurements, not guarantees of editor frame time; grayscale/texture rendering was not measured by this NullRHI smoke check.
- The existing `BP_CGHSLM` accepted a computed 32×16 pattern after the native full-grid/cancellation checks. All **11 pre-existing asset/map hashes** remained unchanged. The asset generator was syntax-checked but not executed.
- `git diff --check` and local documentation links passed. The real-RHI render test was not rerun; full interactive GUI, Simulation, optical reconstruction, and packaging remain separate work.

Reports: `Saved/Automation/CGHPointFocus/index.json`. Logs: `Saved/Logs/CGHPointFocusEditorBuild_2026-09-21.log`, `CGHPointFocusGameBuild_2026-09-21.log`, `CGHPointFocusTests_2026-09-21.log`, and `CGHPointFocusSmoke_2026-09-21.log`. The no-save smoke script is retained at `Saved/Automation/CGHPointFocus/verify_default_grid.py`. These are local ignored evidence files. Repeat the C++ suite with the command in [Recreate assets and verify](CGH_Actor_Scaffold.md#recreate-assets-and-verify).
