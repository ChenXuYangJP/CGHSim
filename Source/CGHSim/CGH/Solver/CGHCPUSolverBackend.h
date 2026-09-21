#pragma once

#include "CGH/Solver/CGHSolverBackend.h"
#include "CGHCPUSolverBackend.generated.h"

/**
 * Reference CPU complex-field superposition for Point targets and sampled Mesh targets.
 * The queued computation owns its scene/cloud snapshots and never accesses actor/UObject data.
 */
UCLASS()
class CGHSIM_API UCGHCPUSolverBackend : public UCGHSolverBackend
{
	GENERATED_BODY()

public:
	virtual TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe> Submit(FCGHSolverInput&& Input) override;
};
