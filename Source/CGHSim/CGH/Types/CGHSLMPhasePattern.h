#pragma once

#include "CoreMinimal.h"
#include "CGHSLMPhasePattern.generated.h"

/**
 * Solver phase values in radians: PhaseRad[row * ResolutionX + column].
 * Columns increase along SLM-local +Y; rows increase along -Z; local +X is the optical normal.
 * Pixel centers in meters: X = 0,
 * Y = (column - (ResolutionX - 1) / 2.0) * PixelPitchXM,
 * Z = ((ResolutionY - 1) / 2.0 - row) * PixelPitchYM, using the SLM description's pitches.
 * Row zero is the canonical image's top row (maximum Z); column zero is its left (minimum Y).
 * Canonical +X-side front image: +Y right, +Z up. An ordinary Unreal camera looking -X
 * with +Z up needs a horizontal display mirror to match it. See Docs/SLM_Pixel_Coordinates.md.
 */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHSLMPhasePattern
{
	GENERATED_BODY()

	/** Horizontal column count (SLM-local Y extent); X names the image/grid axis. */
	UPROPERTY(BlueprintReadWrite, Category = "CGH|SLM|Phase")
	int32 ResolutionX = 0;

	/** Vertical row count (SLM-local Z extent); increasing row points along -Z. */
	UPROPERTY(BlueprintReadWrite, Category = "CGH|SLM|Phase")
	int32 ResolutionY = 0;

	/** Full-resolution phase data; intentionally omitted from the Details panel. */
	UPROPERTY(BlueprintReadWrite, Category = "CGH|SLM|Phase")
	TArray<double> PhaseRad;

	/** Changes when the owning SLM accepts or clears phase data. */
	UPROPERTY()
	uint64 Revision = 0;
};