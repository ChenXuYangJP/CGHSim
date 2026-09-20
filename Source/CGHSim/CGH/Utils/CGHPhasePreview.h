#pragma once

#include "CGH/Types/CGHSLMPhasePattern.h"

/** Runtime-safe phase validation and conversion, independent of Slate and texture resources. */
namespace CGHPhasePreview
{
	inline constexpr int32 MaximumAxisResolution = 16384;
	inline constexpr int64 MaximumPixelCount = 64ll * 1024 * 1024;

	/** Require matching positive dimensions, supported image size, exact sample count and finite phases. */
	CGHSIM_API bool ValidatePattern(const FCGHSLMPhasePattern& Pattern, int32 ExpectedX, int32 ExpectedY,
		FString& OutError);

	/**
	 * Wrap radians into [0, 2*pi), then linearly quantize to [0, 255].
	 * Zero and whole cycles are black; phases approaching 2*pi are white.
	 * Negative phases wrap identically. Nonfinite input returns black.
	 */
	CGHSIM_API uint8 PhaseToGray(double PhaseRad);

	/**
	 * Convert a valid pattern to opaque grayscale FColor pixels without changing row/column order.
	 * The caller caches the result by revision; this function allocates only when explicitly called.
	 * Invalid input returns false, clears OutPixels and writes an explanation to OutError.
	 */
	CGHSIM_API bool BuildGrayscale(const FCGHSLMPhasePattern& Pattern, TArray<FColor>& OutPixels,
		FString& OutError);
}
