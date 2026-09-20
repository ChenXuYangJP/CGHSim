#include "CGH/Actors/CGHSLMActor.h"

#include "CGH/Types/CGHPhasePatternAsset.h"
#include "CGH/Utils/CGHUnitConversion.h"
#include "CGH/Utils/CGHPhasePreview.h"
#include "CGH/Components/CGHSLMPreviewComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ACGHSLMActor::ACGHSLMActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	StoredPhasePattern = TSoftObjectPtr<UCGHPhasePatternAsset>(FSoftObjectPath(
		TEXT("/Game/CGHSim/PhasePatterns/DA_SLMPreviewPattern.DA_SLMPreviewPattern")));
	PhasePattern.ResolutionX = Parameters.ResolutionX;
	PhasePattern.ResolutionY = Parameters.ResolutionY;
#if WITH_EDITORONLY_DATA
	PhasePreview = CreateEditorOnlyDefaultSubobject<UCGHSLMPreviewComponent>(TEXT("PhasePreview"));
#endif
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	ActiveAreaRoot = CreateDefaultSubobject<USceneComponent>(TEXT("ActiveAreaRoot"));
	ActiveAreaRoot->SetupAttachment(Root);
	ActiveAreaMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ActiveAreaMesh"));
	ActiveAreaMesh->SetupAttachment(ActiveAreaRoot);
	ActiveAreaMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ActiveAreaMesh->SetCastShadow(false);
	// Engine cube is 100 cm wide. Normalize it to a 1 cm cube below ActiveAreaRoot.
	ActiveAreaMesh->SetRelativeScale3D(FVector(0.01));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded())
	{
		ActiveAreaMesh->SetStaticMesh(Cube.Object);
	}
	OpticalNormal = CreateDefaultSubobject<UArrowComponent>(TEXT("OpticalNormal"));
	OpticalNormal->SetupAttachment(Root);
	OpticalNormal->SetArrowColor(FLinearColor::Green);
	OpticalNormal->ArrowLength = 10.0f;
	OpticalNormal->ArrowSize = 0.25f;
	OpticalNormal->SetHiddenInGame(false);
	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(Root);
	Label->SetRelativeLocation(FVector(0.0, 0.0, 8.0));
	Label->SetWorldSize(2.0f);
	Label->SetHorizontalAlignment(EHTA_Center);
}

void ACGHSLMActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisualization();
}

void ACGHSLMActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (!IsTemplate())
	{
		RefreshVisualization();
	}
}

void ACGHSLMActor::BeginPlay()
{
	Super::BeginPlay();
	RefreshVisualization();
}

void ACGHSLMActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// No phase-array scans here. Direct C++/Blueprint resolution edits invalidate old data.
	SynchronizePhasePattern();
}

bool ACGHSLMActor::HasValidPhasePattern() const
{
	return bHasPhaseData && Parameters.ResolutionX > 0 && Parameters.ResolutionY > 0
		&& PhasePattern.ResolutionX == Parameters.ResolutionX && PhasePattern.ResolutionY == Parameters.ResolutionY
		&& PhasePattern.PhaseRad.Num() == int64(Parameters.ResolutionX) * Parameters.ResolutionY;
}

void ACGHSLMActor::SynchronizePhasePattern()
{
	if (PhasePattern.ResolutionX != Parameters.ResolutionX || PhasePattern.ResolutionY != Parameters.ResolutionY)
	{
		ClearPhasePattern();
	}
}

bool ACGHSLMActor::SetPhasePattern(const FCGHSLMPhasePattern& Pattern)
{
	SynchronizePhasePattern();
	if (!CGHPhasePreview::ValidatePattern(Pattern, Parameters.ResolutionX, Parameters.ResolutionY, PhasePatternError))
	{
		return false;
	}
	if (!HasValidPhasePattern() || PhasePattern.PhaseRad != Pattern.PhaseRad)
	{
		const uint64 NextRevision = PhasePattern.Revision + 1;
		PhasePattern = Pattern;
		PhasePattern.Revision = NextRevision;
	}
	bHasPhaseData = true;
	bIsPreviewPhasePattern = false;
	PhasePatternLabel = FText::GetEmpty();
	GenerationState = ECGHGenerationState::Ready;
	UpdateVisualizationComponents();
	return true;
}

void ACGHSLMActor::LoadStoredPhasePattern()
{
	SynchronizePhasePattern();
	if (StoredPhasePattern.IsNull())
	{
		PhasePatternError = TEXT("Choose a stored phase-pattern asset before loading it.");
		return;
	}
	const UCGHPhasePatternAsset* Asset = StoredPhasePattern.LoadSynchronous();
	if (!Asset)
	{
		PhasePatternError = TEXT("The selected phase-pattern asset could not be loaded.");
		return;
	}
	FCGHSLMPhasePattern LoadedPattern;
	if (!Asset->BuildPatternForResolution(Parameters.ResolutionX, Parameters.ResolutionY,
		LoadedPattern, PhasePatternError))
	{
		return;
	}
	if (SetPhasePattern(LoadedPattern))
	{
		bIsPreviewPhasePattern = Asset->bIsPreviewPattern;
		PhasePatternLabel = Asset->PatternLabel.IsEmpty() ? FText::FromString(Asset->GetName()) : Asset->PatternLabel;
		UpdateVisualizationComponents();
	}
}

