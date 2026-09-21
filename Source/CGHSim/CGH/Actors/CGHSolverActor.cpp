#include "CGH/Actors/CGHSolverActor.h"

#include "CGH/Actors/CGHWorkbenchActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHTargetActor.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Solver/CGHCPUSolverBackend.h"
#include "CGH/Solver/CGHPointFocus.h"
#include "Components/SceneComponent.h"

ACGHSolverActor::ACGHSolverActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

bool ACGHSolverActor::CaptureSubmission(FCGHSolverSubmission& Out, FString& Error)
{
	Out = FCGHSolverSubmission();
	const auto Available = [this](const AActor* Actor)
	{
		return IsValid(Actor) && !Actor->IsActorBeingDestroyed() && Actor->GetWorld() == GetWorld();
	};
	if (!Available(Workbench))
	{
		Error = TEXT("Assign a valid Workbench in the solver's world.");
		return false;
	}
	if (!Available(Workbench->SLM) || !Available(Workbench->ReconstructionLight))
	{
		Error = TEXT("PointFocus requires a valid SLM and plane-wave light in the same world.");
		return false;
	}
	if (Workbench->Targets.IsEmpty() || Workbench->Targets.Num() > CGHPointFocus::MaximumEmitterCount)
	{
		Error = TEXT("Assign at least one target, within the 1,000,000 aggregate source-sample limit.");
		return false;
	}
	if (Workbench->SLM->GetActorTransform().ContainsNaN()
		|| !Workbench->SLM->GetActorScale3D().Equals(FVector::OneVector, KINDA_SMALL_NUMBER))
	{
		Error = TEXT("The SLM transform must be finite and its actor scale must be (1, 1, 1).");
		return false;
	}
	TSet<ACGHTargetActor*> UniqueTargets;
	for (int32 Index = 0; Index < Workbench->Targets.Num(); ++Index)
	{
		ACGHTargetActor* Target = Workbench->Targets[Index];
		if (!Available(Target) || Target->GetActorTransform().ContainsNaN())
		{
			Error = FString::Printf(TEXT("Target[%d] must be a valid actor in this world with a finite transform."), Index);
			return false;
		}
		if (UniqueTargets.Contains(Target))
		{
			Error = FString::Printf(TEXT("Target[%d] duplicates another target actor. List each target once."), Index);
			return false;
		}
		UniqueTargets.Add(Target);
		if (Target->Parameters.TargetType != ECGHTargetType::Point && Target->Parameters.TargetType != ECGHTargetType::Mesh)
		{
			Error = FString::Printf(TEXT("Target[%d] must have type Point or Mesh."), Index);
			return false;
		}
	}

	// Target actors own/rebuild their sampling caches. Polling copies only the small descriptions.
	Workbench->UpdateSceneDescription();
	int64 SourceCount = 0;
	Out.Targets.Reserve(Workbench->Targets.Num());
	for (int32 Index = 0; Index < Workbench->Targets.Num(); ++Index)
	{
		ACGHTargetActor* Target = Workbench->Targets[Index];
		if (Target->GetReferenceSLM() != Workbench->SLM || !Target->bResourcesValid)
		{
			Error = FString::Printf(TEXT("Target[%d] must have valid resources in this workbench's SLM frame. %s"),
				Index, *Target->ResourceError);
			return false;
		}
		if (Target->TargetDescription.TargetType == ECGHTargetType::Mesh)
		{
			const FCGHPointCloudResource& Cloud = Target->PointCloudResource;
			if (Cloud.Points.IsEmpty() || Cloud.ResourceId == 0 || Cloud.Revision == 0
				|| Cloud.ResourceId != Target->TargetDescription.ResourceId
				|| Cloud.Revision != Target->TargetDescription.Revision)
			{
				Error = FString::Printf(TEXT("Target[%d] needs a nonempty point cloud matching its resource identity and revision."), Index);
				return false;
			}
			SourceCount += Cloud.Points.Num();
		}
		else
		{
			++SourceCount;
		}
		Out.Targets.Add(Target);
	}
	if (SourceCount > CGHPointFocus::MaximumEmitterCount)
	{
		Error = TEXT("Combined point and mesh targets exceed the 1,000,000 source-sample limit. Reduce mesh sampling density.");
		return false;
	}
	Workbench->SLM->SynchronizePhasePattern();
	Out.Input.Scene = Workbench->SceneDescription;
	Out.Input.Algorithm = Parameters.Algorithm;
	Out.Input.PropagationConvention = ECGHPropagationConvention::ExpPositiveIKR;
	Out.Parameters = Parameters;
	Out.Workbench = Workbench;
	Out.SLM = Workbench->SLM;
	Out.Light = Workbench->ReconstructionLight;
	Out.PhaseRevision = Workbench->SLM->GetPhasePatternRevision();
	// Deep point-cloud validation/transformation is worker work, not per-frame game-thread work.
	return CGHPointFocus::ValidateScene(Out.Input, Error);
}

