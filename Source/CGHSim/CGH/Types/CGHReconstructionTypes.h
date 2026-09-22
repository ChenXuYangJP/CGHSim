#pragma once

#include "CoreMinimal.h"
#include "CGH/Types/CGHComplexField.h"
#include "CGH/Types/CGHSolverTypes.h"
#include <atomic>
#include "CGHReconstructionTypes.generated.h"

UENUM(BlueprintType)
enum class ECGHReconstructionBackend : uint8
{
	CPU,
	Docker UMETA(DisplayName = "Docker (Not Implemented)")
};

UENUM(BlueprintType)
enum class ECGHReconstructionMode : uint8
{
	ObserverPlane,
	Camera UMETA(DisplayName = "Camera (Not Implemented)")
};

UENUM(BlueprintType)
enum class ECGHReconstructionJobState : uint8
{
	Idle,
	Queued,
	Running,
	Ready,
	Failed
};

USTRUCT(BlueprintType)
struct CGHSIM_API FCGHReconstructionParameters
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Reconstruction")
	ECGHReconstructionBackend ReconstructionBackend = ECGHReconstructionBackend::CPU;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Reconstruction")
	ECGHReconstructionMode Mode = ECGHReconstructionMode::ObserverPlane;
};

/** Owned SI-unit optical snapshot. Worker threads never access actors or other UObjects. */
struct CGHSIM_API FCGHReconstructionInput
{
	FCGHSLMDescription SLM;
	FCGHReconstructionLightDescription Light;
	FCGHObserverPlaneDescription ObserverPlane;
	FCGHSLMPhasePattern Pattern;
	ECGHReconstructionMode Mode = ECGHReconstructionMode::ObserverPlane;
	ECGHPropagationConvention PropagationConvention = ECGHPropagationConvention::ExpPositiveIKR;
};

/** Failed and cancelled jobs never publish a partially computed complex field. */
struct CGHSIM_API FCGHReconstructionResult
{
	FCGHComplexField Field;
	FString Error;
	bool bSucceeded = false;
	double ComputeSeconds = 0.0;
	ECGHPropagationConvention PropagationConvention = ECGHPropagationConvention::ExpPositiveIKR;
};

/**
 * Actor-independent mailbox: the worker alone writes Result before release-storing
 * bFinished. The game thread acquire-loads bFinished before reading/moving Result.
 */
struct CGHSIM_API FCGHReconstructionJob
{
	explicit FCGHReconstructionJob(FCGHReconstructionInput&& InInput) : Input(MoveTemp(InInput)) {}

	const FCGHReconstructionInput Input;
	std::atomic<bool> bCancelRequested{false};
	std::atomic<bool> bStarted{false};
	std::atomic<bool> bFinished{false};
	FCGHReconstructionResult Result;
};
