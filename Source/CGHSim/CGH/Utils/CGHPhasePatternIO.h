#pragma once

#include "CoreMinimal.h"
#include "CGH/Types/CGHSLMPhasePattern.h"

/** Paths are filled only after the Unreal asset, numeric export, and grayscale PNG are saved successfully. */
struct CGHSIM_API FCGHPhaseSaveResult
{
	FString AssetPath;
	FString AssetFilename;
	FString BinaryFilename;
	FString MetadataFilename;
	FString ImageFilename;
	FString Error;
};

namespace CGHPhasePatternIO
{
	/**
	 * Editor/game-thread save of an independent asset plus headerless little-endian float64 radians
	 * with a UTF-8 JSON sidecar and a pixel-for-pixel 8-bit grayscale PNG matching PhaseToGray.
	 * Every save has a unique name; existing files are never replaced. The asset/raw samples retain
	 * full precision; only the PNG wraps and quantizes phase. The input and revision are unchanged.
	 * Failed saves attempt to remove
	 * only files created by this call and return empty result paths with an explanatory Error.
	 * AssetFolder is a /Game content folder; relative RawDirectory is relative to the project.
	 */
	CGHSIM_API bool Save(const FCGHSLMPhasePattern& Pattern, double PixelPitchXM, double PixelPitchYM,
		const FString& AssetFolder, const FString& RawDirectory, const FText& Label,
		bool bIsPreviewPattern, FCGHPhaseSaveResult& Out);
}
