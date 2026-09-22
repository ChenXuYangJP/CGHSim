#pragma once

#include "CGH/Reconstruction/CGHReconstructionBackend.h"
#include "CGHDockerReconstructionBackend.generated.h"

/** Asynchronous TCP reconstruction client; workers own numerical/settings snapshots and never access UObjects. */
UCLASS()
class CGHSIM_API UCGHDockerReconstructionBackend : public UCGHReconstructionBackend
{
	GENERATED_BODY()

public:
	/** Copied when submitted; endpoint edits cannot alter an in-flight job. */
	UPROPERTY(EditAnywhere, Category = "CGH|Reconstruction|Docker")
	FCGHDockerSolverSettings Settings;

	virtual TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> Submit(FCGHReconstructionInput&& Input) override;
};
