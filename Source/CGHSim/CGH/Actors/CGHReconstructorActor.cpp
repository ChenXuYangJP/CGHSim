#include "CGH/Actors/CGHReconstructorActor.h"

#include "CGH/Actors/CGHWorkbenchActor.h"
#include "CGH/Actors/CGHCameraActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHObserverPlaneActor.h"
#include "CGH/Reconstruction/CGHReconstruction.h"
#include "CGH/Reconstruction/CGHCPUReconstructionBackend.h"
#include "CGH/Reconstruction/CGHDockerReconstructionBackend.h"
#include "Components/SceneComponent.h"

ACGHReconstructorActor::ACGHReconstructorActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

bool ACGHReconstructorActor::CaptureSubmission(FCGHReconstructionSubmission& Out, FString& Error)
{
	Out = FCGHReconstructionSubmission();
	if (Parameters.Mode != ECGHReconstructionMode::ObserverPlane && Parameters.Mode != ECGHReconstructionMode::Camera)
	{
		Error = TEXT("Unknown reconstruction mode.");
		return false;
	}
	if (!IsValid(Workbench) || Workbench->IsActorBeingDestroyed() || Workbench->GetWorld() != GetWorld())
	{
		Error = TEXT("Assign a valid Workbench in the reconstructor's world.");
		return false;
	}
	if (!Workbench->CaptureReconstructionInput(Out.Input, Error, Parameters.Mode))
	{
		return false;
	}
	Workbench->SLM->SynchronizePhasePattern();
	if (!Workbench->SLM->HasValidPhasePattern())
	{
		Error = TEXT("The SLM needs a valid phase pattern. Generate or load a pattern before reconstruction.");
		return false;
	}
	Out.Input.Mode = Parameters.Mode;
	Out.Input.PropagationConvention = ECGHPropagationConvention::ExpPositiveIKR;
	Out.Parameters = Parameters;
	Out.Workbench = Workbench;
	Out.SLM = Workbench->SLM;
	Out.Light = Workbench->ReconstructionLight;
	Out.PhaseRevision = Workbench->SLM->GetPhasePatternRevision();
	if (Parameters.Mode == ECGHReconstructionMode::Camera)
	{
		Workbench->Camera->SynchronizeComplexField();
		Out.Camera = Workbench->Camera;
		Out.FieldRevision = Workbench->Camera->GetComplexFieldRevision();
	}
	else
	{
		Workbench->ObserverPlane->SynchronizeComplexField();
		Out.ObserverPlane = Workbench->ObserverPlane;
		Out.FieldRevision = Workbench->ObserverPlane->GetComplexFieldRevision();
	}
	return CGHReconstruction::ValidateScene(Out.Input, Error);
}

