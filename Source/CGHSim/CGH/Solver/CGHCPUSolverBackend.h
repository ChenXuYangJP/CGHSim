#pragma once

#include "CGH/Solver/CGHSolverBackend.h"
#include "CGHCPUSolverBackend.generated.h"

/** Reference CPU backend. The queued computation captures only its plain-data job mailbox. */
UCLASS()
class CGHSIM_API UCGHCPUSolverBackend : public UCGHSolverBackend
{
	GENERATED_BODY()

public:
	virtual TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe> Submit(FCGHSolverInput&& Input) override;
};
