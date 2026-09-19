#include "CGH/Actors/CGHReconstructionLightActor.h"

#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"

ACGHReconstructionLightActor::ACGHReconstructionLightActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	PropagationArrow = CreateDefaultSubobject<UArrowComponent>(TEXT("PropagationArrow"));
	PropagationArrow->SetupAttachment(Root);
	PropagationArrow->SetArrowColor(FLinearColor(1.0f, 0.8f, 0.15f));
	PropagationArrow->ArrowLength = 20.0f;
	PropagationArrow->ArrowSize = 0.4f;
	PropagationArrow->SetHiddenInGame(false);

	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(Root);
	Label->SetRelativeLocation(FVector(0.0, 0.0, 15.0));
	Label->SetWorldSize(3.0f);
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetTextRenderColor(FColor(255, 210, 40));
}

void ACGHReconstructionLightActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	RefreshVisualization();
}

FVector ACGHReconstructionLightActor::GetPropagationDirection() const
{
	return GetActorForwardVector();
}

void ACGHReconstructionLightActor::RefreshVisualization()
{
	const FString SourceName = StaticEnum<ECGHSourceType>()->GetDisplayNameTextByValue(
		static_cast<int64>(Parameters.SourceType)).ToString();
	Label->SetText(FText::FromString(FString::Printf(
		TEXT("Reconstruction Light\n%s | %.3g nm\nAmplitude: %.3g\nOptical propagation not implemented"),
		*SourceName, Parameters.WavelengthNm, Parameters.Amplitude)));
}
