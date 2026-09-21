#include "CGH/Utils/CGHPhasePatternIO.h"

#if WITH_EDITOR
#include "CGH/Types/CGHPhasePatternAsset.h"
#include "CGH/Utils/CGHPhasePreview.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include <cstdio>

namespace
{
	// UE's generic file writer currently ignores FILEWRITE_NoReplaceExisting. C11's x mode
	// provides exclusive creation, including when another process creates the path after validation.
	class FExclusiveFileWriter final : public FArchive
	{
	public:
		explicit FExclusiveFileWriter(std::FILE* InFile) : File(InFile) { SetIsSaving(true); }
		virtual ~FExclusiveFileWriter() override { Close(); }
		virtual void Serialize(void* Data, int64 Length) override
		{
			if (!File || Length < 0 || std::fwrite(Data, 1, static_cast<size_t>(Length), File) != static_cast<size_t>(Length))
			{
				SetError();
			}
		}
		virtual bool Close() override
		{
			if (File)
			{
				if (std::fclose(File) != 0) { SetError(); }
				File = nullptr;
			}
			return !IsError();
		}
	private:
		std::FILE* File = nullptr;
	};

	// Owning the writer means this call created the file. Never add a pre-existing path here.
	TUniquePtr<FArchive> CreateOwnedWriter(const FString& Filename, TArray<FString>& OwnedFiles)
	{
#if PLATFORM_WINDOWS
		std::FILE* File = _wfopen(*Filename, TEXT("wbx"));
#else
		std::FILE* File = std::fopen(TCHAR_TO_UTF8(*Filename), "wbx");
#endif
		if (!File) { return nullptr; }
		OwnedFiles.Add(Filename);
		return MakeUnique<FExclusiveFileWriter>(File);
	}

	bool FinishWrite(TUniquePtr<FArchive>& Writer)
	{
		const bool bClosed = Writer->Close();
		return bClosed && !Writer->IsError();
	}

	bool WritePhase(const FString& Filename, const FCGHSLMPhasePattern& Pattern, TArray<FString>& OwnedFiles)
	{
		TUniquePtr<FArchive> Writer = CreateOwnedWriter(Filename, OwnedFiles);
		if (!Writer)
		{
			return false;
		}
#if PLATFORM_LITTLE_ENDIAN
		Writer->Serialize(const_cast<double*>(Pattern.PhaseRad.GetData()), static_cast<int64>(Pattern.PhaseRad.Num()) * sizeof(double));
#else
		// Explicit IEEE-754 byte order also preserves signed zero and all finite values exactly.
		uint8 Bytes[8192];
		for (int32 Offset = 0; Offset < Pattern.PhaseRad.Num();)
		{
			const int32 Count = FMath::Min(1024, Pattern.PhaseRad.Num() - Offset);
			for (int32 I = 0; I < Count; ++I)
			{
				uint64 Bits;
				FMemory::Memcpy(&Bits, &Pattern.PhaseRad[Offset + I], sizeof(Bits));
				for (int32 Byte = 0; Byte < 8; ++Byte)
				{
					Bytes[I * 8 + Byte] = static_cast<uint8>(Bits >> (Byte * 8));
				}
			}
			Writer->Serialize(Bytes, Count * 8);
			Offset += Count;
		}
#endif
		return FinishWrite(Writer);
	}

	bool WriteGrayscalePng(const FString& Filename, const FCGHSLMPhasePattern& Pattern, TArray<FString>& OwnedFiles)
	{
		// Encode the phase grid directly: one byte per SLM pixel, row zero first, no resampling.
		TArray<uint8> Gray;
		Gray.SetNumUninitialized(Pattern.PhaseRad.Num());
		for (int32 Index = 0; Index < Gray.Num(); ++Index)
		{
			Gray[Index] = CGHPhasePreview::PhaseToGray(Pattern.PhaseRad[Index]);
		}
		const FImageView Image(Gray.GetData(), Pattern.ResolutionX, Pattern.ResolutionY, 1,
			ERawImageFormat::G8, EGammaSpace::Linear);
		TArray64<uint8> Png;
		// G8 PNG compression preserves these bytes; no display gamma or float conversion is applied.
		if (!FImageUtils::CompressImage(Png, TEXT("png"), Image) || Png.IsEmpty())
		{
			return false;
		}
		TUniquePtr<FArchive> Writer = CreateOwnedWriter(Filename, OwnedFiles);
		if (!Writer)
		{
			return false;
		}
		Writer->Serialize(Png.GetData(), Png.Num());
		return FinishWrite(Writer);
	}

