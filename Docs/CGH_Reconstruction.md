# Optical reconstruction on an observer plane

`ACGHReconstructorActor` reconstructs the complex optical field from an SLM phase pattern and reconstruction light, then publishes one complex value per pixel to `ACGHObserverPlaneActor`. The CPU backend runs numerical propagation asynchronously; **Docker (TCP/CUDA)** sends the same optical task to the shared V100 service and receives a full-precision complex field. **Camera/lens reconstruction** remains unimplemented.

## Editor setup

1. Build `CGHSimEditor` and restart the editor after adding these native classes. Add **CGH Observer Plane Actor** and **CGH Reconstructor Actor** from Place Actors / the C++ class browser. Existing Blueprint assets and maps do not need regeneration.
2. On the workbench, assign **SLM**, **Reconstruction Light**, and **Observer Plane**. Set the reconstructor's **Workbench** reference. Optionally assign **Reconstructor** on the workbench to use its **Reconstruct** and **Cancel Reconstruction** buttons and mirrored status.
3. Load or generate a valid phase pattern on the SLM. Reconstruction consumes the active numerical phase buffer, including manually generated preview patterns; it does not generate a hologram from targets. A target list and camera are unnecessary for observer reconstruction.
4. Position the observer on the SLM's **+X side**. For an unrotated SLM at the origin, an observer at Unreal location `(50, 0, 0)` is 0.5 meters in front of the SLM. Keep SLM, reconstruction-light, and observer actor scales at `(1, 1, 1)`. Set observer **Resolution X/Y** and **Pixel Pitch X/Y Um** to choose the sampling grid and physical size. All observer pixel centers must remain in the SLM-local `X > 0` half-space, including when the observer is tilted.
5. Start with a small grid, for example a 32×32 SLM and 32×32 observer, to inspect behavior. The observer defaults to **64×64 at 8 µm pitch**; the default CPU calculation is direct diffraction, whose work grows as the product of both grids' pixel counts. Keep **Reconstruction Backend = CPU**, **Mode = Observer Plane**, and click **Reconstruct**.
6. Wait for **Ready**, then select the observer to open its native editor preview inset. Choose **Amplitude** (the default), **Phase**, or **Intensity** under **Preview Mode** in Details. The preview header and legend show the current mode. The preview has no mode-switch button. Changing the display mode reuses the received field and requires no reconstruction. Unreal's camera preview size setting controls the inset's display size.
7. Enable **Auto Reconstruct** if changes to the consumed optical configuration or active phase pattern should automatically request a new result. **Cancel Reconstruction** stops pending publication and preserves the last accepted field.

The workbench's existing **Validate Scene** and **Scene Description Complete** controls still describe the solver scaffold, including its camera and target requirements. They do not gate observer reconstruction. **Observer Plane Description** and its availability flag are separate SI metadata; availability alone does not claim physical validity.

## Responsibilities and asynchronous jobs

The reconstruction light remains an optical source actor. The workbench owns scene associations and captures their optical descriptions. The reconstructor owns reconstruction requests, job state, cancellation, stale-result checks, and publication. Backends own the asynchronous numerical task.

```text
SLM active phase + reconstruction light + observer sampling geometry
    -> Workbench.CaptureReconstructionInput()       [game thread metadata]
    -> Reconstructor captures owned phase snapshot [at launch]
    -> UCGHReconstructionBackend
       -> CPU: UCGHCPUReconstructionBackend          [thread pool]
       -> Docker: TCP -> V100 CUDA Rayleigh–Sommerfeld reconstruction
    -> FCGHReconstructionResult / FCGHComplexField
    -> Reconstructor.PollReconstructor()            [game thread]
    -> ObserverPlane.SetComplexField()
    -> selected-observer phase/amplitude/intensity preview
```

The worker receives plain owned values and never reads scene actors or other UObjects. Metadata comparison does not copy the SLM phase array on every tick. Workbench optical capture also avoids rebuilding target meshes or point clouds. The observer description stays outside `FCGHSceneDescription`, preserving the solver's scene schema. CGHV 1.2 adds separate reconstruction messages; the existing PointFocus payload layout remains unchanged.

