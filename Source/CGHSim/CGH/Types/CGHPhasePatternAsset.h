#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "CGH/Types/CGHSLMPhasePattern.h"
#include "CGHPhasePatternAsset.generated.h"

/** A saved phase grid that can be explicitly loaded into an SLM, with optional nearest-neighbor resizing. */
UCLASS(BlueprintType)
class CGHSIM_API UCGHPhasePatternAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phase Pattern")
	FText PatternLabel = FText::GetEmpty();

	/** Labels this asset as demonstration data rather than a solver-produced result. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Phase Pattern")
	bool bIsPreviewPattern = false;

	/** Validate and copy a full row-major phase grid. Invalid input preserves the current payload. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Phase")
	bool SetPattern(const FCGHSLMPhasePattern& Pattern);

	/** Read without exposing mutable Blueprint access to the stored payload. */
	const FCGHSLMPhasePattern& GetPattern() const { return StoredPattern; }

	UFUNCTION(BlueprintPure, Category = "CGH|Phase")
	int32 GetResolutionX() const { return StoredPattern.ResolutionX; }

	UFUNCTION(BlueprintPure, Category = "CGH|Phase")
	int32 GetResolutionY() const { return StoredPattern.ResolutionY; }

	UFUNCTION(BlueprintPure, Category = "CGH|Phase")
	int32 GetSampleCount() const { return StoredPattern.PhaseRad.Num(); }

	/** Validate the stored payload without changing this asset or its last-write diagnostic. */
	UFUNCTION(BlueprintPure, Category = "CGH|Phase")
	bool HasValidPattern() const;

	/** Error from the last rejected SetPattern call or initial asset-load validation. */
	UFUNCTION(BlueprintPure, Category = "CGH|Phase")
	FString GetValidationError() const { return ValidationError; }

	/**
	 * Copy/resample for explicit preset loading only; this does not calculate a hologram.
	 * Nearest source texel centers preserve phase discontinuities and top-to-bottom row order.
	 * Validate both source and destination before allocating. Failure leaves OutPattern unchanged.
	 * The output revision is zero; the destination SLM assigns its own publication revision.
	 */
	bool BuildPatternForResolution(int32 ResolutionX, int32 ResolutionY,
		FCGHSLMPhasePattern& OutPattern, FString& OutError) const;

protected:
	virtual void PostLoad() override;

private:
	/** Serialized with the asset, but hidden from Details and direct Blueprint mutation. */
	UPROPERTY()
	FCGHSLMPhasePattern StoredPattern;

	UPROPERTY(VisibleAnywhere, Category = "Phase Pattern")
	int32 StoredResolutionX = 0;

	UPROPERTY(VisibleAnywhere, Category = "Phase Pattern")
	int32 StoredResolutionY = 0;

	UPROPERTY(VisibleAnywhere, Category = "Phase Pattern")
	int32 StoredSampleCount = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Phase Pattern")
	FString ValidationError;

	void UpdateSummary();
};
