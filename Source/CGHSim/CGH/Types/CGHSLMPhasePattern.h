#pragma once

#include "CoreMinimal.h"
#include "CGHSLMPhasePattern.generated.h"

/** Solver phase values in radians. Row-major index is Y * ResolutionX + X; row zero is the top row. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHSLMPhasePattern
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "CGH|SLM|Phase")
	int32 ResolutionX = 0;

	UPROPERTY(BlueprintReadWrite, Category = "CGH|SLM|Phase")
	int32 ResolutionY = 0;

	/** Full-resolution phase data; intentionally omitted from the Details panel. */
	UPROPERTY(BlueprintReadWrite, Category = "CGH|SLM|Phase")
	TArray<double> PhaseRad;

	/** Changes when the owning SLM accepts or clears phase data. */
	UPROPERTY()
	uint64 Revision = 0;
};