Jobs use **Idle**, **Queued**, **Running**, **Ready**, and **Failed** states. A newer request replaces the latest queued request and requests cancellation of the running worker. Cancellation is cooperative; cancelling or destroying the actor does not wait for the numerical loop. Publication occurs on the game thread after the result completes and the consumed actor references, optical metadata, SLM phase revision, and destination field revision still match. Replacing or deleting an input actor, editing the sampling geometry, or intervening phase/field publication prevents an old result from overwriting current data.

Failures, cancellation, and obsolete results preserve the last accepted field. Changing observer resolution clears a field that no longer matches its dimensions. The active complex field is transient and does not persist with a saved level. Use **Save Complex Field** for explicit persistence, then select its asset under **Stored Complex Field** and click **Load Stored Complex Field** when needed. Saving an SLM phase pattern remains a separate existing operation. Reconstruction, preview changes, and saving the level do not automatically export observer data.

Numerical work runs on a worker thread. Snapshot copying, result validation, grayscale generation, and preview texture upload still run on the game thread. Large grids can therefore affect frame time even though propagation itself is asynchronous.

## Docker reconstruction

Build and run the shared [V100 Docker backend](../Backend/V100/README.md) with the current source. In **Parameters**, select **Reconstruction Backend = Docker (TCP/CUDA)** and keep **Mode = Observer Plane**. Set **Docker > Address/Port** to the published endpoint (default `127.0.0.1:7000`) and click **Reconstruct**. The same service accepts PointFocus solver jobs and reconstruction jobs; request types select the compute mode automatically.

The Docker settings provide numeric IPv4, port, connect timeout, and total request timeout (defaults 5 and 30 seconds). The total timeout includes serialization, transfer, computation, and conversion. Increase it for large direct-propagation jobs. Settings are captured with the job; changing them invalidates an in-flight result. Cancel, stale input rejection, latest-request handling, and observer publication follow the same actor lifecycle as CPU reconstruction.

The service runs FP64 Rayleigh–Sommerfeld propagation on all visible GPUs, including PlaneWave and PointSource illumination and translated/tilted observers. Each GPU computes complete sums for its own contiguous portion of observer pixels, with ordered compensation across SLM batches. The initial launch configuration retains the PointFocus tuning: **65,536 pixels per tile, 128 threads per block, and 256 contributors per batch**. The existing PointFocus calculation and CPU reconstruction reference are unchanged. CUDA errors fail the job without CPU or dummy fallback.

CGHV **1.2** carries full binary64 phase inputs and complex output pairs. Rebuild both server and Unreal after this protocol update; 1.1 peers are rejected. Each axis is at most 16,384 pixels; transport limits are 16,777,216 SLM pixels and 16,777,214 observer pixels, with at most 256 MiB per frame. Camera mode and the server's explicit dummy fixture do not reconstruct fields. Phase/Amplitude/Intensity previews and all saved data formats work identically for accepted CPU and Docker results.

## Coordinates and field representation

Both SLM and observer sample their local YZ planes. Local **+X** is the optical normal. Samples use row-major order:

```text
index = row * ResolutionX + column
local_pixel_m = (0,
    (column - (ResolutionX - 1) / 2) * PixelPitchXM,
    ((ResolutionY - 1) / 2 - row) * PixelPitchYM)
```

Columns increase along each plane's local **+Y**, and rows increase toward **-Z**. Resolution X and pitch X describe horizontal columns, not the optical normal. The observer's rigid position and rotation are expressed in the SLM-local frame; its sample location is:

```text
Q = ObserverPlane.PositionSLMM
    + ObserverPlane.RotationSLM.RotateVector(local_pixel_m)
```

The workbench converts Unreal centimeters to meters and pixel pitches from micrometers to meters. It rejects nonunit actor scales for reconstruction rather than folding scale into the physical sampling pitch.

`FCGHComplexField` stores dimensions and `Samples`, a `TArray<FCGHComplexSample>`. Each sample has double-precision **Real** and **Imaginary** components of the scalar field. The observer accepts only complete, finite fields matching its configured grid. C++ consumers can use `GetComplexField()` by const reference; Blueprint consumers can explicitly request `GetComplexFieldCopy()`. Field revisions identify changes for job publication and cached preview updates.

## CPU propagation model

