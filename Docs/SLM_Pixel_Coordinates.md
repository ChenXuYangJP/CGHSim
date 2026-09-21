# SLM pixel coordinate convention

Recorded **2026-09-21**. This is the canonical mapping between the phase array, image grid, and SLM-local physical coordinates. Public parameter names remain unchanged.

## Array and local coordinates

```text
Array layout:
    PhaseRad[row * ResolutionX + column]

Pixel indices:
    column in [0, ResolutionX - 1]
    row    in [0, ResolutionY - 1]

SLM local frame:
    +X = optical normal
    +Y = horizontal right in the canonical image
    +Z = vertical up in the canonical image

Pixel index directions:
    increasing column -> +Y
    increasing row    -> -Z

Pixel-center coordinates (meters):
    X = 0
    Y = (column - (ResolutionX - 1) / 2.0) * PixelPitchXM
    Z = ((ResolutionY - 1) / 2.0 - row) * PixelPitchYM
```

Use floating-point division (`2.0` in C++) so even resolutions retain half-pixel offsets. The origin is the active-area center. An odd-sized axis has a pixel center at zero; an even-sized axis places zero between its two central pixel centers. A one-pixel axis has its sole center at zero.

The canonical top-left pixel has minimum Y and maximum Z; the bottom-right has maximum Y and minimum Z. The active area includes the half-pixel border beyond the outermost centers:

```text
ActiveWidthM  = ResolutionX * PixelPitchXM    (extent along local Y)
ActiveHeightM = ResolutionY * PixelPitchYM    (extent along local Z)

Y boundaries = +/- ActiveWidthM / 2.0
Z boundaries = +/- ActiveHeightM / 2.0
```

For a 4-column by 2-row grid, Y centers are `[-1.5, -0.5, +0.5, +1.5] * PixelPitchXM` and Z centers are `[+0.5, -0.5] * PixelPitchYM`. The pitches may differ. To place a center in the Unreal world, convert meters to centimeters and apply the SLM actor's translation and rotation, ignoring its scale, as for other optical coordinates.

## Parameter names describe grid axes

| Existing fields | Image/grid meaning | SLM-local physical axis |
| --- | --- | --- |
| `ResolutionX`, `PixelPitchXM` (editable: `PixelPitchXUm`) | Horizontal columns and positive column spacing | +Y |
| `ResolutionY`, `PixelPitchYM` (editable: `PixelPitchYUm`) | Vertical rows and positive row spacing | Z; increasing row travels toward -Z |

The resolution/pitch X/Y suffixes identify image axes, not the local optical X/Y axes. Pitch is positive; the vertical minus sign belongs in the coordinate formula. Use `row` and `column` when describing indices to distinguish them from local X/Y/Z.

## Canonical front view and Unreal cameras

The requested canonical SLM front view is:

```text
viewed from the +X side toward -X
+Y appears to the right
+Z appears upward
```

This is the project's **2D image/display convention**. The phase inset displays columns left to right and rows top to bottom, independent of the editor viewport or optical camera pose.

There is a handedness conflict if that rule is interpreted as an ordinary unmirrored Unreal camera view. With +Z up, a camera on the SLM's +X side looking toward -X has screen-right along **-Y**, so local +Y appears left. For an unrotated SLM, pitch/roll zero and yaw 180 degrees give camera forward/right/up axes `(-X, -Y, +Z)`. Unreal maps camera-local Y to screen horizontal. A camera on the -X side looking toward +X shows +Y to the right.

To reproduce the requested canonical image with a +X-side Unreal camera, horizontally mirror its displayed image. Keep the phase array and optical coordinate formulas unchanged. The current SLM inset is a custom Slate image and already displays the canonical ordering; its preview-view metadata does not implement a physical front camera. The starter scene's ordinary optical camera is therefore not evidence of the SLM inset's orientation.

This was checked against installed UE 5.8.2 source: `Core/Public/Math/RotationTranslationMatrix.h` defines the camera basis, and `Engine/Private/LocalPlayer.cpp` constructs the view-axis permutation in `GetProjectionData`.

## Current-code audit

Source audit on **2026-09-21** found no row/column transpose, sign inversion, or axis swap in the implemented phase-data and active-dimension paths. The front-camera conflict above remains an explicit integration constraint.

| Path | Finding |
| --- | --- |
| [`FCGHSLMPhasePattern`](../Source/CGHSim/CGH/Types/CGHSLMPhasePattern.h), [`SetPhasePattern` and `GeneratePreviewPhaseRamp`](../Source/CGHSim/CGH/Actors/CGHSLMActor.cpp) | Flat row-major storage and order-preserving publication. Ramp-loop X/Y mean column/row; the ramp increases with column (+Y). Source comments now make the spatial contract explicit. |
| [`BuildPatternForResolution`](../Source/CGHSim/CGH/Types/CGHPhasePatternAsset.cpp) | Nearest-source-center resampling retains row/column order without mirroring or transposing. |
| [`BuildGrayscale`](../Source/CGHSim/CGH/Utils/CGHPhasePreview.cpp), [`UCGHSLMPreviewComponent`](../Source/CGHSim/CGH/Components/CGHSLMPreviewComponent.cpp) | Conversion, texture upload, and Slate drawing preserve the grid order. Inset aspect ratio uses pixel counts independently of pitch. |
| [`create_cgh_phase_sample.py`](../Scripts/create_cgh_phase_sample.py) | Outer row loop, inner column loop; asymmetric marker at canonical top-left (minimum Y, maximum Z). |
| SLM active mesh, [`ACGHWorkbenchActor`](../Source/CGHSim/CGH/Actors/CGHWorkbenchActor.cpp) | Width uses resolution/pitch X along local Y; height uses resolution/pitch Y along local Z. Scene export preserves signed actor-local coordinates and SI units. |

No existing solver or utility computes SLM pixel-center positions, and no phase texture is mapped onto the world-space SLM mesh. The center formulas are a contract for future consumers. Existing scene tests cover signed optical positions; phase tests cover storage, resampling, texture bytes, and display ordering. They do not verify pixel-center positions or a +X-side camera-to-image mapping. The rendered 4-by-2 grayscale fixture is symmetric under 180-degree rotation, so it alone cannot detect a combined horizontal and vertical flip.

This update changes records and source comments only. Property names, scene schema, phase values, preview behavior, and saved assets are preserved.

Verification: `CGHSimEditor Linux Development` build passed and all **26 headless CGH automation tests passed** (exit code 0; zero test failures, warnings, or not-run tests). Report: `Saved/Automation/CGHSLMCoordinates/index.json`; logs: `Saved/Logs/CGHSLMCoordinatesEditorBuild_2026-09-21.log` and `Saved/Logs/CGHSLMCoordinatesTests_2026-09-21.log`. Documentation links and `git diff --check` passed. No new tests were added; the real-RHI render test was not rerun, and the coverage gaps above remain.
