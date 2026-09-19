#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CGH/Types/CGHTypes.h"
#include "CGHSLMActor.generated.h"

class UArrowComponent;
class USceneComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/** Local +X is the optical normal; the physical active area lies in local YZ. */
UCLASS(Blueprintable)
class CGHSIM_API ACGHSLMActor : public AActor
{
	GENERATED_BODY()

public:
	ACGHSLMActor();
	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|SLM")
	FCGHSLMParameters Parameters;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|SLM")
	ECGHGenerationState GenerationState = ECGHGenerationState::NotImplemented;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|SLM")
	bool bHasPhaseData = false;

	UFUNCTION(BlueprintPure, Category = "CGH|SLM")
	double GetActiveWidthMm() const;

	UFUNCTION(BlueprintPure, Category = "CGH|SLM")
	double GetActiveHeightMm() const;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Visualization")
	void RefreshVisualization();

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> Root;

	/** Physical dimensions in cm; the normalized mesh beneath this node spans one cm. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> ActiveAreaRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UStaticMeshComponent> ActiveAreaMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UArrowComponent> OpticalNormal;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UTextRenderComponent> Label;
};
