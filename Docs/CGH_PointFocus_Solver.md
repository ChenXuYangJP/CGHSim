# CPU PointFocus reference solver

Implemented **2026-09-21**. This first optical calculation generates a phase-only SLM pattern for exactly one `ACGHTargetActor` whose target type is `Point`. It uses the workbench's SI scene description and publishes through the existing phase API and selected-SLM preview.

## Optical contract

The propagation convention is part of the input contract:

```text
U(r) proportional to exp(+i k r)
k = 2*pi / WavelengthM
phase_incident(p) = InitialPhaseRad + k * dot(DirectionSLM, p)
phase_slm(p) = wrap_[0, 2*pi)(target_phase - k*r - phase_incident(p))
```

For target position `(Xt, Yt, Zt)` in SLM-local meters, each pixel uses the [canonical SLM coordinates](SLM_Pixel_Coordinates.md):

```text
PhaseRad[row * ResolutionX + column]
Xp = 0
Yp = (column - (ResolutionX - 1) / 2.0) * PixelPitchXM
Zp = ((ResolutionY - 1) / 2.0 - row) * PixelPitchYM
r = sqrt(Xt*Xt + (Yt-Yp)*(Yt-Yp) + (Zt-Zp)*(Zt-Zp))
```

Floating-point division preserves half-pixel centering for even resolutions. Columns increase toward local +Y; rows increase toward -Z. The implementation evaluates distance with `std::hypot` to avoid unnecessary overflow from intermediate squares, calculates in `double`, and returns wrapped phase in radians. Adding the incident phase `phase_incident(p)` and the propagation phase `+k*r` to every returned sample gives the same target phase modulo `2*pi`.

This reference supports **PlaneWave illumination only**. The reconstruction-light actor supplies `WavelengthM`, `InitialPhaseRad`, and the unit propagation direction `DirectionSLM`. The plane-wave phase reference is the **SLM origin**: `InitialPhaseRad` is the incident phase at `p = (0, 0, 0)`. Light `PositionSLMM` is ignored, so moving the light actor does not translate the phase reference. A nonzero tangential direction component produces a phase ramp across the SLM that the generated pattern compensates.

Normal incidence along either local +X or -X has `dot(DirectionSLM, p) = 0` because every pixel has X=0. With zero initial light phase, the formula reduces to `wrap(target_phase - k*r)`.

Light amplitude must be finite and nonnegative, and `PolarizationAngleRad` must be finite. They are validated but do not change this scalar phase-only result; no polarization response or amplitude model is implemented. Target amplitude, target rotation, target marker geometry, and all camera parameters are also ignored. There is no `1/r` amplitude weighting, obliquity factor, phase quantization, propagation image, or sensor simulation. `PointSource` and unknown light source types are rejected.

`ECGHPropagationConvention::ExpPositiveIKR` is carried explicitly with each numerical request and returned in result metadata. The actor rejects a result whose convention differs from the submitted input. A future CUDA, FFT, or network backend must preserve this convention or convert its input/output signs deliberately. A library's negative-exponent transform convention alone does not change this contract.

## Accepted inputs

- Exactly one valid `Point` target and a phase-only SLM, described by scene schema version 2.
- Positive resolution within the existing phase-buffer limits: at most 16,384 pixels per axis and 67,108,864 pixels total.
- Finite positive pixel pitches and wavelength, with finite wave number, propagation phase, incident phase, and combined phase.
- `SourceType = PlaneWave`, finite `InitialPhaseRad`, and finite nonzero unit `DirectionSLM`: `abs(DirectionSLM.SizeSquared() - 1) <= 1e-6`.
- Finite nonnegative light `Amplitude` and finite `PolarizationAngleRad`; valid values are accepted but unused by this scalar phase calculation. Light position is not consumed.
- Finite target position and target phase. SLM-local target X must be nonzero. Signed X on either side of the plane is accepted; this scalar distance calculation uses `X*X` and does not choose a hardware emission side.
- For actor submission, a Workbench, SLM, reconstruction-light actor, and target in the same world; finite SLM/target transforms; unit SLM actor scale; and target resources using that SLM reference.

A camera is optional for PointFocus even though the workbench's general **Scene Description Complete** and **Validate Scene** checks require it. Solver validation checks its consumed optical inputs separately. Mesh targets, multiple targets, an in-plane target, PointSource/unknown light types, invalid numerical inputs, and unsupported backends fail without publishing a partial pattern.

## Ownership, buffering, and asynchronous work

```text
UE scene actors
    -> Workbench.UpdateSceneDescription()       [game thread]
    -> FCGHSolverInput value snapshot
    -> ACGHSolverActor / UCGHSolverBackend
    -> UCGHCPUSolverBackend / CGHPointFocus      [CPU worker]
    -> FCGHSolverResult / FCGHSLMPhasePattern
    -> ACGHSolverActor.PollSolver()             [game thread]
    -> SLM.SetPhasePattern()
    -> selected-SLM phase preview
```

`ACGHSolverActor` owns job state, request identifiers, cancellation, queued input, and accepted publication. `UCGHSolverBackend::Submit()` is the replaceable backend boundary. `UCGHCPUSolverBackend` schedules the pure numerical calculation on Unreal's thread pool. A future `UCGHDockerSolverBackend` can implement this boundary; **Docker** currently produces a clear failure and performs no networking.