bool ACGHReconstructorActor::SameInputs(const FCGHReconstructionSubmission& A, const FCGHReconstructionSubmission& B)
{
	if (A.Workbench != B.Workbench || A.SLM != B.SLM || A.Light != B.Light || A.ObserverPlane != B.ObserverPlane || A.Camera != B.Camera
		|| A.Parameters.ReconstructionBackend != B.Parameters.ReconstructionBackend || A.Parameters.Mode != B.Parameters.Mode
		|| A.PhaseRevision != B.PhaseRevision)
	{
		return false;
	}
	const auto SameScalar = [](double X, double Y)
	{
		return X == Y || (FMath::IsNaN(X) && FMath::IsNaN(Y));
	};
	if (A.Parameters.ReconstructionBackend == ECGHReconstructionBackend::Docker
		&& (A.Parameters.Docker.Address != B.Parameters.Docker.Address
			|| A.Parameters.Docker.Port != B.Parameters.Docker.Port
			|| !SameScalar(A.Parameters.Docker.ConnectTimeoutSeconds, B.Parameters.Docker.ConnectTimeoutSeconds)
			|| !SameScalar(A.Parameters.Docker.RequestTimeoutSeconds, B.Parameters.Docker.RequestTimeoutSeconds)))
	{
		return false;
	}
	const FCGHSLMDescription& SX = A.Input.SLM;
	const FCGHSLMDescription& SY = B.Input.SLM;
	const FCGHReconstructionLightDescription& LX = A.Input.Light;
	const FCGHReconstructionLightDescription& LY = B.Input.Light;
	const FCGHObserverPlaneDescription& OX = A.Input.ObserverPlane;
	const FCGHObserverPlaneDescription& OY = B.Input.ObserverPlane;
	if (SX.ResolutionX != SY.ResolutionX || SX.ResolutionY != SY.ResolutionY
		|| SX.PixelPitchXM != SY.PixelPitchXM || SX.PixelPitchYM != SY.PixelPitchYM || SX.ModulationType != SY.ModulationType
		|| LX.SourceType != LY.SourceType || LX.WavelengthM != LY.WavelengthM
		|| LX.Amplitude != LY.Amplitude || LX.InitialPhaseRad != LY.InitialPhaseRad)
	{
		return false;
	}
	if (A.Parameters.Mode == ECGHReconstructionMode::Camera)
	{
		const FCGHCameraDescription& CX = A.Input.Camera;
		const FCGHCameraDescription& CY = B.Input.Camera;
		if (CX.OpticalPositionSLMM != CY.OpticalPositionSLMM || !CX.OpticalRotationSLM.Equals(CY.OpticalRotationSLM, 0.0)
			|| CX.ForwardDirectionSLM != CY.ForwardDirectionSLM || CX.FocalLengthM != CY.FocalLengthM
			|| CX.FNumber != CY.FNumber || CX.FocusDistanceM != CY.FocusDistanceM
			|| CX.SensorWidthM != CY.SensorWidthM || CX.SensorHeightM != CY.SensorHeightM
			|| CX.OutputResolutionX != CY.OutputResolutionX || CX.OutputResolutionY != CY.OutputResolutionY
			|| CX.PixelPitchXM != CY.PixelPitchXM || CX.PixelPitchYM != CY.PixelPitchYM
			|| CX.PupilResolutionX != CY.PupilResolutionX || CX.PupilResolutionY != CY.PupilResolutionY) return false;
	}
	else if (OX.ResolutionX != OY.ResolutionX || OX.ResolutionY != OY.ResolutionY
		|| OX.PixelPitchXM != OY.PixelPitchXM || OX.PixelPitchYM != OY.PixelPitchYM
		|| OX.PositionSLMM != OY.PositionSLMM || !OX.RotationSLM.Equals(OY.RotationSLM, 0.0))
	{
		return false;
	}
	// Scalar propagation consumes plane-wave direction or point-source position, never polarization.
	return LX.SourceType == ECGHSourceType::PlaneWave
		? LX.DirectionSLM == LY.DirectionSLM : LX.PositionSLMM == LY.PositionSLMM;
}

void ACGHReconstructorActor::FailRequest(const FString& Error)
{
	if (ActiveJob)
	{
		ActiveJob->bCancelRequested.store(true, std::memory_order_relaxed);
	}
	bAcceptActiveResult = false;
	PendingSubmission.Reset();
	JobState = ECGHReconstructionJobState::Failed;
	StatusMessage = Error;
}

bool ACGHReconstructorActor::StartReconstruction()
{
	check(IsInGameThread());
	if (IsTemplate() || IsActorBeingDestroyed() || !GetWorld())
	{
		return false;
	}
	FCGHReconstructionSubmission Submission;
	FString Error;
	if (!CaptureSubmission(Submission, Error))
	{
		LastAttempt.Reset();
		FailRequest(Error);
		return false;
	}
	LastAttempt = Submission;
	++JobId;
	if (Parameters.ReconstructionBackend != ECGHReconstructionBackend::CPU
		&& Parameters.ReconstructionBackend != ECGHReconstructionBackend::Docker)
	{
		FailRequest(TEXT("Unsupported reconstruction backend."));
		return false;
	}
	if (ActiveJob)
	{
		ActiveJob->bCancelRequested.store(true, std::memory_order_relaxed);
	}
	bAcceptActiveResult = false;
	PendingSubmission = MoveTemp(Submission);
	JobState = ECGHReconstructionJobState::Queued;
	StatusMessage = TEXT("Reconstruction request queued.");
	// Reap the previous mailbox before allocating another owned phase snapshot/output buffer.
	if (!ActiveJob)
	{
		SubmitPending();
	}
	return true;
}

