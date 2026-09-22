#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "CGH/Actors/CGHCameraActor.h"
#include "CGH/Components/CGHObserverPreviewComponent.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Layout/Children.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "Tests/AutomationCommon.h"
#include "Widgets/SWindow.h"

namespace
{
	TSharedPtr<SWidget> FindCameraFieldImage(const TSharedRef<SWidget>& Widget, FName Type = FName(TEXT("SCGHFieldImage")))
	{
		if (Widget->GetType() == Type)
		{
			return Widget;
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<SWidget> Found = FindCameraFieldImage(Children->GetChildAt(Index), Type))
			{
				return Found;
			}
		}
		return nullptr;
	}

	struct FCGHCameraPreviewRenderFixture
	{
		FTestWorldWrapper Scene;
		TSharedPtr<SWidget> Preview;
		TSharedPtr<SWindow> Window;
		ACGHCameraActor* Camera = nullptr;

		~FCGHCameraPreviewRenderFixture()
		{
			if (Window.IsValid() && FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().DestroyWindowImmediately(Window.ToSharedRef());
			}
			Window.Reset();
			Preview.Reset();
		}
	};

	class FCGHCaptureCameraPreview final : public IAutomationLatentCommand
	{
	public:
		FCGHCaptureCameraPreview(FAutomationTestBase& InTest, TSharedRef<FCGHCameraPreviewRenderFixture> InFixture)
			: Test(InTest), Fixture(MoveTemp(InFixture))
		{
		}

		virtual bool Update() override
		{
			// Allow the real Slate layout and texture upload to finish; never block the editor tick.
			if (++Frames < 8)
			{
				return false;
			}
			FlushRenderingCommands();

			TArray<FColor> Pixels;
			FIntVector Size = FIntVector::ZeroValue;
			if (!Test.TestTrue(TEXT("Slate captures the actual observer preview widget"),
				FSlateApplication::Get().TakeScreenshot(Fixture->Preview.ToSharedRef(), Pixels, Size)))
			{
				return true;
			}

			const FString Directory = FPaths::ProjectSavedDir() / TEXT("Automation/CGHCameraPreview");
			IFileManager::Get().MakeDirectory(*Directory, true);
			const TCHAR* CaptureNames[] = {TEXT("CameraPhasePreview.png"), TEXT("CameraAmplitudePreview.png"), TEXT("CameraIntensityPreview.png"),
				TEXT("CameraGeometricPreview.png"), TEXT("CameraIntensityRestoredPreview.png")};
			const FString Filename = Directory / CaptureNames[CaptureIndex];
			TArray64<uint8> Compressed;
			FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Compressed);
			Test.TestTrue(TEXT("Save the rendered Camera preview screenshot"), FFileHelper::SaveArrayToFile(Compressed, *Filename));
			Test.AddInfo(FString::Printf(TEXT("Camera field preview screenshot: %s (%d x %d)"), *FPaths::ConvertRelativePathToFull(Filename), Size.X, Size.Y));

			if (CaptureIndex == 3)
			{
				Test.TestTrue(TEXT("Already-open inset hosts the optional geometric camera viewport"),
					FindCameraFieldImage(Fixture->Preview.ToSharedRef(), FName(TEXT("SCGHCameraGeometricViewport"))).IsValid());
				Fixture->Camera->PreviewType = ECGHCameraPreviewType::OpticalField;
				++CaptureIndex; Frames = 0;
				return false;
			}

			TSharedPtr<SWidget> FieldImage = FindCameraFieldImage(Fixture->Preview.ToSharedRef());
			if (!Test.TestTrue(TEXT("Preview includes its complex-field image widget"), FieldImage.IsValid()))
			{
				return true;
			}

			// Read centers of the actual image cells, accounting for letterboxing and Slate DPI.
			const FGeometry& ImageGeometry = FieldImage->GetCachedGeometry();
			const FVector2D Available = ImageGeometry.GetLocalSize();
			const double ImageScale = FMath::Min(Available.X / 4.0, Available.Y / 2.0);
			const FVector2D ImageSize(4.0 * ImageScale, 2.0 * ImageScale);
			const FVector2D Offset = (Available - ImageSize) * 0.5;
			const FVector2D ScreenshotOrigin = Fixture->Preview->GetCachedGeometry().GetAbsolutePosition();
			const uint8 PhaseGray[] = {0, 64, 128, 255, 255, 128, 64, 0};
			const uint8 AmplitudeGray[] = {64, 128, 191, 255, 255, 191, 128, 64};
			const uint8 IntensityGray[] = {16, 64, 143, 255, 255, 143, 64, 16};
			const uint8* ExpectedGrays[] = {PhaseGray, AmplitudeGray, IntensityGray, nullptr, IntensityGray};
			const uint8* ExpectedGray = ExpectedGrays[CaptureIndex];
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(PhaseGray); ++Index)
			{
				const FVector2D LocalCenter = Offset + FVector2D((Index % 4 + 0.5) * ImageScale, (Index / 4 + 0.5) * ImageScale);
				const FVector2D ScreenshotPosition = ImageGeometry.LocalToAbsolute(LocalCenter) - ScreenshotOrigin;
				const int32 X = FMath::RoundToInt(ScreenshotPosition.X);
				const int32 Y = FMath::RoundToInt(ScreenshotPosition.Y);
				if (!Test.TestTrue(FString::Printf(TEXT("Field cell %d lies inside screenshot"), Index), X >= 0 && X < Size.X && Y >= 0 && Y < Size.Y))
				{
					continue;
				}
				const FColor Pixel = Pixels[Y * Size.X + X];
				Test.TestTrue(FString::Printf(TEXT("Field cell %d remains grayscale"), Index),
					FMath::Abs(int32(Pixel.R) - Pixel.G) <= 1 && FMath::Abs(int32(Pixel.R) - Pixel.B) <= 1);
				Test.TestTrue(FString::Printf(TEXT("Field cell %d preserves numerical gray %d (rendered %d)"), Index, ExpectedGray[Index], Pixel.R),
					FMath::Abs(int32(Pixel.R) - ExpectedGray[Index]) <= 3);
			}
			if (++CaptureIndex < UE_ARRAY_COUNT(ExpectedGrays))
			{
				Fixture->Camera->PreviewType = CaptureIndex == 3
					? ECGHCameraPreviewType::Geometric : ECGHCameraPreviewType::OpticalField;
				Fixture->Camera->PreviewMode = CaptureIndex == 1
					? ECGHObserverPreviewMode::Amplitude : ECGHObserverPreviewMode::Intensity;
				Frames = 0;
				return false;
			}
			return true;
		}

	private:
		FAutomationTestBase& Test;
		TSharedRef<FCGHCameraPreviewRenderFixture> Fixture;
		int32 Frames = 0;
		int32 CaptureIndex = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraPreviewRenderTest,
	"CGH.CameraField.RenderedPhaseAmplitudeAndIntensityPreserveGrayscale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter | EAutomationTestFlags::NonNullRHI)

bool FCGHCameraPreviewRenderTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI || !FApp::CanEverRender() || !FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Skipping rendered preview verification: a real RHI and initialized Slate are required. Run UnrealEditor with -RenderOffscreen without -NullRHI."));
		return true;
	}

	TSharedRef<FCGHCameraPreviewRenderFixture> Fixture = MakeShared<FCGHCameraPreviewRenderFixture>();
	if (!Fixture->Scene.CreateTestWorld(EWorldType::Editor))
	{
		Fixture->Scene.ForwardErrorMessages(this);
		return false;
	}
	ACGHCameraActor* Camera = Fixture->Scene.GetTestWorld()->SpawnActor<ACGHCameraActor>();
	if (!TestNotNull(TEXT("Isolated render fixture has an observer"), Camera))
	{
		return false;
	}
	Fixture->Camera = Camera;
	Camera->PreviewMode = ECGHObserverPreviewMode::Phase;
	Camera->Parameters.OutputResolutionX = 4;
	Camera->Parameters.OutputResolutionY = 2;
	FCGHComplexField Pattern;
	Pattern.ResolutionX = 4;
	Pattern.ResolutionY = 2;
	const double NearWhite = 2.0 * UE_DOUBLE_PI - 0.001;
	const double Phases[] = {0.0, UE_DOUBLE_PI / 2.0, UE_DOUBLE_PI, NearWhite,
		NearWhite, UE_DOUBLE_PI, UE_DOUBLE_PI / 2.0, 0.0};
	const double Amplitudes[] = {1.0, 2.0, 3.0, 4.0, 4.0, 3.0, 2.0, 1.0};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Phases); ++Index)
	{
		Pattern.Samples.Emplace(Amplitudes[Index] * FMath::Cos(Phases[Index]), Amplitudes[Index] * FMath::Sin(Phases[Index]));
	}
	if (!TestTrue(TEXT("Fixture accepts the phase pattern"), Camera->SetComplexField(Pattern)))
	{
		return false;
	}
	UCGHObserverPreviewComponent* Component = Camera->FindComponentByClass<UCGHObserverPreviewComponent>();
	if (!TestNotNull(TEXT("Camera has the native preview component"), Component))
	{
		return false;
	}
	Fixture->Preview = Component->GetCustomEditorPreviewWidget();
	if (!TestTrue(TEXT("Camera supplies the custom preview widget"), Fixture->Preview.IsValid())
		|| !TestNotNull(TEXT("Phase preview allocates its texture"), Component->GetPreviewTexture()))
	{
		return false;
	}
	Fixture->Window = SNew(SWindow)
		.Title(NSLOCTEXT("CGH", "CameraRenderTestTitle", "Camera field preview render test"))
		.ClientSize(FVector2D(480.0, 360.0))
		.SizingRule(ESizingRule::FixedSize)
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.FocusWhenFirstShown(false)
		[
			Fixture->Preview.ToSharedRef()
		];
	FSlateApplication::Get().AddWindow(Fixture->Window.ToSharedRef());
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FCGHCaptureCameraPreview>(*this, Fixture));
	return true;
}

#endif