bool ACGHSolverActor::SameInputs(const FCGHSolverSubmission& A, const FCGHSolverSubmission& B)
{
	if (A.Workbench != B.Workbench || A.SLM != B.SLM || A.Targets != B.Targets || A.Light != B.Light
		|| A.Parameters.SolverBackend != B.Parameters.SolverBackend || A.Parameters.Algorithm != B.Parameters.Algorithm)
	{
		return false;
	}
	const auto SameScalar = [](double X, double Y)
	{
		return X == Y || (FMath::IsNaN(X) && FMath::IsNaN(Y));
	};
	const FCGHSceneDescription& X = A.Input.Scene;
	const FCGHSceneDescription& Y = B.Input.Scene;
	if (X.SchemaVersion != Y.SchemaVersion || X.Targets.Num() != Y.Targets.Num()
		|| X.SLM.ResolutionX != Y.SLM.ResolutionX || X.SLM.ResolutionY != Y.SLM.ResolutionY
		|| !SameScalar(X.SLM.PixelPitchXM, Y.SLM.PixelPitchXM) || !SameScalar(X.SLM.PixelPitchYM, Y.SLM.PixelPitchYM)
		|| X.SLM.ModulationType != Y.SLM.ModulationType
		|| !SameScalar(X.ReconstructionLight.WavelengthM, Y.ReconstructionLight.WavelengthM)
		|| X.ReconstructionLight.SourceType != Y.ReconstructionLight.SourceType
		|| !SameScalar(X.ReconstructionLight.InitialPhaseRad, Y.ReconstructionLight.InitialPhaseRad)
		|| !SameScalar(X.ReconstructionLight.DirectionSLM.X, Y.ReconstructionLight.DirectionSLM.X)
		|| !SameScalar(X.ReconstructionLight.DirectionSLM.Y, Y.ReconstructionLight.DirectionSLM.Y)
		|| !SameScalar(X.ReconstructionLight.DirectionSLM.Z, Y.ReconstructionLight.DirectionSLM.Z))
	{
		return false;
	}
	for (int32 Index = 0; Index < X.Targets.Num(); ++Index)
	{
		const FCGHTargetDescription& First = X.Targets[Index];
		const FCGHTargetDescription& Second = Y.Targets[Index];
		if (First.TargetType != Second.TargetType
			|| !SameScalar(First.PositionSLMM.X, Second.PositionSLMM.X)
			|| !SameScalar(First.PositionSLMM.Y, Second.PositionSLMM.Y)
			|| !SameScalar(First.PositionSLMM.Z, Second.PositionSLMM.Z)
			|| !SameScalar(First.Amplitude, Second.Amplitude)
			|| !SameScalar(First.PhaseRad, Second.PhaseRad))
		{
			return false;
		}
		if (First.TargetType == ECGHTargetType::Mesh
			&& (First.ResourceId != Second.ResourceId || First.Revision != Second.Revision
				|| !First.RotationSLM.Equals(Second.RotationSLM, 0.0)))
		{
			return false;
		}
	}
	// Light magnitude/polarization/position and point orientation do not affect this scalar phase.
	// Mesh revisions track cloud shape, component/actor scale, sampling, and per-sample optical data.
	return true;
}

void ACGHSolverActor::FailRequest(const FString& Error)
{
	if (ActiveJob)
	{
		ActiveJob->bCancelRequested.store(true, std::memory_order_relaxed);
	}
	bAcceptActiveResult = false;
	PendingSubmission.Reset();
	JobState = ECGHSolverJobState::Failed;
	StatusMessage = Error;
}

