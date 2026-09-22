#pragma once

#include "CGH/Reconstruction/CGHReconstructionBackend.h"
#include "CGHDockerReconstructionBackend.generated.h"

/** Reserved backend selection. Reconstruction Docker transport is intentionally not implemented. */
UCLASS()
class CGHSIM_API UCGHDockerReconstructionBackend : public UCGHReconstructionBackend
{
	GENERATED_BODY()

public:
	virtual TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> Submit(FCGHReconstructionInput&& Input) override;
};
