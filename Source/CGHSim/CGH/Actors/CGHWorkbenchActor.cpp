#include "CGH/Actors/CGHWorkbenchActor.h"

#include "CGH/Actors/CGHCameraActor.h"
#include "CGH/Actors/CGHObserverPlaneActor.h"
#include "CGH/Actors/CGHReconstructorActor.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHSolverActor.h"
#include "CGH/Actors/CGHTargetActor.h"
#include "CGH/Utils/CGHUnitConversion.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogCGHWorkbench, Log, All);

ACGHWorkbenchActor::ACGHWorkbenchActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(Root);
	Label->SetWorldSize(4.0f);
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetTextRenderColor(FColor::White);
}

void ACGHWorkbenchActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateSceneDescription();
	UpdateStatusLabel();
}

void ACGHWorkbenchActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (IsTemplate() || !GetWorld())
	{
		return;
	}

#if WITH_EDITOR
	// Registration also runs after reconstruction; never accumulate duplicate subscriptions.
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
	FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &ACGHWorkbenchActor::OnSceneObjectPropertyChanged);
	FCoreUObjectDelegates::OnObjectTransacted.RemoveAll(this);
	FCoreUObjectDelegates::OnObjectTransacted.AddUObject(this, &ACGHWorkbenchActor::OnSceneObjectTransacted);
#endif
	UpdateSceneDescription();
}

void ACGHWorkbenchActor::PostUnregisterAllComponents()
{
	RemoveSceneObservers();
#if WITH_EDITOR
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
	FCoreUObjectDelegates::OnObjectTransacted.RemoveAll(this);
#endif
	Super::PostUnregisterAllComponents();
}

void ACGHWorkbenchActor::BeginPlay()
{
	Super::BeginPlay();
	UpdateSceneDescription();
}

void ACGHWorkbenchActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// Public struct members can be written directly by C++/Blueprint without a setter
	// or property event. Sample after normal actor ticks, including in editor viewports.
	UpdateSceneDescription();
}

bool ACGHWorkbenchActor::IsSceneActorAvailable(const AActor* Actor) const
{
	return IsValid(Actor) && !Actor->IsActorBeingDestroyed() && Actor->GetWorld() == GetWorld();
}

void ACGHWorkbenchActor::UpdateSceneDescription()
{
	if (bUpdatingSceneDescription || IsTemplate() || !GetWorld())
	{
		return;
	}
	TGuardValue<bool> UpdatingGuard(bUpdatingSceneDescription, true);
	UpdateSolverStatus();
	UpdateReconstructionStatus();
	RefreshSceneObservers();

	// Rebuild from scratch so deleted/unassigned references never retain stale data.
	FCGHSceneDescription Updated;
	UpdateReconstructionDescriptions(Updated.SLM, Updated.ReconstructionLight);
	bSceneDescriptionComplete = false;
	if (!IsSceneActorAvailable(SLM))
	{
		SceneDescription = MoveTemp(Updated);
		return; // Without the reference frame there are no meaningful local positions.
	}

	const FTransform SLMTransform = SLM->GetActorTransform();
	const auto LocalPositionMeters = [&SLMTransform](const FVector& WorldPosition)
	{
		// Preserve signed projections onto the SLM axes: +X is the optical normal.
		// Points along that normal have X > 0; points behind its plane have X < 0.
		return SLMTransform.InverseTransformPositionNoScale(WorldPosition) * CGHUnits::CmToM(1.0);
	};
	const auto LocalDirection = [&SLMTransform](const FVector& WorldDirection)
	{
		return SLMTransform.InverseTransformVectorNoScale(WorldDirection).GetSafeNormal();
	};

	const bool bHasCamera = IsSceneActorAvailable(Camera);
	if (bHasCamera)
	{
		const FCGHCameraParameters& Parameters = Camera->Parameters;
		const FTransform OpticalTransform = Camera->GetOpticalTransform();
		Updated.Camera.OpticalPositionSLMM = LocalPositionMeters(OpticalTransform.GetLocation());
		Updated.Camera.ForwardDirectionSLM = LocalDirection(OpticalTransform.GetUnitAxis(EAxis::X));
		Updated.Camera.FocalLengthM = CGHUnits::MmToM(Parameters.FocalLengthMm);
		Updated.Camera.FNumber = Parameters.FNumber;
		Updated.Camera.FocusDistanceM = CGHUnits::MmToM(Parameters.FocusDistanceMm);
		Updated.Camera.SensorWidthM = CGHUnits::MmToM(Parameters.SensorWidthMm);
		Updated.Camera.SensorHeightM = CGHUnits::MmToM(Parameters.SensorHeightMm);
		Updated.Camera.OutputResolutionX = Parameters.OutputResolutionX;
		Updated.Camera.OutputResolutionY = Parameters.OutputResolutionY;
	}

	const bool bHasLight = IsSceneActorAvailable(ReconstructionLight);

	bool bHasAllTargets = !Targets.IsEmpty();
	Updated.Targets.Reserve(Targets.Num());
	for (ACGHTargetActor* Target : Targets)
	{
		// Retain array indices even when a reference is missing; completeness reports it.
		FCGHTargetDescription& Description = Updated.Targets.AddDefaulted_GetRef();
		if (!IsSceneActorAvailable(Target))
		{
			bHasAllTargets = false;
			continue;
		}
		// The target owns its geometry and sampled points; the scene retains the same
		// resource identity/revision without copying those potentially large buffers.
		Target->SetWorkbenchSLM(SLM, this);
		Target->UpdateTargetResources();
		if (Target->GetReferenceSLM() != SLM)
		{
			bHasAllTargets = false;
			continue; // Never publish coordinates expressed in a different SLM frame.
		}
		Description = Target->TargetDescription;
		bHasAllTargets &= Target->bResourcesValid;
	}

	bSceneDescriptionComplete = bHasCamera && bHasLight && bHasAllTargets;
	SceneDescription = MoveTemp(Updated);
}