	bool WriteText(const FString& Filename, const FString& Text, TArray<FString>& OwnedFiles)
	{
		TUniquePtr<FArchive> Writer = CreateOwnedWriter(Filename, OwnedFiles);
		if (!Writer)
		{
			return false;
		}
		FTCHARToUTF8 Utf8(*Text);
		Writer->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
		return FinishWrite(Writer);
	}

	// A bounded copy avoids platform-specific MoveFile replacement behavior and extra full-grid RAM.
	bool PromoteFile(const FString& Source, const FString& Destination, TArray<FString>& OwnedFiles)
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Source));
		if (!Reader)
		{
			return false;
		}
		TUniquePtr<FArchive> Writer = CreateOwnedWriter(Destination, OwnedFiles);
		if (!Writer)
		{
			return false;
		}
		uint8 Bytes[65536];
		for (int64 Remaining = Reader->TotalSize(); Remaining > 0 && !Reader->IsError() && !Writer->IsError();)
		{
			const int64 Count = FMath::Min<int64>(Remaining, sizeof(Bytes));
			Reader->Serialize(Bytes, Count);
			if (!Reader->IsError())
			{
				Writer->Serialize(Bytes, Count);
			}
			Remaining -= Count;
		}
		const bool bRead = Reader->Close() && !Reader->IsError();
		const bool bWritten = FinishWrite(Writer);
		return bRead && bWritten;
	}
}
#endif

