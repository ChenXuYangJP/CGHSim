#include "CGH/Reconstruction/CGHDockerReconstructionBackend.h"

TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> UCGHDockerReconstructionBackend::Submit(FCGHReconstructionInput&& Input)
{
	check(IsInGameThread());
	TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> Job =
		MakeShared<FCGHReconstructionJob, ESPMode::ThreadSafe>(MoveTemp(Input));
	Job->bStarted.store(true, std::memory_order_release);
	Job->Result.PropagationConvention = Job->Input.PropagationConvention;
	Job->Result.Error = TEXT("Docker reconstruction is not implemented; select the CPU backend.");
	Job->bFinished.store(true, std::memory_order_release);
	return Job;
}
