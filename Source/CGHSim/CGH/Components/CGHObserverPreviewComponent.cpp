#include "CGH/Components/CGHObserverPreviewComponent.h"

#if WITH_EDITOR
#include "CGH/Actors/CGHObserverPlaneActor.h"
#include "CGH/Actors/CGHCameraActor.h"
#include "CineCameraComponent.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"
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
struct FCGHFieldOwner
{
	AActor* Actor = nullptr;
	const FCGHComplexField* Field = nullptr;
	int32 ResolutionX = 0;
	int32 ResolutionY = 0;
	ECGHObserverPreviewMode Mode = ECGHObserverPreviewMode::Amplitude;
	bool bHasField = false;
	bool bCamera = false;
	FString Error;
};

FCGHFieldOwner ReadFieldOwner(const UCGHObserverPreviewComponent* Component, bool bSynchronize = false)
{
	FCGHFieldOwner Out;
	AActor* Owner = Component ? Component->GetOwner() : nullptr;
	if (!IsValid(Owner) || Owner->IsActorBeingDestroyed()) { return Out; }
	if (ACGHObserverPlaneActor* Observer = Cast<ACGHObserverPlaneActor>(Owner))
	{
		if (bSynchronize) { Observer->SynchronizeComplexField(); }
		Out.Actor = Observer; Out.Field = &Observer->GetComplexField();
		Out.ResolutionX = Observer->Parameters.ResolutionX; Out.ResolutionY = Observer->Parameters.ResolutionY;
		Out.Mode = Observer->PreviewMode; Out.bHasField = Observer->HasValidComplexField(); Out.Error = Observer->ComplexFieldError;
	}
	else if (ACGHCameraActor* Camera = Cast<ACGHCameraActor>(Owner))
	{
		if (bSynchronize) { Camera->SynchronizeComplexField(); }
		Out.Actor = Camera; Out.Field = &Camera->GetComplexField(); Out.bCamera = true;
		Out.ResolutionX = Camera->Parameters.OutputResolutionX; Out.ResolutionY = Camera->Parameters.OutputResolutionY;
		Out.Mode = Camera->PreviewMode; Out.bHasField = Camera->HasValidComplexField(); Out.Error = Camera->ComplexFieldError;
	}
	return Out;
}

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
	ECGHObserverPreviewMode GetMode() const
	{
		return ReadFieldOwner(PreviewComponent.Get()).Mode;
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
		const FCGHFieldOwner Owner = ReadFieldOwner(PreviewComponent.Get());
		if (Owner.Actor)
		{
			return FText::Format(NSLOCTEXT("CGH", "ObserverFieldHeader", "{0} | {1} x {2} px"),
				GetModeText(),
				FText::AsNumber(Owner.ResolutionX, &FNumberFormattingOptions::DefaultNoGrouping()),
				FText::AsNumber(Owner.ResolutionY, &FNumberFormattingOptions::DefaultNoGrouping()));
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
		const FCGHFieldOwner Owner = ReadFieldOwner(PreviewComponent.Get());
		if (!Owner.Error.IsEmpty())
		{
			return Owner.bHasField
				? NSLOCTEXT("CGH", "ObserverPreviousField", "Input rejected; displaying previous valid field")
				: FText::FromString(Owner.Error);
		}
		if (Owner.bHasField) { return NSLOCTEXT("CGH", "ObserverFieldReady", "Reconstructed complex field"); }
		return Owner.bCamera
			? NSLOCTEXT("CGH", "CameraFieldHint", "Assign this camera to a Workbench, select Camera reconstruction, and click Reconstruct.")
			: NSLOCTEXT("CGH", "ObserverFieldHint", "Assign this plane to a Workbench and click Reconstruct.");
	}

	TWeakObjectPtr<UCGHObserverPreviewComponent> PreviewComponent;
	TSharedPtr<SCGHFieldImage> FieldImage;
};

/** Small read-only geometric view driven by the existing Cine Camera, created only when requested. */
class FCGHCameraGeometricViewportClient final : public FEditorViewportClient
{
public:
	FCGHCameraGeometricViewportClient(const TSharedRef<SEditorViewport>& Widget, ACGHCameraActor* InCamera)
		: FEditorViewportClient(nullptr, nullptr, Widget), Camera(InCamera)
	{
		ViewportType = LVT_Perspective;
		EngineShowFlags = FEngineShowFlags(ESFIM_Game);
		bSetListenerPosition = false; bDrawAxes = false; bDisableInput = true;
		SetRealtime(true);
	}
	virtual UWorld* GetWorld() const override { return Camera.IsValid() ? Camera->GetWorld() : FEditorViewportClient::GetWorld(); }
	virtual void Tick(float DeltaSeconds) override
	{
		FEditorViewportClient::Tick(DeltaSeconds);
		if (Camera.IsValid())
		{
			Camera->RefreshVisualization();
			if (UCineCameraComponent* Cine = Camera->FindComponentByClass<UCineCameraComponent>())
			{
				Cine->GetCameraView(DeltaSeconds, ControllingActorViewInfo);
				bUseControllingActorViewInfo = true;
				SetViewLocation(ControllingActorViewInfo.Location); SetViewRotation(ControllingActorViewInfo.Rotation);
				ViewFOV = ControllingActorViewInfo.FOV; AspectRatio = ControllingActorViewInfo.AspectRatio;
			}
		}
	}
private:
	TWeakObjectPtr<ACGHCameraActor> Camera;
};

class SCGHCameraGeometricViewport final : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SCGHCameraGeometricViewport) {}
		SLATE_ARGUMENT(TWeakObjectPtr<ACGHCameraActor>, Camera)
	SLATE_END_ARGS()
	void Construct(const FArguments& Args) { Camera = Args._Camera; SEditorViewport::Construct(SEditorViewport::FArguments()); }
protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override
	{
		return MakeShared<FCGHCameraGeometricViewportClient>(SharedThis(this), Camera.Get());
	}
private:
	TWeakObjectPtr<ACGHCameraActor> Camera;
};

/** Native actor insets retain their custom widget, so switch the contents of that same widget in place. */
class SCGHCameraPreviewSwitcher final : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCGHCameraPreviewSwitcher) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UCGHObserverPreviewComponent>, PreviewComponent)
	SLATE_END_ARGS()
	void Construct(const FArguments& Args)
	{
		Component = Args._PreviewComponent;
		FieldWidget = SNew(SCGHObserverFieldPreview).PreviewComponent(Component);
		ChildSlot [ SAssignNew(Content, SBox) [ FieldWidget.ToSharedRef() ] ];
		RefreshContent();
	}
	virtual void Tick(const FGeometry& Geometry, double Time, float DeltaTime) override
	{
		SCompoundWidget::Tick(Geometry, Time, DeltaTime);
		RefreshContent();
		if (Component.IsValid() && CurrentType == ECGHCameraPreviewType::OpticalField) { Component->RefreshPreviewTexture(); }
	}
private:
	void RefreshContent()
	{
		ACGHCameraActor* Camera = Component.IsValid() ? Cast<ACGHCameraActor>(Component->GetOwner()) : nullptr;
		if (!Camera || CurrentType == Camera->PreviewType) { return; }
		CurrentType = Camera->PreviewType;
		if (CurrentType == ECGHCameraPreviewType::Geometric)
		{
			if (GUsingNullRHI)
			{
				Content->SetContent(SNew(STextBlock).Text(NSLOCTEXT("CGH", "CameraGeometryNeedsRendering", "Geometric preview requires rendering")));
			}
			else { Content->SetContent(SNew(SCGHCameraGeometricViewport).Camera(Camera)); }
		}
		else { Content->SetContent(FieldWidget.ToSharedRef()); }
	}
	TWeakObjectPtr<UCGHObserverPreviewComponent> Component;
	TSharedPtr<SWidget> FieldWidget;
	TSharedPtr<SBox> Content;
	ECGHCameraPreviewType CurrentType = ECGHCameraPreviewType::OpticalField;
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
	const FCGHFieldOwner Owner = ReadFieldOwner(this);
	if (!Owner.Actor) { return false; }
	if (ACGHCameraActor* Camera = Cast<ACGHCameraActor>(Owner.Actor))
	{
		if (Camera->PreviewType == ECGHCameraPreviewType::Geometric)
		{
			Camera->RefreshVisualization();
			if (UCineCameraComponent* Cine = Camera->FindComponentByClass<UCineCameraComponent>())
			{
				// The Cine component stays inactive in editor so this component owns the inset.
				Cine->GetCameraView(DeltaTime, ViewOut);
				return true;
			}
		}
		const FTransform Optical = Camera->GetOpticalTransform();
		ViewOut.Location = Optical.GetLocation(); ViewOut.Rotation = Optical.Rotator();
	}
	else
	{
		ViewOut.Location = Owner.Actor->GetActorLocation(); ViewOut.Rotation = Owner.Actor->GetActorRotation();
	}
	ViewOut.FOV = 90.0f;
	const float GridAspectRatio = Owner.ResolutionX > 0 && Owner.ResolutionY > 0
		? static_cast<float>(Owner.ResolutionX) / Owner.ResolutionY : 1.0f;
	ViewOut.AspectRatio = FMath::Clamp(GridAspectRatio, 0.5f, 2.0f);
	ViewOut.bConstrainAspectRatio = true;
	return true;
}

TSharedPtr<SWidget> UCGHObserverPreviewComponent::GetCustomEditorPreviewWidget()
{
	RefreshPreviewTexture();
	if (Cast<ACGHCameraActor>(GetOwner())) { return SNew(SCGHCameraPreviewSwitcher).PreviewComponent(this); }
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
	const FCGHFieldOwner Owner = ReadFieldOwner(this, true);
	if (!Owner.Actor || !Owner.Field)
	{
		PreviewTexture = nullptr;
		return;
	}
	const int32 ResolutionX = Owner.ResolutionX;
	const int32 ResolutionY = Owner.ResolutionY;
	const uint64 Revision = Owner.Field->Revision;
	const bool bHasField = Owner.bHasField;
	if (CachedRevision == Revision && CachedResolutionX == ResolutionX && CachedResolutionY == ResolutionY
		&& bCachedHasField == bHasField && CachedMode == Owner.Mode)
	{
		return;
	}

	CachedRevision = Revision;
	CachedResolutionX = ResolutionX;
	CachedResolutionY = ResolutionY;
	bCachedHasField = bHasField;
	CachedMode = Owner.Mode;
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
		PreviewError = TEXT("Field resolution exceeds the graphics device's maximum texture dimensions.");
		return;
	}

	TArray<FColor> Pixels;
	if (!CGHComplexFieldPreview::BuildGrayscale(*Owner.Field, Owner.Mode, Pixels, PreviewError))
	{
		PreviewTexture = nullptr;
		return;
	}

	if (!PreviewTexture || PreviewTexture->GetSizeX() != ResolutionX || PreviewTexture->GetSizeY() != ResolutionY)
	{
		PreviewTexture = UTexture2D::CreateTransient(ResolutionX, ResolutionY, PF_B8G8R8A8);
		if (!PreviewTexture)
		{
			PreviewError = TEXT("Could not allocate the complex field preview texture.");
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