The worker receives an owned, immutable `FCGHSolverInput` snapshot and a shared plain-data `FCGHSolverJob` containing cancellation/completion flags and the output. It never reads Actors or other UObjects. Only the worker writes its result, and the game thread reads it after the atomic completion flag is published. There is no per-pixel scene query or cross-thread Actor communication.

Each solver allows one running worker and one latest queued request. A newer request replaces the queued input and requests cancellation of the running job. The old worker is reaped before another starts, bounding concurrent output allocation. Cancellation is cooperative, checked before allocation, between rows, and before completion; cancellation and actor destruction never wait for the worker. `EndPlay` cancels/releases pending work and resets state to `Idle`. Backend and job-status metadata are `DuplicateTransient`, so duplicated actors and PIE copies start without inherited in-flight work or stale status.

The actor polls completion in editor and game-world ticks. Before starting queued work and before publishing a result, it checks the currently consumed inputs, actor references, and destination SLM phase revision against the submission. A changed target/grid/pitch/wavelength/phase, light source type or direction, changed reference, deleted actor, or intervening SLM phase publication/clear prevents an obsolete result from overwriting the current pattern. Input validation is repeated before publication, so a light amplitude or polarization value that becomes invalid also prevents publication. Valid light position/amplitude/polarization changes, camera/display changes, target amplitude, and unrelated resource revisions do not change the numerical result.

A successful result is moved once through the validated `SetPhasePattern` overload on the game thread. Failed, cancelled, or rejected jobs retain the last accepted pattern; existing SLM resolution-change behavior still clears a pattern whose dimensions no longer match. Numerical computation runs off-thread, but snapshot capture, phase validation, grayscale conversion, and texture upload remain on the game thread and can still cost frame time at large resolutions. The current design does not promise a fully asynchronous preview update.

## Controls and state

The solver defaults to **CPU**, **PointFocus**, and **Auto Solve disabled** (`bAutoSolve = false`).

| Control or state | Behavior |
| --- | --- |
| **Generate Phase Pattern** / `StartSolve()` | Captures current input and submits or replaces the latest request without waiting for computation. |
| **Save Phase Pattern** / `SaveGeneratedPhasePattern()` | Explicit editor save of the last published result, only while `Ready` and the destination SLM still holds that result. Writes an Unreal asset, raw numerical files, and an exact-resolution grayscale PNG using the SLM's configured folders; reports save status separately from job state. |
| **Cancel Solve** | Drops queued work, requests cancellation, suppresses publication, and returns to `Idle`; preserves the last published SLM pattern. |
| **Auto Solve** | Opt-in mode: submits initially and when consumed optical inputs or references change. Cancelling does not immediately retry unchanged input. Restoring a temporarily missing reference allows automatic recovery; unchanged invalid numerical inputs, including NaNs, do not submit repeated jobs. Light initial phase, direction, and source-type changes participate. Valid light position/amplitude/polarization changes, camera/presentation/target-amplitude changes, and manual phase clear alone do not trigger recomputation; invalid fields still fail validation. |
| `Idle` | No requested result awaiting publication; also used after cancellation. |
| `Queued` | Request accepted, awaiting worker start or the previous worker's cancellation. |
| `Running` | CPU worker has started the active request. |
| `Ready` | Last accepted result was published to the SLM. With automatic updates disabled, subsequent scene edits require another Generate request. |
| `Failed` | Input, backend, execution, or publication failed, or an obsolete result was rejected. Read `StatusMessage`. |
| `JobId`, `LastComputeSeconds` | Request identifier and compute duration of the last successful computation considered for publication. |

The workbench's optional `Solver` reference enables **Solve Phase Pattern**, **Cancel Solve**, and mirrored job status. **Solve Phase Pattern** binds an unassigned solver's `Workbench` to itself, but does not replace another existing workbench binding. The solver remains the sole owner of jobs and buffers; the workbench supplies scene inputs and forwards user commands.

`Ready` means phase data was published. It does not mean an optical reconstruction image or a measured physical focus has been produced. The existing camera continues to show ordinary Unreal geometry.

## Try it in an existing level

1. Rebuild `CGHSimEditor` and restart the editor after adding the native classes.
2. Add the native **CGH Solver Actor** (`ACGHSolverActor`) using Place Actors / the C++ class browser. Set its **Workbench** reference to the existing workbench.
3. On the workbench, assign the SLM, a reconstruction light with **Source Type = PlaneWave**, and exactly one target with **Target Type = Point**. Put the target off the SLM plane. Set light wavelength, initial phase, and actor orientation to define the incident wave; its phase reference stays at the SLM origin. Optionally set the workbench's **Solver** reference for its buttons and status display.
4. Keep the solver at **CPU / PointFocus** and click **Generate Phase Pattern**, or use **Solve Phase Pattern** on the linked workbench. Select the SLM to inspect the generated grayscale pattern after `Ready`.
5. Enable **Auto Solve** if target position/phase, SLM grid/pitch, or light wavelength/initial-phase/direction/source-type edits should request another pattern.
6. To keep a result, wait for `Ready`, configure the save folders on the SLM if needed, then click **Save Phase Pattern** on the solver. After reopening the level, choose the saved asset in the SLM's **Stored Phase Pattern** and click **Load Stored Phase Pattern**. Generation and automatic updates never save files themselves.

No existing Blueprint or map needs to be regenerated. `Scripts/create_cgh_assets.py` can create a missing `BP_CGHSolver` and includes both solver/workbench references when creating a new starter map. It preserves existing assets/maps and does not insert a solver into an existing level. The script has not been executed for this milestone; no solver asset or map has been saved as part of this change.

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