void ACGHWorkbenchActor::UpdateReconstructionDescriptions(
	FCGHSLMDescription& OutSLM, FCGHReconstructionLightDescription& OutLight)
{
	OutSLM = FCGHSLMDescription();
	OutLight = FCGHReconstructionLightDescription();
	ObserverPlaneDescription = FCGHObserverPlaneDescription();
	bObserverPlaneDescriptionAvailable = false;
	if (!IsSceneActorAvailable(SLM))
	{
		return;
	}

	const FCGHSLMParameters& SLMParameters = SLM->Parameters;
	OutSLM.ResolutionX = SLMParameters.ResolutionX;
	OutSLM.ResolutionY = SLMParameters.ResolutionY;
	OutSLM.PixelPitchXM = CGHUnits::UmToM(SLMParameters.PixelPitchXUm);
	OutSLM.PixelPitchYM = CGHUnits::UmToM(SLMParameters.PixelPitchYUm);
	OutSLM.ActiveWidthM = SLMParameters.ResolutionX * OutSLM.PixelPitchXM;
	OutSLM.ActiveHeightM = SLMParameters.ResolutionY * OutSLM.PixelPitchYM;
	OutSLM.ModulationType = SLMParameters.ModulationType;

	const FTransform SLMTransform = SLM->GetActorTransform();
	const auto LocalPositionMeters = [&SLMTransform](const FVector& WorldPosition)
	{
		return SLMTransform.InverseTransformPositionNoScale(WorldPosition) * CGHUnits::CmToM(1.0);
	};
	const auto LocalDirection = [&SLMTransform](const FVector& WorldDirection)
	{
		return SLMTransform.InverseTransformVectorNoScale(WorldDirection).GetSafeNormal();
	};
	if (IsSceneActorAvailable(ReconstructionLight))
	{
		const FCGHLightParameters& Parameters = ReconstructionLight->Parameters;
		OutLight.WavelengthM = CGHUnits::NmToM(Parameters.WavelengthNm);
		OutLight.Amplitude = Parameters.Amplitude;
		OutLight.InitialPhaseRad = Parameters.InitialPhaseRad;
		OutLight.DirectionSLM = LocalDirection(ReconstructionLight->GetPropagationDirection());
		OutLight.SourceType = Parameters.SourceType;
		OutLight.PositionSLMM = LocalPositionMeters(ReconstructionLight->GetActorLocation());
		OutLight.PolarizationAngleRad = FMath::DegreesToRadians(Parameters.PolarizationAngleDeg);
	}

	if (IsSceneActorAvailable(ObserverPlane))
	{
		const FCGHObserverPlaneParameters& Parameters = ObserverPlane->Parameters;
		ObserverPlaneDescription.ResolutionX = Parameters.ResolutionX;
		ObserverPlaneDescription.ResolutionY = Parameters.ResolutionY;
		ObserverPlaneDescription.PixelPitchXM = CGHUnits::UmToM(Parameters.PixelPitchXUm);
		ObserverPlaneDescription.PixelPitchYM = CGHUnits::UmToM(Parameters.PixelPitchYUm);
		ObserverPlaneDescription.PositionSLMM = LocalPositionMeters(ObserverPlane->GetActorLocation());
		ObserverPlaneDescription.RotationSLM = SLMTransform.GetRotation().Inverse() * ObserverPlane->GetActorQuat();
		bObserverPlaneDescriptionAvailable = true;
	}
}