void ACGHSLMActor::ClearPhasePattern()
{
	const bool bChanged = !PhasePattern.PhaseRad.IsEmpty() || bHasPhaseData
		|| PhasePattern.ResolutionX != Parameters.ResolutionX || PhasePattern.ResolutionY != Parameters.ResolutionY;
	PhasePattern.PhaseRad.Empty();
	PhasePattern.ResolutionX = Parameters.ResolutionX;
	PhasePattern.ResolutionY = Parameters.ResolutionY;
	if (bChanged)
	{
		++PhasePattern.Revision;
	}
	bHasPhaseData = false;
	bIsPreviewPhasePattern = false;
	PhasePatternLabel = FText::GetEmpty();
	PhasePatternError.Reset();
	GenerationState = ECGHGenerationState::NotImplemented;
#if WITH_EDITOR
	if (PhasePreview)
	{
		PhasePreview->InvalidatePreviewTexture();
	}
#endif
	UpdateVisualizationComponents();
}

void ACGHSLMActor::GeneratePreviewPhaseRamp()
{
	SynchronizePhasePattern();
	const int64 PixelCount = int64(Parameters.ResolutionX) * Parameters.ResolutionY;
	if (Parameters.ResolutionX <= 0 || Parameters.ResolutionY <= 0
		|| Parameters.ResolutionX > CGHPhasePreview::MaximumAxisResolution
		|| Parameters.ResolutionY > CGHPhasePreview::MaximumAxisResolution
		|| PixelCount > CGHPhasePreview::MaximumPixelCount)
	{
		PhasePatternError = TEXT("Preview dimensions must be positive, at most 16384 per axis, and at most 64 million pixels.");
		return;
	}
	FCGHSLMPhasePattern Ramp;
	Ramp.ResolutionX = Parameters.ResolutionX;
	Ramp.ResolutionY = Parameters.ResolutionY;
	Ramp.PhaseRad.SetNumUninitialized(static_cast<int32>(PixelCount));
	for (int32 Y = 0; Y < Ramp.ResolutionY; ++Y)
	{
		for (int32 X = 0; X < Ramp.ResolutionX; ++X)
		{
			Ramp.PhaseRad[Y * Ramp.ResolutionX + X] = UE_DOUBLE_TWO_PI * X / Ramp.ResolutionX;
		}
	}
	if (SetPhasePattern(Ramp))
	{
		bIsPreviewPhasePattern = true;
		PhasePatternLabel = NSLOCTEXT("CGH", "SLMGeneratedTestRamp", "Preview test ramp");
		UpdateVisualizationComponents();
	}
}

double ACGHSLMActor::GetActiveWidthMm() const
{
	return Parameters.ResolutionX * CGHUnits::UmToMm(Parameters.PixelPitchXUm);
}

double ACGHSLMActor::GetActiveHeightMm() const
{
	return Parameters.ResolutionY * CGHUnits::UmToMm(Parameters.PixelPitchYUm);
}

void ACGHSLMActor::RefreshVisualization()
{
	SynchronizePhasePattern();
	UpdateVisualizationComponents();
}

void ACGHSLMActor::UpdateVisualizationComponents()
{
	const double WidthCm = CGHUnits::MmToCm(GetActiveWidthMm());
	const double HeightCm = CGHUnits::MmToCm(GetActiveHeightMm());
	const bool bValidDimensions = Parameters.ResolutionX > 0 && Parameters.ResolutionY > 0
		&& FMath::IsFinite(WidthCm) && FMath::IsFinite(HeightCm) && WidthCm > 0.0 && HeightCm > 0.0;
	ActiveAreaRoot->SetRelativeScale3D(bValidDimensions ? FVector(0.01, WidthCm, HeightCm) : FVector::ZeroVector);
	ActiveAreaMesh->SetVisibility(bValidDimensions);
	const FString PhaseStatus = !HasValidPhasePattern() ? TEXT("No phase pattern")
		: !PhasePatternLabel.IsEmpty() ? PhasePatternLabel.ToString()
		: bIsPreviewPhasePattern ? TEXT("Preview pattern") : TEXT("Pattern ready");
	Label->SetText(FText::FromString(FString::Printf(
		TEXT("SLM | %d x %d\nPitch: %.3f x %.3f um\nActive: %.3f x %.3f mm\nPhase: %s"),
		Parameters.ResolutionX, Parameters.ResolutionY, Parameters.PixelPitchXUm, Parameters.PixelPitchYUm,
		GetActiveWidthMm(), GetActiveHeightMm(), *PhaseStatus)));
}
