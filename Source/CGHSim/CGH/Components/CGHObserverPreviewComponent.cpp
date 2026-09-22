#include "CGH/Components/CGHObserverPreviewComponent.h"

#if WITH_EDITOR
#include "CGH/Actors/CGHObserverPlaneActor.h"
#include "CGH/Utils/CGHComplexFieldPreview.h"
#include "Camera/CameraTypes.h"
#include "Engine/Texture2D.h"
#include "Rendering/DrawElements.h"
#include "RHI.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
/** A numerical image: linear bytes and no display-gamma conversion preserve the numerical grayscale mapping. */
class SCGHFieldImage final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SCGHFieldImage) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UCGHObserverPreviewComponent>, PreviewComponent)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		PreviewComponent = InArgs._PreviewComponent;
		ImageBrush.DrawAs = ESlateBrushDrawType::Image;
		ImageBrush.Tiling = ESlateBrushTileType::NoTile;
	}

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override
	{
		return FVector2D(320.0, 240.0);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		const UCGHObserverPreviewComponent* Component = PreviewComponent.Get();
		UTexture2D* Texture = Component ? Component->GetPreviewTexture() : nullptr;
		ImageBrush.SetResourceObject(Texture);
		if (!Texture)
		{
			return LayerId;
		}

		const FVector2D Available = AllottedGeometry.GetLocalSize();
		const FVector2D Grid(Texture->GetSizeX(), Texture->GetSizeY());
		const double Scale = FMath::Min(Available.X / Grid.X, Available.Y / Grid.Y);
		if (Scale <= 0.0)
		{
			return LayerId;
		}

		const FVector2D Size = Grid * Scale;
		const FVector2D Offset = (Available - Size) * 0.5;
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform(Offset)),
			&ImageBrush, ESlateDrawEffect::NoGamma, InWidgetStyle.GetColorAndOpacityTint());
		return LayerId;
	}

private:
	TWeakObjectPtr<UCGHObserverPreviewComponent> PreviewComponent;
	mutable FSlateBrush ImageBrush;
};

class SCGHObserverFieldPreview final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCGHObserverFieldPreview) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UCGHObserverPreviewComponent>, PreviewComponent)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		PreviewComponent = InArgs._PreviewComponent;
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.012f, 0.012f, 0.012f))
			.Padding(FMargin(8.0f, 24.0f, 8.0f, 18.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 5.0f)
				[
					SNew(STextBlock).Text(this, &SCGHObserverFieldPreview::GetHeaderText)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				]
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SAssignNew(FieldImage, SCGHFieldImage).PreviewComponent(PreviewComponent)
					]
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(NSLOCTEXT("CGH", "ObserverNoReconstruction", "No reconstructed field"))
						.Visibility(this, &SCGHObserverFieldPreview::GetEmptyVisibility)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(this, &SCGHObserverFieldPreview::GetLegendText)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(this, &SCGHObserverFieldPreview::GetStatusText)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FLinearColor(0.55f, 0.65f, 0.7f)).AutoWrapText(true)
				]
			]
		];
	}

	virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override
	{
		SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
		if (UCGHObserverPreviewComponent* Component = PreviewComponent.Get())
		{
			const uint64 PreviousRevision = Component->GetPreviewRevision();
			const ECGHObserverPreviewMode PreviousMode = Component->GetPreviewMode();
			UTexture2D* PreviousTexture = Component->GetPreviewTexture();
			Component->RefreshPreviewTexture();
			if (PreviousRevision != Component->GetPreviewRevision() || PreviousMode != Component->GetPreviewMode()
				|| PreviousTexture != Component->GetPreviewTexture())
			{
				FieldImage->Invalidate(EInvalidateWidgetReason::Paint);
			}
		}
	}

