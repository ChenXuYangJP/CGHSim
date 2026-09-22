#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "CGH/Types/CGHComplexField.h"
#include "CGHComplexFieldAsset.generated.h"

/** A saved complex optical field with its physical sampling grid. */
UCLASS(BlueprintType)
class CGHSIM_API UCGHComplexFieldAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Complex Field")
	FText FieldLabel = FText::GetEmpty();

	/** Validate and copy a complete field and sampling pitches in meters. Invalid input preserves the payload. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Field")
	bool SetField(const FCGHComplexField& Field, double PixelPitchXM, double PixelPitchYM);

	/** Read without copying or exposing mutable Blueprint access to the stored samples. */
	const FCGHComplexField& GetField() const { return StoredField; }

	UFUNCTION(BlueprintPure, Category = "CGH|Field")
	int32 GetResolutionX() const { return StoredField.ResolutionX; }

	UFUNCTION(BlueprintPure, Category = "CGH|Field")
	int32 GetResolutionY() const { return StoredField.ResolutionY; }

	UFUNCTION(BlueprintPure, Category = "CGH|Field")
	int32 GetSampleCount() const { return StoredField.Samples.Num(); }

	UFUNCTION(BlueprintPure, Category = "CGH|Field")
	double GetPixelPitchXM() const { return StoredPixelPitchXM; }

	UFUNCTION(BlueprintPure, Category = "CGH|Field")
	double GetPixelPitchYM() const { return StoredPixelPitchYM; }

	/** Validate stored data without changing this asset or its last-write diagnostic. */
	UFUNCTION(BlueprintPure, Category = "CGH|Field")
	bool HasValidField() const;

	/** Error from the last rejected SetField call or initial asset-load validation. */
	UFUNCTION(BlueprintPure, Category = "CGH|Field")
	FString GetValidationError() const { return ValidationError; }

protected:
	virtual void PostLoad() override;

private:
	/** Serialized bulk storage is hidden from Details and direct Blueprint mutation. */
	UPROPERTY()
	FCGHComplexField StoredField;

	UPROPERTY(VisibleAnywhere, Category = "Complex Field")
	int32 StoredResolutionX = 0;

	UPROPERTY(VisibleAnywhere, Category = "Complex Field")
	int32 StoredResolutionY = 0;

	UPROPERTY(VisibleAnywhere, Category = "Complex Field")
	int32 StoredSampleCount = 0;

	UPROPERTY(VisibleAnywhere, Category = "Complex Field", meta = (Units = "m"))
	double StoredPixelPitchXM = 0.0;

	UPROPERTY(VisibleAnywhere, Category = "Complex Field", meta = (Units = "m"))
	double StoredPixelPitchYM = 0.0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Complex Field")
	FString ValidationError;

	void UpdateSummary();
};
