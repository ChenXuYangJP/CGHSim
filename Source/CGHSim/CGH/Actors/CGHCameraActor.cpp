#include "CGH/Actors/CGHCameraActor.h"

#include "CGH/Utils/CGHUnitConversion.h"
#include "CineCameraComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"

namespace
{
	float GetPreviewValue(double Value, double DefaultValue)
	{
		// Keep invalid optical inputs available for validation, but out of rendering.
		const double SafeValue = FMath::IsFinite(Value) && Value > 0.0 ? Value : DefaultValue;
		return static_cast<float>(FMath::Clamp(SafeValue, 0.001, 1.0e9));
	}
}

ACGHCameraActor::ACGHCameraActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	OpticalReference = CreateDefaultSubobject<USceneComponent>(TEXT("OpticalReference"));
	OpticalReference->SetupAttachment(Root);

	PreviewCamera = CreateDefaultSubobject<UCineCameraComponent>(TEXT("PreviewCamera"));
	PreviewCamera->SetupAttachment(OpticalReference);

	OpticalForward = CreateDefaultSubobject<UArrowComponent>(TEXT("OpticalForward"));
	OpticalForward->SetupAttachment(OpticalReference);
	OpticalForward->SetArrowColor(FLinearColor(0.2f, 0.65f, 1.0f));
	OpticalForward->ArrowLength = 15.0f;
	OpticalForward->ArrowSize = 0.4f;
	OpticalForward->SetHiddenInGame(false);

	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(Root);
	Label->SetRelativeLocation(FVector(0.0, 0.0, 15.0));
	Label->SetWorldSize(3.0f);
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetTextRenderColor(FColor(80, 170, 255));
}

void ACGHCameraActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisualization();
}

void ACGHCameraActor::RefreshVisualization()
{
	UpdatePreviewCamera();

	Label->SetText(FText::FromString(FString::Printf(
		TEXT("CGH Camera\nGeometric preview\n%.3g mm | f/%.3g\nFocus: %.3g mm\n%d x %d"),
		Parameters.FocalLengthMm, Parameters.FNumber, Parameters.FocusDistanceMm,
		Parameters.OutputResolutionX, Parameters.OutputResolutionY)));
}

void ACGHCameraActor::UpdatePreviewCamera()
{
	const float FocalLengthMm = GetPreviewValue(Parameters.FocalLengthMm, 50.0);
	const float FNumber = GetPreviewValue(Parameters.FNumber, 4.0);

	// Match the derived lens limits to the input instead of inheriting a preset's
	// zoom/aperture limits, which otherwise silently clamp custom optical values.
	FCameraLensSettings LensSettings;
	LensSettings.MinFocalLength = FocalLengthMm;
	LensSettings.MaxFocalLength = FocalLengthMm;
	LensSettings.MinFStop = FNumber;
	LensSettings.MaxFStop = FNumber;
	LensSettings.MinimumFocusDistance = 0.0f;
	PreviewCamera->SetLensSettings(LensSettings);

	FCameraFilmbackSettings Filmback;
	Filmback.SensorWidth = GetPreviewValue(Parameters.SensorWidthMm, 36.0);
	Filmback.SensorHeight = GetPreviewValue(Parameters.SensorHeightMm, 24.0);
	PreviewCamera->SetFilmback(Filmback);
	PreviewCamera->SetCurrentFocalLength(FocalLengthMm);
	PreviewCamera->SetCurrentAperture(FNumber);

	FCameraFocusSettings FocusSettings;
	FocusSettings.FocusMethod = ECameraFocusMethod::Manual;
	FocusSettings.ManualFocusDistance = static_cast<float>(CGHUnits::MmToCm(
		GetPreviewValue(Parameters.FocusDistanceMm, 1000.0)));
	FocusSettings.bSmoothFocusChanges = false;
	PreviewCamera->SetFocusSettings(FocusSettings);
}
