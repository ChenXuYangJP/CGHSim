#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CGH/Types/CGHTypes.h"
#include "CGH/Types/CGHSLMPhasePattern.h"
#include "CGHSLMActor.generated.h"

class UArrowComponent;
class UCGHSLMPreviewComponent;
class UCGHPhasePatternAsset;
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
	virtual void PostRegisterAllComponents() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|SLM")
	FCGHSLMParameters Parameters;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|SLM")
	ECGHGenerationState GenerationState = ECGHGenerationState::NotImplemented;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|SLM")
	bool bHasPhaseData = false;

	/** Saved phase grid to activate with Load Stored Phase Pattern. The bundled sample is selected by default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Phase")
	TSoftObjectPtr<UCGHPhasePatternAsset> StoredPhasePattern;

	/** Identifies the currently loaded sample; empty for ordinary phase publication. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Phase")
	FText PhasePatternLabel;

	/** An explicitly activated preview pattern supplied these samples, rather than a solver. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Phase")
	bool bIsPreviewPhasePattern = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Phase")
	FString PhasePatternError;

	/** Publish a complete finite row-major pattern matching Parameters.ResolutionX/Y.
	 * Invalid input leaves the current valid pattern intact. Incoming Revision is ignored. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Phase")
	bool SetPhasePattern(const FCGHSLMPhasePattern& Pattern);

	/** Load the saved phase grid, using nearest-neighbor sampling to match the current SLM resolution. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Phase")
	void LoadStoredPhasePattern();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Phase")
	void ClearPhasePattern();

	/** Explicit display test: horizontal 0..2*pi ramp. This does not run a CGH solver. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Phase")
	void GeneratePreviewPhaseRamp();

	UFUNCTION(BlueprintPure, Category = "CGH|Phase")
	bool HasValidPhasePattern() const;

	/** Explicit Blueprint readback; copies the full array. Use the C++ const getter for bulk consumers. */
	UFUNCTION(BlueprintPure, Category = "CGH|Phase")
	FCGHSLMPhasePattern GetPhasePatternCopy() const { return PhasePattern; }

	/** Read-only storage access without copying a potentially large phase array. */
	const FCGHSLMPhasePattern& GetPhasePattern() const { return PhasePattern; }
	uint64 GetPhasePatternRevision() const { return PhasePattern.Revision; }

	/** Cheap dimension check; clears data whose resolution no longer matches the SLM. */
	void SynchronizePhasePattern();

	UFUNCTION(BlueprintPure, Category = "CGH|SLM")
	double GetActiveWidthMm() const;

	UFUNCTION(BlueprintPure, Category = "CGH|SLM")
	double GetActiveHeightMm() const;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Visualization")
	void RefreshVisualization();

protected:
	virtual void BeginPlay() override;

#if WITH_EDITORONLY_DATA
	/** Supplies the native selected-actor picture-in-picture panel; stripped when cooking. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UCGHSLMPreviewComponent> PhasePreview;
#endif

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

private:
	/** Full-resolution data is deliberately hidden from Details to keep selection inexpensive. */
	UPROPERTY(Transient, NonTransactional)
	FCGHSLMPhasePattern PhasePattern;

	void UpdateVisualizationComponents();
};
