#include "CGH/Actors/CGHWorkbenchActor.h"

#include "CGH/Actors/CGHCameraActor.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHTargetActor.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogCGHWorkbench, Log, All);

ACGHWorkbenchActor::ACGHWorkbenchActor()
{
	PrimaryActorTick.bCanEverTick = false;

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
	// Referenced actors may still be loading. Only update our own presentation.
	Label->SetText(FText::FromString(
		TEXT("CGH Workbench\nRun Validate Scene\nOptical solver: not implemented")));
}

bool ACGHWorkbenchActor::ValidateScene()
{
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
	const auto CheckActor = [this, &Check](const AActor* Actor, const FString& Name)
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
		Check(Actor->GetActorScale3D().Equals(FVector::OneVector, KINDA_SMALL_NUMBER),
			FString::Printf(TEXT("%s actor scale must be (1, 1, 1); resize visual components instead."), *Name));
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
		if (!CheckActor(Target, Name))
		{
			continue;
		}

		Check(!UniqueTargets.Contains(Target),
			FString::Printf(TEXT("%s duplicates another target reference."), *Name));
		UniqueTargets.Add(Target);

		const FCGHTargetParameters& Parameters = Target->Parameters;
		Check(Parameters.TargetType == ECGHTargetType::Point,
			FString::Printf(TEXT("%s must be a Point; mesh targets are not implemented."), *Name));
		CheckNonNegative(Parameters.Amplitude, Name + TEXT(" amplitude"));
		CheckFinite(Parameters.InitialPhaseRad, Name + TEXT(" initial phase (rad)"));
	}
	Check(UniqueTargets.Num() > 0, TEXT("At least one valid target actor is required."));

	bSceneValid = ValidationMessages.IsEmpty();
	if (bSceneValid)
	{
		ValidationMessages.Add(FString::Printf(
			TEXT("Scene configuration valid (%d target(s)). Optical solver remains unimplemented."),
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
		TEXT("CGH Workbench\n%s\nOptical solver: not implemented"), *Status)));
	Label->SetTextRenderColor(bSceneValid ? FColor(100, 230, 130) : FColor(255, 170, 70));
}