bool ACGHWorkbenchActor::CaptureReconstructionInput(FCGHReconstructionInput& OutInput, FString& OutError)
{
	OutInput = FCGHReconstructionInput();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Reconstruction inputs must be captured on the game thread.");
		return false;
	}
	if (IsTemplate() || !GetWorld())
	{
		OutError = TEXT("Reconstruction requires a workbench in a world.");
		return false;
	}
	const auto CheckActor = [this, &OutError](const AActor* Actor, const TCHAR* Name)
	{
		if (!IsSceneActorAvailable(Actor))
		{
			OutError = FString::Printf(TEXT("%s must reference an available actor in the workbench's world."), Name);
			return false;
		}
		const FTransform Transform = Actor->GetActorTransform();
		if (Transform.ContainsNaN() || !Transform.GetRotation().IsNormalized())
		{
			OutError = FString::Printf(TEXT("%s requires a finite transform and normalized rotation."), Name);
			return false;
		}
		if (!Transform.GetScale3D().Equals(FVector::OneVector, KINDA_SMALL_NUMBER))
		{
			OutError = FString::Printf(TEXT("%s actor scale must be (1, 1, 1); use pixel pitch to set physical size."), Name);
			return false;
		}
		return true;
	};
	if (!CheckActor(SLM, TEXT("SLM")) || !CheckActor(ReconstructionLight, TEXT("Reconstruction light"))
		|| !CheckActor(ObserverPlane, TEXT("Observer plane")))
	{
		return false;
	}

	// Reconstruction needs only optical metadata. Do not rebuild target meshes/clouds
	// or require a camera merely to capture or compare a reconstruction request.
	UpdateReconstructionDescriptions(SceneDescription.SLM, SceneDescription.ReconstructionLight);
	OutInput.SLM = SceneDescription.SLM;
	OutInput.Light = SceneDescription.ReconstructionLight;
	OutInput.ObserverPlane = ObserverPlaneDescription;
	OutInput.Mode = ECGHReconstructionMode::ObserverPlane;
	return true;
}

void ACGHWorkbenchActor::RefreshSceneObservers()
{
	TArray<TWeakObjectPtr<AActor>> Actors;
	TArray<TWeakObjectPtr<USceneComponent>> Components;
	const auto ObserveActor = [this, &Actors, &Components](AActor* Actor)
	{
		if (IsSceneActorAvailable(Actor))
		{
			Actors.AddUnique(Actor);
			if (USceneComponent* Component = Actor->GetRootComponent())
			{
				Components.AddUnique(Component);
			}
		}
	};
	ObserveActor(SLM);
	ObserveActor(Camera);
	ObserveActor(ReconstructionLight);
	ObserveActor(ObserverPlane);
	for (ACGHTargetActor* Target : Targets)
	{
		ObserveActor(Target);
		if (IsSceneActorAvailable(Target) && IsValid(Target->GeometryMesh))
		{
			Components.AddUnique(Target->GeometryMesh);
		}
	}
	if (IsSceneActorAvailable(Camera) && IsValid(Camera->GetOpticalReference()))
	{
		Components.AddUnique(Camera->GetOpticalReference());
	}

	if (Actors == ObservedActors && Components == ObservedComponents)
	{
		return;
	}

	RemoveSceneObservers();
	ObservedActors = MoveTemp(Actors);
	ObservedComponents = MoveTemp(Components);
	for (const TWeakObjectPtr<AActor>& Actor : ObservedActors)
	{
		Actor->OnDestroyed.AddUniqueDynamic(this, &ACGHWorkbenchActor::OnSceneActorDestroyed);
	}
	for (const TWeakObjectPtr<USceneComponent>& Component : ObservedComponents)
	{
		Component->TransformUpdated.AddUObject(this, &ACGHWorkbenchActor::OnSceneTransformUpdated);
	}
}

