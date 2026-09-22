#include "CGH/Actors/CGHCameraActor.h"

#include "CGH/Utils/CGHUnitConversion.h"
#include "CGH/Components/CGHObserverPreviewComponent.h"
#include "CGH/Types/CGHComplexFieldAsset.h"
#include "CGH/Utils/CGHComplexFieldIO.h"
#include "CineCameraComponent.h"
#include "Engine/World.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#if WITH_EDITOR
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#endif

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
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	FieldAssetSaveFolder.Path = TEXT("/Game/CGHSim/CameraFields/Generated");
	FieldRawSaveDirectory.Path = TEXT("Saved/CGHSim/CameraFields");
	ComplexField.ResolutionX = Parameters.OutputResolutionX;
	ComplexField.ResolutionY = Parameters.OutputResolutionY;
#if WITH_EDITORONLY_DATA
	FieldPreview = CreateEditorOnlyDefaultSubobject<UCGHObserverPreviewComponent>(TEXT("FieldPreview"));
#endif

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	OpticalReference = CreateDefaultSubobject<USceneComponent>(TEXT("OpticalReference"));
	OpticalReference->SetupAttachment(Root);

	PreviewCamera = CreateDefaultSubobject<UCineCameraComponent>(TEXT("PreviewCamera"));
	PreviewCamera->SetupAttachment(OpticalReference);
#if WITH_EDITOR
	// Native selection prioritizes active camera components. Route the inset through our switching field preview.
	PreviewCamera->bAutoActivate = false;
	PreviewCamera->SetActive(false);
#endif

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

FTransform ACGHCameraActor::GetOpticalTransform() const
{
	return IsValid(OpticalReference) ? OpticalReference->GetComponentTransform() : GetActorTransform();
}

USceneComponent* ACGHCameraActor::GetOpticalReference() const
{
	return OpticalReference;
}

void ACGHCameraActor::RefreshVisualization()
{
	SynchronizeComplexField();
	UpdatePreviewCamera();
	UpdateVisualizationComponents();
}

void ACGHCameraActor::UpdateVisualizationComponents()
{
	Label->SetText(FText::FromString(FString::Printf(
		TEXT("CGH Camera\n%.3g mm | f/%.3g\nFocus: %.3g mm\n%d x %d | %.3g x %.3g um\n%s"),
		Parameters.FocalLengthMm, Parameters.FNumber, Parameters.FocusDistanceMm,
		Parameters.OutputResolutionX, Parameters.OutputResolutionY,
		GetSensorPixelPitchXM() * 1.e6, GetSensorPixelPitchYM() * 1.e6,
		HasValidComplexField() ? TEXT("Sensor field ready") : TEXT("No optical reconstruction"))));
}

void ACGHCameraActor::UpdatePreviewCamera()
{
#if WITH_EDITOR
	// Editor actor selection uses the optical inset; PIE/game camera views still need an active Cine Camera.
	PreviewCamera->SetActive(GetWorld() && GetWorld()->IsGameWorld());
#endif
	const float FocalLengthMm = GetPreviewValue(Parameters.FocalLengthMm, 50.0);
	const float FNumber = GetPreviewValue(Parameters.FNumber, 4.0);

	// Keep the preview range open: equal min/max values lock the native Details
	// controls before their property-change events can update our optical inputs.
	FCameraLensSettings LensSettings;
	LensSettings.MinFocalLength = 0.001f;
	LensSettings.MaxFocalLength = 1.0e9f;
	LensSettings.MinFStop = 0.001f;
	LensSettings.MaxFStop = 1.0e9f;
	LensSettings.MinimumFocusDistance = 0.0f;
	PreviewCamera->SetLensSettings(LensSettings);

	FCameraFilmbackSettings Filmback;
	Filmback.SensorWidth = GetPreviewValue(GetSensorWidthM() * 1000.0, 36.0);
	Filmback.SensorHeight = GetPreviewValue(GetSensorHeightM() * 1000.0, 24.0);
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

void ACGHCameraActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (!IsTemplate())
	{
#if WITH_EDITOR
		FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
		FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &ACGHCameraActor::OnPreviewCameraPropertyChanged);
#endif
		RefreshVisualization();
	}
}

void ACGHCameraActor::PostUnregisterAllComponents()
{
#if WITH_EDITOR
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
#endif
	Super::PostUnregisterAllComponents();
}