bool CGHPhasePatternIO::Save(const FCGHSLMPhasePattern& Pattern, double PixelPitchXM, double PixelPitchYM,
	const FString& AssetFolder, const FString& RawDirectory, const FText& Label,
	bool bIsPreviewPattern, FCGHPhaseSaveResult& Out)
{
	Out = FCGHPhaseSaveResult();
#if !WITH_EDITOR
	Out.Error = TEXT("Saving phase-pattern assets is available in the Unreal Editor only.");
	return false;
#else
	if (!IsInGameThread())
	{
		Out.Error = TEXT("Phase-pattern saving must run on the game thread.");
		return false;
	}
	if (!CGHPhasePreview::ValidatePattern(Pattern, Pattern.ResolutionX, Pattern.ResolutionY, Out.Error))
	{
		return false;
	}
	if (!FMath::IsFinite(PixelPitchXM) || !FMath::IsFinite(PixelPitchYM) || PixelPitchXM <= 0.0 || PixelPitchYM <= 0.0)
	{
		Out.Error = TEXT("Pixel pitches must be finite, positive values in meters.");
		return false;
	}
	FString Folder = AssetFolder.TrimStartAndEnd();
	Folder.RemoveFromEnd(TEXT("/"));
	FText Reason;
	if ((Folder != TEXT("/Game") && !Folder.StartsWith(TEXT("/Game/")))
		|| !FPackageName::IsValidLongPackageName(Folder + TEXT("/PhasePattern"), false, &Reason))
	{
		Out.Error = FString::Printf(TEXT("Asset folder must be a valid /Game content folder: '%s'. %s"), *AssetFolder, *Reason.ToString());
		return false;
	}
	if (RawDirectory.TrimStartAndEnd().IsEmpty())
	{
		Out.Error = TEXT("Choose a nonempty raw export directory.");
		return false;
	}
	// ProjectDir can itself be relative to the engine binaries directory. Resolve it once first.
	FString RawFolder = FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), RawDirectory);
	FPaths::NormalizeDirectoryName(RawFolder);
	if (!FPaths::CollapseRelativeDirectories(RawFolder) || !FPaths::ValidatePath(RawFolder, &Reason))
	{
		Out.Error = FString::Printf(TEXT("Invalid raw export directory '%s': %s"), *RawDirectory, *Reason.ToString());
		return false;
	}

	FCGHPhaseSaveResult Result;
	FString Name;
	FString PackageName;
	FString StagingDirectory;
	IFileManager& Files = IFileManager::Get();
	for (int32 Attempt = 0; Attempt < 8; ++Attempt)
	{
		Name = TEXT("Phase_") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		PackageName = Folder / Name;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Result.AssetFilename, FPackageName::GetAssetPackageExtension()))
		{
			Out.Error = FString::Printf(TEXT("Cannot resolve asset package '%s' to a content filename."), *PackageName);
			return false;
		}
		Result.AssetFilename = FPaths::ConvertRelativePathToFull(Result.AssetFilename);
		if (!FPaths::IsUnderDirectory(Result.AssetFilename, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())))
		{
			Out.Error = FString::Printf(TEXT("Asset folder must resolve inside this project's Content directory: '%s'."), *AssetFolder);
			return false;
		}
		Result.AssetPath = PackageName + TEXT(".") + Name;
		Result.BinaryFilename = RawFolder / (Name + TEXT(".bin"));
		Result.MetadataFilename = RawFolder / (Name + TEXT(".json"));
		Result.ImageFilename = RawFolder / (Name + TEXT(".png"));
		StagingDirectory = RawFolder / (TEXT(".") + Name + TEXT("_staging"));
		if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName)
			&& !Files.FileExists(*Result.AssetFilename) && !Files.FileExists(*Result.BinaryFilename)
			&& !Files.FileExists(*Result.MetadataFilename) && !Files.FileExists(*Result.ImageFilename)
			&& !Files.DirectoryExists(*StagingDirectory)
			&& !Files.FileExists(*StagingDirectory))
		{
			break;
		}
		if (Attempt == 7)
		{
			Out.Error = TEXT("Could not allocate a unique phase-pattern filename.");
			return false;
		}
	}

	if (!Files.MakeDirectory(*FPaths::GetPath(Result.AssetFilename), true))
	{
		Out.Error = FString::Printf(TEXT("Cannot create asset directory '%s'."), *FPaths::GetPath(Result.AssetFilename));
		return false;
	}
	if (!Files.MakeDirectory(*RawFolder, true) || !Files.MakeDirectory(*StagingDirectory, false))
	{
		Out.Error = FString::Printf(TEXT("Cannot create raw export directory or staging directory under '%s'."), *RawFolder);
		return false;
	}

	bool bSuccess = false;
	UPackage* Package = nullptr;
	UCGHPhasePatternAsset* Asset = nullptr;
	TArray<FString> OwnedFiles;
	ON_SCOPE_EXIT
	{
		for (const FString& Filename : OwnedFiles)
		{
			if (!bSuccess || FPaths::IsUnderDirectory(Filename, StagingDirectory))
			{
				if (!Files.Delete(*Filename, false, false, true))
				{
					Out.Error += FString::Printf(TEXT(" Could not remove temporary output '%s'."), *Filename);
				}
			}
		}
		Files.DeleteDirectory(*StagingDirectory, false, false);
		if (!bSuccess && Package)
		{
			if (Asset)
			{
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				FAssetRegistryModule::AssetDeleted(Asset);
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->MarkAsGarbage();
			}
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	};

	const TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
	Metadata->SetNumberField(TEXT("format_version"), 1);
	Metadata->SetStringField(TEXT("asset_path"), Result.AssetPath);
	Metadata->SetStringField(TEXT("binary_filename"), FPaths::GetCleanFilename(Result.BinaryFilename));
	Metadata->SetStringField(TEXT("image_filename"), FPaths::GetCleanFilename(Result.ImageFilename));
	Metadata->SetStringField(TEXT("image_format"), TEXT("PNG"));
	Metadata->SetNumberField(TEXT("image_bit_depth"), 8);
	Metadata->SetStringField(TEXT("image_mapping"), TEXT("round(255 * wrap_0_2pi(phase_rad) / (2*pi))"));
	Metadata->SetStringField(TEXT("image_row_order"), TEXT("top-to-bottom"));
	Metadata->SetNumberField(TEXT("resolution_x"), Pattern.ResolutionX);
	Metadata->SetNumberField(TEXT("resolution_y"), Pattern.ResolutionY);
	Metadata->SetStringField(TEXT("phase_unit"), TEXT("radians"));
	Metadata->SetStringField(TEXT("dtype"), TEXT("float64"));
	Metadata->SetStringField(TEXT("endianness"), TEXT("little"));
	Metadata->SetStringField(TEXT("array_order"), TEXT("row-major"));
	Metadata->SetStringField(TEXT("index"), TEXT("row * resolution_x + column"));
	Metadata->SetNumberField(TEXT("pixel_pitch_x_m"), PixelPitchXM);
	Metadata->SetNumberField(TEXT("pixel_pitch_y_m"), PixelPitchYM);
	Metadata->SetStringField(TEXT("coordinate_frame"), TEXT("SLM-local"));
	Metadata->SetStringField(TEXT("pixel_center_convention"), TEXT("centered"));
	Metadata->SetStringField(TEXT("optical_normal_slm"), TEXT("+X"));
	Metadata->SetNumberField(TEXT("pixel_plane_x_m"), 0.0);
	Metadata->SetStringField(TEXT("column_direction_slm"), TEXT("+Y"));
	Metadata->SetStringField(TEXT("row_direction_slm"), TEXT("-Z"));
	Metadata->SetStringField(TEXT("pattern_label"), Label.ToString());
	Metadata->SetBoolField(TEXT("is_preview_pattern"), bIsPreviewPattern);
	FString Json;
	if (!FJsonSerializer::Serialize(Metadata, TJsonWriterFactory<>::Create(&Json)))
	{
		Out.Error = TEXT("Cannot serialize phase-pattern metadata.");
		return false;
	}
	const FString StagedBinary = StagingDirectory / (Name + TEXT(".bin"));
	const FString StagedMetadata = StagingDirectory / (Name + TEXT(".json"));
	const FString StagedAsset = StagingDirectory / (Name + TEXT(".uasset"));
	const FString StagedImage = StagingDirectory / (Name + TEXT(".png"));
	if (!WritePhase(StagedBinary, Pattern, OwnedFiles))
	{
		Out.Error = FString::Printf(TEXT("Cannot write phase samples to '%s'."), *StagedBinary);
		return false;
	}
	if (!WriteText(StagedMetadata, Json, OwnedFiles))
	{
		Out.Error = FString::Printf(TEXT("Cannot write phase metadata to '%s'."), *StagedMetadata);
		return false;
	}

	if (!WriteGrayscalePng(StagedImage, Pattern, OwnedFiles))
	{
		Out.Error = FString::Printf(TEXT("Cannot encode or write the phase grayscale PNG '%s'."), *StagedImage);
		return false;
	}

	Package = CreatePackage(*PackageName);
	Asset = NewObject<UCGHPhasePatternAsset>(Package, FName(*Name), RF_Public | RF_Standalone);
	Asset->PatternLabel = Label;
	Asset->bIsPreviewPattern = bIsPreviewPattern;
	if (!Asset->SetPattern(Pattern))
	{
		Out.Error = Asset->GetValidationError();
		return false;
	}
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	SaveArgs.Error = GWarn;
	SaveArgs.bSlowTask = false;
	// The staging directory is unique to this call, so a failed save may be cleaned safely.
	OwnedFiles.Add(StagedAsset);
	if (!UPackage::SavePackage(Package, Asset, *StagedAsset, SaveArgs))
	{
		Out.Error = FString::Printf(TEXT("Cannot save phase-pattern asset '%s'."), *StagedAsset);
		return false;
	}
	for (const TPair<FString, FString>& Paths : {TPair<FString, FString>(StagedBinary, Result.BinaryFilename),
		TPair<FString, FString>(StagedMetadata, Result.MetadataFilename), TPair<FString, FString>(StagedImage, Result.ImageFilename),
		TPair<FString, FString>(StagedAsset, Result.AssetFilename)})
	{
		if (!PromoteFile(Paths.Key, Paths.Value, OwnedFiles))
		{
			Out.Error = FString::Printf(TEXT("Cannot finalize phase-pattern output '%s'; existing files are never replaced."), *Paths.Value);
			return false;
		}
	}
	Package->SetLoadedPath(FPackagePath::FromPackageNameChecked(PackageName));
	Package->SetDirtyFlag(false);
	FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	FAssetRegistryModule::AssetCreated(Asset);
	bSuccess = true;
	Out = MoveTemp(Result);
	return true;
#endif
}
