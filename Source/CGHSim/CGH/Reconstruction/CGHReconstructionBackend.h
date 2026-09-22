#pragma once

#include "CoreMinimal.h"
#include "CGH/Types/CGHReconstructionTypes.h"
#include "UObject/Object.h"
#include "CGHReconstructionBackend.generated.h"

/** Shared asynchronous boundary for reconstruction implementations. */
UCLASS(Abstract)
class CGHSIM_API UCGHReconstructionBackend : public UObject
{
	GENERATED_BODY()

public:
	/** Game-thread entry point; returns immediately with an independently owned mailbox. */
	virtual TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> Submit(FCGHReconstructionInput&& Input)
		PURE_VIRTUAL(UCGHReconstructionBackend::Submit, return nullptr;);
};
