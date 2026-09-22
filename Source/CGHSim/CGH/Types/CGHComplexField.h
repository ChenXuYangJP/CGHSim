#pragma once

#include "CoreMinimal.h"
#include "CGHComplexField.generated.h"

UENUM(BlueprintType)
enum class ECGHObserverPreviewMode : uint8
{
	Phase UMETA(DisplayName = "Phase"),
	Amplitude UMETA(DisplayName = "Amplitude"),
	Intensity UMETA(DisplayName = "Intensity")
};

/** Double-precision scalar optical field value, Real + i * Imaginary. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHComplexSample
{
	GENERATED_BODY()

	FCGHComplexSample() = default;
	FCGHComplexSample(double InReal, double InImaginary) : Real(InReal), Imaginary(InImaginary) {}

	UPROPERTY(BlueprintReadWrite, Category = "CGH|Field")
	double Real = 0.0;

	UPROPERTY(BlueprintReadWrite, Category = "CGH|Field")
	double Imaginary = 0.0;

	bool operator==(const FCGHComplexSample& Other) const
	{
		return Real == Other.Real && Imaginary == Other.Imaginary;
	}
};

/** Row-major complex samples: columns point along observer +Y, rows along -Z. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHComplexField
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "CGH|Field")
	int32 ResolutionX = 0;

	UPROPERTY(BlueprintReadWrite, Category = "CGH|Field")
	int32 ResolutionY = 0;

	UPROPERTY(BlueprintReadWrite, Category = "CGH|Field")
	TArray<FCGHComplexSample> Samples;

	/** Assigned by the receiving observer whenever samples change. */
	UPROPERTY()
	uint64 Revision = 0;

	/** Full validation; receivers cache validity to avoid per-frame sample scans. */
	bool IsValid(FString* OutError = nullptr) const;
};

USTRUCT(BlueprintType)
struct CGHSIM_API FCGHObserverPlaneParameters
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Observer Plane", meta = (ClampMin = "1"))
	int32 ResolutionX = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Observer Plane", meta = (ClampMin = "1"))
	int32 ResolutionY = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Observer Plane", meta = (ClampMin = "0.001", Units = "um"))
	double PixelPitchXUm = 8.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Observer Plane", meta = (ClampMin = "0.001", Units = "um"))
	double PixelPitchYUm = 8.0;
};

/** SI sampling geometry in the SLM's rigid local frame. Actor scale never changes optics. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHObserverPlaneDescription
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Observer Plane")
	int32 ResolutionX = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Observer Plane")
	int32 ResolutionY = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Observer Plane", meta = (Units = "m"))
	double PixelPitchXM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Observer Plane", meta = (Units = "m"))
	double PixelPitchYM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Observer Plane", meta = (Units = "m"))
	FVector PositionSLMM = FVector::ZeroVector;

	/** Rotates observer-local vectors into SLM-local vectors; observer normal is local +X. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Observer Plane")
	FQuat RotationSLM = FQuat::Identity;
};
