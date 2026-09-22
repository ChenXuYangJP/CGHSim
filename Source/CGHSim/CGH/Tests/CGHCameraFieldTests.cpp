#include "CGH/Actors/CGHCameraActor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Types/CGHComplexFieldAsset.h"
#include "CineCameraComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UnrealType.h"
#include <limits>

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "CGH/Components/CGHObserverPreviewComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Layout/Children.h"
#include "LevelEditorViewport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "Widgets/Text/STextBlock.h"
#endif

namespace
{
	FCGHComplexField MakeCameraFieldFixture()
	{
		FCGHComplexField Field;
		Field.ResolutionX = 2; Field.ResolutionY = 2; Field.Revision = 999;
		Field.Samples = {{1.0, 0.0}, {0.0, 2.0}, {-3.0, 0.0}, {0.0, -4.0}};
		return Field;
	}
#if WITH_EDITOR
	void GatherCameraPreviewText(const TSharedRef<SWidget>& Widget, TArray<FString>& Text)
	{
		if (Widget->GetType() == FName(TEXT("STextBlock"))) Text.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index) GatherCameraPreviewText(Children->GetChildAt(Index), Text);
	}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraSamplingStorageTest,
	"CGH.CameraField.SamplingAndAtomicStorage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHCameraSamplingStorageTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Game)) { Scene.ForwardErrorMessages(this); return false; }
	ACGHCameraActor* Camera = Scene.GetTestWorld()->SpawnActor<ACGHCameraActor>();
	if (!TestNotNull(TEXT("Camera fixture spawns"), Camera)) return false;
	TestTrue(TEXT("Existing sensor dimensions remain authoritative by default"), Camera->Parameters.SensorSampling == ECGHCameraSensorSampling::SensorSize);
	TestEqual(TEXT("Default horizontal pitch follows legacy sensor width"), Camera->GetSensorPixelPitchXM(), 18.75e-6, 1.e-16);
	TestEqual(TEXT("Default vertical pitch follows legacy sensor height"), Camera->GetSensorPixelPitchYM(), 24.e-3 / 1080, 1.e-16);
	TestTrue(TEXT("Camera optical preview defaults to intensity"), Camera->PreviewMode == ECGHObserverPreviewMode::Intensity && Camera->PreviewType == ECGHCameraPreviewType::OpticalField);
	TestFalse(TEXT("New camera does not fabricate sensor samples"), Camera->HasValidComplexField());
	Camera->Parameters.SensorWidthMm = 13.5; Camera->Parameters.SensorHeightMm = 9.25;
	Camera->Parameters.OutputResolutionX = 7; Camera->Parameters.OutputResolutionY = 5;
	Camera->RefreshVisualization();
	TestEqual(TEXT("Sensor-size mode derives horizontal pitch"), Camera->GetSensorPixelPitchXM(), 0.0135 / 7, 1.e-16);
	TestEqual(TEXT("Sensor-size mode derives vertical pitch"), Camera->GetSensorPixelPitchYM(), 0.00925 / 5, 1.e-16);
	Camera->Parameters.SensorSampling = ECGHCameraSensorSampling::PixelPitch;
	Camera->Parameters.PixelPitchXUm = 3.25; Camera->Parameters.PixelPitchYUm = 4.75;
	Camera->SetActorScale3D(FVector(2.0, 3.0, 4.0));
	Camera->RefreshVisualization();
	TestEqual(TEXT("Pixel-pitch mode derives width without actor scale"), Camera->GetSensorWidthM(), 7 * 3.25e-6, 1.e-16);
	TestEqual(TEXT("Pixel-pitch mode derives height without actor scale"), Camera->GetSensorHeightM(), 5 * 4.75e-6, 1.e-16);
	TestEqual(TEXT("Sampling mode preserves inactive legacy sensor width"), Camera->Parameters.SensorWidthMm, 13.5);
	TestEqual(TEXT("Sampling mode preserves inactive legacy sensor height"), Camera->Parameters.SensorHeightMm, 9.25);
	UCineCameraComponent* Cine = Camera->FindComponentByClass<UCineCameraComponent>();
	if (TestNotNull(TEXT("Optional geometric Cine Camera is retained"), Cine))
	{
		TestTrue(TEXT("Game worlds retain an active Cine Camera for ordinary runtime views"), Cine->IsActive());
		TestEqual(TEXT("Geometric filmback follows effective sensor width"), double(Cine->Filmback.SensorWidth), Camera->GetSensorWidthM() * 1000.0, 1.e-6);
		TestEqual(TEXT("Geometric filmback follows effective sensor height"), double(Cine->Filmback.SensorHeight), Camera->GetSensorHeightM() * 1000.0, 1.e-6);
	}
	Camera->Parameters.OutputResolutionX = 2; Camera->Parameters.OutputResolutionY = 2;
	FCGHComplexField Field = MakeCameraFieldFixture();
	TestTrue(TEXT("Matching sensor field publishes"), Camera->SetComplexField(Field));
	const uint64 Revision = Camera->GetComplexFieldRevision();
	TestTrue(TEXT("Camera owns publication revisions"), Revision > 0 && Revision != Field.Revision);
	const FCGHComplexSample* Storage = Camera->GetComplexField().Samples.GetData();
	Field.Samples[0].Real = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("Nonfinite field cannot replace accepted camera data"), Camera->SetComplexField(Field));
	TestTrue(TEXT("Rejected field retains accepted allocation"), Camera->GetComplexField().Samples.GetData() == Storage);
	TestEqual(TEXT("Rejected field retains camera revision"), Camera->GetComplexFieldRevision(), Revision);
	TestTrue(TEXT("Identical sensor publication succeeds"), Camera->SetComplexField(MakeCameraFieldFixture()));
	TestEqual(TEXT("Identical sensor publication reuses revision"), Camera->GetComplexFieldRevision(), Revision);
	FCGHComplexField Copy = Camera->GetComplexFieldCopy(); Copy.Samples[0].Real = 17.0;
	TestEqual(TEXT("Blueprint readback cannot mutate accepted data"), Camera->GetComplexField().Samples[0].Real, 1.0);
	FCGHComplexField Owned = MakeCameraFieldFixture(); Owned.Samples[0].Real = 0.25;
	const FCGHComplexSample* OwnedStorage = Owned.Samples.GetData();
	TestTrue(TEXT("Camera accepts backend ownership transfer"), Camera->SetComplexField(MoveTemp(Owned)));
	TestTrue(TEXT("Ownership transfer avoids a bulk copy"), Camera->GetComplexField().Samples.GetData() == OwnedStorage);
	const FProperty* StorageProperty = FindFProperty<FProperty>(ACGHCameraActor::StaticClass(), TEXT("ComplexField"));
	if (TestNotNull(TEXT("Camera storage is reflected"), StorageProperty))
	{
		TestTrue(TEXT("Camera bulk field is transient and not duplicated"), StorageProperty->HasAllPropertyFlags(CPF_Transient | CPF_DuplicateTransient));
		TestFalse(TEXT("Details and Blueprint cannot mutate bulk storage directly"), StorageProperty->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible));
	}
	Camera->Parameters.OutputResolutionX = 3;
	TestFalse(TEXT("Sensor resolution edit invalidates old field immediately"), Camera->HasValidComplexField());
	Camera->Tick(0.0f);
	TestTrue(TEXT("Camera tick releases mismatched field samples"), Camera->GetComplexField().Samples.IsEmpty());
	const uint64 EmptyRevision = Camera->GetComplexFieldRevision();
	Camera->ClearComplexField();
	TestEqual(TEXT("Clearing an empty sensor is idempotent"), Camera->GetComplexFieldRevision(), EmptyRevision);
	Scene.ForwardErrorMessages(this);
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraSelectedPreviewTest,
	"CGH.CameraField.SelectedOpticalPreviewAndModeSwitching", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHCameraSelectedPreviewTest::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Editor)) { Scene.ForwardErrorMessages(this); return false; }
	ACGHCameraActor* Camera = Scene.GetTestWorld()->SpawnActor<ACGHCameraActor>();
	if (!TestNotNull(TEXT("Preview camera spawns"), Camera)) return false;
	Camera->Parameters.OutputResolutionX = 2; Camera->Parameters.OutputResolutionY = 2;
	Camera->SetComplexField(MakeCameraFieldFixture());
	Camera->RefreshVisualization();
	UCGHObserverPreviewComponent* Preview = Camera->FindComponentByClass<UCGHObserverPreviewComponent>();
	if (!TestNotNull(TEXT("Camera reuses the complex-field preview component"), Preview)) return false;
	TestFalse(TEXT("Editor Cine Camera is inactive so it cannot take priority over the optical inset"), Camera->FindComponentByClass<UCineCameraComponent>()->IsActive());
	TestTrue(TEXT("Native camera selection chooses the optical preview instead of Cine geometry"), FLevelEditorViewportClient::FindViewComponentForActor(Camera) == Preview);
	TSharedPtr<SWidget> Widget = Preview->GetCustomEditorPreviewWidget();
	if (!TestTrue(TEXT("Camera supplies a field preview widget"), Widget.IsValid())) return false;
	UTexture2D* Texture = Preview->GetPreviewTexture();
	if (!TestNotNull(TEXT("Camera field allocates a preview texture"), Texture)) return false;
	const uint64 Revision = Camera->GetComplexFieldRevision();
	FProperty* ModeProperty = FindFProperty<FProperty>(ACGHCameraActor::StaticClass(), TEXT("PreviewMode"));
	if (!TestNotNull(TEXT("Camera field mode is exposed to Details"), ModeProperty)) return false;
	const ECGHObserverPreviewMode Modes[] = {ECGHObserverPreviewMode::Intensity, ECGHObserverPreviewMode::Phase, ECGHObserverPreviewMode::Amplitude};
	const TCHAR* Headers[] = {TEXT("Intensity | 2 x 2 px"), TEXT("Phase | 2 x 2 px"), TEXT("Amplitude | 2 x 2 px")};
	const uint8 Expected[][4] = {{16, 64, 143, 255}, {0, 64, 128, 191}, {64, 128, 191, 255}};
	for (int32 Mode = 0; Mode < UE_ARRAY_COUNT(Modes); ++Mode)
	{
		Camera->PreEditChange(ModeProperty); Camera->PreviewMode = Modes[Mode];
		FPropertyChangedEvent Event(ModeProperty, EPropertyChangeType::ValueSet); Camera->PostEditChangeProperty(Event);
		Widget->Tick(FGeometry(), 0.0, 0.0f);
		TestTrue(TEXT("Already-open camera preview follows Details mode"), Preview->GetPreviewMode() == Modes[Mode]);
		TestTrue(TEXT("Camera mode changes reuse texture storage"), Preview->GetPreviewTexture() == Texture);
		TArray<FString> Text; GatherCameraPreviewText(Widget.ToSharedRef(), Text);
		TestTrue(TEXT("Camera preview header identifies current field mode"), Text.Contains(Headers[Mode]));
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		const FColor* Pixels = static_cast<const FColor*>(Mip.BulkData.LockReadOnly());
		for (int32 Index = 0; Index < 4; ++Index) TestEqual(TEXT("Camera field preview preserves numerical grayscale"), Pixels[Index].R, Expected[Mode][Index]);
		Mip.BulkData.Unlock();
	}
	Camera->PreviewType = ECGHCameraPreviewType::Geometric;
	Widget->Tick(FGeometry(), 0.0, 0.0f);
	FMinimalViewInfo View;
	TestTrue(TEXT("Geometric option supplies Cine Camera view metadata"), Preview->GetEditorPreviewInfo(0.0f, View));
	FMinimalViewInfo CineView;
	Camera->FindComponentByClass<UCineCameraComponent>()->GetCameraView(0.0f, CineView);
	TestEqual(TEXT("Geometric option uses the lens field of view"), View.FOV, CineView.FOV);
	Camera->PreviewType = ECGHCameraPreviewType::OpticalField;
	Widget->Tick(FGeometry(), 0.0, 0.0f);
	TArray<FString> RestoredText; GatherCameraPreviewText(Widget.ToSharedRef(), RestoredText);
	TestTrue(TEXT("Switching back restores optical content without reselection"), RestoredText.Contains(TEXT("Amplitude | 2 x 2 px")));
	TestEqual(TEXT("Preview changes preserve sensor data revision"), Camera->GetComplexFieldRevision(), Revision);
	Camera->ClearComplexField();
	TestNull(TEXT("Clearing the camera releases its stale preview texture"), Preview->GetPreviewTexture());
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraFieldSaveTest,
	"CGH.CameraField.ExplicitSaveAndExactSensorLoad", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHCameraFieldSaveTest::RunTest(const FString& Parameters)
{
	struct FCameraSaveCleanup
	{
		FString Name = TEXT("CameraField_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString AssetFolder = TEXT("/Game/CGHSimTests/") + Name;
		FString ContentFolder = FPaths::ProjectContentDir() / TEXT("CGHSimTests") / Name;
		FString RawFolder = FPaths::ProjectSavedDir() / TEXT("Automation") / Name;
		TWeakObjectPtr<UCGHComplexFieldAsset> Asset;
		~FCameraSaveCleanup()
		{
			if (Asset.IsValid())
			{
				FAssetRegistryModule::AssetDeleted(Asset.Get()); Asset->GetPackage()->SetDirtyFlag(false);
				Asset->ClearFlags(RF_Public | RF_Standalone); Asset->MarkAsGarbage();
			}
			IFileManager::Get().DeleteDirectory(*ContentFolder, false, true);
			IFileManager::Get().DeleteDirectory(*RawFolder, false, true);
		}
		int32 Count() const
		{
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *ContentFolder, TEXT("*"), true, false);
			IFileManager::Get().FindFilesRecursive(Files, *RawFolder, TEXT("*"), true, false, false);
			return Files.Num();
		}
	} Files;
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Editor)) { Scene.ForwardErrorMessages(this); return false; }
	ACGHCameraActor* Camera = Scene.GetTestWorld()->SpawnActor<ACGHCameraActor>();
	if (!TestNotNull(TEXT("Camera save fixture spawns"), Camera)) return false;
	Camera->FieldAssetSaveFolder.Path = Files.AssetFolder; Camera->FieldRawSaveDirectory.Path = FPaths::ConvertRelativePathToFull(Files.RawFolder);
	Camera->Parameters.SensorSampling = ECGHCameraSensorSampling::PixelPitch;
	Camera->Parameters.OutputResolutionX = 2; Camera->Parameters.OutputResolutionY = 2;
	Camera->Parameters.PixelPitchXUm = 8.0; Camera->Parameters.PixelPitchYUm = 9.0;
	TestFalse(TEXT("An empty camera cannot save"), Camera->SaveCurrentComplexField());
	Camera->SetComplexField(MakeCameraFieldFixture()); Camera->Tick(0.0f);
	TestEqual(TEXT("Camera publication does not autosave"), Files.Count(), 0);
	const uint64 Revision = Camera->GetComplexFieldRevision();
	const FCGHComplexSample* Storage = Camera->GetComplexField().Samples.GetData();
	Camera->SaveComplexField();
	Files.Asset = Camera->LastSavedFieldAsset.Get();
	if (!TestEqual(TEXT("Camera saves an asset, binary, JSON, and three grayscale PNGs"), Files.Count(), 6)) { AddError(Camera->FieldSaveStatus); return false; }
	TestEqual(TEXT("Camera save preserves publication revision"), Camera->GetComplexFieldRevision(), Revision);
	TestTrue(TEXT("Camera save preserves the active allocation"), Camera->GetComplexField().Samples.GetData() == Storage);
	TestTrue(TEXT("Camera save preserves the selected asset"), Camera->StoredComplexField.IsNull());
	FString Json;
	TestTrue(TEXT("Camera metadata is readable"), FFileHelper::LoadFileToString(Json, *Camera->LastSavedFieldMetadataFile));
	TSharedPtr<FJsonObject> Metadata;
	if (!TestTrue(TEXT("Camera metadata parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Metadata) && Metadata.IsValid())) return false;
	TestEqual(TEXT("Camera export uses sensor-local coordinates"), Metadata->GetStringField(TEXT("coordinate_frame")), FString(TEXT("camera-sensor-local")));
	TestEqual(TEXT("Camera export origin is the sensor center"), Metadata->GetStringField(TEXT("coordinate_origin")), FString(TEXT("sensor center")));
	TestEqual(TEXT("Camera export records effective horizontal pitch"), Metadata->GetNumberField(TEXT("pixel_pitch_x_m")), 8.e-6);
	TestEqual(TEXT("Camera export records effective vertical pitch"), Metadata->GetNumberField(TEXT("pixel_pitch_y_m")), 9.e-6);
	const FString SavedBinary = Camera->LastSavedFieldBinaryFile;
	Camera->FieldRawSaveDirectory.Path.Reset();
	TestFalse(TEXT("Invalid camera save destination fails"), Camera->SaveCurrentComplexField());
	TestEqual(TEXT("Failed camera save preserves last successful paths"), Camera->LastSavedFieldBinaryFile, SavedBinary);
	Camera->StoredComplexField = Camera->LastSavedFieldAsset;
	Camera->ClearComplexField();
	Camera->Parameters.SensorSampling = ECGHCameraSensorSampling::SensorSize;
	Camera->Parameters.SensorWidthMm = 0.016; Camera->Parameters.SensorHeightMm = 0.018;
	Camera->LoadStoredComplexField();
	TestTrue(TEXT("Equivalent effective sampling loads across sensor modes"), Camera->HasValidComplexField());
	TestTrue(TEXT("Camera load restores exact complex components"), Camera->GetComplexField().Samples == MakeCameraFieldFixture().Samples);
	const uint64 LoadedRevision = Camera->GetComplexFieldRevision();
	Camera->Parameters.SensorHeightMm = 0.019;
	Camera->LoadStoredComplexField();
	TestFalse(TEXT("Camera load explains incompatible effective sampling"), Camera->ComplexFieldError.IsEmpty());
	TestEqual(TEXT("Incompatible load preserves field revision"), Camera->GetComplexFieldRevision(), LoadedRevision);
	TestEqual(TEXT("Incompatible load preserves configured sensor size"), Camera->Parameters.SensorHeightMm, 0.019);
	TestEqual(TEXT("Load attempts create no new files"), Files.Count(), 6);
	Scene.ForwardErrorMessages(this);
	return true;
}
#endif

#endif
