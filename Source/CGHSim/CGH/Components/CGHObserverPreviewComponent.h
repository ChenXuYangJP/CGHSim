#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CGH/Types/CGHComplexField.h"
#include "CGHObserverPreviewComponent.generated.h"

class UTexture2D;

/** Native selected-actor inset showing the observer's complex field. Stripped when cooking. */
UCLASS(ClassGroup = CGH)
class CGHSIM_API UCGHObserverPreviewComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCGHObserverPreviewComponent();

#if WITH_EDITOR
	virtual bool GetEditorPreviewInfo(float DeltaTime, FMinimalViewInfo& ViewOut) override;
	virtual TSharedPtr<SWidget> GetCustomEditorPreviewWidget() override;
	void RefreshPreviewTexture();
	void InvalidatePreviewTexture();
	UTexture2D* GetPreviewTexture() const { return PreviewTexture; }
	uint64 GetPreviewRevision() const { return CachedRevision; }
	ECGHObserverPreviewMode GetPreviewMode() const { return CachedMode; }
	const FString& GetPreviewError() const { return PreviewError; }
#endif

private:
#if WITH_EDITORONLY_DATA
	UPROPERTY(Transient, DuplicateTransient, NonTransactional)
	TObjectPtr<UTexture2D> PreviewTexture;
#endif

#if WITH_EDITOR
	uint64 CachedRevision = MAX_uint64;
	int32 CachedResolutionX = 0;
	int32 CachedResolutionY = 0;
	bool bCachedHasField = false;
	ECGHObserverPreviewMode CachedMode = ECGHObserverPreviewMode::Amplitude;
	FString PreviewError;
#endif
};
