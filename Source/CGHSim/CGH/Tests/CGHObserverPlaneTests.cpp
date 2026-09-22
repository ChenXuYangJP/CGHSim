#include "CGH/Actors/CGHObserverPlaneActor.h"
#include "CGH/Utils/CGHComplexFieldPreview.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UnrealType.h"
#include <limits>

#if WITH_EDITOR
#include "CGH/Components/CGHObserverPreviewComponent.h"
#include "Camera/CameraTypes.h"
#include "Engine/Texture2D.h"
#include "Layout/Children.h"
#include "LevelEditorViewport.h"
#include "Widgets/Text/STextBlock.h"
#endif

namespace
{
	FCGHComplexField MakeObserverTestField()
	{
		FCGHComplexField Field;
		Field.ResolutionX = 2;
		Field.ResolutionY = 2;
		Field.Samples = {{1.0, 0.0}, {0.0, 2.0}, {-3.0, 0.0}, {0.0, -4.0}};
		return Field;
	}

#if WITH_EDITOR
	void GatherObserverPreviewText(const TSharedRef<SWidget>& Widget, TArray<FString>& Text)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			Text.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		}
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			GatherObserverPreviewText(Children->GetChildAt(Index), Text);
		}
	}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHComplexFieldPreviewTest,
	"CGH.ObserverPlane.ComplexFieldValidationAndGrayscale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHComplexFieldPreviewTest::RunTest(const FString& Parameters)
{
	FCGHComplexField Field = MakeObserverTestField();
	FString Error;
	TArray<FColor> Pixels;
	TestTrue(TEXT("Finite field validates"), Field.IsValid(&Error));
	TestTrue(TEXT("Phase conversion succeeds"), CGHComplexFieldPreview::BuildGrayscale(Field, ECGHObserverPreviewMode::Phase, Pixels, Error));
	const uint8 PhaseGray[] = {0, 64, 128, 191};
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		TestEqual(TEXT("Phase follows complex argument and preserves row-major orientation"),
			Pixels[Index], FColor(PhaseGray[Index], PhaseGray[Index], PhaseGray[Index], 255));
	}
	TestTrue(TEXT("Amplitude conversion succeeds"), CGHComplexFieldPreview::BuildGrayscale(Field, ECGHObserverPreviewMode::Amplitude, Pixels, Error));
	const uint8 AmplitudeGray[] = {64, 128, 191, 255};
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		TestEqual(TEXT("Amplitude is magnitude normalized to maximum, not intensity"),
			Pixels[Index], FColor(AmplitudeGray[Index], AmplitudeGray[Index], AmplitudeGray[Index], 255));
	}
	TestTrue(TEXT("Intensity conversion succeeds"), CGHComplexFieldPreview::BuildGrayscale(Field, ECGHObserverPreviewMode::Intensity, Pixels, Error));
	const uint8 IntensityGray[] = {16, 64, 143, 255};
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		TestEqual(TEXT("Intensity is squared magnitude normalized to maximum and preserves row-major orientation"),
			Pixels[Index], FColor(IntensityGray[Index], IntensityGray[Index], IntensityGray[Index], 255));
	}
	Field.Samples = {{0.0, 0.0}, {-0.0, -0.0}, {0.0, -0.0}, {-0.0, 0.0}};
	for (const ECGHObserverPreviewMode Mode : {ECGHObserverPreviewMode::Phase, ECGHObserverPreviewMode::Amplitude, ECGHObserverPreviewMode::Intensity})
	{
		TestTrue(TEXT("Zero field converts without undefined normalization"), CGHComplexFieldPreview::BuildGrayscale(Field, Mode, Pixels, Error));
		for (const FColor& Pixel : Pixels) { TestEqual(TEXT("Zero field is opaque black"), Pixel, FColor::Black); }
	}
	for (const double Scale : {std::numeric_limits<double>::max(), std::numeric_limits<double>::min()})
	{
		Field.Samples = {{Scale, Scale}, {0.5 * Scale, 0.5 * Scale}, {0.0, 0.0}, {-Scale, Scale}};
		TestTrue(TEXT("Extreme finite components normalize without overflow or underflow"),
			CGHComplexFieldPreview::BuildGrayscale(Field, ECGHObserverPreviewMode::Amplitude, Pixels, Error));
		if (TestEqual(TEXT("Extreme field has four pixels"), Pixels.Num(), 4))
		{
			TestEqual(TEXT("Maximum diagonal magnitude is white"), Pixels[0].R, uint8(255));
			TestEqual(TEXT("Half magnitude is middle gray"), Pixels[1].R, uint8(128));
			TestEqual(TEXT("Zero amplitude remains black"), Pixels[2].R, uint8(0));
		}
		TestTrue(TEXT("Extreme finite components produce intensity without overflow or underflow"),
			CGHComplexFieldPreview::BuildGrayscale(Field, ECGHObserverPreviewMode::Intensity, Pixels, Error));
		if (TestEqual(TEXT("Extreme intensity field has four pixels"), Pixels.Num(), 4))
		{
			TestEqual(TEXT("Maximum diagonal intensity is white"), Pixels[0].R, uint8(255));
			TestEqual(TEXT("Half magnitude yields one-quarter intensity"), Pixels[1].R, uint8(64));
			TestEqual(TEXT("Zero intensity remains black"), Pixels[2].R, uint8(0));
			TestEqual(TEXT("Complex component signs do not alter intensity"), Pixels[3].R, uint8(255));
		}
	}
	Field = MakeObserverTestField();
	Field.Samples[3].Imaginary = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("NaN imaginary component rejects the whole field"), CGHComplexFieldPreview::BuildGrayscale(Field, ECGHObserverPreviewMode::Phase, Pixels, Error));
	TestTrue(TEXT("Failure clears previous pixels and returns a diagnostic"), Pixels.IsEmpty() && !Error.IsEmpty());
	Field.Samples[3].Imaginary = 0.0;
	Field.Samples[3].Real = std::numeric_limits<double>::infinity();
	TestFalse(TEXT("Infinite real component is rejected"), Field.IsValid());
	Field = MakeObserverTestField();
	Field.Samples.Pop();
	TestFalse(TEXT("Incomplete samples are rejected"), Field.IsValid());
	Field.ResolutionX = MAX_int32;
	Field.ResolutionY = MAX_int32;
	TestFalse(TEXT("Huge dimensions fail without integer overflow"), Field.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHObserverStorageTest,
	"CGH.ObserverPlane.AtomicStorageAndResolutionChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHObserverStorageTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Editor)) { Scene.ForwardErrorMessages(this); return false; }
	ACGHObserverPlaneActor* Observer = Scene.GetTestWorld()->SpawnActor<ACGHObserverPlaneActor>();
	if (!TestNotNull(TEXT("Observer actor spawns"), Observer)) { return false; }
	TestFalse(TEXT("New observers do not fabricate reconstructed data"), Observer->HasValidComplexField());
	TestTrue(TEXT("New observers allocate no field samples"), Observer->GetComplexField().Samples.IsEmpty());
	Observer->Parameters.ResolutionX = 2;
	Observer->Parameters.ResolutionY = 2;
	FCGHComplexField Field = MakeObserverTestField();
	Field.Revision = 12345;
	TestTrue(TEXT("Matching complex field publishes"), Observer->SetComplexField(Field));
	const uint64 Revision = Observer->GetComplexFieldRevision();
	TestTrue(TEXT("Publication revision belongs to the observer"), Revision != Field.Revision && Revision > 0);
	TestTrue(TEXT("Observer owns independent exact data"), Observer->GetComplexField().Samples == Field.Samples);
	Field.Samples[0].Real = 22.0;
	TestEqual(TEXT("Editing caller data cannot mutate stored samples"), Observer->GetComplexField().Samples[0].Real, 1.0);
	FCGHComplexField Copy = Observer->GetComplexFieldCopy();
	Copy.Samples[0].Imaginary = 42.0;
	TestEqual(TEXT("Blueprint readback cannot mutate stored samples"), Observer->GetComplexField().Samples[0].Imaginary, 0.0);
	const FProperty* Storage = FindFProperty<FProperty>(ACGHObserverPlaneActor::StaticClass(), TEXT("ComplexField"));
	if (TestNotNull(TEXT("Field is reflected"), Storage))
	{
		TestTrue(TEXT("Bulk samples are transient"), Storage->HasAnyPropertyFlags(CPF_Transient));
		TestFalse(TEXT("Details and Blueprint cannot edit bulk storage"), Storage->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible));
	}
	FCGHComplexField Invalid = MakeObserverTestField();
	Invalid.Samples[3].Imaginary = std::numeric_limits<double>::quiet_NaN();
	const FCGHComplexSample* StoragePointer = Observer->GetComplexField().Samples.GetData();
	TestFalse(TEXT("Nonfinite input is rejected"), Observer->SetComplexField(Invalid));
	TestTrue(TEXT("Failed publication leaves previous valid storage intact"), Observer->HasValidComplexField()
		&& Observer->GetComplexField().Samples.GetData() == StoragePointer);
	TestEqual(TEXT("Rejected data cannot advance revision"), Observer->GetComplexFieldRevision(), Revision);
	TestTrue(TEXT("Identical data publishes successfully"), Observer->SetComplexField(MakeObserverTestField()));
	TestEqual(TEXT("Identical publication reuses revision"), Observer->GetComplexFieldRevision(), Revision);
	TestTrue(TEXT("Successful publication clears validation error"), Observer->ComplexFieldError.IsEmpty());
	FCGHComplexField Moved = MakeObserverTestField();
	Moved.Samples[0].Real = 9.0;
	const FCGHComplexSample* MovedStorage = Moved.Samples.GetData();
	TestTrue(TEXT("Owned backend output publishes"), Observer->SetComplexField(MoveTemp(Moved)));
	TestTrue(TEXT("Ownership-transfer publication avoids an array copy"), Observer->GetComplexField().Samples.GetData() == MovedStorage);
	Observer->Parameters.ResolutionX = 3;
	TestFalse(TEXT("Dimension changes immediately invalidate readback"), Observer->HasValidComplexField());
	Observer->Tick(0.0f);
	TestTrue(TEXT("Tick releases stale field samples"), Observer->GetComplexField().Samples.IsEmpty());
	TestEqual(TEXT("Empty storage tracks current dimensions"), Observer->GetComplexField().ResolutionX, 3);
	const uint64 EmptyRevision = Observer->GetComplexFieldRevision();
	Observer->ClearComplexField();
	TestEqual(TEXT("Clearing empty data is idempotent"), Observer->GetComplexFieldRevision(), EmptyRevision);
	Observer->Parameters.PixelPitchXUm = 12.0;
	Observer->SetActorScale3D(FVector(2.0, 3.0, 4.0));
	TestEqual(TEXT("Optical width ignores actor scale"), Observer->GetActiveWidthMm(), 0.036, 1.e-12);
	Scene.ForwardErrorMessages(this);
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHObserverEditorPreviewTest,
	"CGH.ObserverPlane.SelectedEditorPreviewModesAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHObserverEditorPreviewTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Editor)) { Scene.ForwardErrorMessages(this); return false; }
	ACGHObserverPlaneActor* Observer = Scene.GetTestWorld()->SpawnActor<ACGHObserverPlaneActor>();
	if (!TestNotNull(TEXT("Observer actor spawns"), Observer)) { return false; }
	UCGHObserverPreviewComponent* Preview = Observer->FindComponentByClass<UCGHObserverPreviewComponent>();
	if (!TestNotNull(TEXT("Observer owns editor preview component"), Preview)) { return false; }
	TestTrue(TEXT("Native selection discovers the observer preview"), FLevelEditorViewportClient::FindViewComponentForActor(Observer) == Preview);
	FMinimalViewInfo View;
	TestTrue(TEXT("Preview supplies frame metadata"), Preview->GetEditorPreviewInfo(0.0f, View));
	TSharedPtr<SWidget> PreviewWidget = Preview->GetCustomEditorPreviewWidget();
	if (!TestTrue(TEXT("Preview supplies a custom Slate widget"), PreviewWidget.IsValid())) { return false; }
	TestNull(TEXT("No texture exists before reconstruction"), Preview->GetPreviewTexture());
	Observer->Parameters.ResolutionX = 2;
	Observer->Parameters.ResolutionY = 2;
	Observer->SetComplexField(MakeObserverTestField());
	FProperty* ModeProperty = FindFProperty<FProperty>(ACGHObserverPlaneActor::StaticClass(),
		GET_MEMBER_NAME_CHECKED(ACGHObserverPlaneActor, PreviewMode));
	if (!TestNotNull(TEXT("Preview mode is exposed for Details editing"), ModeProperty)) { return false; }
	const auto EditPreviewMode = [Observer, ModeProperty, &PreviewWidget](ECGHObserverPreviewMode Mode)
	{
		Observer->PreEditChange(ModeProperty);
		Observer->PreviewMode = Mode;
		FPropertyChangedEvent Event(ModeProperty, EPropertyChangeType::ValueSet);
		Observer->PostEditChangeProperty(Event);
		// Keep the already-open preview and let its normal tick observe the Details edit.
		PreviewWidget->Tick(FGeometry(), 0.0, 0.0f);
	};
	const uint64 Revision = Observer->GetComplexFieldRevision();
	EditPreviewMode(ECGHObserverPreviewMode::Phase);
	TestTrue(TEXT("Details mode edit retains the existing preview component"),
		Observer->FindComponentByClass<UCGHObserverPreviewComponent>() == Preview);
	TestTrue(TEXT("Details mode edit preserves the complex samples through construction"),
		Observer->GetComplexField().Samples == MakeObserverTestField().Samples);
	TestTrue(TEXT("Already-open preview follows Phase selected in Details"),
		Preview->GetPreviewMode() == ECGHObserverPreviewMode::Phase);
	TArray<FString> PreviewText;
	GatherObserverPreviewText(PreviewWidget.ToSharedRef(), PreviewText);
	TestTrue(TEXT("Phase preview header identifies the current mode"), PreviewText.Contains(TEXT("Phase | 2 x 2 px")));
	TestTrue(TEXT("Phase preview legend identifies the displayed data"),
		PreviewText.Contains(TEXT("Phase: 0 (black) to 2\u03c0 (white), wrapped")));
	UTexture2D* Texture = Preview->GetPreviewTexture();
	if (!TestNotNull(TEXT("Complex field generates a preview texture"), Texture)) { return false; }
	TestEqual(TEXT("Texture preserves horizontal sampling"), Texture->GetSizeX(), 2);
	TestEqual(TEXT("Texture preserves vertical sampling"), Texture->GetSizeY(), 2);
	TestFalse(TEXT("Numerical grayscale has no sRGB conversion"), Texture->SRGB);
	TestTrue(TEXT("Pixels use nearest sampling"), Texture->Filter == TF_Nearest);
	const auto ReadGray = [Texture](int32 Index)
	{
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		const FColor* Pixels = static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
		const uint8 Gray = Pixels[Index].R;
		Mip.BulkData.Unlock();
		return Gray;
	};
	TestEqual(TEXT("Phase mode displays first sample argument"), ReadGray(0), uint8(0));
	Preview->RefreshPreviewTexture();
	TestTrue(TEXT("Unchanged field reuses texture"), Preview->GetPreviewTexture() == Texture);
	EditPreviewMode(ECGHObserverPreviewMode::Amplitude);
	TestTrue(TEXT("Already-open preview follows Amplitude selected in Details"),
		Preview->GetPreviewMode() == ECGHObserverPreviewMode::Amplitude);
	PreviewText.Reset();
	GatherObserverPreviewText(PreviewWidget.ToSharedRef(), PreviewText);
	TestTrue(TEXT("Amplitude preview header identifies the current mode"), PreviewText.Contains(TEXT("Amplitude | 2 x 2 px")));
	TestTrue(TEXT("Amplitude preview legend identifies the displayed data"),
		PreviewText.Contains(TEXT("Amplitude: 0 (black) to field maximum (white)")));
	TestTrue(TEXT("Mode switching reuses texture allocation"), Preview->GetPreviewTexture() == Texture);
	TestEqual(TEXT("Mode switching updates pixels even without a data revision"), ReadGray(0), uint8(64));
	TestEqual(TEXT("Mode switching leaves the complex data unchanged"), Observer->GetComplexFieldRevision(), Revision);
	EditPreviewMode(ECGHObserverPreviewMode::Intensity);
	TestTrue(TEXT("Already-open preview follows Intensity selected in Details"),
		Preview->GetPreviewMode() == ECGHObserverPreviewMode::Intensity);
	PreviewText.Reset();
	GatherObserverPreviewText(PreviewWidget.ToSharedRef(), PreviewText);
	TestTrue(TEXT("Intensity preview header identifies the current mode"), PreviewText.Contains(TEXT("Intensity | 2 x 2 px")));
	TestTrue(TEXT("Intensity preview legend identifies the displayed data"),
		PreviewText.Contains(TEXT("Intensity: 0 (black) to field maximum (white)")));
	TestTrue(TEXT("Intensity switching reuses texture allocation"), Preview->GetPreviewTexture() == Texture);
	const uint8 IntensityGray[] = {16, 64, 143, 255};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(IntensityGray); ++Index)
	{
		TestEqual(TEXT("Intensity mode updates the existing texture with normalized squared magnitudes"), ReadGray(Index), IntensityGray[Index]);
	}
	TestEqual(TEXT("Intensity switching leaves the complex data revision unchanged"), Observer->GetComplexFieldRevision(), Revision);
	TestTrue(TEXT("Intensity switching preserves the complex samples"), Observer->GetComplexField().Samples == MakeObserverTestField().Samples);
	Observer->ClearComplexField();
	TestNull(TEXT("Clear immediately releases stale texture"), Preview->GetPreviewTexture());
	Scene.ForwardErrorMessages(this);
	return true;
}
#endif

#endif