#if WITH_EDITOR
void ACGHCameraActor::OnPreviewCameraPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	// UObject broadcasts before the component reruns owner construction. Capture only
	// the explicitly edited control; copying all floats would lose optical precision.
	if (Object != PreviewCamera || IsTemplate() || IsActorBeingDestroyed()) { return; }
	const FName Name = Event.GetPropertyName();
	double* OpticalValue = nullptr;
	double EditedValue = 0.0;
	if (Name == GET_MEMBER_NAME_CHECKED(UCineCameraComponent, CurrentFocalLength))
	{
		OpticalValue = &Parameters.FocalLengthMm;
		EditedValue = PreviewCamera->CurrentFocalLength;
	}
	else if (Name == GET_MEMBER_NAME_CHECKED(UCineCameraComponent, CurrentAperture))
	{
		OpticalValue = &Parameters.FNumber;
		EditedValue = PreviewCamera->CurrentAperture;
	}
	else if (Name == GET_MEMBER_NAME_CHECKED(FCameraFocusSettings, ManualFocusDistance)
		|| Name == GET_MEMBER_NAME_CHECKED(UCineCameraComponent, FocusSettings))
	{
		OpticalValue = &Parameters.FocusDistanceMm;
		EditedValue = double(PreviewCamera->FocusSettings.ManualFocusDistance) * 10.0; // UE cm -> optical mm
	}
	if (!OpticalValue || *OpticalValue == EditedValue) { return; }

	// Join the component's Details transaction so Undo/Redo restores both representations.
	Modify();
	*OpticalValue = EditedValue;
	RefreshVisualization();

	// Notify consumers without triggering a second construction pass inside the component's edit.
	FPropertyChangedEvent ParametersEvent(FindFProperty<FProperty>(StaticClass(), GET_MEMBER_NAME_CHECKED(ACGHCameraActor, Parameters)), Event.ChangeType);
	UObject::PostEditChangeProperty(ParametersEvent);
}
#endif

void ACGHCameraActor::BeginPlay()
{
	Super::BeginPlay();
	RefreshVisualization();
}

void ACGHCameraActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	SynchronizeComplexField();
}

double ACGHCameraActor::GetSensorPixelPitchXM() const
{
	switch (Parameters.SensorSampling)
	{
	case ECGHCameraSensorSampling::SensorSize: return Parameters.OutputResolutionX > 0 ? CGHUnits::MmToM(Parameters.SensorWidthMm) / Parameters.OutputResolutionX : 0.0;
	case ECGHCameraSensorSampling::PixelPitch: return CGHUnits::UmToM(Parameters.PixelPitchXUm);
	default: return 0.0;
	}
}

double ACGHCameraActor::GetSensorPixelPitchYM() const
{
	switch (Parameters.SensorSampling)
	{
	case ECGHCameraSensorSampling::SensorSize: return Parameters.OutputResolutionY > 0 ? CGHUnits::MmToM(Parameters.SensorHeightMm) / Parameters.OutputResolutionY : 0.0;
	case ECGHCameraSensorSampling::PixelPitch: return CGHUnits::UmToM(Parameters.PixelPitchYUm);
	default: return 0.0;
	}
}

double ACGHCameraActor::GetSensorWidthM() const
{
	switch (Parameters.SensorSampling)
	{
	case ECGHCameraSensorSampling::SensorSize: return CGHUnits::MmToM(Parameters.SensorWidthMm);
	case ECGHCameraSensorSampling::PixelPitch: return Parameters.OutputResolutionX * CGHUnits::UmToM(Parameters.PixelPitchXUm);
	default: return 0.0;
	}
}

double ACGHCameraActor::GetSensorHeightM() const
{
	switch (Parameters.SensorSampling)
	{
	case ECGHCameraSensorSampling::SensorSize: return CGHUnits::MmToM(Parameters.SensorHeightMm);
	case ECGHCameraSensorSampling::PixelPitch: return Parameters.OutputResolutionY * CGHUnits::UmToM(Parameters.PixelPitchYUm);
	default: return 0.0;
	}
}

bool ACGHCameraActor::HasValidComplexField() const
{
	return bHasComplexData && Parameters.OutputResolutionX > 0 && Parameters.OutputResolutionY > 0
		&& ComplexField.ResolutionX == Parameters.OutputResolutionX && ComplexField.ResolutionY == Parameters.OutputResolutionY
		&& ComplexField.Samples.Num() == int64(Parameters.OutputResolutionX) * Parameters.OutputResolutionY;
}

void ACGHCameraActor::SynchronizeComplexField()
{
	if (ComplexField.ResolutionX != Parameters.OutputResolutionX || ComplexField.ResolutionY != Parameters.OutputResolutionY)
	{
		ClearComplexField();
	}
}

bool ACGHCameraActor::SetComplexField(const FCGHComplexField& Field)
{
	return PublishComplexField(Field, nullptr);
}

bool ACGHCameraActor::SetComplexField(FCGHComplexField&& Field)
{
	return PublishComplexField(Field, &Field);
}

bool ACGHCameraActor::PublishComplexField(const FCGHComplexField& Field, FCGHComplexField* OwnedField)
{
	check(IsInGameThread());
	SynchronizeComplexField();
	if (Field.ResolutionX != Parameters.OutputResolutionX || Field.ResolutionY != Parameters.OutputResolutionY)
	{
		ComplexFieldError = TEXT("Complex-field dimensions must match the camera sensor's resolution.");
		return false;
	}
	if (!Field.IsValid(&ComplexFieldError)) { return false; }
	if (!HasValidComplexField() || ComplexField.Samples != Field.Samples)
	{
		const uint64 NextRevision = ComplexField.Revision + 1;
		if (OwnedField) { ComplexField = MoveTemp(*OwnedField); }
		else { ComplexField = Field; }
		ComplexField.Revision = NextRevision;
	}
	bHasComplexData = true;
	UpdateVisualizationComponents();
	return true;
}

