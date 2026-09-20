#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CGHSLMPreviewComponent.generated.h"

class UTexture2D;

/** Supplies the SLM's phase grid to Unreal's native selected-actor preview window. */
UCLASS(ClassGroup = CGH)
class CGHSIM_API UCGHSLMPreviewComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCGHSLMPreviewComponent();

#if WITH_EDITOR
	virtual bool GetEditorPreviewInfo(float DeltaTime, FMinimalViewInfo& ViewOut) override;
	virtual TSharedPtr<SWidget> GetCustomEditorPreviewWidget() override;

	/** Updates only when the phase revision or pixel dimensions change; empty data allocates no texture. */
	void RefreshPreviewTexture();
	/** Release stale preview memory immediately, including while the actor is deselected. */
	void InvalidatePreviewTexture();
	UTexture2D* GetPreviewTexture() const { return PreviewTexture; }
	uint64 GetPreviewRevision() const { return CachedRevision; }
	const FString& GetPreviewError() const { return PreviewError; }
#endif

private:
#if WITH_EDITORONLY_DATA
	/** Keeps the texture alive independently of Slate widgets and actor selection changes. */
	UPROPERTY(Transient, NonTransactional)
	TObjectPtr<UTexture2D> PreviewTexture;
#endif

#if WITH_EDITOR
	uint64 CachedRevision = MAX_uint64;
	int32 CachedResolutionX = 0;
	int32 CachedResolutionY = 0;
	bool bCachedHasPattern = false;
	FString PreviewError;
#endif
};
