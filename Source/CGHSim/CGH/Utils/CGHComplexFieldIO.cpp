#include "CGH/Utils/CGHComplexFieldIO.h"

#if WITH_EDITOR
#include "CGH/Types/CGHComplexFieldAsset.h"
#include "CGH/Utils/CGHComplexFieldPreview.h"
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

namespace CGHComplexFieldIOPrivate
{
	// UE's generic file writer ignores FILEWRITE_NoReplaceExisting on some platforms.
	// Exclusive creation protects existing files even if they appear after path validation.
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

	bool WriteField(const FString& Filename, const FCGHComplexField& Field, TArray<FString>& OwnedFiles)
	{
		TUniquePtr<FArchive> Writer = CreateOwnedWriter(Filename, OwnedFiles);
		if (!Writer)
		{
			return false;
		}
		static_assert(sizeof(double) == sizeof(uint64), "Complex export requires 64-bit doubles.");
		// Serialize components explicitly, without depending on USTRUCT padding or host byte order.
		// The bounded buffer also avoids another full-field allocation for large reconstructions.
		uint8 Bytes[16384];
		for (int32 Offset = 0; Offset < Field.Samples.Num() && !Writer->IsError();)
		{
			const int32 Count = FMath::Min(1024, Field.Samples.Num() - Offset);
			for (int32 I = 0; I < Count; ++I)
			{
				const FCGHComplexSample& Sample = Field.Samples[Offset + I];
				const double Components[] = {Sample.Real, Sample.Imaginary};
				for (int32 Component = 0; Component < 2; ++Component)
				{
					uint64 Bits;
					FMemory::Memcpy(&Bits, &Components[Component], sizeof(Bits));
					for (int32 Byte = 0; Byte < 8; ++Byte)
					{
						Bytes[I * 16 + Component * 8 + Byte] = static_cast<uint8>(Bits >> (Byte * 8));
					}
				}
			}
			Writer->Serialize(Bytes, Count * 16);
			Offset += Count;
		}
		return FinishWrite(Writer);
	}

