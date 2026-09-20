#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CGH/Types/CGHTypes.h"
#include "CGHCameraActor.generated.h"

class UArrowComponent;
class UCineCameraComponent;
class USceneComponent;
class UTextRenderComponent;

/** Editable optical camera parameters with an ordinary UE geometric preview. */
UCLASS(Blueprintable)
class CGHSIM_API ACGHCameraActor : public AActor
{
	GENERATED_BODY()

public:
	ACGHCameraActor();

	virtual void OnConstruction(const FTransform& Transform) override;

	/** Authoritative optical data; the Cine Camera is only a derived preview. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Camera")
	FCGHCameraParameters Parameters;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Camera")
	void RefreshVisualization();

	/** World-space lens pose used when exporting the optical scene description. */
	UFUNCTION(BlueprintPure, Category = "CGH|Camera")
	FTransform GetOpticalTransform() const;

	/** Lens reference whose transform drives optical scene updates. */
	UFUNCTION(BlueprintPure, Category = "CGH|Camera")
	USceneComponent* GetOpticalReference() const;

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> Root;

	/** Lens reference, independent of any decorative camera body in a Blueprint. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> OpticalReference;

	/** Driven by Parameters. This view does not represent optical reconstruction. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UCineCameraComponent> PreviewCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UArrowComponent> OpticalForward;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UTextRenderComponent> Label;

	void UpdatePreviewCamera();
};
