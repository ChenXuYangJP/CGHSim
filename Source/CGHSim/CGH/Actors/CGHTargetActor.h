#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CGH/Types/CGHTypes.h"
#include "CGHTargetActor.generated.h"

class USceneComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

UCLASS(Blueprintable)
class CGHSIM_API ACGHTargetActor : public AActor
{
	GENERATED_BODY()

public:
	ACGHTargetActor();
	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Target")
	FCGHTargetParameters Parameters;

	/** Selection aid only; never represents the extent of the mathematical point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Visualization", meta = (ClampMin = "0.01", Units = "cm"))
	double MarkerRadiusCm = 5.0;

	/** Position in reference-local meters, ignoring reference scale. Null uses world axes/origin. */
	UFUNCTION(BlueprintPure, Category = "CGH|Target")
	FVector GetOpticalPositionMeters(const AActor* ReferenceActor) const;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Visualization")
	void RefreshVisualization();

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UStaticMeshComponent> MarkerMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UTextRenderComponent> Label;
};
