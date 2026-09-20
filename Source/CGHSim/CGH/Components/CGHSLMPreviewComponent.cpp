#include "CGH/Components/CGHSLMPreviewComponent.h"

#if WITH_EDITOR
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Utils/CGHPhasePreview.h"
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
/** A numerical image: linear bytes and no display-gamma conversion preserve phase-to-gray mapping. */
class SCGHPhaseImage final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SCGHPhaseImage) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UCGHSLMPreviewComponent>, PreviewComponent)
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
		const UCGHSLMPreviewComponent* Component = PreviewComponent.Get();
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
	TWeakObjectPtr<UCGHSLMPreviewComponent> PreviewComponent;
	mutable FSlateBrush ImageBrush;
};

class SCGHSLMPhasePreview final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCGHSLMPhasePreview) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UCGHSLMPreviewComponent>, PreviewComponent)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		PreviewComponent = InArgs._PreviewComponent;
		// The native actor-preview frame supplies the actor title and pin/detach controls.
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
					SNew(STextBlock)
					.Text(this, &SCGHSLMPhasePreview::GetHeaderText)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FLinearColor(0.85f, 0.85f, 0.85f))
				]
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SAssignNew(PhaseImage, SCGHPhaseImage).PreviewComponent(PreviewComponent)
					]
					+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(this, &SCGHSLMPhasePreview::GetEmptyText)
						.Visibility(this, &SCGHSLMPhasePreview::GetEmptyVisibility)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
						.ColorAndOpacity(FLinearColor(0.65f, 0.65f, 0.65f))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SNew(STextBlock).Text(NSLOCTEXT("CGH", "SLMPhaseBlack", "0 rad  (black)"))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(STextBlock).Text(NSLOCTEXT("CGH", "SLMPhaseWhite", "2\u03c0 rad  (white)"))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(this, &SCGHSLMPhasePreview::GetStatusText)
					.ToolTipText(this, &SCGHSLMPhasePreview::GetStatusTooltip)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FLinearColor(0.55f, 0.65f, 0.7f))
					.AutoWrapText(true)
				]
			]
		];
	}

	virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override
	{
		SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
		if (UCGHSLMPreviewComponent* Component = PreviewComponent.Get())
		{
			const uint64 PreviousRevision = Component->GetPreviewRevision();
			UTexture2D* PreviousTexture = Component->GetPreviewTexture();
			Component->RefreshPreviewTexture();
			if (PreviousRevision != Component->GetPreviewRevision() || PreviousTexture != Component->GetPreviewTexture())
			{
				PhaseImage->Invalidate(EInvalidateWidgetReason::Paint);
			}
		}
	}

private:
	const ACGHSLMActor* GetSLM() const
	{
		const UCGHSLMPreviewComponent* Component = PreviewComponent.Get();
		return Component ? Cast<ACGHSLMActor>(Component->GetOwner()) : nullptr;
	}

	FText GetHeaderText() const
	{
		if (const ACGHSLMActor* SLM = GetSLM())
		{
			return FText::Format(NSLOCTEXT("CGH", "SLMPhaseHeader", "SLM phase  |  {0} \u00d7 {1} px"),
				FText::AsNumber(SLM->Parameters.ResolutionX, &FNumberFormattingOptions::DefaultNoGrouping()),
				FText::AsNumber(SLM->Parameters.ResolutionY, &FNumberFormattingOptions::DefaultNoGrouping()));
		}
		return NSLOCTEXT("CGH", "SLMPhaseTitle", "SLM phase");
	}

	EVisibility GetEmptyVisibility() const
	{
		const UCGHSLMPreviewComponent* Component = PreviewComponent.Get();
		return Component && Component->GetPreviewTexture() ? EVisibility::Collapsed : EVisibility::Visible;
	}

	FText GetEmptyText() const
	{
		const UCGHSLMPreviewComponent* Component = PreviewComponent.Get();
		return Component && !Component->GetPreviewError().IsEmpty()
			? NSLOCTEXT("CGH", "SLMPreviewUnavailable", "Phase preview unavailable")
			: NSLOCTEXT("CGH", "SLMNoPhasePattern", "No phase pattern");
	}

	FText GetStatusText() const
	{
		if (const UCGHSLMPreviewComponent* Component = PreviewComponent.Get(); Component && !Component->GetPreviewError().IsEmpty())
		{
			return FText::FromString(Component->GetPreviewError());
		}
		if (const ACGHSLMActor* SLM = GetSLM())
		{
			if (!SLM->PhasePatternError.IsEmpty())
			{
				return SLM->HasValidPhasePattern()
					? NSLOCTEXT("CGH", "SLMPreviousValidPattern", "Input rejected; displaying previous valid pattern")
					: FText::FromString(SLM->PhasePatternError);
			}
			if (SLM->HasValidPhasePattern())
			{
				if (!SLM->PhasePatternLabel.IsEmpty())
				{
					return SLM->PhasePatternLabel;
				}
				return SLM->bIsPreviewPhasePattern
					? NSLOCTEXT("CGH", "SLMTestPattern", "Preview phase pattern")
					: NSLOCTEXT("CGH", "SLMPhasePatternLoaded", "Phase pattern");
			}
		}
		return NSLOCTEXT("CGH", "SLMPhaseEmptyHint", "Click Load Stored Phase Pattern in Details to preview the saved sample.");
	}

	FText GetStatusTooltip() const
	{
		if (const ACGHSLMActor* SLM = GetSLM(); SLM && !SLM->PhasePatternError.IsEmpty())
		{
			return FText::FromString(SLM->PhasePatternError);
		}
		return NSLOCTEXT("CGH", "SLMPhaseMapping", "Phase wraps into [0, 2\u03c0) radians and maps linearly to black through white. Texture pixels match the SLM resolution; display scaling uses nearest sampling.");
	}

	TWeakObjectPtr<UCGHSLMPreviewComponent> PreviewComponent;
	TSharedPtr<SCGHPhaseImage> PhaseImage;
};
}
#endif

