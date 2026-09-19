#include "CGH/Actors/CGHTargetActor.h"

#include "CGH/Utils/CGHUnitConversion.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ACGHTargetActor::ACGHTargetActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	MarkerMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MarkerMesh"));
	MarkerMesh->SetupAttachment(Root);
	MarkerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MarkerMesh->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded())
	{
		MarkerMesh->SetStaticMesh(Sphere.Object);
	}
	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(Root);
	Label->SetWorldSize(2.0f);
	Label->SetHorizontalAlignment(EHTA_Center);
}

void ACGHTargetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisualization();
}

FVector ACGHTargetActor::GetOpticalPositionMeters(const AActor* ReferenceActor) const
{
	const FVector PositionCm = IsValid(ReferenceActor)
		? ReferenceActor->GetActorTransform().InverseTransformPositionNoScale(GetActorLocation())
		: GetActorLocation();
	return PositionCm * CGHUnits::CmToM(1.0);
}

void ACGHTargetActor::RefreshVisualization()
{
	const double Radius = FMath::IsFinite(MarkerRadiusCm) ? FMath::Max(0.01, MarkerRadiusCm) : 5.0;
	// Engine sphere has a 50 cm radius. Marker scaling is separate from optical parameters.
	MarkerMesh->SetRelativeScale3D(FVector(Radius / 50.0));
	Label->SetRelativeLocation(FVector(0.0, 0.0, Radius + 3.0));
	Label->SetText(FText::FromString(FString::Printf(TEXT("%s\nAmplitude: %.3f | Phase: %.3f rad"),
		Parameters.TargetType == ECGHTargetType::Point ? TEXT("Target Point (marker only)") : TEXT("Mesh Target (not implemented)"),
		Parameters.Amplitude, Parameters.InitialPhaseRad)));
}
