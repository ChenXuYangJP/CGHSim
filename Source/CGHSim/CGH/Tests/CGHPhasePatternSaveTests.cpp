#include "CGH/Utils/CGHPhasePatternIO.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CGH/Types/CGHPhasePatternAsset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include <limits>

namespace
{
	FCGHSLMPhasePattern MakeSavePattern()
	{
		FCGHSLMPhasePattern Pattern;
		Pattern.ResolutionX = 3;
		Pattern.ResolutionY = 2;
		Pattern.PhaseRad = {-0.0, -1.2345678901234567, UE_DOUBLE_PI, 2.0 * UE_DOUBLE_PI, 1.e-100, 123456789.12345679};
		Pattern.Revision = 987;
		return Pattern;
	}

	struct FSaveTestFiles
	{
		FString AssetFolder;
		FString ContentFolder;
		FString RawFolder;
		TArray<FString> AssetPaths;

		FSaveTestFiles()
		{
			const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			AssetFolder = TEXT("/Game/CGHSimTests/PhaseSave_") + Id;
			ContentFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("CGHSimTests") / (TEXT("PhaseSave_") + Id));
			RawFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / (TEXT("CGHPhaseSave_") + Id));
		}

		~FSaveTestFiles()
		{
			for (const FString& Path : AssetPaths)
			{
				if (UCGHPhasePatternAsset* Asset = FindObject<UCGHPhasePatternAsset>(nullptr, *Path))
				{
					FAssetRegistryModule::AssetDeleted(Asset);
					Asset->ClearFlags(RF_Public | RF_Standalone);
					Asset->MarkAsGarbage();
					Asset->GetPackage()->SetDirtyFlag(false);
				}
			}
			// These GUID-named roots belong solely to this fixture, never project/user assets.
			IFileManager::Get().DeleteDirectory(*ContentFolder, false, true);
			IFileManager::Get().DeleteDirectory(*RawFolder, false, true);
		}

		bool Save(const FCGHSLMPhasePattern& Pattern, FCGHPhaseSaveResult& Result)
		{
			const bool bSaved = CGHPhasePatternIO::Save(Pattern, 8.e-6, 9.e-6, AssetFolder, RawFolder,
				FText::FromString(TEXT("Exact phase \"test\" 波面")), true, Result);
			if (bSaved) { AssetPaths.Add(Result.AssetPath); }
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

	bool CheckGrayPNG(FAutomationTestBase& Test, const FString& Filename,
		int32 Width, int32 Height, const TArray<uint8>& ExpectedPixels)
	{
		TArray<uint8> Compressed;
		if (!Test.TestTrue(TEXT("Saved PNG bytes can be read"), FFileHelper::LoadFileToArray(Compressed, *Filename)))
		{
			return false;
		}
		const uint8 PNGSignature[] = {137, 80, 78, 71, 13, 10, 26, 10};
		if (!Test.TestTrue(TEXT("Export has a PNG signature and IHDR header"), Compressed.Num() >= 33
			&& FMemory::Memcmp(Compressed.GetData(), PNGSignature, sizeof(PNGSignature)) == 0
			&& FMemory::Memcmp(Compressed.GetData() + 12, "IHDR", 4) == 0))
		{
			return false;
		}
		Test.TestEqual(TEXT("PNG stores eight bits per grayscale sample"), Compressed[24], uint8(8));
		Test.TestEqual(TEXT("PNG stores grayscale color type without RGB or alpha"), Compressed[25], uint8(0));
		FImage Decoded;
		if (!Test.TestTrue(TEXT("PNG decodes successfully"), FImageUtils::DecompressImage(Compressed.GetData(), Compressed.Num(), Decoded)))
		{
			return false;
		}
		Test.TestEqual(TEXT("PNG width matches the SLM columns exactly"), Decoded.SizeX, Width);
		Test.TestEqual(TEXT("PNG height matches the SLM rows exactly"), Decoded.SizeY, Height);
		Test.TestTrue(TEXT("PNG decodes as native single-channel grayscale"), Decoded.Format == ERawImageFormat::G8);
		if (!Test.TestEqual(TEXT("Decoded image contains exactly one byte per SLM pixel"), Decoded.RawData.Num(), int64(ExpectedPixels.Num())))
		{
			return false;
		}
		for (int32 Index = 0; Index < ExpectedPixels.Num(); ++Index)
		{
			Test.TestEqual(FString::Printf(TEXT("PNG pixel row %d column %d preserves wrapped phase intensity and orientation"), Index / Width, Index % Width),
				Decoded.RawData[Index], ExpectedPixels[Index]);
		}
		return true;
	}

	TArray<uint8> ExpectedPhaseBytes(const FCGHSLMPhasePattern& Pattern)
	{
		TArray<uint8> Bytes;
		for (double Phase : Pattern.PhaseRad)
		{
			uint64 Bits;
			FMemory::Memcpy(&Bits, &Phase, sizeof(Bits));
			for (int32 I = 0; I < 8; ++I) { Bytes.Add(static_cast<uint8>(Bits >> (I * 8))); }
		}
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPhaseSaveRoundTripTest,
	"CGH.PhasePatternSave.ExactNumericExportAndDiskReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPhaseSaveRoundTripTest::RunTest(const FString& Parameters)
{
	FSaveTestFiles Files;
	FCGHSLMPhasePattern Pattern = MakeSavePattern();
	const TArray<uint8> OriginalBytes = ExpectedPhaseBytes(Pattern);
	const double* OriginalStorage = Pattern.PhaseRad.GetData();
	FCGHPhaseSaveResult Result;
	if (!TestTrue(TEXT("A complete phase asset and numeric export can be saved"), Files.Save(Pattern, Result)))
	{
		AddError(Result.Error);
		return false;
	}
	TestTrue(TEXT("Successful save has no diagnostic"), Result.Error.IsEmpty());
	TestEqual(TEXT("Only the four final files remain"), Files.FileCount(), 4);
	TestTrue(TEXT("Input doubles and signed zero are preserved"), OriginalBytes == ExpectedPhaseBytes(Pattern));
	TestTrue(TEXT("Save does not replace the active phase allocation"), OriginalStorage == Pattern.PhaseRad.GetData());
	TestEqual(TEXT("Save does not change the active publication revision"), Pattern.Revision, uint64(987));
	TestTrue(TEXT("The grayscale PNG exists"), IFileManager::Get().FileExists(*Result.ImageFilename));
	TestEqual(TEXT("The image is in the same directory as the raw samples"), FPaths::GetPath(Result.ImageFilename), FPaths::GetPath(Result.BinaryFilename));
	TestEqual(TEXT("The image shares the numeric save identity"), FPaths::GetBaseFilename(Result.ImageFilename), FPaths::GetBaseFilename(Result.BinaryFilename));
	TestEqual(TEXT("The image uses the PNG extension"), FPaths::GetExtension(Result.ImageFilename), FString(TEXT("png")));
	TArray<uint8> Bytes;
	TestTrue(TEXT("Raw binary can be read"), FFileHelper::LoadFileToArray(Bytes, *Result.BinaryFilename));
	TestTrue(TEXT("Binary is exactly headerless row-major little-endian float64, including signed zero"), Bytes == OriginalBytes);
	FString Json;
	TestTrue(TEXT("Metadata is readable UTF-8"), FFileHelper::LoadFileToString(Json, *Result.MetadataFilename));
	TSharedPtr<FJsonObject> Metadata;
	if (TestTrue(TEXT("Sidecar is valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Metadata)) && Metadata)
	{
		TestEqual(TEXT("Format is versioned"), Metadata->GetIntegerField(TEXT("format_version")), 1);
		TestEqual(TEXT("Width"), Metadata->GetIntegerField(TEXT("resolution_x")), 3);
		TestEqual(TEXT("Height"), Metadata->GetIntegerField(TEXT("resolution_y")), 2);
		TestEqual(TEXT("Raw file reference is local to the JSON sidecar"), Metadata->GetStringField(TEXT("binary_filename")), FPaths::GetCleanFilename(Result.BinaryFilename));
		TestEqual(TEXT("Image file reference is local to the JSON sidecar"), Metadata->GetStringField(TEXT("image_filename")), FPaths::GetCleanFilename(Result.ImageFilename));
		TestEqual(TEXT("Image format is explicit"), Metadata->GetStringField(TEXT("image_format")), FString(TEXT("PNG")));
		TestEqual(TEXT("Image bit depth is explicit"), Metadata->GetIntegerField(TEXT("image_bit_depth")), 8);
		TestEqual(TEXT("Image phase mapping is explicit"), Metadata->GetStringField(TEXT("image_mapping")), FString(TEXT("round(255 * wrap_0_2pi(phase_rad) / (2*pi))")));
		TestEqual(TEXT("Image rows follow canonical phase-array order"), Metadata->GetStringField(TEXT("image_row_order")), FString(TEXT("top-to-bottom")));
		TestEqual(TEXT("Metadata links to the saved Unreal asset"), Metadata->GetStringField(TEXT("asset_path")), Result.AssetPath);
		TestEqual(TEXT("Numeric type"), Metadata->GetStringField(TEXT("dtype")), FString(TEXT("float64")));
		TestEqual(TEXT("Byte order"), Metadata->GetStringField(TEXT("endianness")), FString(TEXT("little")));
		TestEqual(TEXT("Phase unit"), Metadata->GetStringField(TEXT("phase_unit")), FString(TEXT("radians")));
		TestEqual(TEXT("Array order"), Metadata->GetStringField(TEXT("array_order")), FString(TEXT("row-major")));
		TestEqual(TEXT("Column direction"), Metadata->GetStringField(TEXT("column_direction_slm")), FString(TEXT("+Y")));
		TestEqual(TEXT("Row direction"), Metadata->GetStringField(TEXT("row_direction_slm")), FString(TEXT("-Z")));
		TestEqual(TEXT("Horizontal pitch"), Metadata->GetNumberField(TEXT("pixel_pitch_x_m")), 8.e-6);
		TestEqual(TEXT("Vertical pitch"), Metadata->GetNumberField(TEXT("pixel_pitch_y_m")), 9.e-6);
		TestTrue(TEXT("Preview provenance is explicit"), Metadata->GetBoolField(TEXT("is_preview_pattern")));
		TestEqual(TEXT("Unicode label is escaped and retained"), Metadata->GetStringField(TEXT("pattern_label")), FString(TEXT("Exact phase \"test\" 波面")));
		TestFalse(TEXT("Unrecorded source wavelength is not invented"), Metadata->HasField(TEXT("wavelength_m")));
	}

	UPackage* Package = FindPackage(nullptr, *FPackageName::ObjectPathToPackageName(Result.AssetPath));
	if (!TestNotNull(TEXT("Saved package exists in memory"), Package)) { return false; }
	FText UnloadError;
	if (!TestTrue(TEXT("The saved package can be unloaded before disk reload"), UPackageTools::UnloadPackages({Package}, UnloadError)))
	{
		AddError(UnloadError.ToString());
		return false;
	}
	UCGHPhasePatternAsset* Restored = LoadObject<UCGHPhasePatternAsset>(nullptr, *Result.AssetPath);
	if (TestNotNull(TEXT("The generated .uasset loads from disk"), Restored))
	{
		TestTrue(TEXT("Reloaded pattern is valid"), Restored->HasValidPattern());
		TestEqual(TEXT("Reloaded width"), Restored->GetResolutionX(), Pattern.ResolutionX);
		TestEqual(TEXT("Reloaded height"), Restored->GetResolutionY(), Pattern.ResolutionY);
		TestTrue(TEXT("Asset retains all double bits and row order"), ExpectedPhaseBytes(Restored->GetPattern()) == OriginalBytes);
		TestEqual(TEXT("Stored asset revision is independent of the active SLM"), Restored->GetPattern().Revision, uint64(0));
		TestTrue(TEXT("Preview flag survives disk save"), Restored->bIsPreviewPattern);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPhaseSaveGrayImageTest,
	"CGH.PhasePatternSave.PixelAccurateGrayscalePNG",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPhaseSaveGrayImageTest::RunTest(const FString& Parameters)
{
	FSaveTestFiles Files;
	FCGHSLMPhasePattern Pattern;
	Pattern.ResolutionX = 3;
	Pattern.ResolutionY = 2;
	// The asymmetric two-row image exposes mirroring, transposition, gamma conversion,
	// phase wrapping, and row alignment mistakes at an odd image width.
	Pattern.PhaseRad = {0.0, 0.5 * UE_DOUBLE_PI, UE_DOUBLE_PI,
		1.5 * UE_DOUBLE_PI, 2.0 * UE_DOUBLE_PI, -0.5 * UE_DOUBLE_PI};
	const TArray<uint8> Before = ExpectedPhaseBytes(Pattern);
	FCGHPhaseSaveResult Result;
	if (!TestTrue(TEXT("An asymmetric phase image can be saved"), Files.Save(Pattern, Result)))
	{
		AddError(Result.Error);
		return false;
	}
	if (!CheckGrayPNG(*this, Result.ImageFilename, 3, 2, {0, 64, 128, 191, 0, 191}))
	{
		return false;
	}
	TestTrue(TEXT("Quantizing the image does not modify the original phase values"), ExpectedPhaseBytes(Pattern) == Before);
	TArray<uint8> RawBytes;
	TestTrue(TEXT("The unquantized numerical export remains readable"), FFileHelper::LoadFileToArray(RawBytes, *Result.BinaryFilename));
	TestTrue(TEXT("Numerical export preserves exact unwrapped values despite image quantization"), RawBytes == Before);

	Pattern.ResolutionX = 1;
	Pattern.ResolutionY = 1;
	Pattern.PhaseRad = {-2.0 * UE_DOUBLE_PI};
	if (!TestTrue(TEXT("A single-pixel phase image can be saved"), Files.Save(Pattern, Result)))
	{
		AddError(Result.Error);
		return false;
	}
	return CheckGrayPNG(*this, Result.ImageFilename, 1, 1, {0});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPhaseSaveUniqueTest,
	"CGH.PhasePatternSave.RepeatedSavesNeverReplace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPhaseSaveUniqueTest::RunTest(const FString& Parameters)
{
	FSaveTestFiles Files;
	FCGHSLMPhasePattern Pattern = MakeSavePattern();
	FCGHPhaseSaveResult First;
	FCGHPhaseSaveResult Second;
	if (!TestTrue(TEXT("First save succeeds"), Files.Save(Pattern, First))) { AddError(First.Error); return false; }
	const TArray<uint8> OriginalBytes = ExpectedPhaseBytes(Pattern);
	TArray<uint8> OriginalImageBytes;
	TestTrue(TEXT("First PNG can be read before another save"), FFileHelper::LoadFileToArray(OriginalImageBytes, *First.ImageFilename));
	Pattern.PhaseRad[2] = -345.678;
	if (!TestTrue(TEXT("Second save succeeds"), Files.Save(Pattern, Second))) { AddError(Second.Error); return false; }
	TestNotEqual(TEXT("Each click creates a distinct asset"), First.AssetPath, Second.AssetPath);
	TestNotEqual(TEXT("Each click creates distinct numeric files"), First.BinaryFilename, Second.BinaryFilename);
	TestNotEqual(TEXT("Each click creates a distinct PNG image"), First.ImageFilename, Second.ImageFilename);
	TestTrue(TEXT("The second image exists"), IFileManager::Get().FileExists(*Second.ImageFilename));
	TArray<uint8> FirstImageAfterSecondSave;
	TestTrue(TEXT("The first image remains readable"), FFileHelper::LoadFileToArray(FirstImageAfterSecondSave, *First.ImageFilename));
	TestTrue(TEXT("Later saves do not alter an earlier PNG"), FirstImageAfterSecondSave == OriginalImageBytes);
	TestEqual(TEXT("All eight outputs remain with no temporary files"), Files.FileCount(), 8);
	TArray<uint8> FirstBytes;
	TArray<uint8> SecondBytes;
	FFileHelper::LoadFileToArray(FirstBytes, *First.BinaryFilename);
	FFileHelper::LoadFileToArray(SecondBytes, *Second.BinaryFilename);
	TestTrue(TEXT("Previous numeric output remains unchanged"), FirstBytes == OriginalBytes);
	TestTrue(TEXT("The next output captures the changed phase"), SecondBytes == ExpectedPhaseBytes(Pattern));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPhaseSaveRejectedTest,
	"CGH.PhasePatternSave.InvalidInputAndDirectoryFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPhaseSaveRejectedTest::RunTest(const FString& Parameters)
{
	FSaveTestFiles Files;
	FCGHPhaseSaveResult Result;
	const auto ExpectRejected = [&](const FCGHSLMPhasePattern& Pattern, double PitchX, const FString& AssetFolder, const FString& RawFolder)
	{
		Result.AssetPath = TEXT("stale previous result");
		Result.ImageFilename = TEXT("stale previous image");
		TestFalse(TEXT("Unsupported input or destination is rejected"), CGHPhasePatternIO::Save(Pattern, PitchX, 9.e-6,
			AssetFolder, RawFolder, FText::GetEmpty(), false, Result));
		TestFalse(TEXT("Rejected save explains the failure"), Result.Error.IsEmpty());
		TestTrue(TEXT("Rejected save exposes no success paths"), Result.AssetPath.IsEmpty() && Result.AssetFilename.IsEmpty()
			&& Result.BinaryFilename.IsEmpty() && Result.ImageFilename.IsEmpty() && Result.MetadataFilename.IsEmpty());
	};
	ExpectRejected(FCGHSLMPhasePattern(), 8.e-6, Files.AssetFolder, Files.RawFolder);
	FCGHSLMPhasePattern Invalid = MakeSavePattern();
	Invalid.PhaseRad[1] = std::numeric_limits<double>::quiet_NaN();
	ExpectRejected(Invalid, 8.e-6, Files.AssetFolder, Files.RawFolder);
	Invalid = MakeSavePattern();
	Invalid.PhaseRad.Pop();
	ExpectRejected(Invalid, 8.e-6, Files.AssetFolder, Files.RawFolder);
	ExpectRejected(MakeSavePattern(), 0.0, Files.AssetFolder, Files.RawFolder);
	ExpectRejected(MakeSavePattern(), std::numeric_limits<double>::infinity(), Files.AssetFolder, Files.RawFolder);
	ExpectRejected(MakeSavePattern(), 8.e-6, TEXT("/Engine/CGHSaveTest"), Files.RawFolder);
	ExpectRejected(MakeSavePattern(), 8.e-6, TEXT("/Game/../Outside"), Files.RawFolder);
	ExpectRejected(MakeSavePattern(), 8.e-6, Files.AssetFolder, TEXT(""));
	TestEqual(TEXT("Validation failures write no files"), Files.FileCount(), 0);

	IFileManager::Get().MakeDirectory(*Files.RawFolder, true);
	const FString BlockedRawPath = Files.RawFolder / TEXT("keep_existing_file");
	const FString Sentinel(TEXT("Existing user content remains intact."));
	TestTrue(TEXT("Create a file that blocks directory creation"), FFileHelper::SaveStringToFile(Sentinel, *BlockedRawPath));
	ExpectRejected(MakeSavePattern(), 8.e-6, Files.AssetFolder, BlockedRawPath / TEXT("exports"));
	FString Preserved;
	TestTrue(TEXT("The blocking file is still readable"), FFileHelper::LoadFileToString(Preserved, *BlockedRawPath));
	TestEqual(TEXT("Directory failure preserves existing contents"), Preserved, Sentinel);
	TestEqual(TEXT("Directory failure leaves no partial final outputs"), Files.FileCount(), 1);

	IFileManager::Get().MakeDirectory(*Files.ContentFolder, true);
	const FString BlockedAssetPath = Files.ContentFolder / TEXT("keep_asset_folder_file");
	TestTrue(TEXT("Create a file that blocks the asset directory"), FFileHelper::SaveStringToFile(Sentinel, *BlockedAssetPath));
	ExpectRejected(MakeSavePattern(), 8.e-6, Files.AssetFolder / TEXT("keep_asset_folder_file") / TEXT("exports"), Files.RawFolder);
	TestTrue(TEXT("Existing asset-directory blocker remains readable"), FFileHelper::LoadFileToString(Preserved, *BlockedAssetPath));
	TestEqual(TEXT("Existing asset-directory blocker is unchanged"), Preserved, Sentinel);
	TestEqual(TEXT("No partial output is left behind"), Files.FileCount(), 2);
	return true;
}
#endif