UCGHSLMPreviewComponent::UCGHSLMPreviewComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bAutoActivate = true;
	bIsEditorOnly = true;
}

#if WITH_EDITOR
bool UCGHSLMPreviewComponent::GetEditorPreviewInfo(float DeltaTime, FMinimalViewInfo& ViewOut)
{
	const ACGHSLMActor* SLM = Cast<ACGHSLMActor>(GetOwner());
	if (!IsValid(SLM) || SLM->IsActorBeingDestroyed())
	{
		return false;
	}

	ViewOut.Location = SLM->GetActorLocation();
	ViewOut.Rotation = SLM->GetActorRotation();
	ViewOut.FOV = 90.0f;
	// Bound only the native preview frame; the image independently fits the exact pixel-grid ratio.
	const float GridAspectRatio = SLM->Parameters.ResolutionX > 0 && SLM->Parameters.ResolutionY > 0
		? static_cast<float>(SLM->Parameters.ResolutionX) / SLM->Parameters.ResolutionY : 1.0f;
	ViewOut.AspectRatio = FMath::Clamp(GridAspectRatio, 0.5f, 2.0f);
	ViewOut.bConstrainAspectRatio = true;
	return true;
}

TSharedPtr<SWidget> UCGHSLMPreviewComponent::GetCustomEditorPreviewWidget()
{
	RefreshPreviewTexture();
	return SNew(SCGHSLMPhasePreview).PreviewComponent(this);
}

void UCGHSLMPreviewComponent::InvalidatePreviewTexture()
{
	PreviewTexture = nullptr;
	CachedRevision = MAX_uint64;
	CachedResolutionX = 0;
	CachedResolutionY = 0;
	bCachedHasPattern = false;
	PreviewError.Reset();
}

void UCGHSLMPreviewComponent::RefreshPreviewTexture()
{
	ACGHSLMActor* SLM = Cast<ACGHSLMActor>(GetOwner());
	if (!IsValid(SLM) || SLM->IsActorBeingDestroyed())
	{
		PreviewTexture = nullptr;
		return;
	}

	SLM->SynchronizePhasePattern();
	const int32 ResolutionX = SLM->Parameters.ResolutionX;
	const int32 ResolutionY = SLM->Parameters.ResolutionY;
	const uint64 Revision = SLM->GetPhasePatternRevision();
	const bool bHasPattern = SLM->HasValidPhasePattern();
	if (CachedRevision == Revision && CachedResolutionX == ResolutionX && CachedResolutionY == ResolutionY
		&& bCachedHasPattern == bHasPattern)
	{
		return;
	}

	CachedRevision = Revision;
	CachedResolutionX = ResolutionX;
	CachedResolutionY = ResolutionY;
	bCachedHasPattern = bHasPattern;
	PreviewError.Reset();
	if (!bHasPattern)
	{
		PreviewTexture = nullptr;
		return;
	}

	const int32 MaximumTextureDimension = GMaxTextureDimensions;
	if (MaximumTextureDimension > 0
		&& (ResolutionX > MaximumTextureDimension || ResolutionY > MaximumTextureDimension))
	{
		PreviewTexture = nullptr;
		PreviewError = TEXT("SLM resolution exceeds the graphics device's maximum texture dimensions.");
		return;
	}

	TArray<FColor> Pixels;
	if (!CGHPhasePreview::BuildGrayscale(SLM->GetPhasePattern(), Pixels, PreviewError))
	{
		PreviewTexture = nullptr;
		return;
	}

	if (!PreviewTexture || PreviewTexture->GetSizeX() != ResolutionX || PreviewTexture->GetSizeY() != ResolutionY)
	{
		PreviewTexture = UTexture2D::CreateTransient(ResolutionX, ResolutionY, PF_B8G8R8A8);
		if (!PreviewTexture)
		{
			PreviewError = TEXT("Could not allocate the SLM phase preview texture.");
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
