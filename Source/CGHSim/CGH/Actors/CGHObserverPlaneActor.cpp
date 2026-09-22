#include "CGH/Actors/CGHObserverPlaneActor.h"

#include "CGH/Components/CGHObserverPreviewComponent.h"
#include "CGH/Types/CGHComplexFieldAsset.h"
#include "CGH/Utils/CGHComplexFieldIO.h"
#include "CGH/Utils/CGHUnitConversion.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ACGHObserverPlaneActor::ACGHObserverPlaneActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	FieldAssetSaveFolder.Path = TEXT("/Game/CGHSim/ObserverFields/Generated");
	FieldRawSaveDirectory.Path = TEXT("Saved/CGHSim/ObserverFields");
	ComplexField.ResolutionX = Parameters.ResolutionX;
	ComplexField.ResolutionY = Parameters.ResolutionY;
#if WITH_EDITORONLY_DATA
	FieldPreview = CreateEditorOnlyDefaultSubobject<UCGHObserverPreviewComponent>(TEXT("FieldPreview"));
#endif
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	ActiveAreaRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ActiveAreaRoot"));
	ActiveAreaRoot->SetupAttachment(Root);
	ActiveAreaMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ActiveAreaMesh"));
	ActiveAreaMesh->SetupAttachment(ActiveAreaRoot);
	ActiveAreaMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ActiveAreaMesh->SetCastShadow(false);
	ActiveAreaMesh->SetRelativeScale3D(FVector(0.01));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded()) { ActiveAreaMesh->SetStaticMesh(Cube.Object); }
	OpticalNormal = CreateDefaultSubobject<UArrowComponent>(TEXT("OpticalNormal"));
	OpticalNormal->SetupAttachment(Root);
	OpticalNormal->SetArrowColor(FLinearColor(0.0f, 1.0f, 1.0f));
	OpticalNormal->ArrowLength = 10.0f;
	OpticalNormal->ArrowSize = 0.25f;
	OpticalNormal->SetHiddenInGame(false);
	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(Root);
	Label->SetRelativeLocation(FVector(0.0, 0.0, 8.0));
	Label->SetWorldSize(2.0f);
	Label->SetHorizontalAlignment(EHTA_Center);
}

void ACGHObserverPlaneActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisualization();
}

void ACGHObserverPlaneActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (!IsTemplate()) { RefreshVisualization(); }
}

void ACGHObserverPlaneActor::BeginPlay()
{
	Super::BeginPlay();
	RefreshVisualization();
}

void ACGHObserverPlaneActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	SynchronizeComplexField();
}

bool ACGHObserverPlaneActor::HasValidComplexField() const
{
	return bHasComplexData && Parameters.ResolutionX > 0 && Parameters.ResolutionY > 0
		&& ComplexField.ResolutionX == Parameters.ResolutionX && ComplexField.ResolutionY == Parameters.ResolutionY
		&& ComplexField.Samples.Num() == int64(Parameters.ResolutionX) * Parameters.ResolutionY;
}

void ACGHObserverPlaneActor::SynchronizeComplexField()
{
	if (ComplexField.ResolutionX != Parameters.ResolutionX || ComplexField.ResolutionY != Parameters.ResolutionY)
	{
		ClearComplexField();
	}
}

bool ACGHObserverPlaneActor::SetComplexField(const FCGHComplexField& Field)
{
	return PublishComplexField(Field, nullptr);
}

bool ACGHObserverPlaneActor::SetComplexField(FCGHComplexField&& Field)
{
	return PublishComplexField(Field, &Field);
}

