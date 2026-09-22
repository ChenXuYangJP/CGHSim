#pragma once

#include "CoreMinimal.h"
#include "CGH/Types/CGHReconstructionTypes.h"
#include "GameFramework/Actor.h"
#include "CGHReconstructorActor.generated.h"

class ACGHWorkbenchActor;
class ACGHSLMActor;
class ACGHReconstructionLightActor;
class ACGHObserverPlaneActor;
class UCGHReconstructionBackend;

/** Lightweight publication guard. Only the worker mailbox owns a phase-array snapshot. */
struct FCGHReconstructionSubmission
{
	FCGHReconstructionInput Input;
	FCGHReconstructionParameters Parameters;
	TWeakObjectPtr<ACGHWorkbenchActor> Workbench;
	TWeakObjectPtr<ACGHSLMActor> SLM;
	TWeakObjectPtr<ACGHReconstructionLightActor> Light;
	TWeakObjectPtr<ACGHObserverPlaneActor> ObserverPlane;
	uint64 PhaseRevision = 0;
	uint64 FieldRevision = 0;
};

/** Captures optical inputs on the game thread, queues backend work, and publishes accepted fields. */
UCLASS(Blueprintable)
class CGHSIM_API ACGHReconstructorActor : public AActor
{
	GENERATED_BODY()

public:
	ACGHReconstructorActor();
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	virtual void Destroyed() override;
	virtual void BeginDestroy() override;

	/** Supplies SLM phase, illumination, and the destination observer plane. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Reconstruction")
	TObjectPtr<ACGHWorkbenchActor> Workbench;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Reconstruction")
	FCGHReconstructionParameters Parameters;

	/** Reconstruct when consumed optical inputs change; cancellation suppresses unchanged-input retries. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Reconstruction")
	bool bAutoReconstruct = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Reconstruction")
	ECGHReconstructionJobState JobState = ECGHReconstructionJobState::Idle;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Reconstruction")
	FString StatusMessage = TEXT("Ready to reconstruct the SLM phase onto an observer plane.");

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Reconstruction")
	int64 JobId = 0;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Reconstruction", meta = (Units = "s"))
	double LastComputeSeconds = 0.0;

	/** Snapshot inputs and queue the latest request without waiting for a worker. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Reconstruction")
	bool StartReconstruction();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Reconstruction")
	void Reconstruct();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Reconstruction")
	void CancelReconstruction();

	/** Nonblocking completion polling, also called by editor and game ticks. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Reconstruction")
	void PollReconstructor();

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<UCGHReconstructionBackend> Backend;

	TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> ActiveJob;
	TOptional<FCGHReconstructionSubmission> ActiveSubmission;
	TOptional<FCGHReconstructionSubmission> PendingSubmission;
	TOptional<FCGHReconstructionSubmission> LastAttempt;
	bool bAcceptActiveResult = false;
	bool bPolling = false;

	bool CaptureSubmission(FCGHReconstructionSubmission& Out, FString& Error);
	static bool SameInputs(const FCGHReconstructionSubmission& A, const FCGHReconstructionSubmission& B);
	void SubmitPending();
	void FailRequest(const FString& Error);
	void StopJobs();
};
