#include "CGH/Utils/CGHComplexFieldIO.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CGH/Actors/CGHObserverPlaneActor.h"
#include "CGH/Actors/CGHCameraActor.h"
#include "CGH/Actors/CGHReconstructorActor.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "CGH/Types/CGHComplexFieldAsset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"
#include <limits>

namespace
{
	FCGHComplexField MakeFieldSaveFixture()
	{
		FCGHComplexField Field;
		Field.ResolutionX = 3;
		Field.ResolutionY = 2;
		Field.Samples = {{1.0, 0.0}, {0.0, 2.0}, {-3.0, 0.0}, {0.0, -4.0}, {-0.0, 0.0}, {1.2345678901234567, -1.e-100}};
		Field.Revision = 987;
		return Field;
	}

	TArray<uint8> FieldSaveExpectedBytes(const FCGHComplexField& Field)
	{
		TArray<uint8> Bytes;
		for (const FCGHComplexSample& Sample : Field.Samples)
		{
			for (double Value : {Sample.Real, Sample.Imaginary})
			{
				uint64 Bits;
				FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
				for (int32 Byte = 0; Byte < 8; ++Byte) Bytes.Add(static_cast<uint8>(Bits >> (Byte * 8)));
			}
		}
		return Bytes;
	}

	struct FComplexFieldSaveFiles
	{
		FString AssetFolder;
		FString ContentFolder;
		FString RawFolder;
		TArray<FString> AssetPaths;

		FComplexFieldSaveFiles()
		{
			const FString Name = TEXT("ObserverSave_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			AssetFolder = TEXT("/Game/CGHSimTests/") + Name;
			ContentFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("CGHSimTests") / Name);
			RawFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / Name);
		}