bool ACGHSolverActor::StartSolve()
{
	check(IsInGameThread());
	if (IsTemplate() || IsActorBeingDestroyed() || !GetWorld())
	{
		return false;
	}
	FCGHSolverSubmission Submission;
	FString Error;
	if (!CaptureSubmission(Submission, Error))
	{
		LastAttempt.Reset();
		FailRequest(Error);
		return false;
	}
	LastAttempt = Submission;
	++JobId;
	if (Parameters.SolverBackend != ECGHSolverBackend::CPU)
	{
		FailRequest(TEXT("Docker/TCP backend is not implemented. Select CPU for PointFocus."));
		return false;
	}
	if (ActiveJob)
	{
		ActiveJob->bCancelRequested.store(true, std::memory_order_relaxed);
	}
	bAcceptActiveResult = false;
	PendingSubmission = MoveTemp(Submission);
	JobState = ECGHSolverJobState::Queued;
	StatusMessage = TEXT("PointFocus complex-field superposition queued (propagation exp(+i k r)).");
	// A replaced worker is reaped before another starts, bounding large output buffers.
	if (!ActiveJob)
	{
		SubmitPending();
	}
	return true;
}

void ACGHSolverActor::GeneratePhasePattern()
{
	StartSolve();
}

bool ACGHSolverActor::SaveGeneratedPhasePattern()
{
	check(IsInGameThread());
	if (IsTemplate() || IsActorBeingDestroyed() || JobState != ECGHSolverJobState::Ready
		|| ActiveJob || PendingSubmission.IsSet())
	{
		PhaseSaveStatus = TEXT("Wait for a generated phase pattern to reach Ready before saving.");
		return false;
	}
	ACGHSLMActor* SLM = LastPublishedSLM.Get();
	if (!IsValid(Workbench) || Workbench->IsActorBeingDestroyed() || Workbench->GetWorld() != GetWorld()
		|| !IsValid(SLM) || SLM->IsActorBeingDestroyed() || SLM->GetWorld() != GetWorld()
		|| Workbench->SLM != SLM || !SLM->HasValidPhasePattern()
		|| SLM->GetPhasePatternRevision() != LastPublishedPhaseRevision)
	{
		PhaseSaveStatus = TEXT("The generated result is no longer the active SLM pattern. Generate again, or save the current pattern from the SLM.");
		return false;
	}
	const bool bSaved = SLM->SaveCurrentPhasePattern();
	PhaseSaveStatus = SLM->PhaseSaveStatus;
	return bSaved;
}

void ACGHSolverActor::SavePhasePattern()
{
	SaveGeneratedPhasePattern();
}

void ACGHSolverActor::SubmitPending()
{
	check(!ActiveJob && PendingSubmission.IsSet());
	FCGHSolverSubmission Current;
	FString Error;
	if (!CaptureSubmission(Current, Error)
		|| !SameInputs(Current, PendingSubmission.GetValue())
		|| Current.PhaseRevision != PendingSubmission->PhaseRevision)
	{
		FailRequest(Error.IsEmpty() ? TEXT("Queued inputs or SLM phase changed; generate again.") : Error);
		return;
	}
	if (!Backend)
	{
		Backend = NewObject<UCGHCPUSolverBackend>(this);
	}
	ActiveSubmission = MoveTemp(PendingSubmission.GetValue());
	PendingSubmission.Reset();
	// Submit a separate value copy: workers receive no actor/UObject pointers.
	FCGHSolverInput Input = ActiveSubmission->Input;
	for (const TWeakObjectPtr<ACGHTargetActor>& Target : ActiveSubmission->Targets)
	{
		if (Target->TargetDescription.TargetType == ECGHTargetType::Mesh)
		{
			// One owned copy at launch; no bulk arrays in pending/active/last-attempt guards.
			// Later actor resource rebuilds cannot mutate a running job's cloud.
			Input.PointClouds.Add(Target->PointCloudResource);
		}
	}
	ActiveJob = Backend->Submit(MoveTemp(Input));
	bAcceptActiveResult = ActiveJob.IsValid();
	if (!ActiveJob)
	{
		ActiveSubmission.Reset();
		FailRequest(TEXT("Backend could not queue the solver job."));
	}
}

