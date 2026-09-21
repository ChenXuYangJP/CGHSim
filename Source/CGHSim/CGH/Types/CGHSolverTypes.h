#pragma once

#include "CoreMinimal.h"
#include "CGH/Types/CGHSLMPhasePattern.h"
#include "CGH/Types/CGHTypes.h"
#include <atomic>
#include "CGHSolverTypes.generated.h"

UENUM(BlueprintType)
enum class ECGHSolverBackend : uint8
{
	CPU,
	Docker UMETA(DisplayName = "Docker (TCP/CUDA)")
};

UENUM(BlueprintType)
enum class ECGHSolverAlgorithm : uint8
{
	/** Complex-field superposition of point targets and mesh point-cloud samples. */
	PointFocus
};

UENUM(BlueprintType)
enum class ECGHSolverJobState : uint8
{
	Idle,
	Queued,
	Running,
	Ready,
	Failed
};

/** Part of the solver contract: future FFT/network backends must preserve or explicitly convert this sign. */
UENUM(BlueprintType)
enum class ECGHPropagationConvention : uint8
{
	/** U(r) is proportional to exp(+i*k*r); focusing subtracts k*r and the incident phase from target_phase. */
	ExpPositiveIKR
};

/** Copied into each network job; workers never access endpoint UObject state. */
USTRUCT(BlueprintType)
struct CGHSIM_API FCGHDockerSolverSettings
{
	GENERATED_BODY()

	/** Numeric IPv4 address of the published TCP port (no blocking DNS lookup). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Docker")
	FString Address = TEXT("127.0.0.1");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Docker", meta = (ClampMin = "1", ClampMax = "65535"))
	int32 Port = 7000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Docker", meta = (ClampMin = "0.1", Units = "s"))
	double ConnectTimeoutSeconds = 5.0;

	/** Total request deadline including serialization, connection, send, and reception. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Docker", meta = (ClampMin = "0.1", Units = "s"))
	double RequestTimeoutSeconds = 30.0;
};

USTRUCT(BlueprintType)
struct CGHSIM_API FCGHSolverParameters
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver")
	ECGHSolverBackend SolverBackend = ECGHSolverBackend::CPU;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver")
	ECGHSolverAlgorithm Algorithm = ECGHSolverAlgorithm::PointFocus;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (EditCondition = "SolverBackend == ECGHSolverBackend::Docker"))
	FCGHDockerSolverSettings Docker;
};

/** Owned numerical snapshot: no actor pointers or UObject access is permitted in workers. */
struct CGHSIM_API FCGHSolverInput
{
	FCGHSceneDescription Scene;
	/** Owned snapshots for mesh targets, matched by ResourceId and Revision; absent for points. */
	TArray<FCGHPointCloudResource> PointClouds;
	ECGHSolverAlgorithm Algorithm = ECGHSolverAlgorithm::PointFocus;
	ECGHPropagationConvention PropagationConvention = ECGHPropagationConvention::ExpPositiveIKR;
};

/** A failed or cancelled result never contains a partial phase pattern. */
struct CGHSIM_API FCGHSolverResult
{
	/** Returned explicitly so callers cannot silently accept an incompatible backend convention. */
	ECGHPropagationConvention PropagationConvention = ECGHPropagationConvention::ExpPositiveIKR;
	FCGHSLMPhasePattern Pattern;
	FString Error;
	bool bSucceeded = false;
	/** True only for a remote transport fixture; real CPU/CUDA PointFocus results leave this false. */
	bool bIsDummy = false;
	double ComputeSeconds = 0.0;
};

/**
 * Shared input/output mailbox, owned independently of the submitting actor/backend.
 * The worker alone writes Result, then release-stores bFinished. The game thread
 * must acquire-load bFinished before reading/moving Result. Cancellation is cooperative.
 */
struct CGHSIM_API FCGHSolverJob
{
	explicit FCGHSolverJob(FCGHSolverInput&& InInput) : Input(MoveTemp(InInput)) {}

	const FCGHSolverInput Input;
	std::atomic<bool> bCancelRequested{false};
	std::atomic<bool> bStarted{false};
	std::atomic<bool> bFinished{false};
	FCGHSolverResult Result;
};