void ACGHWorkbenchActor::RemoveSceneObservers()
{
	for (const TWeakObjectPtr<AActor>& Actor : ObservedActors)
	{
		if (Actor.IsValid())
		{
			Actor->OnDestroyed.RemoveDynamic(this, &ACGHWorkbenchActor::OnSceneActorDestroyed);
		}
	}
	for (const TWeakObjectPtr<USceneComponent>& Component : ObservedComponents)
	{
		if (Component.IsValid())
		{
			Component->TransformUpdated.RemoveAll(this);
		}
	}
	ObservedActors.Reset();
	ObservedComponents.Reset();
}

void ACGHWorkbenchActor::OnSceneTransformUpdated(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport)
{
	UpdateSceneDescription();
}

void ACGHWorkbenchActor::OnSceneActorDestroyed(AActor* Actor)
{
	UpdateSceneDescription();
}

#if WITH_EDITOR
bool ACGHWorkbenchActor::IsSceneObject(const UObject* Object) const
{
	if (!IsValid(Object) || Object->IsTemplate())
	{
		return false;
	}
	const AActor* Actor = Cast<AActor>(Object);
	if (!Actor)
	{
		Actor = Object->GetTypedOuter<AActor>();
	}
	return Actor && (Actor == this || Actor == SLM || Actor == Camera || Actor == ReconstructionLight || Actor == ObserverPlane
		|| Targets.Contains(Actor));
}

void ACGHWorkbenchActor::OnSceneObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	if (IsSceneObject(Object))
	{
		UpdateSceneDescription();
	}
}

void ACGHWorkbenchActor::OnSceneObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event)
{
	if (IsSceneObject(Object))
	{
		UpdateSceneDescription();
	}
}
#endif