private:
	ACGHObserverPlaneActor* GetObserver() const
	{
		const UCGHObserverPreviewComponent* Component = PreviewComponent.Get();
		return Component ? Cast<ACGHObserverPlaneActor>(Component->GetOwner()) : nullptr;
	}

	ECGHObserverPreviewMode GetMode() const
	{
		const ACGHObserverPlaneActor* Observer = GetObserver();
		return Observer ? Observer->PreviewMode : ECGHObserverPreviewMode::Amplitude;
	}

	FText GetModeText() const
	{
		switch (GetMode())
		{
		case ECGHObserverPreviewMode::Phase:
			return NSLOCTEXT("CGH", "ObserverPhase", "Phase");
		case ECGHObserverPreviewMode::Amplitude:
			return NSLOCTEXT("CGH", "ObserverAmplitude", "Amplitude");
		case ECGHObserverPreviewMode::Intensity:
			return NSLOCTEXT("CGH", "ObserverIntensity", "Intensity");
		default:
			return FText::GetEmpty();
		}
	}

	FText GetHeaderText() const
	{
		if (const ACGHObserverPlaneActor* Observer = GetObserver())
		{
			return FText::Format(NSLOCTEXT("CGH", "ObserverFieldHeader", "{0} | {1} x {2} px"),
				GetModeText(),
				FText::AsNumber(Observer->Parameters.ResolutionX, &FNumberFormattingOptions::DefaultNoGrouping()),
				FText::AsNumber(Observer->Parameters.ResolutionY, &FNumberFormattingOptions::DefaultNoGrouping()));
		}
		return NSLOCTEXT("CGH", "ObserverFieldTitle", "Reconstructed field");
	}

	FText GetLegendText() const
	{
		switch (GetMode())
		{
		case ECGHObserverPreviewMode::Phase:
			return NSLOCTEXT("CGH", "ObserverPhaseLegend", "Phase: 0 (black) to 2\u03c0 (white), wrapped");
		case ECGHObserverPreviewMode::Amplitude:
			return NSLOCTEXT("CGH", "ObserverAmplitudeLegend", "Amplitude: 0 (black) to field maximum (white)");
		case ECGHObserverPreviewMode::Intensity:
			return NSLOCTEXT("CGH", "ObserverIntensityLegend", "Intensity: 0 (black) to field maximum (white)");
		default:
			return FText::GetEmpty();
		}
	}

	EVisibility GetEmptyVisibility() const
	{
		const UCGHObserverPreviewComponent* Component = PreviewComponent.Get();
		return Component && Component->GetPreviewTexture() ? EVisibility::Collapsed : EVisibility::Visible;
	}

	FText GetStatusText() const
	{
		if (const UCGHObserverPreviewComponent* Component = PreviewComponent.Get(); Component && !Component->GetPreviewError().IsEmpty())
		{
			return FText::FromString(Component->GetPreviewError());
		}
		if (const ACGHObserverPlaneActor* Observer = GetObserver())
		{
			if (!Observer->ComplexFieldError.IsEmpty())
			{
				return Observer->HasValidComplexField()
					? NSLOCTEXT("CGH", "ObserverPreviousField", "Input rejected; displaying previous valid field")
					: FText::FromString(Observer->ComplexFieldError);
			}
			if (Observer->HasValidComplexField())
			{
				return NSLOCTEXT("CGH", "ObserverFieldReady", "Reconstructed complex field");
			}
		}
		return NSLOCTEXT("CGH", "ObserverFieldHint", "Assign this plane to a Workbench and click Reconstruct.");
	}

	TWeakObjectPtr<UCGHObserverPreviewComponent> PreviewComponent;
	TSharedPtr<SCGHFieldImage> FieldImage;
};
}
#endif

UCGHObserverPreviewComponent::UCGHObserverPreviewComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bAutoActivate = true;
	bIsEditorOnly = true;
}

#if WITH_EDITOR
bool UCGHObserverPreviewComponent::GetEditorPreviewInfo(float DeltaTime, FMinimalViewInfo& ViewOut)
{
	const ACGHObserverPlaneActor* Observer = Cast<ACGHObserverPlaneActor>(GetOwner());
	if (!IsValid(Observer) || Observer->IsActorBeingDestroyed())
	{
		return false;
	}

	// Metadata for the native preview frame; the custom Slate widget draws the canonical
	// image (columns toward +Y/right, rows toward -Z/down), not this world camera view.
	ViewOut.Location = Observer->GetActorLocation();
	ViewOut.Rotation = Observer->GetActorRotation();
	ViewOut.FOV = 90.0f;
	// Bound only the native preview frame; the image independently fits the exact pixel-grid ratio.
	const float GridAspectRatio = Observer->Parameters.ResolutionX > 0 && Observer->Parameters.ResolutionY > 0
		? static_cast<float>(Observer->Parameters.ResolutionX) / Observer->Parameters.ResolutionY : 1.0f;
	ViewOut.AspectRatio = FMath::Clamp(GridAspectRatio, 0.5f, 2.0f);
	ViewOut.bConstrainAspectRatio = true;
	return true;
}

