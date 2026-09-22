#include "CGH/Reconstruction/CGHCPUReconstructionBackend.h"

#include "Async/Async.h"
#include "CGH/Reconstruction/CGHReconstruction.h"

TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> UCGHCPUReconstructionBackend::Submit(FCGHReconstructionInput&& Input)
{
	check(IsInGameThread());
	TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> Job =
		MakeShared<FCGHReconstructionJob, ESPMode::ThreadSafe>(MoveTemp(Input));
	Async(EAsyncExecution::ThreadPool, [Job]()
	{
		Job->bStarted.store(true, std::memory_order_release);
		Job->Result = CGHReconstruction::Reconstruct(Job->Input, Job->bCancelRequested);
		Job->bFinished.store(true, std::memory_order_release);
	});
	return Job;
}