bool ACGHWorkbenchActor::ValidateScene()
{
	UpdateSceneDescription();
	ValidationMessages.Reset();

	const auto Check = [this](bool bCondition, const FString& Message)
	{
		if (!bCondition)
		{
			ValidationMessages.Add(Message);
		}
	};
	const auto CheckPositive = [&Check](double Value, const TCHAR* Name)
	{
		Check(FMath::IsFinite(Value) && Value > 0.0,
			FString::Printf(TEXT("%s must be finite and greater than zero."), Name));
	};
	const auto CheckNonNegative = [&Check](double Value, const FString& Name)
	{
		Check(FMath::IsFinite(Value) && Value >= 0.0,
			FString::Printf(TEXT("%s must be finite and nonnegative."), *Name));
	};
	const auto CheckFinite = [&Check](double Value, const FString& Name)
	{
		Check(FMath::IsFinite(Value), FString::Printf(TEXT("%s must be finite."), *Name));
	};
	const auto CheckActor = [this, &Check](const AActor* Actor, const FString& Name, bool bAllowScale = false)
	{
		if (!IsValid(Actor))
		{
			Check(false, FString::Printf(TEXT("%s must reference a valid actor."), *Name));
			return false;
		}

		Check(Actor->GetWorld() == GetWorld(),
			FString::Printf(TEXT("%s must belong to the same world as the workbench."), *Name));
		Check(!Actor->GetActorTransform().ContainsNaN(),
			FString::Printf(TEXT("%s transform must contain only finite values."), *Name));
		if (!bAllowScale)
		{
			Check(Actor->GetActorScale3D().Equals(FVector::OneVector, KINDA_SMALL_NUMBER),
				FString::Printf(TEXT("%s actor scale must be (1, 1, 1); resize visual components instead."), *Name));
		}
		return true;
	};

	if (CheckActor(SLM, TEXT("SLM")))
	{
		const FCGHSLMParameters& Parameters = SLM->Parameters;
		Check(Parameters.ResolutionX > 0 && Parameters.ResolutionY > 0,
			TEXT("SLM resolutions must both be greater than zero."));
		CheckPositive(Parameters.PixelPitchXUm, TEXT("SLM pixel pitch X (um)"));
		CheckPositive(Parameters.PixelPitchYUm, TEXT("SLM pixel pitch Y (um)"));
	}

	if (CheckActor(Camera, TEXT("Camera")))
	{
		const FCGHCameraParameters& Parameters = Camera->Parameters;
		CheckPositive(Parameters.FocalLengthMm, TEXT("Camera focal length (mm)"));
		CheckPositive(Parameters.FNumber, TEXT("Camera f-number"));
		CheckPositive(Parameters.FocusDistanceMm, TEXT("Camera focus distance (mm)"));
		CheckPositive(Parameters.SensorWidthMm, TEXT("Camera sensor width (mm)"));
		CheckPositive(Parameters.SensorHeightMm, TEXT("Camera sensor height (mm)"));
		Check(Parameters.OutputResolutionX > 0 && Parameters.OutputResolutionY > 0,
			TEXT("Camera output resolutions must both be greater than zero."));
	}

	if (CheckActor(ReconstructionLight, TEXT("Reconstruction Light")))
	{
		const FCGHLightParameters& Parameters = ReconstructionLight->Parameters;
		CheckPositive(Parameters.WavelengthNm, TEXT("Light wavelength (nm)"));
		CheckNonNegative(Parameters.Amplitude, TEXT("Light amplitude"));
		CheckFinite(Parameters.InitialPhaseRad, TEXT("Light initial phase (rad)"));
		CheckFinite(Parameters.PolarizationAngleDeg, TEXT("Light polarization angle (deg)"));
	}

	TSet<ACGHTargetActor*> UniqueTargets;
	for (int32 Index = 0; Index < Targets.Num(); ++Index)
	{
		ACGHTargetActor* Target = Targets[Index];
		const FString Name = FString::Printf(TEXT("Target[%d]"), Index);
		if (!CheckActor(Target, Name, true))
		{
			continue;
		}

		Check(!UniqueTargets.Contains(Target),
			FString::Printf(TEXT("%s duplicates another target reference."), *Name));
		UniqueTargets.Add(Target);

		const FCGHTargetParameters& Parameters = Target->Parameters;
		Check(Target->GetReferenceSLM() == SLM,
			FString::Printf(TEXT("%s must use the same SLM reference as the workbench."), *Name));
		Check(Target->bResourcesValid,
			FString::Printf(TEXT("%s resources are invalid: %s"), *Name, *Target->ResourceError));
		CheckNonNegative(Parameters.Amplitude, Name + TEXT(" amplitude"));
		CheckFinite(Parameters.InitialPhaseRad, Name + TEXT(" initial phase (rad)"));
	}
	Check(UniqueTargets.Num() > 0, TEXT("At least one valid target actor is required."));

	bSceneValid = ValidationMessages.IsEmpty();
	if (bSceneValid)
	{
		ValidationMessages.Add(FString::Printf(
			TEXT("Scene configuration valid (%d target(s))."),
			UniqueTargets.Num()));
		UE_LOG(LogCGHWorkbench, Display, TEXT("%s"), *ValidationMessages[0]);
	}
	else
	{
		for (const FString& Message : ValidationMessages)
		{
			UE_LOG(LogCGHWorkbench, Warning, TEXT("%s"), *Message);
		}
	}

	UpdateStatusLabel();
	return bSceneValid;
}

void ACGHWorkbenchActor::ValidateSceneInEditor()
{
	ValidateScene();
}