void ACGHReconstructorActor::Reconstruct()
{
	StartReconstruction();
}

bool ACGHReconstructorActor::SaveReconstructedComplexField()
{
	check(IsInGameThread());
	if (IsTemplate() || IsActorBeingDestroyed() || JobState != ECGHReconstructionJobState::Ready
		|| ActiveJob || PendingSubmission.IsSet())
	{
		FieldSaveStatus = TEXT("Wait for a reconstructed complex field to reach Ready before saving.");
		return false;
	}
	if (!IsValid(Workbench) || Workbench->IsActorBeingDestroyed() || Workbench->GetWorld() != GetWorld()
		|| Parameters.Mode != LastPublishedMode)
	{
		FieldSaveStatus = TEXT("The selected destination no longer matches the accepted reconstruction. Reconstruct again.");
		return false;
	}
	if (LastPublishedMode == ECGHReconstructionMode::Camera)
	{
		ACGHCameraActor* Camera = LastPublishedCamera.Get();
		if (!IsValid(Camera) || Camera->IsActorBeingDestroyed() || Camera->GetWorld() != GetWorld()
			|| Workbench->Camera != Camera || !Camera->HasValidComplexField()
			|| Camera->GetComplexFieldRevision() != LastPublishedFieldRevision)
		{
			FieldSaveStatus = TEXT("The reconstructed result is no longer the active camera field. Reconstruct again, or save the current field from the camera.");
			return false;
		}
		const bool bSaved = Camera->SaveCurrentComplexField();
		FieldSaveStatus = Camera->FieldSaveStatus;
		return bSaved;
	}
	ACGHObserverPlaneActor* Observer = LastPublishedObserver.Get();
	if (!IsValid(Observer) || Observer->IsActorBeingDestroyed() || Observer->GetWorld() != GetWorld()
		|| Workbench->ObserverPlane != Observer || !Observer->HasValidComplexField()
		|| Observer->GetComplexFieldRevision() != LastPublishedFieldRevision)
	{
		FieldSaveStatus = TEXT("The reconstructed result is no longer the active observer field. Reconstruct again, or save the current field from the observer plane.");
		return false;
	}
	const bool bSaved = Observer->SaveCurrentComplexField();
	FieldSaveStatus = Observer->FieldSaveStatus;
	return bSaved;
}

void ACGHReconstructorActor::SaveComplexField()
{
	SaveReconstructedComplexField();
}

void ACGHReconstructorActor::SubmitPending()
{
	check(!ActiveJob && PendingSubmission.IsSet());
	FCGHReconstructionSubmission Current;
	FString Error;
	if (!CaptureSubmission(Current, Error) || !SameInputs(Current, PendingSubmission.GetValue())
		|| Current.FieldRevision != PendingSubmission->FieldRevision)
	{
		FailRequest(Error.IsEmpty() ? TEXT("Queued inputs or destination field changed; reconstruct again.") : Error);
		return;
	}
	if (PendingSubmission->Parameters.ReconstructionBackend == ECGHReconstructionBackend::Docker)
	{
		if (!Cast<UCGHDockerReconstructionBackend>(Backend))
		{
			Backend = NewObject<UCGHDockerReconstructionBackend>(this);
		}
		CastChecked<UCGHDockerReconstructionBackend>(Backend)->Settings = PendingSubmission->Parameters.Docker;
	}
	else if (!Cast<UCGHCPUReconstructionBackend>(Backend))
	{
		Backend = NewObject<UCGHCPUReconstructionBackend>(this);
	}
	ActiveSubmission = MoveTemp(PendingSubmission.GetValue());
	PendingSubmission.Reset();
	FCGHReconstructionInput Input = ActiveSubmission->Input;
	// Copy once at launch. Subsequent SLM edits cannot mutate the worker's immutable snapshot.
	Input.Pattern = ActiveSubmission->SLM->GetPhasePattern();
	ActiveJob = Backend->Submit(MoveTemp(Input));
	bAcceptActiveResult = ActiveJob.IsValid();
	if (!ActiveJob)
	{
		ActiveSubmission.Reset();
		FailRequest(TEXT("Backend could not queue the reconstruction job."));
	}
}

