#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CGH/Types/CGHComplexField.h"
#include "UObject/SoftObjectPath.h"
#include "CGHObserverPlaneActor.generated.h"

class UArrowComponent;
class UCGHComplexFieldAsset;
class UCGHObserverPreviewComponent;
class USceneComponent;
class UStaticMeshComponent;
class UTextRenderComponent;

/** A sampled observation surface in local YZ, with optical normal +X.
 * Pixel centers: (0, (column - (ResolutionX - 1)/2) * pitchX,
 * ((ResolutionY - 1)/2 - row) * pitchY). Actor scale is visual only.
 * Selecting this actor opens Unreal's native phase/amplitude/intensity preview inset. */
UCLASS(Blueprintable)
class CGHSIM_API ACGHObserverPlaneActor : public AActor
{
	GENERATED_BODY()

public:
	ACGHObserverPlaneActor();
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Observer Plane")
	FCGHObserverPlaneParameters Parameters;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Preview")
	ECGHObserverPreviewMode PreviewMode = ECGHObserverPreviewMode::Amplitude;

	/** Saved field to activate explicitly with Load Stored Complex Field on a matching sampling grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Field")
	TSoftObjectPtr<UCGHComplexFieldAsset> StoredComplexField;

	/** Content Browser destination for uniquely named reusable complex-field assets. Must be under /Game. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Field|Save", meta = (ContentDir))
	FDirectoryPath FieldAssetSaveFolder;

	/** Raw .bin/.json and phase/amplitude/intensity .png destination; relative paths use the project directory. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Field|Save")
	FDirectoryPath FieldRawSaveDirectory;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Field|Save")
	FString FieldSaveStatus = TEXT("No complex field saved.");

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Field|Save")
	TSoftObjectPtr<UCGHComplexFieldAsset> LastSavedFieldAsset;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Field|Save")
	FString LastSavedFieldBinaryFile;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Field|Save")
	FString LastSavedFieldMetadataFile;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Field|Save")
	FString LastSavedFieldPhaseImageFile;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Field|Save")
	FString LastSavedFieldAmplitudeImageFile;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Field|Save")
	FString LastSavedFieldIntensityImageFile;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Field")
	bool bHasComplexData = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, DuplicateTransient, NonTransactional, Category = "CGH|Field")
	FString ComplexFieldError;

	/** Accept only finite, complete fields matching this plane. Invalid input preserves valid data.
	 * Incoming Revision is ignored; identical samples keep the current revision. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Field")
	bool SetComplexField(const FCGHComplexField& Field);
	bool SetComplexField(FCGHComplexField&& Field);

	/** Explicit exact-grid load. Resolution and physical pixel pitches must match; samples are never resampled. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Field")
	void LoadStoredComplexField();

	/** Editor-only explicit save of active samples to an asset, full-precision raw files, and all three previews. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Field|Save")
	bool SaveCurrentComplexField();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Field|Save")
	void SaveComplexField();

	UFUNCTION(BlueprintPure, Category = "CGH|Field")
	bool HasValidComplexField() const;

	/** Explicit Blueprint copy. C++ bulk readers should use the const-reference getter. */
	UFUNCTION(BlueprintPure, Category = "CGH|Field")
	FCGHComplexField GetComplexFieldCopy() const { return ComplexField; }

	const FCGHComplexField& GetComplexField() const { return ComplexField; }
	uint64 GetComplexFieldRevision() const { return ComplexField.Revision; }
	void SynchronizeComplexField();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Field")
	void ClearComplexField();

	UFUNCTION(BlueprintPure, Category = "CGH|Observer Plane")
	double GetActiveWidthMm() const;

	UFUNCTION(BlueprintPure, Category = "CGH|Observer Plane")
	double GetActiveHeightMm() const;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Visualization")
	void RefreshVisualization();

protected:
	virtual void BeginPlay() override;

#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UCGHObserverPreviewComponent> FieldPreview;
#endif

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> ActiveAreaRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UStaticMeshComponent> ActiveAreaMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UArrowComponent> OpticalNormal;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UTextRenderComponent> Label;

private:
	/** Transient bulk storage is hidden from Details and never exposed as a mutable Blueprint reference. */
	UPROPERTY(Transient, DuplicateTransient, NonTransactional)
	FCGHComplexField ComplexField;

	bool PublishComplexField(const FCGHComplexField& Field, FCGHComplexField* OwnedField);
	void UpdateVisualizationComponents();
};
