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
	int32 ResolutionX = 4096;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SLM", meta = (ClampMin = "1"))
	int32 ResolutionY = 4096;

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
	double FocusDistanceMm = 500.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.001", Units = "mm"))
	double SensorWidthMm = 36.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.001", Units = "mm"))
	double SensorHeightMm = 24.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "1"))
	int32 OutputResolutionX = 1920;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "1"))
	int32 OutputResolutionY = 1080;
};

/** SLM sampling geometry in SI units, independent of actor scale. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHSLMDescription
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM")
	int32 ResolutionX = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM")
	int32 ResolutionY = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM", meta = (Units = "m"))
	double PixelPitchXM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM", meta = (Units = "m"))
	double PixelPitchYM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM", meta = (Units = "m"))
	double ActiveWidthM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM", meta = (Units = "m"))
	double ActiveHeightM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM")
	ECGHSLMModulationType ModulationType = ECGHSLMModulationType::PhaseOnly;
};

/** Reconstruction illumination in SI units and the SLM's rigid local frame. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHReconstructionLightDescription
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light", meta = (Units = "m"))
	double WavelengthM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light")
	double Amplitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light", meta = (Units = "rad"))
	double InitialPhaseRad = 0.0;

	/** Unit propagation direction expressed in SLM-local axes. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light")
	FVector DirectionSLM = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light")
	ECGHSourceType SourceType = ECGHSourceType::PlaneWave;

	/** Point-source position in meters relative to the SLM actor origin. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light", meta = (Units = "m"))
	FVector PositionSLMM = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light", meta = (Units = "rad"))
	double PolarizationAngleRad = 0.0;
};

/** One target in SI units and the SLM's rigid local frame. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHTargetDescription
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target", meta = (Units = "m"))
	FVector PositionSLMM = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target")
	double Amplitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target", meta = (Units = "rad"))
	double PhaseRad = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target")
	ECGHTargetType TargetType = ECGHTargetType::Point;
};

/** Optical camera data in SI units; pose comes from the lens reference component. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHCameraDescription
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (Units = "m"))
	FVector OpticalPositionSLMM = FVector::ZeroVector;

	/** Unit optical forward direction expressed in SLM-local axes. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	FVector ForwardDirectionSLM = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (Units = "m"))
	double FocalLengthM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	double FNumber = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (Units = "m"))
	double FocusDistanceM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (Units = "m"))
	double SensorWidthM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (Units = "m"))
	double SensorHeightM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	int32 OutputResolutionX = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	int32 OutputResolutionY = 0;
};

/**
 * Solver snapshot using meters and radians. Positions and directions use the
 * CGHSLMActor's origin and rotation: +X optical normal, +Y horizontal, +Z vertical.
 * Position X is signed depth: positive along the normal, negative behind the SLM plane.
 * Reference actor scale is ignored, so local positions retain physical distances.
 * Descriptions for missing actors retain zero values; targets require an SLM.
 */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHSceneDescription
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH")
	int32 SchemaVersion = 1;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH")
	FCGHSLMDescription SLM;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH")
	FCGHReconstructionLightDescription ReconstructionLight;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH")
	TArray<FCGHTargetDescription> Targets;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH")
	FCGHCameraDescription Camera;
};
