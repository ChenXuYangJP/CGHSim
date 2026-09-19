#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CGHWorkbenchActor.generated.h"

class ACGHCameraActor;
class ACGHReconstructionLightActor;
class ACGHSLMActor;
class ACGHTargetActor;
class USceneComponent;
class UTextRenderComponent;

/** Explicitly associates the optical actors and checks their configuration. */
UCLASS(Blueprintable)
class CGHSIM_API ACGHWorkbenchActor : public AActor
{
	GENERATED_BODY()

public:
	ACGHWorkbenchActor();

	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Scene")
	TObjectPtr<ACGHSLMActor> SLM;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Scene")
	TObjectPtr<ACGHCameraActor> Camera;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Scene")
	TObjectPtr<ACGHReconstructionLightActor> ReconstructionLight;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Scene")
	TArray<TObjectPtr<ACGHTargetActor>> Targets;

	/** Result of the most recent explicit validation, not a solver result. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "CGH|Validation")
	bool bSceneValid = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "CGH|Validation")
	TArray<FString> ValidationMessages;

	UFUNCTION(BlueprintCallable, Category = "CGH|Scene")
	bool ValidateScene();

	/** Editor buttons require a void function with no parameters. */
	UFUNCTION(CallInEditor, Category = "CGH|Scene", meta = (DisplayName = "Validate Scene"))
	void ValidateSceneInEditor();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Scene")
	void RefreshVisualization();

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UTextRenderComponent> Label;

	void UpdateStatusLabel();
};
