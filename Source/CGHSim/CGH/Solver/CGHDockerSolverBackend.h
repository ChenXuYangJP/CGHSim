#pragma once

#include "CGH/Solver/CGHSolverBackend.h"
#include "CGHDockerSolverBackend.generated.h"

/**
 * Asynchronous TCP client for the standalone solver service in Backend/V100.
 * Workers own input/settings snapshots and a job mailbox; they never access this UObject.
 * This backend only transports requests/results. Numerical work belongs to the service.
 */
UCLASS()
class CGHSIM_API UCGHDockerSolverBackend : public UCGHSolverBackend
{
	GENERATED_BODY()

public:
	/** Copied at submission, so changing settings cannot alter an in-flight job. */
	UPROPERTY(EditAnywhere, Category = "CGH|Solver|Docker")
	FCGHDockerSolverSettings Settings;

	virtual TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe> Submit(FCGHSolverInput&& Input) override;
};
