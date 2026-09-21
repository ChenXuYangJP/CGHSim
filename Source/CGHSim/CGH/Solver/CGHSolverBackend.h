#pragma once

#include "CoreMinimal.h"
#include "CGH/Types/CGHSolverTypes.h"
#include "UObject/Object.h"
#include "CGHSolverBackend.generated.h"

/** Backend boundary shared by local CPU and future network solver implementations. */
UCLASS(Abstract)
class CGHSIM_API UCGHSolverBackend : public UObject
{
	GENERATED_BODY()

public:
	/** Called on the game thread; returns immediately with an independently owned job mailbox. */
	virtual TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe> Submit(FCGHSolverInput&& Input)
		PURE_VIRTUAL(UCGHSolverBackend::Submit, return nullptr;);
};
