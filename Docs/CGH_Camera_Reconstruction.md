# Optical reconstruction through a camera lens

`ACGHReconstructorActor` now supports **Mode = Camera (Thin Lens)** on both **CPU** and **Docker (TCP/CUDA)**. It propagates the active SLM field through the lens defined by `ACGHCameraActor`, then publishes a double-precision complex sample at each camera sensor pixel. It uses the same asynchronous jobs, cancellation, stale-result checks, preview mappings, and explicit save workflow as observer reconstruction.

## Editor and Blueprint setup

1. Rebuild `CGHSimEditor` and restart the editor. For Docker, rebuild the shared `Backend/V100` image as well: both endpoints now require **CGHV 1.4**.
2. Assign **SLM**, **Reconstruction Light**, and **Camera** on the workbench. Assign that **Workbench** to the reconstructor. Camera mode does not require an observer plane, target list, or Unreal scene lighting. Generate or load an SLM phase pattern first.
3. Place the camera lens on the SLM's **+X side** and point the camera's **optical +X toward the SLM**. For a default SLM at the origin, a camera at `(50, 0, 0)` cm with yaw `180°` has a lens half a meter away, looking at the SLM. The camera's `OpticalReference` component supplies the lens position and complete rotation, including component offsets and roll. Keep the SLM, light, camera actor, and optical reference world scales at `(1,1,1)`.
4. Set camera **Focal Length Mm**, **F Number**, and **Focus Distance Mm** under **CGH → Camera → Parameters**, or use the component lens controls described below. Focus distance is the desired object distance measured forward from the lens. It must be greater than focal length. The sensor is positioned behind the lens using the thin-lens relation; it is not another actor that needs manual placement.
5. Set **Output Resolution X/Y**. Under **Sensor Sampling**, choose **Pixel Pitch** to edit **Pixel Pitch X/Y Um** directly, as on the SLM and observer. The physical sensor extent then equals resolution times pitch. **Sensor Size** remains the default for compatibility with existing levels; in that mode **Sensor Width/Height Mm** determine pitch by division by resolution. Inactive settings are retained but do not affect reconstruction.
6. Set **Pupil Resolution X/Y** for lens integration. These counts are independent of sensor resolution. Start with a small sensor and pupil while configuring the scene, then increase pupil resolution and compare results until they converge. See the sampling limits below.
7. On the reconstructor choose **Camera (Thin Lens)**, select **CPU** or **Docker (TCP/CUDA)**, and click **Reconstruct**. Docker uses the same default endpoint as the solver and observer, `127.0.0.1:7000`; no server mode switch is needed.
8. After **Ready**, select the camera. **Preview Type = Reconstructed Optical Field** displays the reconstructed sensor. **Preview Mode = Intensity** is the default; **Amplitude** and **Phase** are also available. **Geometric Camera View** selects the ordinary Cine Camera preview. Preview selection changes display only and does not rerun propagation.

Blueprint can set the reflected camera `Parameters` struct and reconstructor `Parameters.Mode`/backend, then call `StartReconstruction`. **Auto Reconstruct** watches the consumed SLM, light, camera lens pose, lens parameters, sampling, and active phase revision. Changing an input or destination while a job runs rejects the old result; cancellation retains the previous field. Preview settings and unused observer parameters do not invalidate a camera job.

### Editing the PreviewCamera component

On a placed camera actor, select **PreviewCamera** in the Components list. Its Details controls update the same optical parameters used by CPU and Docker reconstruction:

| PreviewCamera control | Optical parameter |
| --- | --- |
| Current Focal Length | Focal Length Mm |
| Current Aperture | F Number |
| Focus Settings → Manual Focus Distance | Focus Distance Mm |

Manual focus uses Unreal **centimeters** (the editor may display converted units); the actor parameter uses **millimeters**. **Current Focus Distance** is Unreal's read-only output, so edit **Manual Focus Distance** instead. Edits survive preview refreshes and construction, and Undo/Redo restores both representations. For Blueprint defaults and runtime changes, set the actor's `Parameters`. Sensor geometry remains controlled by those parameters.

## Optical calculation

