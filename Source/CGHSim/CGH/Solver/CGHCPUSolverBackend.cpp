#include "CGH/Solver/CGHCPUSolverBackend.h"

#include "Async/Async.h"
#include "CGH/Solver/CGHPointFocus.h"

TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe> UCGHCPUSolverBackend::Submit(FCGHSolverInput&& Input)
{
	check(IsInGameThread());
	TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe> Job =
		MakeShared<FCGHSolverJob, ESPMode::ThreadSafe>(MoveTemp(Input));
	Async(EAsyncExecution::ThreadPool, [Job]()
	{
		Job->bStarted.store(true, std::memory_order_release);
		Job->Result = CGHPointFocus::Solve(Job->Input, Job->bCancelRequested);
		Job->bFinished.store(true, std::memory_order_release);
	});
	return Job;
}
