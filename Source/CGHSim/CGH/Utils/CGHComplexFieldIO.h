#pragma once

#include "CoreMinimal.h"
#include "CGH/Types/CGHComplexField.h"

/** Paths are filled only after the asset, numeric export, and all three grayscale PNGs are saved. */
struct CGHSIM_API FCGHComplexFieldSaveResult
{
	FString AssetPath;
	FString AssetFilename;
	FString BinaryFilename;
	FString MetadataFilename;
	FString PhaseImageFilename;
	FString AmplitudeImageFilename;
	FString IntensityImageFilename;
	FString Error;
};

namespace CGHComplexFieldIO
{
	enum class ECoordinateFrame : uint8 { ObserverPlane, CameraSensor };
	/**
	 * Editor/game-thread save of an independent asset plus headerless little-endian complex128:
	 * row-major float64 pairs [real, imaginary], a UTF-8 JSON sidecar, and three pixel-for-pixel G8 PNGs.
	 * Phase, amplitude, and intensity images use CGHComplexFieldPreview's mappings, independent of preview mode.
	 * The asset and binary preserve the full complex samples; only the PNGs normalize and quantize.
	 * Every save has a unique name. Existing files are never replaced, and failed saves attempt to
	 * remove only files created by this call. Failure returns empty paths with an explanatory Error.
	 * AssetFolder is a /Game content folder; relative RawDirectory is relative to the project.
	 * The input field and its revision are unchanged.
	 */
	CGHSIM_API bool Save(const FCGHComplexField& Field, double PixelPitchXM, double PixelPitchYM,
		const FString& AssetFolder, const FString& RawDirectory, const FText& Label,
		FCGHComplexFieldSaveResult& Out, ECoordinateFrame CoordinateFrame = ECoordinateFrame::ObserverPlane);
}
