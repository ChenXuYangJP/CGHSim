#include "CGH/Actors/CGHWorkbenchActor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Actors/CGHCameraActor.h"
#include "CGH/Actors/CGHObserverPlaneActor.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHReconstructorActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHTargetActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

namespace
{
	struct FCGHReconstructionWorkbenchFixture : FTestWorldWrapper
	{
		ACGHWorkbenchActor* Workbench = nullptr;
		ACGHSLMActor* SLM = nullptr;
		ACGHReconstructionLightActor* Light = nullptr;
		ACGHObserverPlaneActor* Observer = nullptr;

		bool Initialize(FAutomationTestBase& Test)
		{
			if (!CreateTestWorld(EWorldType::Game))
			{
				ForwardErrorMessages(&Test);
				return false;
			}
			Workbench = GetTestWorld()->SpawnActor<ACGHWorkbenchActor>();
			SLM = GetTestWorld()->SpawnActor<ACGHSLMActor>();
			Light = GetTestWorld()->SpawnActor<ACGHReconstructionLightActor>();
			Observer = GetTestWorld()->SpawnActor<ACGHObserverPlaneActor>();
			if (!Workbench || !SLM || !Light || !Observer)
			{
				Test.AddError(TEXT("Could not create reconstruction workbench fixture."));
				return false;
			}
			Workbench->SLM = SLM;
			Workbench->ReconstructionLight = Light;
			Workbench->ObserverPlane = Observer;
			Observer->SetActorLocation(FVector(50.0, 0.0, 0.0));
			return true;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructionWorkbenchSnapshotTest,
	"CGH.Reconstruction.Workbench.OpticalSnapshotAndPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCGHReconstructionWorkbenchSnapshotTest::RunTest(const FString& Parameters)
{
	FCGHReconstructionWorkbenchFixture Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	Scene.SLM->Parameters.ResolutionX = 7;
	Scene.SLM->Parameters.ResolutionY = 5;
	Scene.SLM->Parameters.PixelPitchXUm = 9.0;
	Scene.SLM->SetActorLocationAndRotation(FVector(120.0, -75.0, 31.0), FRotator(17.0, 63.0, -9.0));
	Scene.Light->Parameters.WavelengthNm = 632.8;
	Scene.Light->Parameters.Amplitude = 2.5;
	Scene.Light->SetActorRotation(Scene.SLM->GetActorRotation());
	const FVector ObserverPositionM(0.4, 0.012, -0.023);
	const FQuat ObserverRotation = FRotator(3.0, -8.0, 12.0).Quaternion();
	Scene.Observer->SetActorLocationAndRotation(
		Scene.SLM->GetActorTransform().TransformPositionNoScale(ObserverPositionM * 100.0),
		Scene.SLM->GetActorQuat() * ObserverRotation);
	Scene.Observer->Parameters.ResolutionX = 11;
	Scene.Observer->Parameters.ResolutionY = 3;
	Scene.Observer->Parameters.PixelPitchXUm = 40.0;
	Scene.Observer->Parameters.PixelPitchYUm = 70.0;

	FCGHReconstructionInput Input;
	FString Error;
	TestTrue(TEXT("No camera or targets are required for reconstruction metadata"),
		Scene.Workbench->CaptureReconstructionInput(Input, Error));
	TestTrue(TEXT("Successful capture clears error"), Error.IsEmpty());
	TestEqual(TEXT("SLM grid is captured"), Input.SLM.ResolutionX, 7);
	TestEqual(TEXT("SLM pitch uses meters"), Input.SLM.PixelPitchXM, 9.0e-6, 1.0e-15);
	TestEqual(TEXT("Wavelength uses meters"), Input.Light.WavelengthM, 632.8e-9, 1.0e-15);
	TestEqual(TEXT("Incident amplitude is captured"), Input.Light.Amplitude, 2.5);
	TestTrue(TEXT("Light direction is expressed in the SLM frame"), Input.Light.DirectionSLM.Equals(FVector::ForwardVector, 1.0e-12));
	TestEqual(TEXT("Observer horizontal count is independent of SLM"), Input.ObserverPlane.ResolutionX, 11);
	TestEqual(TEXT("Observer vertical pitch uses meters"), Input.ObserverPlane.PixelPitchYM, 70.0e-6, 1.0e-15);
	TestTrue(TEXT("Translated and rotated observer uses SLM coordinates"), Input.ObserverPlane.PositionSLMM.Equals(ObserverPositionM, 1.0e-12));
	TestTrue(TEXT("Observer retains its relative orientation"), Input.ObserverPlane.RotationSLM.Equals(ObserverRotation, 1.0e-12));
	TestTrue(TEXT("Capture publishes observer metadata availability"), Scene.Workbench->bObserverPlaneDescriptionAvailable);
	TestTrue(TEXT("Routine capture does not copy phase samples"), Input.Pattern.PhaseRad.IsEmpty());
	TestFalse(TEXT("Optical capture does not claim the legacy scene is complete"), Scene.Workbench->bSceneDescriptionComplete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructionWorkbenchTrackingTest,
	"CGH.Reconstruction.Workbench.ObserverTrackingAndLegacyCompleteness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCGHReconstructionWorkbenchTrackingTest::RunTest(const FString& Parameters)
{
	FCGHReconstructionWorkbenchFixture Scene;
	if (!Scene.Initialize(*this) || !Scene.BeginPlayInTestWorld())
	{
		return false;
	}
	Scene.Workbench->UpdateSceneDescription();
	Scene.Observer->SetActorLocation(FVector(65.0, 4.0, -2.0));
	TestTrue(TEXT("Observer transform notifications immediately update description"),
		Scene.Workbench->ObserverPlaneDescription.PositionSLMM.Equals(FVector(0.65, 0.04, -0.02), 1.0e-12));
	Scene.Observer->Parameters.ResolutionX = 13;
	Scene.TickTestWorld();
	TestEqual(TEXT("Runtime polling sees direct parameter writes"), Scene.Workbench->ObserverPlaneDescription.ResolutionX, 13);
	Scene.Observer->Destroy();
	TestFalse(TEXT("Destroyed observer clears availability immediately"), Scene.Workbench->bObserverPlaneDescriptionAvailable);
	TestEqual(TEXT("Destroyed observer clears cached grid"), Scene.Workbench->ObserverPlaneDescription.ResolutionX, 0);

	Scene.Workbench->ObserverPlane = nullptr;
	Scene.Workbench->Camera = Scene.GetTestWorld()->SpawnActor<ACGHCameraActor>();
	ACGHTargetActor* Target = Scene.GetTestWorld()->SpawnActor<ACGHTargetActor>();
	Scene.Workbench->Targets = {Target};
	Scene.Workbench->UpdateSceneDescription();
	TestTrue(TEXT("Observer remains optional for existing solver scene completeness"), Scene.Workbench->bSceneDescriptionComplete);
	TestEqual(TEXT("Solver scene schema remains unchanged"), Scene.Workbench->SceneDescription.SchemaVersion, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructionWorkbenchValidationTest,
	"CGH.Reconstruction.Workbench.WorldAndScaleValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCGHReconstructionWorkbenchValidationTest::RunTest(const FString& Parameters)
{
	FCGHReconstructionWorkbenchFixture Scene;
	FCGHReconstructionWorkbenchFixture OtherScene;
	if (!Scene.Initialize(*this) || !OtherScene.Initialize(*this))
	{
		return false;
	}
	FCGHReconstructionInput Input;
	FString Error;
	Scene.Workbench->ObserverPlane = OtherScene.Observer;
	TestFalse(TEXT("Cross-world observer is rejected"), Scene.Workbench->CaptureReconstructionInput(Input, Error));
	TestTrue(TEXT("Cross-world diagnostic identifies the world constraint"), Error.Contains(TEXT("world")));
	Scene.Workbench->ObserverPlane = Scene.Observer;
	Scene.Observer->SetActorScale3D(FVector(2.0, 1.0, 1.0));
	TestFalse(TEXT("Scaled observer is rejected"), Scene.Workbench->CaptureReconstructionInput(Input, Error));
	Scene.Observer->SetActorScale3D(FVector::OneVector);
	Scene.SLM->SetActorScale3D(FVector(1.0, 0.5, 1.0));
	TestFalse(TEXT("Scaled SLM is rejected"), Scene.Workbench->CaptureReconstructionInput(Input, Error));
	Scene.SLM->SetActorScale3D(FVector::OneVector);
	Scene.Light->SetActorScale3D(FVector(1.0, 1.0, 3.0));
	TestFalse(TEXT("Scaled source is rejected"), Scene.Workbench->CaptureReconstructionInput(Input, Error));
	Scene.Light->SetActorScale3D(FVector::OneVector);
	TestTrue(TEXT("Restoring required optical actors allows capture"), Scene.Workbench->CaptureReconstructionInput(Input, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructionWorkbenchOwnershipTest,
	"CGH.Reconstruction.Workbench.CoordinatorOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCGHReconstructionWorkbenchOwnershipTest::RunTest(const FString& Parameters)
{
	FCGHReconstructionWorkbenchFixture Scene;
	FCGHReconstructionWorkbenchFixture OtherScene;
	if (!Scene.Initialize(*this) || !OtherScene.Initialize(*this))
	{
		return false;
	}
	ACGHWorkbenchActor* Owner = Scene.GetTestWorld()->SpawnActor<ACGHWorkbenchActor>();
	ACGHReconstructorActor* Coordinator = Scene.GetTestWorld()->SpawnActor<ACGHReconstructorActor>();
	if (!TestNotNull(TEXT("Second same-world workbench exists"), Owner)
		|| !TestNotNull(TEXT("Reconstructor exists"), Coordinator))
	{
		return false;
	}
	Coordinator->Workbench = Owner;
	Coordinator->JobState = ECGHReconstructionJobState::Running;
	Coordinator->StatusMessage = TEXT("Other workbench owns this coordinator.");
	Scene.Workbench->Reconstructor = Coordinator;
	Scene.Workbench->Reconstruct();
	TestEqual(TEXT("Workbench does not steal another workbench's reconstructor"), Coordinator->Workbench.Get(), Owner);
	TestEqual(TEXT("Foreign-owner reconstruction does not submit work"), Coordinator->JobId, int64(0));
	Scene.Workbench->CancelReconstruction();
	TestTrue(TEXT("Foreign-owner cancellation does not change job state"), Coordinator->JobState == ECGHReconstructionJobState::Running);
	TestTrue(TEXT("Unowned status is not mirrored as a running local job"), Scene.Workbench->ReconstructionJobState == ECGHReconstructionJobState::Idle);

	ACGHReconstructorActor* Foreign = OtherScene.GetTestWorld()->SpawnActor<ACGHReconstructorActor>();
	if (!TestNotNull(TEXT("Cross-world reconstructor exists"), Foreign))
	{
		return false;
	}
	Foreign->JobState = ECGHReconstructionJobState::Running;
	Scene.Workbench->Reconstructor = Foreign;
	Scene.Workbench->Reconstruct();
	Scene.Workbench->CancelReconstruction();
	TestNull(TEXT("Cross-world reconstructor is never bound"), Foreign->Workbench.Get());
	TestTrue(TEXT("Cross-world coordinator state is untouched"), Foreign->JobState == ECGHReconstructionJobState::Running);
	return true;
}

#endif
