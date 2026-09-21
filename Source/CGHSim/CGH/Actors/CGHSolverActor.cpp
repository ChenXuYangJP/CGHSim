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
	if (Workbench->Targets.Num() != 1 || !Available(Workbench->Targets[0]))
	{
		Error = TEXT("PointFocus requires exactly one valid point target actor in the same world.");
		return false;
	}
	ACGHTargetActor* Target = Workbench->Targets[0];
	if (Target->Parameters.TargetType != ECGHTargetType::Point)
	{
		Error = TEXT("PointFocus supports a single Point target only; mesh targets are not supported.");
		return false;
	}
	if (Workbench->SLM->GetActorTransform().ContainsNaN()
		|| Target->GetActorTransform().ContainsNaN()
		|| !Workbench->SLM->GetActorScale3D().Equals(FVector::OneVector, KINDA_SMALL_NUMBER))
	{
		Error = TEXT("SLM/target transforms must be finite, and the SLM actor scale must be (1, 1, 1).");
		return false;
	}

	Workbench->UpdateSceneDescription();
	if (Target->GetReferenceSLM() != Workbench->SLM || !Target->bResourcesValid)
	{
		Error = TEXT("The target must have valid resources in this workbench's SLM frame.");
		return false;
	}
	Workbench->SLM->SynchronizePhasePattern();
	Out.Input.Scene = Workbench->SceneDescription;
	Out.Input.Algorithm = Parameters.Algorithm;
	Out.Input.PropagationConvention = ECGHPropagationConvention::ExpPositiveIKR;
	Out.Parameters = Parameters;
	Out.Workbench = Workbench;
	Out.SLM = Workbench->SLM;
	Out.Target = Target;
	Out.Light = Workbench->ReconstructionLight;
	Out.PhaseRevision = Workbench->SLM->GetPhasePatternRevision();
	// Validate even fields that do not affect the scalar phase (amplitude/polarization).
	// This also rejects invalid edits made after submission, before any publication.
	return CGHPointFocus::ValidateInput(Out.Input, Error);
}

bool ACGHSolverActor::SameInputs(const FCGHSolverSubmission& A, const FCGHSolverSubmission& B)
{
	if (A.Workbench != B.Workbench || A.SLM != B.SLM || A.Target != B.Target || A.Light != B.Light
		|| A.Parameters.SolverBackend != B.Parameters.SolverBackend || A.Parameters.Algorithm != B.Parameters.Algorithm)
	{
		return false;
	}
	const auto SameScalar = [](double X, double Y)
	{
		// Stable invalid input must not enqueue a new failed request every editor tick.
		return X == Y || (FMath::IsNaN(X) && FMath::IsNaN(Y));
	};
	const FCGHSceneDescription& X = A.Input.Scene;
	const FCGHSceneDescription& Y = B.Input.Scene;
	if (X.SchemaVersion != Y.SchemaVersion || X.Targets.Num() != 1 || Y.Targets.Num() != 1
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
	// Capture validates all accepted inputs. Valid amplitude/polarization changes,
	// plane-wave position, camera, display, and resource revisions do not change phase.
	return X.Targets[0].TargetType == Y.Targets[0].TargetType
		&& SameScalar(X.Targets[0].PositionSLMM.X, Y.Targets[0].PositionSLMM.X)
		&& SameScalar(X.Targets[0].PositionSLMM.Y, Y.Targets[0].PositionSLMM.Y)
		&& SameScalar(X.Targets[0].PositionSLMM.Z, Y.Targets[0].PositionSLMM.Z)
		&& SameScalar(X.Targets[0].PhaseRad, Y.Targets[0].PhaseRad);
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
	StatusMessage = TEXT("PointFocus queued (propagation exp(+i k r)).");
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
					JobState = ECGHSolverJobState::Ready;
					StatusMessage = TEXT("PointFocus phase published with plane-wave illumination compensation (exp(+i k r)).");
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
		StatusMessage = TEXT("Computing PointFocus on the CPU worker.");
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
