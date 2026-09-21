#pragma once

#include "CoreMinimal.h"
#include "CGH/Types/CGHSolverTypes.h"
#include "GameFramework/Actor.h"
#include "CGHSolverActor.generated.h"

class ACGHWorkbenchActor;
class ACGHSLMActor;
class ACGHTargetActor;
class ACGHReconstructionLightActor;
class UCGHSolverBackend;

/** Game-thread publication guards. Input holds descriptions only; bulk clouds belong to the worker job. */
struct FCGHSolverSubmission
{
	FCGHSolverInput Input;
	FCGHSolverParameters Parameters;
	TWeakObjectPtr<ACGHWorkbenchActor> Workbench;
	TWeakObjectPtr<ACGHSLMActor> SLM;
	TArray<TWeakObjectPtr<ACGHTargetActor>> Targets;
	TWeakObjectPtr<ACGHReconstructionLightActor> Light;
	uint64 PhaseRevision = 0;
};

/** Owns asynchronous jobs and publishes accepted results on the game thread. */
UCLASS(Blueprintable)
class CGHSIM_API ACGHSolverActor : public AActor
{
	GENERATED_BODY()

public:
	ACGHSolverActor();
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	virtual void Destroyed() override;
	virtual void BeginDestroy() override;

	/** Supplies the SI snapshot, plane-wave illumination, and destination SLM. Camera is optional for PointFocus. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Solver")
	TObjectPtr<ACGHWorkbenchActor> Workbench;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Solver")
	FCGHSolverParameters Parameters;

	/** Recompute when consumed optical inputs change. Cancellation suppresses retry of unchanged input. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Solver")
	bool bAutoSolve = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Solver")
	ECGHSolverJobState JobState = ECGHSolverJobState::Idle;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Solver")
	FString StatusMessage = TEXT("Ready to generate a phase pattern from point and mesh targets.");

	/** Monotonic request identifier for this actor instance; active buffers are transient. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Solver")
	int64 JobId = 0;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Solver", meta = (Units = "s"))
	double LastComputeSeconds = 0.0;

	/** Snapshot input and queue the latest request; never waits for a worker. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Solver")
	bool StartSolve();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Solver")
	void GeneratePhasePattern();

	/** Saves this solver's accepted result if it is still the active SLM pattern. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Solver|Save")
	bool SaveGeneratedPhasePattern();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Solver|Save")
	void SavePhasePattern();

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Solver|Save")
	FString PhaseSaveStatus = TEXT("Generate a phase pattern, then press Save Phase Pattern.");

	/** Cooperative cancellation leaves the last published SLM pattern intact. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Solver")
	void CancelSolve();

	/** Nonblocking completion polling, also performed by Tick in editor and game worlds. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Solver")
	void PollSolver();

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<UCGHSolverBackend> Backend;

	TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe> ActiveJob;
	TOptional<FCGHSolverSubmission> ActiveSubmission;
	TOptional<FCGHSolverSubmission> PendingSubmission;
	TOptional<FCGHSolverSubmission> LastAttempt;
	bool bAcceptActiveResult = false;
	bool bPolling = false;
	TWeakObjectPtr<ACGHSLMActor> LastPublishedSLM;
	uint64 LastPublishedPhaseRevision = 0;

	bool CaptureSubmission(FCGHSolverSubmission& Out, FString& Error);
	static bool SameInputs(const FCGHSolverSubmission& A, const FCGHSolverSubmission& B);
	void SubmitPending();
	void FailRequest(const FString& Error);
	void StopJobs();
};