The CPU backend evaluates scalar Rayleigh–Sommerfeld I diffraction with the existing **`exp(+i k r)`** convention. Each SLM sample represents a pixel-center midpoint quadrature contribution over area `DeltaA = PixelPitchXM * PixelPitchYM`. For source pixel `P`, observer pixel `Q`, `r = |Q - P|`, `z = Q.X`, and `k = 2*pi/wavelength`:

```text
U(Q) = sum_P [DeltaA * U_inc(P) * exp(i * phase_slm(P))
              * exp(+i*k*r) * z/(2*pi*r*r) * (1/r - i*k)]
```

This is the first Rayleigh–Sommerfeld kernel, including its distance and obliquity terms, sampled across the SLM aperture. See [Appendix A of the EPFL diffraction reference](https://www.epfl.ch/labs/lo/wp-content/uploads/2018/08/OC_284_1686_Mar2011.pdf) for the integral formulation. The implementation supports a translated or tilted observer through each sample's three-dimensional position; it requires every sample strictly in front of the SLM plane.

The incident source can be:

- **PlaneWave:** `U_inc(P) = A * exp(i * (initial_phase + k * dot(direction, P)))`. Phase is referenced to the SLM origin, matching the PointFocus solver. Translating a plane-wave light does not change that convention.
- **PointSource:** `U_inc(P) = A * (1 meter / r_source) * exp(i * (initial_phase + k * r_source))`, where `r_source = |P - source_position|`. The configured amplitude is the field amplitude at one meter from the source, and initial phase is the source phase offset. A source coincident with an SLM sample is invalid. The existing PointFocus solver's source restrictions remain separate.

The SLM must use **PhaseOnly** modulation and contain a finite phase sample for every pixel. Light amplitude must be finite and nonnegative; wavelength and pixel pitches must be finite and positive. Polarization angle is validated but does not affect this scalar model. Camera mode does not evaluate lens parameters yet.

The kernel preserves its computed complex amplitude; it does not normalize the stored field. It uses pixel-center quadrature, not analytical integration over each finite pixel aperture. Sampling accuracy depends on wavelength, aperture pitch, propagation distance, and observation geometry. Coarse grids and near-plane observations can be poorly sampled. This is a numerical reference with cost **O(SLM pixel count × observer pixel count)**, without FFT acceleration or adaptive quadrature.

## Preview mappings

Each field sample maps to one grayscale texel. Preview scaling changes only the displayed inset size; it does not change the field grid.

| Mode | Mapping |
| --- | --- |
| **Phase** | `round(255 * wrap_[0,2*pi)(atan2(Imaginary, Real)) / (2*pi))`. A zero complex sample displays black because its phase is undefined. |
| **Amplitude** | `round(255 * abs(U) / max(abs(U)))`, using the maximum amplitude of the current field. An entirely zero field displays black. |
| **Intensity** | `round(255 * abs(U)^2 / max(abs(U)^2))`, using the maximum intensity of the current field. An entirely zero field displays black. |

Amplitude is the field magnitude `sqrt(Real^2 + Imaginary^2)`. Intensity is its square, `Real^2 + Imaginary^2`, proportional to optical power per unit area in this scalar model. The display shows relative intensity without detector response or absolute power calibration. Amplitude and intensity each normalize to their own field maximum, so the same grayscale brightness across different reconstructions does not establish equal absolute values. For example, half the maximum amplitude appears near gray 128 in Amplitude mode and gray 64 in Intensity mode. Both mappings scale the components before calculation to handle very large or small finite samples safely, and apply no gamma correction. Display conversion leaves the double-precision complex samples unchanged.

## Saving and loading observer data

After the reconstructor reaches **Ready**, open **CGH > Reconstruction > Save** on **CGH Reconstructor Actor** and click **Save Complex Field**. This uses the linked observer's save directories and shows its result in **Field Save Status**. Like the solver's save action, it requires that actor's accepted result to remain the active observer field; pending/cancelled/failed jobs or a replaced destination/field are rejected.

The observer also keeps its own **CGH > Field > Save > Save Complex Field** button for any valid current field, including loaded data. Both buttons use the same export operation. Each successful save creates six uniquely named files:

| Output | Contents |
| --- | --- |
| Unreal `UCGHComplexFieldAsset` | Full-precision complex samples, resolution, pixel pitches, and a label. |
| `.bin` | Headerless little-endian `complex128`: one float64 real value followed by one float64 imaginary value for each row-major pixel. |
| `.json` | Versioned format, dimensions, pitches in meters, coordinate conventions, component order, image mappings, and output filenames. |
| `_phase.png` | One 8-bit grayscale pixel per observer sample using the phase preview mapping. |
| `_amplitude.png` | One 8-bit grayscale pixel per observer sample using the normalized field-amplitude preview mapping. |
| `_intensity.png` | One 8-bit grayscale pixel per observer sample using the normalized squared-magnitude preview mapping. |

All three images are saved regardless of **Preview Mode**. Their quantization and amplitude/intensity normalization do not change the numerical data in the asset or binary file. Samples retain their original row order: columns toward observer-local +Y, rows toward -Z. The metadata describes the observer-local sampling grid; it does not store a complete scene or imply that the observer's current pose was the pose used to compute the field.

**Field Asset Save Folder** defaults to `/Game/CGHSim/ObserverFields/Generated`. **Field Raw Save Directory** defaults to `Saved/CGHSim/ObserverFields` under the project; it may also be an absolute directory. **Field Save Status** and the **Last Saved Field** properties show the result paths. Saving is an explicit editor operation, follows the SLM's synchronous save workflow, and may take time for large grids. It does not change the current samples, revision, preview mode, or selected stored asset. Repeated saves create new names and never replace earlier files. A failed save reports an error and attempts to remove only its own partial outputs.

To reload, select the saved asset under **Stored Complex Field**, configure the observer with that asset's resolution and pixel pitches, and click **Load Stored Complex Field**. Loading requires the same sampling grid and preserves exact complex samples; it does not resample the field or silently change the observer's configuration. Invalid assets and mismatched grids leave the current field intact. The saved asset displays its resolution, sample count, and pixel pitches for reference. Saving does not automatically change the stored-asset selection.

For example, read the raw field in NumPy using the JSON dimensions:

```python
import json
from pathlib import Path
import numpy as np

metadata_path = Path("ObserverField_<timestamp>_<id>.json")
metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
field = np.fromfile(metadata_path.parent / metadata["binary_filename"], dtype="<c16")
field = field.reshape(metadata["resolution_y"], metadata["resolution_x"])
phase = np.angle(field)
amplitude = np.abs(field)
intensity = amplitude**2
```

## Verification

**2026-09-22 — reconstructor save button:** Editor and Game builds passed, as did all **10 focused persistence/lifecycle tests**. The new button writes all six observer formats, reports delegated success/failure, preserves field and job state, and rejects unpublished or replaced results. Report: `Saved/Automation/CGHReconstructorSave/index.json`.

**2026-09-22 — Docker/CUDA reconstruction:** The CUDA 12.9.2 / Ubuntu 22.04 image and Linux Development Editor/Game targets built successfully. All **93 headless Unreal tests passed**, with zero warnings, skips, or failures, using the live Docker service on both V100s and the native executable for controlled transport fixtures. The five Docker reconstruction tests cover malformed/fragmented responses, invalid inputs/settings, timeout and cancellation, immutable settings snapshots, stale endpoint rejection, automatic recovery, CPU/CUDA complex parity, observer publication, and switching back to CPU.

All **6/6 standalone CTest suites passed**: wire codec, TCP integration, PointFocus, observer reconstruction, multi-GPU PointFocus, and multi-GPU reconstruction. Independent complex-field fixtures cover oblique PlaneWave and PointSource illumination, tilted planes, absolute amplitude, zero light, invalid geometry, and source/tile boundaries. Single- and dual-GPU complex arrays were bit-identical in the tested cases, including **513 SLM contributors and a 513×257 observer** (131,841 samples). Concurrent jobs, cancellation isolation, disconnect recovery, and no-visible-GPU errors passed. A CUDA 12.9.2 device-link audit with the release/separable-compilation flags reported 60 registers per reconstruction thread, a 40-byte stack, and zero static local memory. The full 65,536-pixel tile launches 512 blocks of 128 threads. This is correctness and compiler-resource coverage, not a reconstruction performance benchmark.

The tuned PointFocus CUDA file and both CPU numerical reference files are unchanged, as are all 19 existing Content files. The rebuilt `cgh-v100:latest` container is running on `127.0.0.1:7000`, exposing host GPUs 1 and 3. The separate test container was removed. Unreal's report is `Saved/Automation/CGHDockerReconstructionFinal/index.json`; standalone logs and builds are `/tmp/CGHDockerReconstructionCTestFinal.log` and `/tmp/cgh-docker-reconstruction-tests`. Restart the editor to load the new native backend and protocol. Camera/lens mode, cooking, and packaged deployment remain unverified/unimplemented as applicable.

**2026-09-22 — intensity preview and export:** Editor and Game Linux Development builds passed. The headless suite passed **82 tests**, with six optional external Docker/CUDA tests skipped and zero failures. Intensity checks cover normalized squared magnitude, zero and extreme finite fields, Details mode changes on an open widget, exact G8 PNG pixels, six-file saves, and failure handling. The Vulkan/Slate render test passed for all three modes, including the intensity header, grayscale cells, and row order. Reports are under `Saved/Automation/CGHIntensity` and `Saved/Automation/CGHIntensityRender`; the intensity screenshot is `Saved/Automation/CGHObserverPreview/ObserverIntensityPreview.png`. All 19 existing Content files were unchanged.

**2026-09-22 — observer saving and Details-only preview:** Editor and Game Linux Development builds passed. The full headless suite passed **82 tests**, with six optional existing external Docker/CUDA tests skipped and no failures. All four new persistence tests passed, including exact complex binary/asset round trips (with signed zero), both G8 PNG mappings, unique saves, failure handling, and explicit same-grid loading. The Vulkan/Slate preview test passed after removing the mode button.

A separate write process and fresh Unreal reload process verified all 35 complex samples in a 7×5 field. Independent Python/Pillow decoding verified both PNGs pixel for pixel and all 70 float64 binary components. These scripts used an unsaved temporary world; the generated test asset was removed. The existing user-edited map's hash was unchanged. Reports are under `Saved/Automation/CGHObserverSave`, `CGHObserverSavePreviewRender`, and `CGHObserverSaveSmoke`.


On **2026-09-22**, the Linux Development Editor and Game builds passed. The final headless CGH run passed **78 tests** with **zero failures**; six existing external Docker/CUDA integration tests reported skips because their optional servers were not supplied. All **18 new headless reconstruction/observer tests** passed. The separate Vulkan/Slate observer preview test also passed, checking actual rendered phase and amplitude grayscale values and row order while switching modes on the same widget.

Coverage includes analytical single-pixel diffraction, an independent complex-field oracle for tilted observers and oblique illumination, point-source attenuation, amplitude linearity, and a phase pattern from the existing solver producing a coherent bright focus. Lifecycle checks cover owned worker inputs, cancellation, latest-request publication, stale input/destination rejection, automatic runtime updates, duplication, workbench ownership, and explicit failure of the reserved modes. Observer tests cover finite complex storage, resolution changes, grayscale mapping, and preview caching.

The headless suites are `CGH.Reconstruction`, `CGH.ReconstructorActor`, and `CGH.ObserverPlane`. Run the full suite after building using the command in the [README](../README.md#asset-generation-and-checks). Verification artifacts in this workspace:

- `Saved/Automation/CGHReconstruction/index.json`: headless regression report.
- `Saved/Automation/CGHObserverPreviewRender/index.json`: actual-widget render report.
- `Saved/Automation/CGHObserverPreview/ObserverPhasePreview.png` and `ObserverAmplitudePreview.png`, and `ObserverIntensityPreview.png`: rendered preview captures.

The rendering test can be repeated with a real graphics device:

```bash
"$CGHSIM_UE_ROOT/Engine/Binaries/Linux/UnrealEditor" \
  "$CGHSIM_PROJECT_ROOT/CGHSim.uproject" -RenderOffscreen -unattended -nosplash -nosound \
  -ExecCmds="Automation RunTests CGH.ObserverPlane.RenderedPhaseAmplitudeAndIntensityPreserveGrayscale; Quit"
```

No existing Content assets or maps were changed. Full interactive editor use, large-grid performance, cooking, and packaging were not verified. The final builds used `-NoUBA` after a build-accelerator file-read/compiler crash; both targets compiled successfully with that option.