void ACGHWorkbenchActor::RefreshVisualization()
{
	if (IsValid(SLM))
	{
		SLM->RefreshVisualization();
	}
	if (IsValid(Camera))
	{
		Camera->RefreshVisualization();
	}
	if (IsValid(ReconstructionLight))
	{
		ReconstructionLight->RefreshVisualization();
	}
	if (IsValid(ObserverPlane))
	{
		ObserverPlane->RefreshVisualization();
	}
	for (ACGHTargetActor* Target : Targets)
	{
		if (IsValid(Target))
		{
			Target->RefreshVisualization();
		}
	}

	ValidateScene();
}

void ACGHWorkbenchActor::UpdateStatusLabel()
{
	const FString Status = bSceneValid
		? TEXT("Configuration valid")
		: FString::Printf(TEXT("Configuration invalid (%d issue(s))"), ValidationMessages.Num());
	Label->SetText(FText::FromString(FString::Printf(
		TEXT("CGH Workbench\n%s\nSolver: %s\nReconstruction: %s"),
		*Status, *SolverStatusMessage, *ReconstructionStatusMessage)));
	Label->SetTextRenderColor(bSceneValid ? FColor(100, 230, 130) : FColor(255, 170, 70));
}

void ACGHWorkbenchActor::UpdateSolverStatus()
{
	const ECGHSolverJobState PreviousState = SolverJobState;
	const FString PreviousMessage = SolverStatusMessage;
	if (!IsSceneActorAvailable(Solver))
	{
		SolverJobState = ECGHSolverJobState::Idle;
		SolverStatusMessage = TEXT("No solver assigned in this world.");
	}
	else if (Solver->Workbench != this)
	{
		SolverJobState = ECGHSolverJobState::Idle;
		SolverStatusMessage = TEXT("Assign this workbench on the solver, or press Solve Phase Pattern to bind an unassigned solver.");
	}
	else
	{
		SolverJobState = Solver->JobState;
		SolverStatusMessage = Solver->StatusMessage;
	}
	if (PreviousState != SolverJobState || PreviousMessage != SolverStatusMessage)
	{
		UpdateStatusLabel();
	}
}

void ACGHWorkbenchActor::SolvePhasePattern()
{
	if (IsSceneActorAvailable(Solver))
	{
		if (!IsValid(Solver->Workbench))
		{
			Solver->Workbench = this;
		}
		if (Solver->Workbench == this)
		{
			Solver->StartSolve();
		}
	}
	UpdateSolverStatus();
}

void ACGHWorkbenchActor::CancelSolve()
{
	if (IsSceneActorAvailable(Solver) && Solver->Workbench == this)
	{
		Solver->CancelSolve();
	}
	UpdateSolverStatus();
}

void ACGHWorkbenchActor::UpdateReconstructionStatus()
{
	const ECGHReconstructionJobState PreviousState = ReconstructionJobState;
	const FString PreviousMessage = ReconstructionStatusMessage;
	if (!IsSceneActorAvailable(Reconstructor))
	{
		ReconstructionJobState = ECGHReconstructionJobState::Idle;
		ReconstructionStatusMessage = TEXT("No reconstructor assigned in this world.");
	}
	else if (Reconstructor->Workbench != this)
	{
		ReconstructionJobState = ECGHReconstructionJobState::Idle;
		ReconstructionStatusMessage = TEXT("Assign this workbench on the reconstructor, or press Reconstruct to bind an unassigned reconstructor.");
	}
	else
	{
		ReconstructionJobState = Reconstructor->JobState;
		ReconstructionStatusMessage = Reconstructor->StatusMessage;
	}
	if (PreviousState != ReconstructionJobState || PreviousMessage != ReconstructionStatusMessage)
	{
		UpdateStatusLabel();
	}
}

void ACGHWorkbenchActor::Reconstruct()
{
	if (IsSceneActorAvailable(Reconstructor))
	{
		if (!IsValid(Reconstructor->Workbench))
		{
			Reconstructor->Workbench = this;
		}
		if (Reconstructor->Workbench == this)
		{
			Reconstructor->StartReconstruction();
		}
	}
	UpdateReconstructionStatus();
}

void ACGHWorkbenchActor::CancelReconstruction()
{
	if (IsSceneActorAvailable(Reconstructor) && Reconstructor->Workbench == this)
	{
		Reconstructor->CancelReconstruction();
	}
	UpdateReconstructionStatus();
}