TSharedPtr<SWidget> UCGHObserverPreviewComponent::GetCustomEditorPreviewWidget()
{
	RefreshPreviewTexture();
	return SNew(SCGHObserverFieldPreview).PreviewComponent(this);
}

void UCGHObserverPreviewComponent::InvalidatePreviewTexture()
{
	PreviewTexture = nullptr;
	CachedRevision = MAX_uint64;
	CachedResolutionX = 0;
	CachedResolutionY = 0;
	bCachedHasField = false;
	PreviewError.Reset();
}

void UCGHObserverPreviewComponent::RefreshPreviewTexture()
{
	ACGHObserverPlaneActor* Observer = Cast<ACGHObserverPlaneActor>(GetOwner());
	if (!IsValid(Observer) || Observer->IsActorBeingDestroyed())
	{
		PreviewTexture = nullptr;
		return;
	}

	Observer->SynchronizeComplexField();
	const int32 ResolutionX = Observer->Parameters.ResolutionX;
	const int32 ResolutionY = Observer->Parameters.ResolutionY;
	const uint64 Revision = Observer->GetComplexFieldRevision();
	const bool bHasField = Observer->HasValidComplexField();
	if (CachedRevision == Revision && CachedResolutionX == ResolutionX && CachedResolutionY == ResolutionY
		&& bCachedHasField == bHasField && CachedMode == Observer->PreviewMode)
	{
		return;
	}

	CachedRevision = Revision;
	CachedResolutionX = ResolutionX;
	CachedResolutionY = ResolutionY;
	bCachedHasField = bHasField;
	CachedMode = Observer->PreviewMode;
	PreviewError.Reset();
	if (!bHasField)
	{
		PreviewTexture = nullptr;
		return;
	}

	const int32 MaximumTextureDimension = GMaxTextureDimensions;
	if (MaximumTextureDimension > 0
		&& (ResolutionX > MaximumTextureDimension || ResolutionY > MaximumTextureDimension))
	{
		PreviewTexture = nullptr;
		PreviewError = TEXT("Observer resolution exceeds the graphics device's maximum texture dimensions.");
		return;
	}

	TArray<FColor> Pixels;
	if (!CGHComplexFieldPreview::BuildGrayscale(Observer->GetComplexField(), Observer->PreviewMode, Pixels, PreviewError))
	{
		PreviewTexture = nullptr;
		return;
	}

	if (!PreviewTexture || PreviewTexture->GetSizeX() != ResolutionX || PreviewTexture->GetSizeY() != ResolutionY)
	{
		PreviewTexture = UTexture2D::CreateTransient(ResolutionX, ResolutionY, PF_B8G8R8A8);
		if (!PreviewTexture)
		{
			PreviewError = TEXT("Could not allocate the observer field preview texture.");
			return;
		}
		PreviewTexture->SRGB = false;
		PreviewTexture->Filter = TF_Nearest;
		PreviewTexture->AddressX = TA_Clamp;
		PreviewTexture->AddressY = TA_Clamp;
		PreviewTexture->NeverStream = true;
		PreviewTexture->LODGroup = TEXTUREGROUP_UI;
		PreviewTexture->MipGenSettings = TMGS_NoMipmaps;
	}

	FTexture2DMipMap& Mip = PreviewTexture->GetPlatformData()->Mips[0];
	const int64 ByteCount = static_cast<int64>(Pixels.Num()) * sizeof(FColor);
	Mip.BulkData.Lock(LOCK_READ_WRITE);
	void* Destination = Mip.BulkData.Realloc(ByteCount);
	FMemory::Memcpy(Destination, Pixels.GetData(), static_cast<SIZE_T>(ByteCount));
	Mip.BulkData.Unlock();
	PreviewTexture->UpdateResource();
}
#endif