		~FComplexFieldSaveFiles()
		{
			for (const FString& Path : AssetPaths)
			{
				if (UCGHComplexFieldAsset* Asset = FindObject<UCGHComplexFieldAsset>(nullptr, *Path))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
					Asset->GetPackage()->SetDirtyFlag(false);
					Asset->ClearFlags(RF_Public | RF_Standalone);
					Asset->MarkAsGarbage();
				}
			}
			// Only GUID-named fixture directories are removed; never touch user assets or maps.
			IFileManager::Get().DeleteDirectory(*ContentFolder, false, true);
			IFileManager::Get().DeleteDirectory(*RawFolder, false, true);
		}

		bool Save(const FCGHComplexField& Field, FCGHComplexFieldSaveResult& Out)
		{
			const bool bSaved = CGHComplexFieldIO::Save(Field, 8.e-6, 9.e-6, AssetFolder, RawFolder,
				FText::FromString(TEXT("Observer \"field\" 波面")), Out);
			if (bSaved) AssetPaths.Add(Out.AssetPath);
			return bSaved;
		}

		int32 FileCount() const
		{
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *ContentFolder, TEXT("*"), true, false);
			IFileManager::Get().FindFilesRecursive(Files, *RawFolder, TEXT("*"), true, false, false);
			return Files.Num();
		}
	};

	bool CheckObserverSavedPNG(FAutomationTestBase& Test, const FString& Filename, const TArray<uint8>& Expected)
	{
		TArray<uint8> Compressed;
		if (!Test.TestTrue(TEXT("PNG is readable"), FFileHelper::LoadFileToArray(Compressed, *Filename))) return false;
		if (!Test.TestTrue(TEXT("PNG contains an 8-bit grayscale IHDR"), Compressed.Num() >= 33
			&& FMemory::Memcmp(Compressed.GetData() + 12, "IHDR", 4) == 0 && Compressed[24] == 8 && Compressed[25] == 0)) return false;
		FImage Image;
		if (!Test.TestTrue(TEXT("PNG decodes"), FImageUtils::DecompressImage(Compressed.GetData(), Compressed.Num(), Image))) return false;
		Test.TestEqual(TEXT("PNG width matches independent observer grid"), Image.SizeX, 3);
		Test.TestEqual(TEXT("PNG height matches independent observer grid"), Image.SizeY, 2);
		Test.TestTrue(TEXT("PNG uses single-channel grayscale"), Image.Format == ERawImageFormat::G8);
		if (!Test.TestEqual(TEXT("One grayscale byte per complex sample"), Image.RawData.Num(), int64(Expected.Num()))) return false;
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			Test.TestEqual(FString::Printf(TEXT("PNG sample %d retains the expected numerical grayscale and row order"), Index), Image.RawData[Index], Expected[Index]);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHComplexFieldSaveRoundTripTest,
	"CGH.ComplexFieldSave.ExactComplexExportImagesAndDiskReload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHComplexFieldSaveRoundTripTest::RunTest(const FString& Parameters)
{
	FComplexFieldSaveFiles Files;
	const FCGHComplexField Field = MakeFieldSaveFixture();
	const TArray<uint8> Original = FieldSaveExpectedBytes(Field);
	const FCGHComplexSample* OriginalStorage = Field.Samples.GetData();
	FCGHComplexFieldSaveResult Saved;
	if (!TestTrue(TEXT("Save all complex-field formats"), Files.Save(Field, Saved))) { AddError(Saved.Error); return false; }
	TestEqual(TEXT("Only asset, binary, JSON and three PNGs remain"), Files.FileCount(), 6);
	TestTrue(TEXT("Saving preserves every input bit"), FieldSaveExpectedBytes(Field) == Original);
	TestTrue(TEXT("Saving preserves active allocation"), Field.Samples.GetData() == OriginalStorage);
	TestEqual(TEXT("Saving preserves publication revision"), Field.Revision, uint64(987));
	TArray<uint8> Raw;
	TestTrue(TEXT("Read headerless binary"), FFileHelper::LoadFileToArray(Raw, *Saved.BinaryFilename));
	TestTrue(TEXT("Binary is exactly interleaved real/imaginary little-endian float64"), Raw == Original);
	CheckObserverSavedPNG(*this, Saved.PhaseImageFilename, {0, 64, 128, 191, 0, 255});
	CheckObserverSavedPNG(*this, Saved.AmplitudeImageFilename, {64, 128, 191, 255, 0, 79});
	CheckObserverSavedPNG(*this, Saved.IntensityImageFilename, {16, 64, 143, 255, 0, 24});
	FString Json;
	TestTrue(TEXT("Metadata is readable"), FFileHelper::LoadFileToString(Json, *Saved.MetadataFilename));
	TSharedPtr<FJsonObject> Metadata;
	if (!TestTrue(TEXT("Metadata is valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Metadata) && Metadata.IsValid())) return false;
	TestEqual(TEXT("Versioned binary schema"), Metadata->GetIntegerField(TEXT("format_version")), 1);
	TestEqual(TEXT("Grid columns"), Metadata->GetIntegerField(TEXT("resolution_x")), 3);
	TestEqual(TEXT("Grid rows"), Metadata->GetIntegerField(TEXT("resolution_y")), 2);
	TestEqual(TEXT("Complex dtype"), Metadata->GetStringField(TEXT("dtype")), FString(TEXT("complex128")));
	TestEqual(TEXT("Component dtype"), Metadata->GetStringField(TEXT("component_dtype")), FString(TEXT("float64")));
	TestEqual(TEXT("Explicit component order"), Metadata->GetStringField(TEXT("component_order")), FString(TEXT("real,imaginary")));
	TestEqual(TEXT("Little endian"), Metadata->GetStringField(TEXT("endianness")), FString(TEXT("little")));
	TestEqual(TEXT("Observer coordinates"), Metadata->GetStringField(TEXT("coordinate_frame")), FString(TEXT("observer-local")));
	TestEqual(TEXT("Column direction"), Metadata->GetStringField(TEXT("column_direction_observer")), FString(TEXT("+Y")));
	TestEqual(TEXT("Row direction"), Metadata->GetStringField(TEXT("row_direction_observer")), FString(TEXT("-Z")));
	TestEqual(TEXT("Horizontal pitch in meters"), Metadata->GetNumberField(TEXT("pixel_pitch_x_m")), 8.e-6);
	TestEqual(TEXT("Vertical pitch in meters"), Metadata->GetNumberField(TEXT("pixel_pitch_y_m")), 9.e-6);
	TestEqual(TEXT("Metadata points to phase image"), Metadata->GetStringField(TEXT("phase_image_filename")), FPaths::GetCleanFilename(Saved.PhaseImageFilename));
	TestEqual(TEXT("Metadata points to amplitude image"), Metadata->GetStringField(TEXT("amplitude_image_filename")), FPaths::GetCleanFilename(Saved.AmplitudeImageFilename));
	TestEqual(TEXT("Metadata points to intensity image"), Metadata->GetStringField(TEXT("intensity_image_filename")), FPaths::GetCleanFilename(Saved.IntensityImageFilename));
	TestEqual(TEXT("Intensity image documents normalized squared magnitude"), Metadata->GetStringField(TEXT("intensity_image_mapping")),
		FString(TEXT("round(255 * abs(sample)^2 / max(abs(field)^2)); zero field maps to 0")));
	TestEqual(TEXT("UTF-8 label survives"), Metadata->GetStringField(TEXT("field_label")), FString(TEXT("Observer \"field\" 波面")));
	UPackage* Package = FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(Saved.AssetPath));
	if (!TestNotNull(TEXT("Saved package exists"), Package)) return false;
	FText UnloadError;
	if (!TestTrue(TEXT("Unload before disk reload"), UPackageTools::UnloadPackages({Package}, UnloadError))) { AddError(UnloadError.ToString()); return false; }
	UCGHComplexFieldAsset* Restored = LoadObject<UCGHComplexFieldAsset>(nullptr, *Saved.AssetPath);
	if (!TestNotNull(TEXT("Asset reloads from disk"), Restored)) return false;
	TestTrue(TEXT("Reloaded field is valid"), Restored->HasValidField());
	TestTrue(TEXT("Asset preserves exact double bits including signed zero"), FieldSaveExpectedBytes(Restored->GetField()) == Original);
	TestEqual(TEXT("Asset restores pitch"), Restored->GetPixelPitchXM(), 8.e-6);
	TestEqual(TEXT("Asset stores an independent revision"), Restored->GetField().Revision, uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHComplexFieldSaveFailureTest,
	"CGH.ComplexFieldSave.UniqueSavesAndAtomicFailures", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHComplexFieldSaveFailureTest::RunTest(const FString& Parameters)
{
	FComplexFieldSaveFiles Files;
	FCGHComplexField Field = MakeFieldSaveFixture();
	FCGHComplexFieldSaveResult First, Second;
	if (!TestTrue(TEXT("Initial save succeeds"), Files.Save(Field, First))) { AddError(First.Error); return false; }
	TArray<uint8> FirstBytes;
	FFileHelper::LoadFileToArray(FirstBytes, *First.BinaryFilename);
	Field.Samples[0].Imaginary = 1.25;
	if (!TestTrue(TEXT("Repeated save succeeds"), Files.Save(Field, Second))) { AddError(Second.Error); return false; }
	TestNotEqual(TEXT("Distinct asset per save"), First.AssetPath, Second.AssetPath);
	TestNotEqual(TEXT("Distinct raw output per save"), First.BinaryFilename, Second.BinaryFilename);
	TArray<uint8> AfterBytes;
	FFileHelper::LoadFileToArray(AfterBytes, *First.BinaryFilename);
	TestTrue(TEXT("Earlier save is never replaced"), AfterBytes == FirstBytes);
	TestEqual(TEXT("Two complete saves leave twelve files"), Files.FileCount(), 12);
	const auto Reject = [&](const FCGHComplexField& Input, double Pitch, const FString& AssetFolder, const FString& RawFolder)
	{
		FCGHComplexFieldSaveResult Failed = First;
		TestFalse(TEXT("Invalid save rejected"), CGHComplexFieldIO::Save(Input, Pitch, 9.e-6, AssetFolder, RawFolder, FText::GetEmpty(), Failed));
		TestFalse(TEXT("Failed save gives an error"), Failed.Error.IsEmpty());
		TestTrue(TEXT("Failed save has no published paths"), Failed.AssetPath.IsEmpty() && Failed.AssetFilename.IsEmpty()
			&& Failed.BinaryFilename.IsEmpty() && Failed.MetadataFilename.IsEmpty() && Failed.PhaseImageFilename.IsEmpty() && Failed.AmplitudeImageFilename.IsEmpty() && Failed.IntensityImageFilename.IsEmpty());
	};
	Reject(FCGHComplexField(), 8.e-6, Files.AssetFolder, Files.RawFolder);
	Field.Samples[0].Imaginary = std::numeric_limits<double>::quiet_NaN();
	Reject(Field, 8.e-6, Files.AssetFolder, Files.RawFolder);
	Field = MakeFieldSaveFixture();
	Reject(Field, 0.0, Files.AssetFolder, Files.RawFolder);
	Reject(Field, 8.e-6, TEXT("/Engine/ObserverSaveTest"), Files.RawFolder);
	Reject(Field, 8.e-6, Files.AssetFolder, TEXT(""));
	TestEqual(TEXT("Validation failures create no outputs"), Files.FileCount(), 12);
	const FString Blocker = Files.RawFolder / TEXT("existing_file");
	TestTrue(TEXT("Create a fixture directory blocker"), FFileHelper::SaveStringToFile(TEXT("preserve this file"), *Blocker));
	Reject(Field, 8.e-6, Files.AssetFolder, Blocker / TEXT("child"));
	FString Preserved;
	FFileHelper::LoadFileToString(Preserved, *Blocker);
	TestEqual(TEXT("Failure preserves preexisting files"), Preserved, FString(TEXT("preserve this file")));
	TestEqual(TEXT("Failure leaves no partial outputs"), Files.FileCount(), 13);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHObserverSaveActorTest,
	"CGH.ComplexFieldSave.ActorExplicitSaveAndExactLoad", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHObserverSaveActorTest::RunTest(const FString& Parameters)
{
	FComplexFieldSaveFiles Files;
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Editor)) { Scene.ForwardErrorMessages(this); return false; }
	ACGHObserverPlaneActor* Observer = Scene.GetTestWorld()->SpawnActor<ACGHObserverPlaneActor>();
	if (!TestNotNull(TEXT("Observer fixture spawned"), Observer)) return false;
	Observer->FieldAssetSaveFolder.Path = Files.AssetFolder;
	Observer->FieldRawSaveDirectory.Path = Files.RawFolder;
	TestFalse(TEXT("Empty observer cannot save"), Observer->SaveCurrentComplexField());
	Observer->Parameters.ResolutionX = 3;
	Observer->Parameters.ResolutionY = 2;
	Observer->Parameters.PixelPitchXUm = 8.0;
	Observer->Parameters.PixelPitchYUm = 9.0;
	Observer->SetComplexField(MakeFieldSaveFixture());
	Observer->PreviewMode = ECGHObserverPreviewMode::Phase;
	Observer->Tick(0.0f);
	TestEqual(TEXT("Publication, tick and display mode changes do not autosave"), Files.FileCount(), 0);
	const uint64 Revision = Observer->GetComplexFieldRevision();
	const TArray<uint8> Before = FieldSaveExpectedBytes(Observer->GetComplexField());
	const FCGHComplexSample* Storage = Observer->GetComplexField().Samples.GetData();
	if (!TestTrue(TEXT("Explicit observer save succeeds"), Observer->SaveCurrentComplexField())) { AddError(Observer->FieldSaveStatus); return false; }
	Files.AssetPaths.Add(Observer->LastSavedFieldAsset.ToSoftObjectPath().ToString());
	TestEqual(TEXT("Saving writes all six formats even in phase preview mode"), Files.FileCount(), 6);
	TestTrue(TEXT("All three PNG paths are reported"), !Observer->LastSavedFieldPhaseImageFile.IsEmpty()
		&& !Observer->LastSavedFieldAmplitudeImageFile.IsEmpty() && !Observer->LastSavedFieldIntensityImageFile.IsEmpty());
	CheckObserverSavedPNG(*this, Observer->LastSavedFieldIntensityImageFile, {16, 64, 143, 255, 0, 24});
	TestTrue(TEXT("Save preserves active complex bytes"), Before == FieldSaveExpectedBytes(Observer->GetComplexField()));
	TestTrue(TEXT("Save preserves active sample allocation"), Storage == Observer->GetComplexField().Samples.GetData());
	TestEqual(TEXT("Save preserves active revision"), Observer->GetComplexFieldRevision(), Revision);
	TestTrue(TEXT("Save does not select a stored field automatically"), Observer->StoredComplexField.IsNull());
	const FString SavedBinary = Observer->LastSavedFieldBinaryFile;
	const FString SavedIntensity = Observer->LastSavedFieldIntensityImageFile;
	Observer->FieldRawSaveDirectory.Path = TEXT("");
	TestFalse(TEXT("Invalid destination fails without altering accepted outputs"), Observer->SaveCurrentComplexField());
	TestEqual(TEXT("Last successful path survives failure"), Observer->LastSavedFieldBinaryFile, SavedBinary);
	TestEqual(TEXT("Last successful intensity image survives failure"), Observer->LastSavedFieldIntensityImageFile, SavedIntensity);
	TestEqual(TEXT("Failed save preserves field revision"), Observer->GetComplexFieldRevision(), Revision);
	Observer->StoredComplexField = Observer->LastSavedFieldAsset;
	Observer->ClearComplexField();
	Observer->LoadStoredComplexField();
	TestTrue(TEXT("Saved asset loads explicitly on the matching grid"), Observer->HasValidComplexField());
	TestTrue(TEXT("Explicit load restores exact complex samples"), Before == FieldSaveExpectedBytes(Observer->GetComplexField()));
	const uint64 LoadedRevision = Observer->GetComplexFieldRevision();
	Observer->Parameters.PixelPitchXUm = 16.0;
	Observer->LoadStoredComplexField();
	TestFalse(TEXT("Pitch mismatch is explained"), Observer->ComplexFieldError.IsEmpty());
	TestEqual(TEXT("Rejected pitch load leaves data revision unchanged"), Observer->GetComplexFieldRevision(), LoadedRevision);
	TestTrue(TEXT("Rejected pitch load preserves samples"), Before == FieldSaveExpectedBytes(Observer->GetComplexField()));
	Observer->Parameters.PixelPitchXUm = 8.0;
	Observer->Parameters.ResolutionX = 4;
	Observer->LoadStoredComplexField();
	TestFalse(TEXT("Grid mismatch is explained"), Observer->ComplexFieldError.IsEmpty());
	TestEqual(TEXT("Load does not change requested dimensions"), Observer->Parameters.ResolutionX, 4);
	TestTrue(TEXT("Rejected grid load does not overwrite retained samples"), Before == FieldSaveExpectedBytes(Observer->GetComplexField()));
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructorComplexFieldSaveTest,
	"CGH.ComplexFieldSave.ReconstructorButtonAndPublicationGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructorComplexFieldSaveTest::RunTest(const FString& Parameters)
{
	FComplexFieldSaveFiles Files;
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Editor)) { Scene.ForwardErrorMessages(this); return false; }
	UWorld* World = Scene.GetTestWorld();
	ACGHSLMActor* SLM = World->SpawnActor<ACGHSLMActor>();
	ACGHReconstructionLightActor* Light = World->SpawnActor<ACGHReconstructionLightActor>();
	ACGHObserverPlaneActor* Observer = World->SpawnActor<ACGHObserverPlaneActor>();
	ACGHWorkbenchActor* Workbench = World->SpawnActor<ACGHWorkbenchActor>();
	ACGHReconstructorActor* Reconstructor = World->SpawnActor<ACGHReconstructorActor>();
	if (!SLM || !Light || !Observer || !Workbench || !Reconstructor) { AddError(TEXT("Could not create reconstructor save fixture.")); return false; }
	SLM->Parameters.ResolutionX = 2;
	SLM->Parameters.ResolutionY = 2;
	SLM->GeneratePreviewPhaseRamp();
	Observer->Parameters.ResolutionX = 3;
	Observer->Parameters.ResolutionY = 2;
	Observer->SetActorLocation(FVector(40.0, 0.0, 0.0));
	Observer->FieldAssetSaveFolder.Path = Files.AssetFolder;
	Observer->FieldRawSaveDirectory.Path = Files.RawFolder;
	Workbench->SLM = SLM;
	Workbench->ReconstructionLight = Light;
	Workbench->ObserverPlane = Observer;
	Workbench->Reconstructor = Reconstructor;
	Reconstructor->Workbench = Workbench;
	const auto WaitReady = [&]()
	{
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		do
		{
			Reconstructor->PollReconstructor();
			if (Reconstructor->JobState != ECGHReconstructionJobState::Queued && Reconstructor->JobState != ECGHReconstructionJobState::Running)
			{
				return TestTrue(*Reconstructor->StatusMessage, Reconstructor->JobState == ECGHReconstructionJobState::Ready);
			}
			FPlatformProcess::Sleep(0.001f);
		} while (FPlatformTime::Seconds() < Deadline);
		Reconstructor->CancelReconstruction();
		AddError(TEXT("Timed out waiting for reconstructor save fixture."));
		return false;
	};
	TestFalse(TEXT("An idle reconstructor cannot save"), Reconstructor->SaveReconstructedComplexField());
	Observer->SetComplexField(MakeFieldSaveFixture());
	TestFalse(TEXT("Unrelated observer data is not this reconstructor's publication"), Reconstructor->SaveReconstructedComplexField());
	if (!TestTrue(TEXT("Tiny CPU reconstruction starts"), Reconstructor->StartReconstruction())) return false;
	TestFalse(TEXT("An unpublished active job cannot save"), Reconstructor->SaveReconstructedComplexField());
	if (!WaitReady()) return false;
	TestEqual(TEXT("Reconstruction does not automatically save"), Files.FileCount(), 0);
	const FCGHComplexField Before = Observer->GetComplexField();
	const FCGHComplexSample* BeforeStorage = Observer->GetComplexField().Samples.GetData();
	const int64 BeforeJobId = Reconstructor->JobId;
	const double BeforeCompute = Reconstructor->LastComputeSeconds;
	const FString BeforeStatus = Reconstructor->StatusMessage;
	Reconstructor->SaveComplexField();
	if (!Observer->LastSavedFieldAsset.IsNull()) Files.AssetPaths.Add(Observer->LastSavedFieldAsset.ToSoftObjectPath().ToString());
	if (!TestEqual(TEXT("Reconstructor editor button writes the asset, binary, metadata, and three PNGs"), Files.FileCount(), 6))
	{
		AddError(Reconstructor->FieldSaveStatus); return false;
	}
	TestEqual(TEXT("Reconstructor reports the observer's save result"), Reconstructor->FieldSaveStatus, Observer->FieldSaveStatus);
	TestTrue(TEXT("Save status identifies the exported binary"), Reconstructor->FieldSaveStatus.Contains(Observer->LastSavedFieldBinaryFile));
	TestTrue(TEXT("Save preserves Ready state"), Reconstructor->JobState == ECGHReconstructionJobState::Ready);
	TestEqual(TEXT("Save preserves job identity"), Reconstructor->JobId, BeforeJobId);
	TestEqual(TEXT("Save preserves calculation duration"), Reconstructor->LastComputeSeconds, BeforeCompute);
	TestEqual(TEXT("Save preserves calculation status"), Reconstructor->StatusMessage, BeforeStatus);
	TestEqual(TEXT("Save preserves observer revision"), Observer->GetComplexFieldRevision(), Before.Revision);
	TestTrue(TEXT("Save preserves every complex sample bit"), FieldSaveExpectedBytes(Observer->GetComplexField()) == FieldSaveExpectedBytes(Before));
	TestTrue(TEXT("Save preserves the active sample allocation"), Observer->GetComplexField().Samples.GetData() == BeforeStorage);
	TestTrue(TEXT("Save does not change the stored-field selection"), Observer->StoredComplexField.IsNull());
	Observer->FieldRawSaveDirectory.Path.Reset();
	TestFalse(TEXT("Observer export failures propagate through the reconstructor"), Reconstructor->SaveReconstructedComplexField());
	TestEqual(TEXT("Delegated failure status remains actionable"), Reconstructor->FieldSaveStatus, Observer->FieldSaveStatus);
	Observer->FieldRawSaveDirectory.Path = Files.RawFolder;
	Workbench->ObserverPlane = nullptr;
	TestFalse(TEXT("A missing observer reference blocks saving"), Reconstructor->SaveReconstructedComplexField());
	ACGHObserverPlaneActor* OtherObserver = World->SpawnActor<ACGHObserverPlaneActor>();
	if (!TestNotNull(TEXT("Replacement observer spawns"), OtherObserver)) return false;
	OtherObserver->Parameters = Observer->Parameters;
	OtherObserver->SetComplexField(Before);
	Workbench->ObserverPlane = OtherObserver;
	TestFalse(TEXT("A different observer with identical samples is not the published destination"), Reconstructor->SaveReconstructedComplexField());
	Workbench->ObserverPlane = Observer;
	Reconstructor->Workbench = nullptr;
	TestFalse(TEXT("A missing workbench blocks saving"), Reconstructor->SaveReconstructedComplexField());
	Reconstructor->Workbench = Workbench;
	FCGHComplexField Replacement = Before;
	Replacement.Samples[0].Real += 1.0;
	Observer->SetComplexField(Replacement);
	TestFalse(TEXT("Externally replaced samples invalidate publication ownership"), Reconstructor->SaveReconstructedComplexField());
	Observer->ClearComplexField();
	TestFalse(TEXT("A cleared field cannot be saved through the reconstructor"), Reconstructor->SaveReconstructedComplexField());
	if (!Reconstructor->StartReconstruction() || !WaitReady()) return false;
	if (!Reconstructor->StartReconstruction()) return false;
	Reconstructor->CancelReconstruction();
	TestFalse(TEXT("A cancelled job cannot save retained previous output"), Reconstructor->SaveReconstructedComplexField());
	if (!Reconstructor->StartReconstruction() || !WaitReady()) return false;
	Reconstructor->Parameters.Mode = static_cast<ECGHReconstructionMode>(255);
	TestFalse(TEXT("Unknown reconstruction request fails"), Reconstructor->StartReconstruction());
	TestFalse(TEXT("A failed job cannot save retained previous output"), Reconstructor->SaveReconstructedComplexField());
	TestEqual(TEXT("Rejected saves leave the original six output files intact"), Files.FileCount(), 6);
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraReconstructorSaveTest,
	"CGH.ComplexFieldSave.CameraReconstructorPublicationGuards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHCameraReconstructorSaveTest::RunTest(const FString& Parameters)
{
	FComplexFieldSaveFiles Files;
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Editor)) { Scene.ForwardErrorMessages(this); return false; }
	UWorld* World = Scene.GetTestWorld();
	ACGHSLMActor* SLM = World->SpawnActor<ACGHSLMActor>();
	ACGHReconstructionLightActor* Light = World->SpawnActor<ACGHReconstructionLightActor>();
	ACGHCameraActor* Camera = World->SpawnActor<ACGHCameraActor>();
	ACGHWorkbenchActor* Workbench = World->SpawnActor<ACGHWorkbenchActor>();
	ACGHReconstructorActor* Reconstructor = World->SpawnActor<ACGHReconstructorActor>();
	if (!SLM || !Light || !Camera || !Workbench || !Reconstructor) return false;
	SLM->Parameters.ResolutionX = 2;
	SLM->Parameters.ResolutionY = 2;
	SLM->GeneratePreviewPhaseRamp();
	Camera->Parameters.OutputResolutionX = 3;
	Camera->Parameters.OutputResolutionY = 2;
	Camera->Parameters.SensorSampling = ECGHCameraSensorSampling::PixelPitch;
	Camera->Parameters.PixelPitchXUm = 8.0;
	Camera->Parameters.PixelPitchYUm = 9.0;
	Camera->Parameters.FocalLengthMm = 50.0;
	Camera->Parameters.FocusDistanceMm = 400.0;
	Camera->Parameters.FNumber = 100.0;
	Camera->Parameters.PupilResolutionX = 4;
	Camera->Parameters.PupilResolutionY = 4;
	Camera->SetActorLocationAndRotation(FVector(40.0, 0.0, 0.0), FRotator(0.0, 180.0, 0.0));
	Camera->FieldAssetSaveFolder.Path = Files.AssetFolder;
	Camera->FieldRawSaveDirectory.Path = Files.RawFolder;
	Workbench->SLM = SLM;
	Workbench->ReconstructionLight = Light;
	Workbench->Camera = Camera;
	Workbench->Reconstructor = Reconstructor;
	Reconstructor->Workbench = Workbench;
	Reconstructor->Parameters.Mode = ECGHReconstructionMode::Camera;
	const auto WaitReady = [&]()
	{
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		do
		{
			Reconstructor->PollReconstructor();
			if (Reconstructor->JobState != ECGHReconstructionJobState::Queued && Reconstructor->JobState != ECGHReconstructionJobState::Running)
				return TestTrue(*Reconstructor->StatusMessage, Reconstructor->JobState == ECGHReconstructionJobState::Ready);
			FPlatformProcess::Sleep(0.001f);
		} while (FPlatformTime::Seconds() < Deadline);
		Reconstructor->CancelReconstruction();
		AddError(TEXT("Timed out waiting for camera reconstruction save fixture."));
		return false;
	};
	TestFalse(TEXT("Unpublished camera cannot be saved through the reconstructor"), Reconstructor->SaveReconstructedComplexField());
	if (!TestTrue(TEXT("Camera reconstruction starts without observer"), Reconstructor->StartReconstruction())) return false;
	TestFalse(TEXT("An active camera job cannot save"), Reconstructor->SaveReconstructedComplexField());
	if (!WaitReady()) return false;
	const FCGHComplexField Before = Camera->GetComplexField();
	const FCGHComplexSample* Storage = Camera->GetComplexField().Samples.GetData();
	const int64 JobId = Reconstructor->JobId;
	TestEqual(TEXT("Camera publication does not autosave"), Files.FileCount(), 0);
	if (!TestTrue(TEXT("Reconstructor delegates accepted camera save"), Reconstructor->SaveReconstructedComplexField())) { AddError(Reconstructor->FieldSaveStatus); return false; }
	Files.AssetPaths.Add(Camera->LastSavedFieldAsset.ToSoftObjectPath().ToString());
	TestEqual(TEXT("Camera save emits asset, binary, metadata and all three PNGs"), Files.FileCount(), 6);
	TestEqual(TEXT("Camera save status propagates to reconstructor"), Reconstructor->FieldSaveStatus, Camera->FieldSaveStatus);
	TArray<uint8> Raw;
	TestTrue(TEXT("Camera complex export is readable"), FFileHelper::LoadFileToArray(Raw, *Camera->LastSavedFieldBinaryFile));
	TestTrue(TEXT("Saved camera result preserves exact complex samples"), Raw == FieldSaveExpectedBytes(Before));
	TestEqual(TEXT("Saving preserves camera publication revision"), Camera->GetComplexFieldRevision(), Before.Revision);
	TestEqual(TEXT("Saving does not queue another camera job"), Reconstructor->JobId, JobId);
	TestTrue(TEXT("Saving retains camera sample allocation"), Camera->GetComplexField().Samples.GetData() == Storage);
	Reconstructor->Parameters.Mode = ECGHReconstructionMode::ObserverPlane;
	TestFalse(TEXT("Switching destination mode invalidates camera save selection"), Reconstructor->SaveReconstructedComplexField());
	Reconstructor->Parameters.Mode = ECGHReconstructionMode::Camera;
	Workbench->Camera = nullptr;
	TestFalse(TEXT("Missing camera reference blocks delegated save"), Reconstructor->SaveReconstructedComplexField());
	ACGHCameraActor* Replacement = World->SpawnActor<ACGHCameraActor>();
	if (!TestNotNull(TEXT("Replacement camera exists"), Replacement)) return false;
	Replacement->Parameters = Camera->Parameters;
	Replacement->SetComplexField(Before);
	Workbench->Camera = Replacement;
	TestFalse(TEXT("Identical data on another camera is not the accepted destination"), Reconstructor->SaveReconstructedComplexField());
	Workbench->Camera = Camera;
	Camera->FieldRawSaveDirectory.Path.Reset();
	TestFalse(TEXT("Camera save failures propagate"), Reconstructor->SaveReconstructedComplexField());
	TestEqual(TEXT("Delegated camera errors remain actionable"), Reconstructor->FieldSaveStatus, Camera->FieldSaveStatus);
	Camera->FieldRawSaveDirectory.Path = Files.RawFolder;
	FCGHComplexField Changed = Before;
	Changed.Samples[0].Real += 1.0;
	Camera->SetComplexField(Changed);
	TestFalse(TEXT("External camera field replacement invalidates ownership"), Reconstructor->SaveReconstructedComplexField());
	Camera->ClearComplexField();
	TestFalse(TEXT("Cleared camera cannot save a previous publication"), Reconstructor->SaveReconstructedComplexField());
	if (!Reconstructor->StartReconstruction() || !WaitReady() || !Reconstructor->StartReconstruction()) return false;
	Reconstructor->CancelReconstruction();
	TestFalse(TEXT("Cancelled camera job cannot save retained output"), Reconstructor->SaveReconstructedComplexField());
	TestEqual(TEXT("Rejected saves create no additional outputs"), Files.FileCount(), 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHComplexFieldAssetValidationTest,
	"CGH.ComplexFieldSave.AssetRejectsInvalidReplacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHComplexFieldAssetValidationTest::RunTest(const FString& Parameters)
{
	UCGHComplexFieldAsset* Asset = NewObject<UCGHComplexFieldAsset>();
	FCGHComplexField Field = MakeFieldSaveFixture();
	TestTrue(TEXT("Valid asset payload accepted"), Asset->SetField(Field, 8.e-6, 9.e-6));
	const TArray<uint8> Before = FieldSaveExpectedBytes(Asset->GetField());
	Field.Samples[1].Real = std::numeric_limits<double>::infinity();
	TestFalse(TEXT("Nonfinite asset replacement rejected"), Asset->SetField(Field, 8.e-6, 9.e-6));
	TestTrue(TEXT("Invalid field preserves saved payload"), Before == FieldSaveExpectedBytes(Asset->GetField()));
	TestFalse(TEXT("Invalid pitch rejected"), Asset->SetField(MakeFieldSaveFixture(), -1.0, 9.e-6));
	TestEqual(TEXT("Invalid pitch preserves previous sampling geometry"), Asset->GetPixelPitchXM(), 8.e-6);
	TestTrue(TEXT("Asset remains valid after rejected replacement"), Asset->HasValidField());
	return true;
}

#endif
