#pragma once

#include "CoreMinimal.h"
#include "CGHTypes.generated.h"

UENUM(BlueprintType)
enum class ECGHTargetType : uint8
{
	Point UMETA(DisplayName = "Point"),
	Mesh UMETA(DisplayName = "Mesh")
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

	/** Horizontal column count along SLM-local Y. X names the image/grid axis. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SLM", meta = (ClampMin = "1"))
	int32 ResolutionX = 256;

	/** Vertical row count along SLM-local Z; increasing row points toward -Z. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SLM", meta = (ClampMin = "1"))
	int32 ResolutionY = 256;

	/** Positive horizontal column spacing in micrometers, along SLM-local +Y. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SLM", meta = (ClampMin = "0.001", Units = "um"))
	double PixelPitchXUm = 8.0;

	/** Positive vertical row spacing in micrometers; row steps point along SLM-local -Z. */
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

	/** Field amplitude of a point, or of each sampled mesh point (not total mesh power). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target", meta = (ClampMin = "0.0"))
	double Amplitude = 1.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target", meta = (Units = "rad", ForceUnits = "rad"))
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

	/** Incident field amplitude for PlaneWave; PointSource reconstruction uses amplitude at one meter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light", meta = (ClampMin = "0.0"))
	double Amplitude = 1.0;

	/** PlaneWave phase at the SLM origin; PointSource emitted spherical-wave phase offset. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light", meta = (Units = "rad", ForceUnits = "rad"))
	double InitialPhaseRad = 0.0;

	/** Exported in radians; validated but unused by the scalar PointFocus solver. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light", meta = (Units = "deg"))
	double PolarizationAngleDeg = 0.0;
};

/** Select which sensor dimensions are authoritative; the inactive values remain stored unchanged. */
UENUM(BlueprintType)
enum class ECGHCameraSensorSampling : uint8
{
	SensorSize UMETA(DisplayName = "Sensor Size"),
	PixelPitch UMETA(DisplayName = "Pixel Pitch")
};

/** Source of truth for optics; the float-valued Cine Camera is only a derived geometric preview. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHCameraParameters
{
	GENERATED_BODY()

	/** Thin-lens focal length; the circular pupil diameter is focal length divided by f-number. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.001", Units = "mm"))
	double FocalLengthMm = 50.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.001"))
	double FNumber = 4.0;

	/** Object-side focus distance measured from the lens. Must exceed focal length; sensor distance is f/(1-f/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera", meta = (ClampMin = "0.001", Units = "mm"))
	double FocusDistanceMm = 500.0;

	/** Sensor Size preserves existing sensor extents and derives pixel pitch. Pixel Pitch derives extents from pitch times resolution. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sensor")
	ECGHCameraSensorSampling SensorSampling = ECGHCameraSensorSampling::SensorSize;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sensor", meta = (ClampMin = "0.001", Units = "mm", EditCondition = "SensorSampling == ECGHCameraSensorSampling::SensorSize"))
	double SensorWidthMm = 36.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sensor", meta = (ClampMin = "0.001", Units = "mm", EditCondition = "SensorSampling == ECGHCameraSensorSampling::SensorSize"))
	double SensorHeightMm = 24.0;

	/** Sensor columns along lens-local +Y. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sensor", meta = (ClampMin = "1"))
	int32 OutputResolutionX = 1920;

	/** Sensor rows along lens-local -Z. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sensor", meta = (ClampMin = "1"))
	int32 OutputResolutionY = 1080;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sensor", meta = (ClampMin = "0.001", Units = "um", EditCondition = "SensorSampling == ECGHCameraSensorSampling::PixelPitch"))
	double PixelPitchXUm = 18.75;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Sensor", meta = (ClampMin = "0.001", Units = "um", EditCondition = "SensorSampling == ECGHCameraSensorSampling::PixelPitch"))
	double PixelPitchYUm = 22.2222222222222;

	/** Midpoint integration columns across the circular pupil (1 to 2048). Coarse grids can alias optical phase; increase until the result converges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Pupil Sampling", meta = (ClampMin = "1", ClampMax = "2048"))
	int32 PupilResolutionX = 64;

	/** Midpoint integration rows across the circular pupil (1 to 2048). More samples increase reconstruction cost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Pupil Sampling", meta = (ClampMin = "1", ClampMax = "2048"))
	int32 PupilResolutionY = 64;
};

/**
 * SLM sampling geometry in SI units, independent of actor scale.
 * Resolution/pitch X and Y are image/grid axes: horizontal local Y and vertical local Z.
 * Pixel centers/order are defined by FCGHSLMPhasePattern and Docs/SLM_Pixel_Coordinates.md.
 */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHSLMDescription
{
	GENERATED_BODY()

	/** Horizontal column count along SLM-local Y. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM")
	int32 ResolutionX = 0;

	/** Vertical row count along SLM-local Z; increasing row points toward -Z. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM")
	int32 ResolutionY = 0;

	/** Positive column spacing in meters along SLM-local +Y, not optical X. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM", meta = (Units = "m"))
	double PixelPitchXM = 0.0;

	/** Positive row spacing in meters; increasing row moves along SLM-local -Z. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM", meta = (Units = "m"))
	double PixelPitchYM = 0.0;

	/** Full local Y extent: ResolutionX * PixelPitchXM, including pixel half-widths at the edges. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SLM", meta = (Units = "m"))
	double ActiveWidthM = 0.0;

	/** Full local Z extent: ResolutionY * PixelPitchYM, including pixel half-heights at the edges. */
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

	/** Nonnegative field amplitude (at one meter for PointSource); unused by phase-only PointFocus. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light")
	double Amplitude = 0.0;

	/** PlaneWave phase at SLM origin: phi_inc(P) = InitialPhaseRad + k*dot(DirectionSLM, P). PointSource uses +k*r. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light", meta = (Units = "rad"))
	double InitialPhaseRad = 0.0;

	/** Unit propagation direction expressed in SLM-local axes. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light")
	FVector DirectionSLM = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light")
	ECGHSourceType SourceType = ECGHSourceType::PlaneWave;

	/** Point-source position in meters relative to the SLM actor origin; ignored for PlaneWave. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light", meta = (Units = "m"))
	FVector PositionSLMM = FVector::ZeroVector;

	/** Finite angle accepted and validated, but unused by scalar PointFocus v0. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Light", meta = (Units = "rad"))
	double PolarizationAngleRad = 0.0;
};

/** One target in SI units and the SLM's rigid local frame. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHTargetDescription
{
	GENERATED_BODY()

	/** Runtime identity shared by this actor's description, geometry and point cloud. */
	UPROPERTY(VisibleAnywhere, Category = "Target")
	uint64 ResourceId = 0;

	/** The description and both resources represent the same revision. */
	UPROPERTY(VisibleAnywhere, Category = "Target")
	uint64 Revision = 0;

	/** Compatibility alias of ResourceId. */
	UPROPERTY(VisibleAnywhere, Category = "Target")
	uint64 TargetId = 0;

	/** Compatibility alias of PositionSLMM; also in meters. */
	FVector3d PositionSLM = FVector3d::ZeroVector;

	/** Rotation from the actor's rigid local axes to SLM-local axes; scale is baked into resources. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target")
	FQuat RotationSLM = FQuat::Identity;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target", meta = (Units = "m"))
	FVector PositionSLMM = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target")
	double Amplitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target", meta = (Units = "rad"))
	double PhaseRad = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target")
	ECGHTargetType TargetType = ECGHTargetType::Point;

	/** Compatibility aliases; zero for point targets, which have no geometry resources. */
	UPROPERTY(VisibleAnywhere, Category = "Target")
	uint64 GeometryResourceId = 0;

	UPROPERTY(VisibleAnywhere, Category = "Target")
	uint64 GeometryRevision = 0;
};

/** Mesh in meters in the target actor's rigid local axes; component/actor scale is already baked in. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHMeshGeometryResource
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Target")
	uint64 ResourceId = 0;

	UPROPERTY(VisibleAnywhere, Category = "Target")
	uint64 Revision = 0;

	UPROPERTY(VisibleAnywhere, Category = "Target")
	TArray<FVector> VerticesM;

	/** Triangle-list indices; always a multiple of three. */
	UPROPERTY(VisibleAnywhere, Category = "Target")
	TArray<uint32> Indices;