void ACGHReconstructorActor::PollReconstructor()
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
			FCGHReconstructionSubmission Current;
			FString Error;
			if (!CaptureSubmission(Current, Error) || !ActiveSubmission.IsSet()
				|| !SameInputs(Current, ActiveSubmission.GetValue()) || Current.FieldRevision != ActiveSubmission->FieldRevision)
			{
				FailRequest(Error.IsEmpty() ? TEXT("Result discarded: optical inputs, Docker settings, SLM phase, or destination field changed during reconstruction.") : Error);
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
				const bool bCamera = Current.Parameters.Mode == ECGHReconstructionMode::Camera;
				const bool bPublished = bCamera ? Current.Camera->SetComplexField(MoveTemp(ActiveJob->Result.Field))
					: Current.ObserverPlane->SetComplexField(MoveTemp(ActiveJob->Result.Field));
				if (bPublished)
				{
					LastComputeSeconds = ActiveJob->Result.ComputeSeconds;
					LastPublishedObserver = Current.ObserverPlane;
					LastPublishedCamera = Current.Camera;
					LastPublishedMode = Current.Parameters.Mode;
					LastPublishedFieldRevision = bCamera ? Current.Camera->GetComplexFieldRevision() : Current.ObserverPlane->GetComplexFieldRevision();
					JobState = ECGHReconstructionJobState::Ready;
					StatusMessage = bCamera ? TEXT("Complex optical field published to the camera sensor. Select the camera to preview phase, amplitude, or intensity.")
						: TEXT("Complex optical field published to the observer plane. Select the plane to preview phase, amplitude, or intensity.");
				}
				else FailRequest(TEXT("The selected destination rejected the reconstructed field."));
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
		JobState = ECGHReconstructionJobState::Running;
		StatusMessage = ActiveSubmission->Parameters.ReconstructionBackend == ECGHReconstructionBackend::CPU
			? TEXT("Reconstructing the complex optical field on the CPU worker.")
			: TEXT("Reconstructing the complex optical field through the Docker TCP service.");
	}
	if (bAutoReconstruct)
	{
		FCGHReconstructionSubmission Current;
		FString Error;
		if (!CaptureSubmission(Current, Error))
		{
			LastAttempt.Reset();
			FailRequest(Error);
		}
		else if (!LastAttempt.IsSet() || !SameInputs(Current, LastAttempt.GetValue()))
		{
			StartReconstruction();
		}
	}
}

void ACGHReconstructorActor::CancelReconstruction()
{
	check(IsInGameThread());
	if (ActiveJob)
	{
		ActiveJob->bCancelRequested.store(true, std::memory_order_relaxed);
	}
	PendingSubmission.Reset();
	bAcceptActiveResult = false;
	JobState = ECGHReconstructionJobState::Idle;
	StatusMessage = TEXT("Cancelled; the last published complex field is retained.");
}

void ACGHReconstructorActor::StopJobs()
{
	if (ActiveJob)
	{
		ActiveJob->bCancelRequested.store(true, std::memory_order_relaxed);
	}
	// Workers retain only their shared numerical mailbox and never dereference this actor.
	ActiveJob.Reset();
	ActiveSubmission.Reset();
	PendingSubmission.Reset();
	LastAttempt.Reset();
	LastPublishedObserver.Reset();
	LastPublishedCamera.Reset();
	LastPublishedMode = ECGHReconstructionMode::ObserverPlane;
	LastPublishedFieldRevision = 0;
	bAcceptActiveResult = false;
	JobState = ECGHReconstructionJobState::Idle;
	StatusMessage = TEXT("Reconstructor stopped; the last published complex field is retained.");
}

void ACGHReconstructorActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	PollReconstructor();
}

void ACGHReconstructorActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopJobs();
	Super::EndPlay(EndPlayReason);
}

void ACGHReconstructorActor::Destroyed()
{
	StopJobs();
	Super::Destroyed();
}

void ACGHReconstructorActor::BeginDestroy()
{
	StopJobs();
	Super::BeginDestroy();
}