	bool WriteGrayscalePng(const FString& Filename, const FCGHComplexField& Field,
		ECGHObserverPreviewMode Mode, TArray<FString>& OwnedFiles)
	{
		TArray<FColor> Pixels;
		FString Error;
		if (!CGHComplexFieldPreview::BuildGrayscale(Field, Mode, Pixels, Error))
		{
			return false;
		}
		// The shared preview mapping sets R=G=B. Encode G8 directly without resampling or gamma.
		TArray<uint8> Gray;
		Gray.SetNumUninitialized(Pixels.Num());
		for (int32 Index = 0; Index < Pixels.Num(); ++Index)
		{
			Gray[Index] = Pixels[Index].R;
		}
		Pixels.Empty();
		const FImageView Image(Gray.GetData(), Field.ResolutionX, Field.ResolutionY, 1,
			ERawImageFormat::G8, EGammaSpace::Linear);
		TArray64<uint8> Png;
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

bool CGHComplexFieldIO::Save(const FCGHComplexField& Field, double PixelPitchXM, double PixelPitchYM,
	const FString& AssetFolder, const FString& RawDirectory, const FText& Label,
	FCGHComplexFieldSaveResult& Out)
{
	Out = FCGHComplexFieldSaveResult();
#if !WITH_EDITOR
	Out.Error = TEXT("Saving complex-field assets is available in the Unreal Editor only.");
	return false;
#else
	if (!IsInGameThread())
	{
		Out.Error = TEXT("Complex-field saving must run on the game thread.");
		return false;
	}
	if (!Field.IsValid(&Out.Error))
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
		|| !FPackageName::IsValidLongPackageName(Folder + TEXT("/ObserverField"), false, &Reason))
	{
		Out.Error = FString::Printf(TEXT("Asset folder must be a valid /Game content folder: '%s'. %s"), *AssetFolder, *Reason.ToString());
		return false;
	}
	if (RawDirectory.TrimStartAndEnd().IsEmpty())
	{
		Out.Error = TEXT("Choose a nonempty raw export directory.");
		return false;
	}
	FString RawFolder = FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), RawDirectory);
	FPaths::NormalizeDirectoryName(RawFolder);
	if (!FPaths::CollapseRelativeDirectories(RawFolder) || !FPaths::ValidatePath(RawFolder, &Reason))
	{
		Out.Error = FString::Printf(TEXT("Invalid raw export directory '%s': %s"), *RawDirectory, *Reason.ToString());
		return false;
	}

	FCGHComplexFieldSaveResult Result;
	FString Name;
	FString PackageName;
	FString StagingDirectory;
	IFileManager& Files = IFileManager::Get();
	for (int32 Attempt = 0; Attempt < 8; ++Attempt)
	{
		Name = TEXT("ObserverField_") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"))
			+ TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
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
		Result.PhaseImageFilename = RawFolder / (Name + TEXT("_phase.png"));
		Result.AmplitudeImageFilename = RawFolder / (Name + TEXT("_amplitude.png"));
		Result.IntensityImageFilename = RawFolder / (Name + TEXT("_intensity.png"));
		StagingDirectory = RawFolder / (TEXT(".") + Name + TEXT("_staging"));
		if (!FindPackage(nullptr, *PackageName) && !FPackageName::DoesPackageExist(PackageName)
			&& !Files.FileExists(*Result.AssetFilename) && !Files.FileExists(*Result.BinaryFilename)
			&& !Files.FileExists(*Result.MetadataFilename) && !Files.FileExists(*Result.PhaseImageFilename)
			&& !Files.FileExists(*Result.AmplitudeImageFilename) && !Files.FileExists(*Result.IntensityImageFilename)
			&& !Files.DirectoryExists(*StagingDirectory) && !Files.FileExists(*StagingDirectory))
		{
			break;
		}
		if (Attempt == 7)
		{
			Out.Error = TEXT("Could not allocate a unique complex-field filename.");
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
	UCGHComplexFieldAsset* Asset = nullptr;
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
	Metadata->SetStringField(TEXT("phase_image_filename"), FPaths::GetCleanFilename(Result.PhaseImageFilename));
	Metadata->SetStringField(TEXT("amplitude_image_filename"), FPaths::GetCleanFilename(Result.AmplitudeImageFilename));
	Metadata->SetStringField(TEXT("intensity_image_filename"), FPaths::GetCleanFilename(Result.IntensityImageFilename));
	Metadata->SetStringField(TEXT("image_format"), TEXT("PNG"));
	Metadata->SetNumberField(TEXT("image_bit_depth"), 8);
	Metadata->SetStringField(TEXT("phase_image_mapping"), TEXT("round(255 * wrap_0_2pi(atan2(imaginary, real)) / (2*pi)); zero samples map to 0"));
	Metadata->SetStringField(TEXT("amplitude_image_mapping"), TEXT("round(255 * abs(sample) / max(abs(field))); zero field maps to 0"));
	Metadata->SetStringField(TEXT("intensity_image_mapping"), TEXT("round(255 * abs(sample)^2 / max(abs(field)^2)); zero field maps to 0"));
	Metadata->SetStringField(TEXT("image_row_order"), TEXT("top-to-bottom"));
	Metadata->SetNumberField(TEXT("resolution_x"), Field.ResolutionX);
	Metadata->SetNumberField(TEXT("resolution_y"), Field.ResolutionY);
	Metadata->SetStringField(TEXT("phase_unit"), TEXT("radians"));
	Metadata->SetStringField(TEXT("dtype"), TEXT("complex128"));
	Metadata->SetStringField(TEXT("component_dtype"), TEXT("float64"));
	Metadata->SetStringField(TEXT("component_order"), TEXT("real,imaginary"));
	Metadata->SetStringField(TEXT("interleaving"), TEXT("real,imaginary"));
	Metadata->SetStringField(TEXT("endianness"), TEXT("little"));
	Metadata->SetStringField(TEXT("array_order"), TEXT("row-major"));
	Metadata->SetStringField(TEXT("index"), TEXT("row * resolution_x + column"));
	Metadata->SetNumberField(TEXT("pixel_pitch_x_m"), PixelPitchXM);
	Metadata->SetNumberField(TEXT("pixel_pitch_y_m"), PixelPitchYM);
	Metadata->SetStringField(TEXT("coordinate_frame"), TEXT("observer-local"));
	Metadata->SetStringField(TEXT("pixel_center_convention"), TEXT("centered"));
	Metadata->SetStringField(TEXT("optical_normal_observer"), TEXT("+X"));
	Metadata->SetNumberField(TEXT("pixel_plane_x_m"), 0.0);
	Metadata->SetStringField(TEXT("column_direction_observer"), TEXT("+Y"));
	Metadata->SetStringField(TEXT("row_direction_observer"), TEXT("-Z"));
	Metadata->SetStringField(TEXT("field_label"), Label.ToString());
	FString Json;
	if (!FJsonSerializer::Serialize(Metadata, TJsonWriterFactory<>::Create(&Json)))
	{
		Out.Error = TEXT("Cannot serialize complex-field metadata.");
		return false;
	}
	const FString StagedBinary = StagingDirectory / (Name + TEXT(".bin"));
	const FString StagedMetadata = StagingDirectory / (Name + TEXT(".json"));
	const FString StagedAsset = StagingDirectory / (Name + TEXT(".uasset"));
	const FString StagedPhaseImage = StagingDirectory / (Name + TEXT("_phase.png"));
	const FString StagedAmplitudeImage = StagingDirectory / (Name + TEXT("_amplitude.png"));
	const FString StagedIntensityImage = StagingDirectory / (Name + TEXT("_intensity.png"));
	if (!CGHComplexFieldIOPrivate::WriteField(StagedBinary, Field, OwnedFiles))
	{
		Out.Error = FString::Printf(TEXT("Cannot write complex samples to '%s'."), *StagedBinary);
		return false;
	}
	if (!CGHComplexFieldIOPrivate::WriteText(StagedMetadata, Json, OwnedFiles))
	{
		Out.Error = FString::Printf(TEXT("Cannot write complex-field metadata to '%s'."), *StagedMetadata);
		return false;
	}
	if (!CGHComplexFieldIOPrivate::WriteGrayscalePng(StagedPhaseImage, Field, ECGHObserverPreviewMode::Phase, OwnedFiles))
	{
		Out.Error = FString::Printf(TEXT("Cannot encode or write the phase grayscale PNG '%s'."), *StagedPhaseImage);
		return false;
	}
	if (!CGHComplexFieldIOPrivate::WriteGrayscalePng(StagedAmplitudeImage, Field, ECGHObserverPreviewMode::Amplitude, OwnedFiles))
	{
		Out.Error = FString::Printf(TEXT("Cannot encode or write the amplitude grayscale PNG '%s'."), *StagedAmplitudeImage);
		return false;
	}

	if (!CGHComplexFieldIOPrivate::WriteGrayscalePng(StagedIntensityImage, Field, ECGHObserverPreviewMode::Intensity, OwnedFiles))
	{
		Out.Error = FString::Printf(TEXT("Cannot encode or write the intensity grayscale PNG '%s'."), *StagedIntensityImage);
		return false;
	}

	Package = CreatePackage(*PackageName);
	Asset = NewObject<UCGHComplexFieldAsset>(Package, FName(*Name), RF_Public | RF_Standalone);
	Asset->FieldLabel = Label;
	if (!Asset->SetField(Field, PixelPitchXM, PixelPitchYM))
	{
		Out.Error = Asset->GetValidationError();
		return false;
	}
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	SaveArgs.Error = GWarn;
	SaveArgs.bSlowTask = false;
	OwnedFiles.Add(StagedAsset);
	if (!UPackage::SavePackage(Package, Asset, *StagedAsset, SaveArgs))
	{
		Out.Error = FString::Printf(TEXT("Cannot save complex-field asset '%s'."), *StagedAsset);
		return false;
	}
	for (const TPair<FString, FString>& Paths : {TPair<FString, FString>(StagedBinary, Result.BinaryFilename),
		TPair<FString, FString>(StagedMetadata, Result.MetadataFilename),
		TPair<FString, FString>(StagedPhaseImage, Result.PhaseImageFilename),
		TPair<FString, FString>(StagedAmplitudeImage, Result.AmplitudeImageFilename),
		TPair<FString, FString>(StagedIntensityImage, Result.IntensityImageFilename),
		TPair<FString, FString>(StagedAsset, Result.AssetFilename)})
	{
		if (!CGHComplexFieldIOPrivate::PromoteFile(Paths.Key, Paths.Value, OwnedFiles))
		{
			Out.Error = FString::Printf(TEXT("Cannot finalize complex-field output '%s'; existing files are never replaced."), *Paths.Value);
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