	/** Unit normals and UV0 have one entry per vertex, or are empty when absent. */
	UPROPERTY(VisibleAnywhere, Category = "Target")
	TArray<FVector3f> Normals;

	UPROPERTY(VisibleAnywhere, Category = "Target")
	TArray<FVector2f> UVs;
};

/**
 * One surface sample in the same scaled, rigid local frame as the mesh resource.
 * Amplitude and Phase already include the target parameters; solvers must not apply them twice.
 */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHObjectPoint
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target", meta = (Units = "m"))
	FVector PositionLocalM = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, Category = "Target")
	FVector3f NormalLocal = FVector3f::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target")
	double Amplitude = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target", meta = (Units = "rad"))
	double Phase = 0.0;

	UPROPERTY(VisibleAnywhere, Category = "Target")
	FVector2f UV = FVector2f::ZeroVector;
};

/**
 * Point cloud in meters in the target actor's rigid local axes.
 * Solvers snapshot these points by ResourceId/Revision; publish a new revision after any data edit.
 */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHPointCloudResource
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Target")
	uint64 ResourceId = 0;

	UPROPERTY(VisibleAnywhere, Category = "Target")
	uint64 Revision = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Target")
	TArray<FCGHObjectPoint> Points;
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

	/** Lens-local axes in the SLM frame. Optical +X faces the scene; sensor is behind the lens along -X. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	FQuat OpticalRotationSLM = FQuat::Identity;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (Units = "m"))
	double PixelPitchXM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera", meta = (Units = "m"))
	double PixelPitchYM = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	int32 PupilResolutionX = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	int32 PupilResolutionY = 0;

};

/**
 * Solver snapshot using meters and radians. Positions and directions use the
 * CGHSLMActor's origin and rotation: +X optical normal, +Y increasing column, +Z decreasing row.
 * Position X is signed depth: positive along the normal, negative behind the SLM plane.
 * Reference actor scale is ignored, so local positions retain physical distances.
 * Descriptions for missing actors retain zero values; targets require an SLM.
 */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHSceneDescription
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH")
	int32 SchemaVersion = 2;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH")
	FCGHSLMDescription SLM;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH")
	FCGHReconstructionLightDescription ReconstructionLight;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH")
	TArray<FCGHTargetDescription> Targets;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH")
	FCGHCameraDescription Camera;
};
