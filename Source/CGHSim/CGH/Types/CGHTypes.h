#pragma once

#include "CoreMinimal.h"
#include "CGHTypes.generated.h"

UENUM(BlueprintType)
enum class ECGHTargetType : uint8
{
	Point UMETA(DisplayName = "Point"),
	Mesh UMETA(DisplayName = "Mesh (Sampling Not Implemented)")
};

UENUM(BlueprintType)
enum class ECGHSLMModulationType : uint8
{
	PhaseOnly UMETA(DisplayName = "Phase Only"),
	Complex UMETA(DisplayName = "Complex Amplitude")
};

UENUM(BlueprintType)
enum class ECGHGenerationState : uint8
{
	NotImplemented,
	Ready,
	Computing,
	Completed,
	Failed
};

UENUM(BlueprintType)
enum class ECGHSourceType : uint8
{
	PlaneWave,
	PointSource
};

/** Editable configuration only. All optical scalar parameters retain double precision. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHSLMParameters
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SLM", meta = (ClampMin = "1"))
	int32 ResolutionX = 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SLM", meta = (ClampMin = "1"))
	int32 ResolutionY = 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SLM", meta = (ClampMin = "0.001", Units = "um"))
	double PixelPitchXUm = 8.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SLM", meta = (ClampMin = "0.001", Units = "um"))
	double PixelPitchYUm = 8.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SLM")
	ECGHSLMModulationType ModulationType = ECGHSLMModulationType::PhaseOnly;
};

USTRUCT(BlueprintType)
struct CGHSIM_API FCGHTargetParameters
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
	ECGHTargetType TargetType = ECGHTargetType::Point;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target", meta = (ClampMin = "0.0"))
	double Amplitude = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target", meta = (Units = "rad"))
	double InitialPhaseRad = 0.0;
};

USTRUCT(BlueprintType)
struct CGHSIM_API FCGHLightParameters
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	ECGHSourceType SourceType = ECGHSourceType::PlaneWave;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light", meta = (ClampMin = "0.001", Units = "nm"))
	double WavelengthNm = 532.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light", meta = (ClampMin = "0.0"))
	double Amplitude = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light", meta = (Units = "rad"))
	double InitialPhaseRad = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light", meta = (Units = "deg"))
	double PolarizationAngleDeg = 0.0;
};

/** Source of truth for optics; the float-valued Cine Camera is only a preview. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHCameraParameters
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.001", Units = "mm"))
	double FocalLengthMm = 50.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.001"))
	double FNumber = 4.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.001", Units = "mm"))
	double FocusDistanceMm = 1000.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.001", Units = "mm"))
	double SensorWidthMm = 36.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.001", Units = "mm"))
	double SensorHeightMm = 24.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "1"))
	int32 OutputResolutionX = 1920;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "1"))
	int32 OutputResolutionY = 1080;
};

/** Parameter snapshot only; future solver input must also export poses and SI units. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHSceneDescription
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH")
	FCGHSLMParameters SLM;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH")
	FCGHCameraParameters Camera;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH")
	FCGHLightParameters Light;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH")
	TArray<FCGHTargetParameters> Targets;
};
