#pragma once

#include "CGH/Reconstruction/CGHReconstructionBackend.h"
#include "CGHCPUReconstructionBackend.generated.h"

/** Worker pool scalar diffraction. Jobs own all input/output data and never access UObjects. */
UCLASS()
class CGHSIM_API UCGHCPUReconstructionBackend : public UCGHReconstructionBackend
{
	GENERATED_BODY()

public:
	virtual TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> Submit(FCGHReconstructionInput&& Input) override;
};