bool ACGHObserverPlaneActor::PublishComplexField(const FCGHComplexField& Field, FCGHComplexField* OwnedField)
{
	check(IsInGameThread());
	SynchronizeComplexField();
	if (Field.ResolutionX != Parameters.ResolutionX || Field.ResolutionY != Parameters.ResolutionY)
	{
		ComplexFieldError = TEXT("Complex-field dimensions must match the observer plane's resolution.");
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

void ACGHObserverPlaneActor::LoadStoredComplexField()
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
	const double PixelPitchXM = CGHUnits::UmToM(Parameters.PixelPitchXUm);
	const double PixelPitchYM = CGHUnits::UmToM(Parameters.PixelPitchYUm);
	const auto MatchesPitch = [](double Current, double Saved)
	{
		return FMath::IsFinite(Current) && Current > 0.0
			&& FMath::Abs(Current - Saved) <= 1.0e-12 * FMath::Max(Current, Saved);
	};
	if (SavedField.ResolutionX != Parameters.ResolutionX || SavedField.ResolutionY != Parameters.ResolutionY
		|| !MatchesPitch(PixelPitchXM, Asset->GetPixelPitchXM())
		|| !MatchesPitch(PixelPitchYM, Asset->GetPixelPitchYM()))
	{
		ComplexFieldError = FString::Printf(
			TEXT("Stored field requires %d x %d pixels at %.12g x %.12g um; this observer uses %d x %d pixels at %.12g x %.12g um. Set the observer resolution and pixel pitches to match before loading. Complex fields are not resampled."),
			SavedField.ResolutionX, SavedField.ResolutionY, Asset->GetPixelPitchXM() * 1.0e6, Asset->GetPixelPitchYM() * 1.0e6,
			Parameters.ResolutionX, Parameters.ResolutionY, Parameters.PixelPitchXUm, Parameters.PixelPitchYUm);
		return;
	}
	SetComplexField(SavedField);
}

bool ACGHObserverPlaneActor::SaveCurrentComplexField()
{
	check(IsInGameThread());
	if (IsTemplate() || IsActorBeingDestroyed() || !HasValidComplexField())
	{
		FieldSaveStatus = TEXT("Nothing to save: reconstruct or load a valid complex field matching the current observer grid first.");
		return false;
	}
	FCGHComplexFieldSaveResult Result;
	if (!CGHComplexFieldIO::Save(ComplexField,
		CGHUnits::UmToM(Parameters.PixelPitchXUm), CGHUnits::UmToM(Parameters.PixelPitchYUm),
		FieldAssetSaveFolder.Path, FieldRawSaveDirectory.Path,
		NSLOCTEXT("CGH", "SavedObserverComplexField", "Observer complex field"), Result))
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

void ACGHObserverPlaneActor::SaveComplexField()
{
	SaveCurrentComplexField();
}

void ACGHObserverPlaneActor::ClearComplexField()
{
	const bool bChanged = bHasComplexData || !ComplexField.Samples.IsEmpty()
		|| ComplexField.ResolutionX != Parameters.ResolutionX || ComplexField.ResolutionY != Parameters.ResolutionY;
	ComplexField.Samples.Empty();
	ComplexField.ResolutionX = Parameters.ResolutionX;
	ComplexField.ResolutionY = Parameters.ResolutionY;
	if (bChanged) { ++ComplexField.Revision; }
	bHasComplexData = false;
	ComplexFieldError.Reset();
#if WITH_EDITOR
	if (FieldPreview) { FieldPreview->InvalidatePreviewTexture(); }
#endif
	UpdateVisualizationComponents();
}

double ACGHObserverPlaneActor::GetActiveWidthMm() const
{
	return Parameters.ResolutionX * CGHUnits::UmToMm(Parameters.PixelPitchXUm);
}

double ACGHObserverPlaneActor::GetActiveHeightMm() const
{
	return Parameters.ResolutionY * CGHUnits::UmToMm(Parameters.PixelPitchYUm);
}

void ACGHObserverPlaneActor::RefreshVisualization()
{
	SynchronizeComplexField();
	UpdateVisualizationComponents();
}

void ACGHObserverPlaneActor::UpdateVisualizationComponents()
{
	const double WidthCm = CGHUnits::MmToCm(GetActiveWidthMm());
	const double HeightCm = CGHUnits::MmToCm(GetActiveHeightMm());
	const bool bValidDimensions = Parameters.ResolutionX > 0 && Parameters.ResolutionY > 0
		&& FMath::IsFinite(WidthCm) && FMath::IsFinite(HeightCm) && WidthCm > 0.0 && HeightCm > 0.0;
	ActiveAreaRoot->SetRelativeScale3D(bValidDimensions ? FVector(0.01, WidthCm, HeightCm) : FVector::ZeroVector);
	ActiveAreaMesh->SetVisibility(bValidDimensions);
	Label->SetText(FText::FromString(FString::Printf(
		TEXT("Observer | %d x %d\nPitch: %.3f x %.3f um\nActive: %.3f x %.3f mm\n%s"),
		Parameters.ResolutionX, Parameters.ResolutionY, Parameters.PixelPitchXUm, Parameters.PixelPitchYUm,
		GetActiveWidthMm(), GetActiveHeightMm(), HasValidComplexField() ? TEXT("Complex field ready") : TEXT("No reconstruction"))));
}