void ACGHSolverActor::PollSolver()
{
	check(IsInGameThread());
	if (bPolling || IsTemplate() || IsActorBeingDestroyed() || !GetWorld())
	{
		return;
	}
	TGuardValue<bool> PollGuard(bPolling, true);
	if (ActiveJob && ActiveJob->bFinished.load(std::memory_order_acquire))
	{
		if (bAcceptActiveResult && !PendingSubmission.IsSet())
		{
			FCGHSolverSubmission Current;
			FString Error;
			if (!CaptureSubmission(Current, Error) || !ActiveSubmission.IsSet()
				|| !SameInputs(Current, ActiveSubmission.GetValue())
				|| Current.PhaseRevision != ActiveSubmission->PhaseRevision)
			{
				FailRequest(Error.IsEmpty() ? TEXT("Result discarded: optical inputs or SLM phase changed during calculation.") : Error);
			}
			else if (ActiveJob->Result.PropagationConvention != ActiveSubmission->Input.PropagationConvention)
			{
				FailRequest(TEXT("Backend result uses an incompatible propagation convention."));
			}
			else if (!ActiveJob->Result.bSucceeded)
			{
				FailRequest(ActiveJob->Result.Error);
			}
			else
			{
				LastComputeSeconds = ActiveJob->Result.ComputeSeconds;
				ACGHSLMActor* Destination = Current.SLM.Get();
				if (Destination->SetPhasePattern(MoveTemp(ActiveJob->Result.Pattern)))
				{
					LastPublishedSLM = Destination;
					LastPublishedPhaseRevision = Destination->GetPhasePatternRevision();
					JobState = ECGHSolverJobState::Ready;
					StatusMessage = TEXT("PointFocus complex-field phase published with plane-wave illumination compensation (exp(+i k r)).");
				}
				else
				{
					FailRequest(Destination->PhasePatternError);
				}
			}
		}
		ActiveJob.Reset();
		ActiveSubmission.Reset();
		bAcceptActiveResult = false;
	}
	if (PendingSubmission.IsSet() && !ActiveJob)
	{
		SubmitPending();
	}
	if (ActiveJob && bAcceptActiveResult && !PendingSubmission.IsSet()
		&& ActiveJob->bStarted.load(std::memory_order_acquire))
	{
		JobState = ECGHSolverJobState::Running;
		StatusMessage = TEXT("Summing point and mesh complex fields on the CPU worker.");
	}
	if (bAutoSolve)
	{
		FCGHSolverSubmission Current;
		FString Error;
		if (!CaptureSubmission(Current, Error))
		{
			LastAttempt.Reset();
			FailRequest(Error);
		}
		else if (!LastAttempt.IsSet() || !SameInputs(Current, LastAttempt.GetValue()))
		{
			StartSolve();
		}
	}
}

void ACGHSolverActor::CancelSolve()
{
	check(IsInGameThread());
	if (ActiveJob)
	{
		ActiveJob->bCancelRequested.store(true, std::memory_order_relaxed);
	}
	PendingSubmission.Reset();
	bAcceptActiveResult = false;
	JobState = ECGHSolverJobState::Idle;
	StatusMessage = TEXT("Cancelled; the last published phase pattern is retained.");
}

void ACGHSolverActor::StopJobs()
{
	if (ActiveJob)
	{
		ActiveJob->bCancelRequested.store(true, std::memory_order_relaxed);
	}
	// The worker owns only plain shared data and exits cooperatively; never wait here.
	ActiveJob.Reset();
	ActiveSubmission.Reset();
	PendingSubmission.Reset();
	LastAttempt.Reset();
	LastPublishedSLM.Reset();
	LastPublishedPhaseRevision = 0;
	bAcceptActiveResult = false;
	JobState = ECGHSolverJobState::Idle;
	StatusMessage = TEXT("Solver stopped; the last published phase pattern is retained.");
}

void ACGHSolverActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	PollSolver();
}

void ACGHSolverActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopJobs();
	Super::EndPlay(EndPlayReason);
}

void ACGHSolverActor::Destroyed()
{
	StopJobs();
	Super::Destroyed();
}

void ACGHSolverActor::BeginDestroy()
{
	StopJobs();
	Super::BeginDestroy();
}