The implementation uses an ideal circular thin lens. Its pupil diameter is `D = f / FNumber`, and its sensor distance is `v = f / (1 - f/s)`, where `f` is focal length and `s` is focus distance. Across the pupil the lens multiplies the incident complex field by `exp(-i*k*(y²+z²)/(2*f))`; the circular aperture blocks points outside radius `D/2`. These phase and image-distance relations follow the paraxial thin-lens model described in [TU Delft's coherent imaging notes](https://qiweb.tudelft.nl/aoi/coherentimaging/coherentimaging/).

The implementation performs two midpoint surface integrals using the [existing Rayleigh–Sommerfeld I propagator](CGH_Reconstruction.md#cpu-propagation-model):

```text
SLM phase + PlaneWave/PointSource illumination
    -> propagate complete SLM aperture to the pupil grid
    -> circular aperture and thin-lens phase
    -> propagate pupil field to the sensor grid
    -> camera complex field, preview, and optional save
```

For either propagation pass, the contribution from source point `P` to receiver `Q` is:

```text
r = |Q-P|
z = dot(source_plane_propagation_normal, Q-P)
k = 2*pi / wavelength
K(Q,P) = source_sample_area * exp(+i*k*r) * z/(2*pi*r*r) * (1/r - i*k)
```

The first pass uses the SLM normal **+X**. The second uses the direction **opposite camera optical +X**, toward the sensor. Areas are SLM pixel area and pupil quadrature-cell area respectively. Lens-pupil samples are traversed in row-major order, with compensated complex accumulation. Neither the pupil nor sensor field is normalized.

Every sensor sample retains `(Real, Imaginary)`. **Amplitude** is `|U|`; **Intensity** is `|U|²` in relative optical units. Samples represent the field at pixel centers, not integrated power over the finite sensor pixel. Exposure time, quantum efficiency, polarization response, color filters, electronic noise, saturation, lens thickness, and aberrations are outside this model. Propagation retains the Rayleigh–Sommerfeld near-field term, but the lens phase itself is still a **paraxial approximation**.

## Sensor coordinates and orientation

The lens and sensor use the camera optical-reference axes:

```text
lens_center = Camera.OpticalPositionSLMM
rotation = Camera.OpticalRotationSLM
sensor_pixel_local = (-v,
    (column - (OutputResolutionX - 1)/2) * PixelPitchXM,
    ((OutputResolutionY - 1)/2 - row) * PixelPitchYM)
sensor_pixel_slm = lens_center + rotation * sensor_pixel_local
index = row * OutputResolutionX + column
```

The stored sensor has columns along camera **+Y** and rows along **-Z**. It shows the raw, optically inverted image; no software flip is applied to make it resemble the geometric Cine Camera view. Camera roll rotates both pupil and sensor sampling axes. The solver's original camera metadata and observer coordinates retain their previous meanings.

The rectangular pupil grid spans `D × D`; its pitches are `D/PupilResolutionX` and `D/PupilResolutionY`. Only sample centers inside the circular aperture contribute to the sensor. Consequently a very coarse grid approximates circular area poorly. The full pupil sampling rectangle must lie strictly in the SLM's positive-X half-space, and every SLM pixel must be in front of the camera lens.

## Sampling, limits, and cost

The default **64 × 64 pupil** is a starting grid, not an accuracy guarantee. Large pupils, visible wavelengths, a wide sensor, or strong phase variation can require many more samples. Increasing sensor resolution alone does not refine the lens integral.

A useful paraxial aliasing estimate for a regularly sampled pupil is a sensor replica spacing of approximately `wavelength * v / pupil_pitch`. For the legacy camera defaults (50 mm focal length, f/4, 500 mm focus, a 36 mm sensor) at 532 nm, a 64-sample pupil spans 12.5 mm with about 195 µm spacing. The estimated sensor replica spacing is only **0.151 mm**, much smaller than the full sensor width. Such a default-grid result can contain severe sampling artifacts. This estimate is guidance derived from the sampled diffraction phase, not a complete convergence test for tilted planes or arbitrary SLM fields.

Use a smaller sensor region, a smaller aperture, or a finer pupil where appropriate; compare the complex field or intensity at increasing pupil resolutions. A complete 36 mm visible-light field may exceed the practical limits of this direct integrator. **Pupil Sampling Guidance** on the camera highlights this limitation. Existing SLM sampling must also resolve its incident/modulated field. The preview's normalization cannot establish physical accuracy or compare absolute brightness between separately normalized images.

CPU work grows as `SLM_pixels * pupil_grid_pixels + active_pupil_samples * sensor_pixels`. CPU output grids retain the existing 16,384-per-axis / 64M-pixel limit. The pupil has **1–2048 samples per axis**, up to 4,194,304 rectangular samples. TCP limits remain 256 MiB per frame, 16,777,216 SLM pixels, and 16,777,214 complex sensor pixels; output axes are at most 16,384. Invalid optical values and numerical overflow fail with an empty result; they are not silently clamped by the solver.

Docker computes both passes in FP64 using the visible GPUs. Each GPU evaluates complete ordered sums for its assigned output pixels; no reduction combines partial complex sums across devices. The first pass completes before the pupil field is clipped and passed to the second. Both passes retain the tuned **65,536 output pixels per tile, 128 threads per block, and 256 contributors per batch**. Cancellation and GPU failures stop the complete job and suppress publication of intermediate pupil data. Increase the request timeout for large jobs.

## Saving and reloading camera data

**Save Complex Field** on the camera saves its current valid sensor field. The same button on the reconstructor saves its last accepted destination only while that result is still active and the job is **Ready**. Queued/running work, mode changes, replaced destinations, or intervening field edits prevent saving an unrelated result.

Camera defaults are `/Game/CGHSim/CameraFields/Generated` for the Unreal asset and `Saved/CGHSim/CameraFields` for exports. Each save creates a reusable `UCGHComplexFieldAsset`, full-precision complex `.bin`, `.json` metadata, and `_phase.png`, `_amplitude.png`, and `_intensity.png` previews. JSON identifies **camera-sensor-local** coordinates. The binary/asset sample layout remains compatible with the observer field utilities. The metadata describes sensor sampling; it is not a complete archived reconstruction request or lens prescription.

The generated Content folder and exports under `Saved` are ignored by Git. Saving is explicit: reconstructing, changing preview mode, or saving the level does not export data. To reload, select **Stored Complex Field** and click **Load Stored Complex Field** on a camera with matching resolution and effective pixel pitches. Loading never resamples the field or reruns the optics.

## Verification

The camera tests cover independent world-frame complex integrals, a single-source analytical case, pupil aperture and lens parameter effects, translated/tilted/rolled cameras, point-source illumination, thin-lens image position, focus and pupil convergence on a resolved fixture, invalid inputs, and cancellation during either propagation stage. Actor tests cover publication, destination and mode switching, stale results, automatic updates, preview selection, and persistence. CPU/CUDA comparisons and one-/two-GPU checks exercise the shared Docker service alongside the existing solver and observer tests.

Recorded on **2026-09-22**:

- Linux Development **Editor and Game builds passed**, and the CUDA 12.9.2 Docker image built successfully.
- **116/116 headless Unreal tests passed**, with zero test warnings, failures, or skips. The run supplied the standalone server and the real two-V100 Docker endpoint, so transport and numerical comparisons executed.
- **2/2 Vulkan/Slate render tests passed**. They check camera and observer Phase/Amplitude/Intensity pixel values, switch the already-open camera inset to the geometric viewport, and switch back to the same intensity field.
- **8/8 CTest suites passed**: wire codec, TCP integration, PointFocus, observer reconstruction, camera reconstruction, and each compute path's multi-GPU suite. Camera checks include independent numerical references and bit-identical single-/dual-GPU results.
- SHA-256 comparison preserved all **24 pre-existing Content files**, including the user's modified map and mesh; tests created no additional Content assets.

Local reports are under `Saved/Automation/CGHCameraFinal` and `Saved/Automation/CGHCameraPreviewRender`. Preview PNGs are under `Saved/Automation/CGHCameraPreview`. Build, backend test, and Unreal logs are archived as `Saved/Logs/CGHCamera*_2026-09-22.log`. These tests establish the covered numerical fixtures and application behavior; they do not establish convergence for arbitrary full-resolution camera scenes or validate a real lens prescription. No camera performance benchmark or packaged build was run.

Lens-control follow-up on **2026-09-22**: Editor/Game builds and **17/17 focused headless tests passed without warnings**. The tests cover native and existing Blueprint component edits, interactive and committed edits, focus-unit conversion, reconstruction snapshots, preservation of untouched double values, and actual Undo/Redo. A separate Vulkan run passed all **18 selected tests**, including the rendered camera preview; its stale-input stress test logged one RHI virtual-memory-budget warning. All **25 Content files present at the start of this follow-up** were preserved. Reports are in `Saved/Automation/CGHCameraLensEditing` and `Saved/Automation/CGHCameraLensEditingRendered`; logs use `Saved/Logs/CGHCameraLens*_2026-09-22.log`.
