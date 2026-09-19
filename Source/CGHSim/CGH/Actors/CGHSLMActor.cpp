#include "CGH/Actors/CGHSLMActor.h"

#include "CGH/Utils/CGHUnitConversion.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ACGHSLMActor::ACGHSLMActor()
{
	PrimaryActorTick.bCanEverTick = false;
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
	const double WidthCm = CGHUnits::MmToCm(GetActiveWidthMm());
	const double HeightCm = CGHUnits::MmToCm(GetActiveHeightMm());
	const bool bValidDimensions = Parameters.ResolutionX > 0 && Parameters.ResolutionY > 0
		&& FMath::IsFinite(WidthCm) && FMath::IsFinite(HeightCm) && WidthCm > 0.0 && HeightCm > 0.0;
	ActiveAreaRoot->SetRelativeScale3D(bValidDimensions ? FVector(0.01, WidthCm, HeightCm) : FVector::ZeroVector);
	ActiveAreaMesh->SetVisibility(bValidDimensions);
	GenerationState = ECGHGenerationState::NotImplemented;
	bHasPhaseData = false;
	Label->SetText(FText::FromString(FString::Printf(
		TEXT("SLM | %d x %d\nPitch: %.3f x %.3f um\nActive: %.3f x %.3f mm\nGenerator: placeholder\nPhase: Not Implemented"),
		Parameters.ResolutionX, Parameters.ResolutionY, Parameters.PixelPitchXUm, Parameters.PixelPitchYUm,
		GetActiveWidthMm(), GetActiveHeightMm())));
}
