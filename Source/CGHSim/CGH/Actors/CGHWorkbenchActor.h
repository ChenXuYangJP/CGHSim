#pragma once

#include "CoreMinimal.h"
#include "CGH/Types/CGHTypes.h"
#include "CGH/Types/CGHSolverTypes.h"
#include "CGH/Types/CGHReconstructionTypes.h"
#include "GameFramework/Actor.h"
#include "CGHWorkbenchActor.generated.h"

class ACGHCameraActor;
class ACGHObserverPlaneActor;
class ACGHReconstructorActor;
class ACGHReconstructionLightActor;
class ACGHSLMActor;
class ACGHSolverActor;
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
	virtual void PostRegisterAllComponents() override;
	virtual void PostUnregisterAllComponents() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Scene")
	TObjectPtr<ACGHSLMActor> SLM;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Scene")
	TObjectPtr<ACGHCameraActor> Camera;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Scene")
	TObjectPtr<ACGHReconstructionLightActor> ReconstructionLight;

	/** Optional sampled destination for optical reconstruction. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Scene")
	TObjectPtr<ACGHObserverPlaneActor> ObserverPlane;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Scene")
	TArray<TObjectPtr<ACGHTargetActor>> Targets;

	/** Optional job coordinator. Existing workbenches remain usable without a solver. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Solver")
	TObjectPtr<ACGHSolverActor> Solver;

	/** Mirrors the linked solver; the solver actor owns state transitions and buffers. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Solver")
	ECGHSolverJobState SolverJobState = ECGHSolverJobState::Idle;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Solver")
	FString SolverStatusMessage = TEXT("No solver assigned.");

	/** Bind an unassigned solver to this workbench and request a fresh point/mesh complex-field phase pattern. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Solver")
	void SolvePhasePattern();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Solver")
	void CancelSolve();

	/** Optional reconstruction coordinator; each coordinator belongs to one workbench. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Reconstruction")
	TObjectPtr<ACGHReconstructorActor> Reconstructor;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Reconstruction")
	ECGHReconstructionJobState ReconstructionJobState = ECGHReconstructionJobState::Idle;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Reconstruction")
	FString ReconstructionStatusMessage = TEXT("No reconstructor assigned.");

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Reconstruction")
	void Reconstruct();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Reconstruction")
	void CancelReconstruction();

	/** Game-thread metadata snapshot; phase samples are copied only when a job launches. */
	bool CaptureReconstructionInput(FCGHReconstructionInput& OutInput, FString& OutError);

	/** Kept separate from the solver's versioned scene/wire description. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "CGH|Reconstruction")
	FCGHObserverPlaneDescription ObserverPlaneDescription;

	/** SLM and observer references are available in this world; not numerical validation. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "CGH|Reconstruction")
	bool bObserverPlaneDescriptionAvailable = false;

	/** Derived SI data in the SLM's unscaled local coordinate system. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "CGH|Scene")
	FCGHSceneDescription SceneDescription;

	/** All references are available in this world; physical validity still requires ValidateScene. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, Category = "CGH|Scene")
	bool bSceneDescriptionComplete = false;

	/** Refresh immediately when consuming data in the same frame as direct parameter writes. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Scene")
	void UpdateSceneDescription();

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
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UTextRenderComponent> Label;

	void UpdateStatusLabel();
	void UpdateSolverStatus();
	void UpdateReconstructionStatus();

private:
	bool IsSceneActorAvailable(const AActor* Actor) const;
	void UpdateReconstructionDescriptions(FCGHSLMDescription& OutSLM, FCGHReconstructionLightDescription& OutLight);
	void RefreshSceneObservers();
	void RemoveSceneObservers();
	void OnSceneTransformUpdated(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport);

	UFUNCTION()
	void OnSceneActorDestroyed(AActor* Actor);

	TArray<TWeakObjectPtr<AActor>> ObservedActors;
	TArray<TWeakObjectPtr<USceneComponent>> ObservedComponents;
	bool bUpdatingSceneDescription = false;

#if WITH_EDITOR
	bool IsSceneObject(const UObject* Object) const;
	void OnSceneObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
	void OnSceneObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event);
#endif
};
