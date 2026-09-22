#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CGH/Types/CGHTypes.h"
#include "CGH/Types/CGHComplexField.h"
#include "UObject/SoftObjectPath.h"
#include "CGHCameraActor.generated.h"

class UArrowComponent;
class UCineCameraComponent;
class UCGHComplexFieldAsset;
class UCGHObserverPreviewComponent;
class USceneComponent;
class UTextRenderComponent;

UENUM(BlueprintType)
enum class ECGHCameraPreviewType : uint8
{
	OpticalField UMETA(DisplayName = "Reconstructed Optical Field"),
	Geometric UMETA(DisplayName = "Geometric Camera View")
};

/** Thin-lens camera. Optical-reference +X faces the scene; the sampled sensor lies behind the lens along -X.
 * Sensor columns follow lens-local +Y and rows follow -Z. Actor scale does not change optical dimensions. */
UCLASS(Blueprintable)
class CGHSIM_API ACGHCameraActor : public AActor
{
	GENERATED_BODY()

public:
	ACGHCameraActor();
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void PostUnregisterAllComponents() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	/** Authoritative optical data. Editing the PreviewCamera lens controls in an editor instance updates these values too. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Camera")
	FCGHCameraParameters Parameters;

	/** Optical Field displays the reconstructed sensor samples. Geometric View displays ordinary scene rendering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Preview")
	ECGHCameraPreviewType PreviewType = ECGHCameraPreviewType::OpticalField;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Preview", meta = (EditCondition = "PreviewType == ECGHCameraPreviewType::OpticalField"))
	ECGHObserverPreviewMode PreviewMode = ECGHObserverPreviewMode::Intensity;

	/** Finite pupil integration requires a convergence check, especially at optical wavelengths. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "CGH|Camera|Pupil Sampling")
	FString PupilSamplingGuidance = TEXT("Coarse pupil grids can alias optical phase. Increase both pupil resolutions until the reconstructed image converges; the default 64 x 64 is a starting point.");

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

	/** Accept only finite, complete fields matching this sensor. Invalid input preserves valid data.
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

	/** Effective sampling geometry; inactive Sensor Size/Pixel Pitch settings are preserved unchanged. */
	UFUNCTION(BlueprintPure, Category = "CGH|Camera")
	double GetSensorPixelPitchXM() const;

	UFUNCTION(BlueprintPure, Category = "CGH|Camera")
	double GetSensorPixelPitchYM() const;

	UFUNCTION(BlueprintPure, Category = "CGH|Camera")
	double GetSensorWidthM() const;

	UFUNCTION(BlueprintPure, Category = "CGH|Camera")
	double GetSensorHeightM() const;

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Camera")
	void RefreshVisualization();

	/** World-space lens pose used when exporting the optical scene description. */
	UFUNCTION(BlueprintPure, Category = "CGH|Camera")
	FTransform GetOpticalTransform() const;

	/** Lens reference whose transform drives optical scene updates. */
	UFUNCTION(BlueprintPure, Category = "CGH|Camera")
	USceneComponent* GetOpticalReference() const;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UCGHObserverPreviewComponent> FieldPreview;
#endif

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> Root;

	/** Lens reference, independent of any decorative camera body in a Blueprint. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> OpticalReference;

	/** Geometric preview. Instance Details edits to focal length, aperture and manual focus also update optical Parameters. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UCineCameraComponent> PreviewCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UArrowComponent> OpticalForward;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UTextRenderComponent> Label;

	void UpdatePreviewCamera();

private:
#if WITH_EDITOR
	void OnPreviewCameraPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
#endif

	UPROPERTY(Transient, DuplicateTransient, NonTransactional)
	FCGHComplexField ComplexField;

	bool PublishComplexField(const FCGHComplexField& Field, FCGHComplexField* OwnedField);
	void UpdateVisualizationComponents();
};
