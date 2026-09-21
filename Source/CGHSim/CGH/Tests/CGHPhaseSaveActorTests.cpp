#include "CGH/Actors/CGHSLMActor.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSolverActor.h"
#include "CGH/Actors/CGHTargetActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "CGH/Types/CGHPhasePatternAsset.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"

namespace
{
	/** Every fixture writes only into its own GUID directories, removed on every exit path. */
	struct FCGHPhaseSaveScene : FTestWorldWrapper
	{
		ACGHSLMActor* SLM = nullptr;
		ACGHReconstructionLightActor* Light = nullptr;
		ACGHTargetActor* Target = nullptr;
		ACGHWorkbenchActor* Workbench = nullptr;
		ACGHSolverActor* Solver = nullptr;
		FString AssetDirectory;
		FString ContentDirectory;
		FString RawDirectory;
		TArray<TWeakObjectPtr<UCGHPhasePatternAsset>> SavedAssets;

		~FCGHPhaseSaveScene()
		{
			for (const TWeakObjectPtr<UCGHPhasePatternAsset>& Asset : SavedAssets)
			{
				if (Asset.IsValid())
				{
					FAssetRegistryModule::AssetDeleted(Asset.Get());
					Asset->GetOutermost()->SetDirtyFlag(false);
				}
			}
			if (!ContentDirectory.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*ContentDirectory, false, true);
			}
			if (!RawDirectory.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*RawDirectory, false, true);
			}
		}

		bool Initialize(FAutomationTestBase& Test)
		{
			if (!CreateTestWorld(EWorldType::Editor))
			{
				ForwardErrorMessages(&Test);
				return false;
			}
			SLM = TestWorld->SpawnActor<ACGHSLMActor>();
			Light = TestWorld->SpawnActor<ACGHReconstructionLightActor>();
			Target = TestWorld->SpawnActor<ACGHTargetActor>();
			Workbench = TestWorld->SpawnActor<ACGHWorkbenchActor>();
			Solver = TestWorld->SpawnActor<ACGHSolverActor>();
			if (!SLM || !Light || !Target || !Workbench || !Solver)
			{
				Test.AddError(TEXT("Could not spawn phase save fixture actors."));
				return false;
			}
			const FString UniqueName = TEXT("Save_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			AssetDirectory = TEXT("/Game/CGHSimTests/") + UniqueName;
			ContentDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("CGHSimTests") / UniqueName);
			RawDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/CGHPhaseSave") / UniqueName);
			SLM->PhaseAssetSaveFolder.Path = AssetDirectory;
			SLM->PhaseRawSaveDirectory.Path = RawDirectory;
			SLM->Parameters.ResolutionX = 4;
			SLM->Parameters.ResolutionY = 3;
			SLM->Parameters.PixelPitchXUm = 7.5;
			SLM->Parameters.PixelPitchYUm = 11.5;
			SLM->ClearPhasePattern();
			Target->SetActorLocation(FVector(50.0, 1.5, -2.4));
			Target->Parameters.InitialPhaseRad = 0.73;
			Workbench->SLM = SLM;
			Workbench->ReconstructionLight = Light;
			Workbench->Targets = {Target};
			Workbench->Solver = Solver;
			Solver->Workbench = Workbench;
			return true;
		}

		int32 FileCount() const
		{
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *ContentDirectory, TEXT("*"), true, false);
			IFileManager::Get().FindFilesRecursive(Files, *RawDirectory, TEXT("*"), true, false, false);
			return Files.Num();
		}

		void RememberSavedAsset()
		{
			if (UCGHPhasePatternAsset* Asset = SLM->LastSavedPhaseAsset.Get())
			{
				SavedAssets.AddUnique(Asset);
			}
		}

		bool WaitForReady(FAutomationTestBase& Test)
		{
			const double Deadline = FPlatformTime::Seconds() + 10.0;
			do
			{
				Solver->PollSolver();
				if (Solver->JobState != ECGHSolverJobState::Queued && Solver->JobState != ECGHSolverJobState::Running)
				{
					return Test.TestTrue(*Solver->StatusMessage, Solver->JobState == ECGHSolverJobState::Ready);
				}
				FPlatformProcess::Sleep(0.001f);
			} while (FPlatformTime::Seconds() < Deadline);
			Solver->CancelSolve();
			Test.AddError(TEXT("Timed out waiting for the phase save fixture's CPU calculation."));
			return false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHGeneratedPhaseSaveTest,
	"CGH.PhaseSaveActor.GeneratedPatternSavesBothFormatsAndReloads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHGeneratedPhaseSaveTest::RunTest(const FString& Parameters)
{
	FCGHPhaseSaveScene Scene;
	if (!Scene.Initialize(*this) || !Scene.Solver->StartSolve() || !Scene.WaitForReady(*this))
	{
		return false;
	}
	TestEqual(TEXT("Generating a phase pattern does not save files"), Scene.FileCount(), 0);
	const FCGHSLMPhasePattern Before = Scene.SLM->GetPhasePattern();
	const double* BeforeStorage = Scene.SLM->GetPhasePattern().PhaseRad.GetData();
	const FText BeforeLabel = Scene.SLM->PhasePatternLabel;
	const bool bBeforePreview = Scene.SLM->bIsPreviewPhasePattern;
	const TSoftObjectPtr<UCGHPhasePatternAsset> BeforeStored = Scene.SLM->StoredPhasePattern;
	const FString BeforeSolverStatus = Scene.Solver->StatusMessage;
	// Saving refers to the accepted pattern still on the SLM, even after an unsolved scene edit.
	Scene.Target->AddActorWorldOffset(FVector(0.0, 1.0, 0.0));
	if (!TestTrue(TEXT("Explicit solver save writes the accepted phase after a target edit"), Scene.Solver->SaveGeneratedPhasePattern()))
	{
		AddError(Scene.Solver->PhaseSaveStatus);
		return false;
	}
	Scene.RememberSavedAsset();
	TestFalse(TEXT("Solver reports a save result"), Scene.Solver->PhaseSaveStatus.IsEmpty());
	TestFalse(TEXT("SLM reports a save result"), Scene.SLM->PhaseSaveStatus.IsEmpty());
	TestTrue(TEXT("Saving keeps the ready job state"), Scene.Solver->JobState == ECGHSolverJobState::Ready);
	TestEqual(TEXT("Saving keeps the calculation status"), Scene.Solver->StatusMessage, BeforeSolverStatus);
	TestEqual(TEXT("Saving preserves the active phase revision"), Scene.SLM->GetPhasePatternRevision(), Before.Revision);
	TestTrue(TEXT("Saving preserves exact active samples"), Scene.SLM->GetPhasePattern().PhaseRad == Before.PhaseRad);
	TestTrue(TEXT("Saving preserves the active allocation"), Scene.SLM->GetPhasePattern().PhaseRad.GetData() == BeforeStorage);
	TestTrue(TEXT("Saving preserves the active label"), Scene.SLM->PhasePatternLabel.EqualTo(BeforeLabel));
	TestEqual(TEXT("Saving preserves the preview marker"), Scene.SLM->bIsPreviewPhasePattern, bBeforePreview);
	TestTrue(TEXT("Saving does not replace the selected stored preset"), Scene.SLM->StoredPhasePattern == BeforeStored);

	const TSoftObjectPtr<UCGHPhasePatternAsset> FirstAsset = Scene.SLM->LastSavedPhaseAsset;
	const FString FirstBinary = Scene.SLM->LastSavedPhaseBinaryFile;
	const FString FirstImage = Scene.SLM->LastSavedPhaseImageFile;
	const FString FirstMetadata = Scene.SLM->LastSavedPhaseMetadataFile;
	UCGHPhasePatternAsset* Saved = FirstAsset.LoadSynchronous();
	if (!TestNotNull(TEXT("The saved phase asset can be loaded"), Saved))
	{
		return false;
	}
	TestTrue(TEXT("Asset retains exact row-major samples"), Saved->GetPattern().PhaseRad == Before.PhaseRad);
	TestEqual(TEXT("Asset retains columns"), Saved->GetResolutionX(), Before.ResolutionX);
	TestEqual(TEXT("Asset retains rows"), Saved->GetResolutionY(), Before.ResolutionY);
	TestEqual(TEXT("Saved asset does not persist the SLM publication revision"), Saved->GetPattern().Revision, uint64(0));
	TestFalse(TEXT("Generated asset is not marked as a preview"), Saved->bIsPreviewPattern);
	const FString AssetFile = FPackageName::LongPackageNameToFilename(Saved->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	TestTrue(TEXT("The Unreal asset exists on disk"), IFileManager::Get().FileExists(*AssetFile));
	TestTrue(TEXT("The binary export exists on disk"), IFileManager::Get().FileExists(*FirstBinary));
	TestTrue(TEXT("The grayscale PNG exists on disk"), IFileManager::Get().FileExists(*FirstImage));
	TestEqual(TEXT("The image is saved beside the numerical export"), FPaths::GetPath(FirstImage), FPaths::GetPath(FirstBinary));
	TestTrue(TEXT("The JSON metadata exists on disk"), IFileManager::Get().FileExists(*FirstMetadata));
	TestEqual(TEXT("The raw file holds exactly one float64 per pixel"), IFileManager::Get().FileSize(*FirstBinary), int64(Before.PhaseRad.Num()) * 8);
	TestEqual(TEXT("Binary and asset share a save identity"), FPaths::GetCleanFilename(FirstBinary), Saved->GetName() + TEXT(".bin"));
	TestEqual(TEXT("Image and asset share a save identity"), FPaths::GetCleanFilename(FirstImage), Saved->GetName() + TEXT(".png"));
	TestEqual(TEXT("Metadata and asset share a save identity"), FPaths::GetCleanFilename(FirstMetadata), Saved->GetName() + TEXT(".json"));
	const int32 FirstFileCount = Scene.FileCount();
	TestEqual(TEXT("One save creates an asset, binary, PNG, and metadata file"), FirstFileCount, 4);

	if (!TestTrue(TEXT("Saving the same active result again succeeds"), Scene.Solver->SaveGeneratedPhasePattern()))
	{
		AddError(Scene.Solver->PhaseSaveStatus);
		return false;
	}
	Scene.RememberSavedAsset();
	TestTrue(TEXT("Repeated saves create distinct assets"), Scene.SLM->LastSavedPhaseAsset != FirstAsset);
	TestNotEqual(TEXT("Repeated saves create distinct raw files"), Scene.SLM->LastSavedPhaseBinaryFile, FirstBinary);
	TestNotEqual(TEXT("Repeated saves create distinct grayscale images"), Scene.SLM->LastSavedPhaseImageFile, FirstImage);
	TestEqual(TEXT("Repeated saves preserve the first output set"), Scene.FileCount(), FirstFileCount + 4);
	TestTrue(TEXT("The first exported asset remains available"), IFileManager::Get().FileExists(*AssetFile));
	TestTrue(TEXT("The first exported PNG remains available"), IFileManager::Get().FileExists(*FirstImage));

	const FString ValidAssetDirectory = Scene.SLM->PhaseAssetSaveFolder.Path;
	Scene.SLM->PhaseAssetSaveFolder.Path = TEXT("/InvalidMount/PhaseSave");
	const int32 BeforeFailureFiles = Scene.FileCount();
	TestFalse(TEXT("An invalid asset destination rejects the whole save"), Scene.Solver->SaveGeneratedPhasePattern());
	TestFalse(TEXT("Invalid destinations produce an actionable diagnostic"), Scene.Solver->PhaseSaveStatus.IsEmpty());
	TestEqual(TEXT("A rejected save leaves no additional output files"), Scene.FileCount(), BeforeFailureFiles);
	TestEqual(TEXT("A rejected save preserves the active phase revision"), Scene.SLM->GetPhasePatternRevision(), Before.Revision);
	Scene.SLM->PhaseAssetSaveFolder.Path = ValidAssetDirectory;

	Scene.SLM->StoredPhasePattern = FirstAsset;
	Scene.SLM->ClearPhasePattern();
	Scene.SLM->LoadStoredPhasePattern();
	TestTrue(TEXT("A generated asset can be activated through the existing preset control"), Scene.SLM->HasValidPhasePattern());
	TestTrue(TEXT("Stored activation restores exact generated phases"), Scene.SLM->GetPhasePattern().PhaseRad == Before.PhaseRad);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHGeneratedPhaseSaveGuardTest,
	"CGH.PhaseSaveActor.RequiresReadyUnreplacedSolverPublication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHGeneratedPhaseSaveGuardTest::RunTest(const FString& Parameters)
{
	FCGHPhaseSaveScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	TestFalse(TEXT("An SLM with no phase data cannot be saved"), Scene.SLM->SaveCurrentPhasePattern());
	TestFalse(TEXT("A solver without a generated result cannot save"), Scene.Solver->SaveGeneratedPhasePattern());
	Scene.SLM->GeneratePreviewPhaseRamp();
	TestFalse(TEXT("Preview data does not count as this solver's generated result"), Scene.Solver->SaveGeneratedPhasePattern());
	if (!TestTrue(TEXT("A CPU request starts"), Scene.Solver->StartSolve()))
	{
		return false;
	}
	TestFalse(TEXT("A pending calculation cannot be saved before publication"), Scene.Solver->SaveGeneratedPhasePattern());
	if (!Scene.WaitForReady(*this))
	{
		return false;
	}
	Scene.Workbench->SLM = nullptr;
	TestFalse(TEXT("A changed workbench SLM reference blocks saving an old publication"), Scene.Solver->SaveGeneratedPhasePattern());
	Scene.Workbench->SLM = Scene.SLM;
	Scene.SLM->GeneratePreviewPhaseRamp();
	TestFalse(TEXT("An externally replaced phase cannot be saved under solver ownership"), Scene.Solver->SaveGeneratedPhasePattern());
	TestEqual(TEXT("All rejected save attempts leave no files"), Scene.FileCount(), 0);

	const FCGHSLMPhasePattern Preview = Scene.SLM->GetPhasePattern();
	const FText PreviewLabel = Scene.SLM->PhasePatternLabel;
	const TSoftObjectPtr<UCGHPhasePatternAsset> BeforeStored = Scene.SLM->StoredPhasePattern;
	if (!TestTrue(TEXT("The SLM can explicitly save its current preview independently"), Scene.SLM->SaveCurrentPhasePattern()))
	{
		AddError(Scene.SLM->PhaseSaveStatus);
		return false;
	}
	Scene.RememberSavedAsset();
	UCGHPhasePatternAsset* PreviewAsset = Scene.SLM->LastSavedPhaseAsset.LoadSynchronous();
	if (TestNotNull(TEXT("The explicitly saved preview asset exists"), PreviewAsset))
	{
		TestTrue(TEXT("Saved preview retains its label"), PreviewAsset->PatternLabel.EqualTo(PreviewLabel));
		TestTrue(TEXT("Saved preview retains its preview provenance"), PreviewAsset->bIsPreviewPattern);
		TestTrue(TEXT("Saved preview retains exact samples"), PreviewAsset->GetPattern().PhaseRad == Preview.PhaseRad);
	}
	TestTrue(TEXT("SLM saving preserves the selected preset"), Scene.SLM->StoredPhasePattern == BeforeStored);
	TestEqual(TEXT("SLM saving preserves the active revision"), Scene.SLM->GetPhasePatternRevision(), Preview.Revision);
	TestTrue(TEXT("SLM saving preserves the active preview label"), Scene.SLM->PhasePatternLabel.EqualTo(PreviewLabel));
	TestTrue(TEXT("SLM saving preserves the active preview flag"), Scene.SLM->bIsPreviewPhasePattern);
	const int32 SavedFiles = Scene.FileCount();
	TestEqual(TEXT("Explicit preview saving writes all four formats"), SavedFiles, 4);
	TestTrue(TEXT("Explicit preview saving reports its image path"), IFileManager::Get().FileExists(*Scene.SLM->LastSavedPhaseImageFile));

	if (!Scene.Solver->StartSolve() || !Scene.WaitForReady(*this))
	{
		return false;
	}
	Scene.SLM->ClearPhasePattern();
	TestFalse(TEXT("Clearing a generated pattern invalidates its solver save guard"), Scene.Solver->SaveGeneratedPhasePattern());
	if (!Scene.Solver->StartSolve() || !Scene.WaitForReady(*this))
	{
		return false;
	}
	Scene.Solver->Parameters.SolverBackend = static_cast<ECGHSolverBackend>(255);
	TestFalse(TEXT("An unsupported new request fails"), Scene.Solver->StartSolve());
	TestFalse(TEXT("Failed jobs cannot save a retained previous pattern through the solver"), Scene.Solver->SaveGeneratedPhasePattern());
	TestEqual(TEXT("Later rejected saves leave previous output files intact"), Scene.FileCount(), SavedFiles);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHAutomaticPhaseNoSaveTest,
	"CGH.PhaseSaveActor.AutomaticGenerationStillRequiresExplicitSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHAutomaticPhaseNoSaveTest::RunTest(const FString& Parameters)
{
	FCGHPhaseSaveScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	FString RelativeRawDirectory = Scene.RawDirectory;
	if (!TestTrue(TEXT("The fixture can express its raw destination relative to the project"),
		FPaths::MakePathRelativeTo(RelativeRawDirectory, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()))))
	{
		return false;
	}
	TestTrue(TEXT("The fixture exercises a project-relative raw destination"), FPaths::IsRelative(RelativeRawDirectory));
	Scene.SLM->PhaseRawSaveDirectory.Path = RelativeRawDirectory;
	Scene.Solver->bAutoSolve = true;
	if (!Scene.WaitForReady(*this))
	{
		return false;
	}
	const uint64 FirstRevision = Scene.SLM->GetPhasePatternRevision();
	TestEqual(TEXT("The initial automatic solve creates no files"), Scene.FileCount(), 0);
	Scene.Target->Parameters.InitialPhaseRad += 0.25;
	if (!Scene.WaitForReady(*this))
	{
		return false;
	}
	TestTrue(TEXT("Automatic optical changes publish a fresh pattern"), Scene.SLM->GetPhasePatternRevision() > FirstRevision);
	TestEqual(TEXT("Repeated automatic solves create no files"), Scene.FileCount(), 0);
	Scene.Solver->SavePhasePattern();
	Scene.RememberSavedAsset();
	if (!TestEqual(TEXT("The solver's editor button explicitly saves all four files"), Scene.FileCount(), 4))
	{
		AddError(FString::Printf(TEXT("Save status: %s; configured raw directory: %s; expected raw directory: %s; reported binary: %s; reported image: %s"),
			*Scene.Solver->PhaseSaveStatus, *Scene.SLM->PhaseRawSaveDirectory.Path,
			*Scene.RawDirectory, *Scene.SLM->LastSavedPhaseBinaryFile, *Scene.SLM->LastSavedPhaseImageFile));
		return false;
	}
	TestFalse(TEXT("The save button reports its result"), Scene.Solver->PhaseSaveStatus.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
