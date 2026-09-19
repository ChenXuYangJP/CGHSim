#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CGH/Types/CGHTypes.h"
#include "CGHReconstructionLightActor.generated.h"

class UArrowComponent;
class USceneComponent;
class UTextRenderComponent;

/** Illumination data and direction marker for future optical reconstruction. */
UCLASS(Blueprintable)
class CGHSIM_API ACGHReconstructionLightActor : public AActor
{
	GENERATED_BODY()

public:
	ACGHReconstructionLightActor();

	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Light")
	FCGHLightParameters Parameters;

	/** World-space propagation direction: the Actor's local +X axis. */
	UFUNCTION(BlueprintPure, Category = "CGH|Light")
	FVector GetPropagationDirection() const;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Light")
	void RefreshVisualization();

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UArrowComponent> PropagationArrow;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UTextRenderComponent> Label;
};