void ACGHCameraActor::LoadStoredComplexField()
{
	check(IsInGameThread());
	if (StoredComplexField.IsNull())
	{
		ComplexFieldError = TEXT("Choose a stored complex-field asset before loading it.");
		return;
	}
	const UCGHComplexFieldAsset* Asset = StoredComplexField.LoadSynchronous();
	if (!Asset)
	{
		ComplexFieldError = TEXT("The selected complex-field asset could not be loaded.");
		return;
	}
	if (!Asset->HasValidField())
	{
		ComplexFieldError = TEXT("The selected complex-field asset contains invalid samples or pixel pitches.");
		return;
	}
	const FCGHComplexField& SavedField = Asset->GetField();
	const double PixelPitchXM = GetSensorPixelPitchXM();
	const double PixelPitchYM = GetSensorPixelPitchYM();
	const auto MatchesPitch = [](double Current, double Saved)
	{
		return FMath::IsFinite(Current) && Current > 0.0
			&& FMath::Abs(Current - Saved) <= 1.0e-12 * FMath::Max(Current, Saved);
	};
	if (SavedField.ResolutionX != Parameters.OutputResolutionX || SavedField.ResolutionY != Parameters.OutputResolutionY
		|| !MatchesPitch(PixelPitchXM, Asset->GetPixelPitchXM())
		|| !MatchesPitch(PixelPitchYM, Asset->GetPixelPitchYM()))
	{
		ComplexFieldError = FString::Printf(
			TEXT("Stored field requires %d x %d pixels at %.12g x %.12g um; this camera uses %d x %d pixels at %.12g x %.12g um. Set the camera sensor resolution and effective pixel pitches to match before loading. Complex fields are not resampled."),
			SavedField.ResolutionX, SavedField.ResolutionY, Asset->GetPixelPitchXM() * 1.0e6, Asset->GetPixelPitchYM() * 1.0e6,
			Parameters.OutputResolutionX, Parameters.OutputResolutionY, GetSensorPixelPitchXM() * 1.e6, GetSensorPixelPitchYM() * 1.e6);
		return;
	}
	SetComplexField(SavedField);
}

bool ACGHCameraActor::SaveCurrentComplexField()
{
	check(IsInGameThread());
	if (IsTemplate() || IsActorBeingDestroyed() || !HasValidComplexField())
	{
		FieldSaveStatus = TEXT("Nothing to save: reconstruct or load a valid complex field matching the current camera sensor grid first.");
		return false;
	}
	FCGHComplexFieldSaveResult Result;
	if (!CGHComplexFieldIO::Save(ComplexField,
		GetSensorPixelPitchXM(), GetSensorPixelPitchYM(),
		FieldAssetSaveFolder.Path, FieldRawSaveDirectory.Path,
		NSLOCTEXT("CGH", "SavedCameraComplexField", "Camera sensor complex field"), Result, CGHComplexFieldIO::ECoordinateFrame::CameraSensor))
	{
		FieldSaveStatus = Result.Error;
		return false;
	}
	LastSavedFieldAsset = TSoftObjectPtr<UCGHComplexFieldAsset>(FSoftObjectPath(Result.AssetPath));
	LastSavedFieldBinaryFile = Result.BinaryFilename;
	LastSavedFieldMetadataFile = Result.MetadataFilename;
	LastSavedFieldPhaseImageFile = Result.PhaseImageFilename;
	LastSavedFieldAmplitudeImageFile = Result.AmplitudeImageFilename;
	LastSavedFieldIntensityImageFile = Result.IntensityImageFilename;
	FieldSaveStatus = FString::Printf(
		TEXT("Saved complex-field asset: %s\nRaw complex field: %s\nMetadata: %s\nPhase image: %s\nAmplitude image: %s\nIntensity image: %s"),
		*Result.AssetPath, *Result.BinaryFilename, *Result.MetadataFilename,
		*Result.PhaseImageFilename, *Result.AmplitudeImageFilename, *Result.IntensityImageFilename);
	return true;
}

void ACGHCameraActor::SaveComplexField()
{
	SaveCurrentComplexField();
}

void ACGHCameraActor::ClearComplexField()
{
	const bool bChanged = bHasComplexData || !ComplexField.Samples.IsEmpty()
		|| ComplexField.ResolutionX != Parameters.OutputResolutionX || ComplexField.ResolutionY != Parameters.OutputResolutionY;
	ComplexField.Samples.Empty();
	ComplexField.ResolutionX = Parameters.OutputResolutionX;
	ComplexField.ResolutionY = Parameters.OutputResolutionY;
	if (bChanged) { ++ComplexField.Revision; }
	bHasComplexData = false;
	ComplexFieldError.Reset();
#if WITH_EDITOR
	if (FieldPreview) { FieldPreview->InvalidatePreviewTexture(); }
#endif
	UpdateVisualizationComponents();
}
