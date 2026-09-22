#include "CGH/Actors/CGHReconstructorActor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Actors/CGHObserverPlaneActor.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	struct FCGHReconstructorTestScene : FTestWorldWrapper
	{
		ACGHSLMActor* SLM = nullptr;
		ACGHReconstructionLightActor* Light = nullptr;
		ACGHObserverPlaneActor* Observer = nullptr;
		ACGHWorkbenchActor* Workbench = nullptr;
		ACGHReconstructorActor* Reconstructor = nullptr;

		bool Initialize(FAutomationTestBase& Test)
		{
			if (!CreateTestWorld(EWorldType::Game))
			{
				ForwardErrorMessages(&Test);
				return false;
			}
			SLM = TestWorld->SpawnActor<ACGHSLMActor>();
			Light = TestWorld->SpawnActor<ACGHReconstructionLightActor>();
			Observer = TestWorld->SpawnActor<ACGHObserverPlaneActor>();
			Workbench = TestWorld->SpawnActor<ACGHWorkbenchActor>();
			Reconstructor = TestWorld->SpawnActor<ACGHReconstructorActor>();
			if (!SLM || !Light || !Observer || !Workbench || !Reconstructor)
			{
				Test.AddError(TEXT("Could not spawn reconstruction fixture."));
				return false;
			}
			SLM->Parameters.ResolutionX = 4;
			SLM->Parameters.ResolutionY = 3;
			SLM->SetActorLocationAndRotation(FVector(10.0, -5.0, 3.0), FRotator(17.0, -21.0, 6.0));
			Light->SetActorRotation(SLM->GetActorRotation());
			Observer->Parameters.ResolutionX = 3;
			Observer->Parameters.ResolutionY = 2;
			Observer->SetActorLocationAndRotation(
				SLM->GetActorTransform().TransformPositionNoScale(FVector(40.0, 0.1, -0.05)), SLM->GetActorQuat());
			Workbench->SLM = SLM;
			Workbench->ReconstructionLight = Light;
			Workbench->ObserverPlane = Observer;
			Workbench->Reconstructor = Reconstructor;
			Reconstructor->Workbench = Workbench;
			return SetPhase(Test, 0.4);
		}

		bool SetPhase(FAutomationTestBase& Test, double Phase)
		{
			FCGHSLMPhasePattern Pattern;
			Pattern.ResolutionX = SLM->Parameters.ResolutionX;
			Pattern.ResolutionY = SLM->Parameters.ResolutionY;
			Pattern.PhaseRad.Init(Phase, Pattern.ResolutionX * Pattern.ResolutionY);
			return Test.TestTrue(TEXT("Publish fixture SLM phase"), SLM->SetPhasePattern(MoveTemp(Pattern)));
		}

		bool Sentinel(FAutomationTestBase& Test)
		{
			FCGHComplexField Field;
			Field.ResolutionX = Observer->Parameters.ResolutionX;
			Field.ResolutionY = Observer->Parameters.ResolutionY;
			FCGHComplexSample Sample;
			Sample.Real = 0.125;
			Sample.Imaginary = -0.75;
			Field.Samples.Init(Sample, Field.ResolutionX * Field.ResolutionY);
			return Test.TestTrue(TEXT("Publish fixture observer field"), Observer->SetComplexField(MoveTemp(Field)));
		}
	};

	bool WaitForReconstructor(FAutomationTestBase& Test, ACGHReconstructorActor& Actor, FTestWorldWrapper* Scene = nullptr)
	{
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		do
		{
			if (Scene) Scene->TickTestWorld();
			else Actor.PollReconstructor();
			if (Actor.JobState != ECGHReconstructionJobState::Queued && Actor.JobState != ECGHReconstructionJobState::Running)
			{
				return true;
			}
			FPlatformProcess::Sleep(0.001f);
		} while (FPlatformTime::Seconds() < Deadline);
		Test.AddError(FString::Printf(TEXT("Reconstructor did not settle: %s"), *Actor.StatusMessage));
		Actor.CancelReconstruction();
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructorPublicationTest,
	"CGH.ReconstructorActor.PublicationAndLatestRequest", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructorPublicationTest::RunTest(const FString& Parameters)
{
	FCGHReconstructorTestScene Scene;
	if (!Scene.Initialize(*this)) return false;
	TestFalse(TEXT("Automatic reconstruction is opt-in"), Scene.Reconstructor->bAutoReconstruct);
	const uint64 PhaseRevision = Scene.SLM->GetPhasePatternRevision();
	Scene.Reconstructor->Workbench = nullptr;
	Scene.Workbench->Reconstruct();
	TestEqual(TEXT("Workbench binds an unassigned reconstructor"), Scene.Reconstructor->Workbench.Get(), Scene.Workbench);
	if (!WaitForReconstructor(*this, *Scene.Reconstructor)) return false;
	TestTrue(TEXT("No targets or camera are required"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Ready);
	TestTrue(TEXT("A complete complex field is published"), Scene.Observer->HasValidComplexField());
	TestEqual(TEXT("Reconstruction leaves input phase unchanged"), Scene.SLM->GetPhasePatternRevision(), PhaseRevision);
	Scene.Workbench->Tick(0.0f);
	TestTrue(TEXT("Workbench mirrors completion"), Scene.Workbench->ReconstructionJobState == ECGHReconstructionJobState::Ready);
	const uint64 Revision = Scene.Observer->GetComplexFieldRevision();
	TestTrue(TEXT("Start a replaceable request"), Scene.Reconstructor->StartReconstruction());
	Scene.Light->Parameters.Amplitude = 0.5;
	TestTrue(TEXT("A newer request replaces active work"), Scene.Reconstructor->StartReconstruction());
	Scene.Light->Parameters.Amplitude = 0.0;
	TestTrue(TEXT("Only the latest pending request is retained"), Scene.Reconstructor->StartReconstruction());
	if (!WaitForReconstructor(*this, *Scene.Reconstructor)) return false;
	TestTrue(TEXT("Latest request reaches ready"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Ready);
	TestEqual(TEXT("Only the latest request publishes"), Scene.Observer->GetComplexFieldRevision(), Revision + 1);
	for (const FCGHComplexSample& Sample : Scene.Observer->GetComplexField().Samples)
	{
		TestEqual(TEXT("Latest zero-amplitude illumination has zero real field"), Sample.Real, 0.0);
		TestEqual(TEXT("Latest zero-amplitude illumination has zero imaginary field"), Sample.Imaginary, 0.0);
	}
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructorStaleTest,
	"CGH.ReconstructorActor.RejectsStaleInputsAndDestinationChanges", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructorStaleTest::RunTest(const FString& Parameters)
{
	for (int32 Mutation = 0; Mutation < 12; ++Mutation)
	{
		FCGHReconstructorTestScene Scene;
		if (!Scene.Initialize(*this) || !Scene.Sentinel(*this)) return false;
		TestTrue(TEXT("Capture initial inputs"), Scene.Reconstructor->StartReconstruction());
		switch (Mutation)
		{
		case 0: Scene.SetPhase(*this, 1.25); break;
		case 1: Scene.Light->Parameters.Amplitude += 0.3; break;
		case 2: Scene.Light->Parameters.InitialPhaseRad += 0.1; break;
		case 3: Scene.Light->Parameters.WavelengthNm += 1.0; break;
		case 4: Scene.Light->AddActorWorldRotation(FRotator(0.0, 1.0, 0.0)); break;
		case 5: Scene.Observer->AddActorWorldOffset(FVector(0.1, 0.2, 0.3)); break;
		case 6: Scene.Observer->AddActorWorldRotation(FRotator(2.0, 1.0, 0.0)); break;
		case 7: Scene.Observer->Parameters.PixelPitchXUm += 1.0; break;
		case 8: Scene.SLM->Parameters.PixelPitchYUm += 1.0; break;
		case 9: Scene.Workbench->ReconstructionLight = nullptr; break;
		case 10: Scene.Observer->ClearComplexField(); break;
		case 11: Scene.Workbench->ObserverPlane = Scene.GetTestWorld()->SpawnActor<ACGHObserverPlaneActor>(); break;
		}
		const uint64 Revision = Scene.Observer->GetComplexFieldRevision();
		if (!WaitForReconstructor(*this, *Scene.Reconstructor)) return false;
		TestTrue(FString::Printf(TEXT("Mutation %d rejects obsolete result"), Mutation), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Failed);
		TestEqual(TEXT("An obsolete result cannot overwrite the destination"), Scene.Observer->GetComplexFieldRevision(), Revision);
		if (Mutation != 10)
		{
			TestEqual(TEXT("Previous field samples are retained"), Scene.Observer->GetComplexField().Samples[0].Real, 0.125);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructorCancellationTest,
	"CGH.ReconstructorActor.CancellationFailureAndUnsupportedModes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructorCancellationTest::RunTest(const FString& Parameters)
{
	FCGHReconstructorTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.Sentinel(*this)) return false;
	const uint64 Revision = Scene.Observer->GetComplexFieldRevision();
	TestTrue(TEXT("Start before cancellation"), Scene.Reconstructor->StartReconstruction());
	Scene.Workbench->CancelReconstruction();
	Scene.Reconstructor->PollReconstructor();
	TestTrue(TEXT("Cancellation leaves idle status"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Idle);
	TestEqual(TEXT("Cancellation preserves previous field"), Scene.Observer->GetComplexFieldRevision(), Revision);
	Scene.Reconstructor->Parameters.ReconstructionBackend = ECGHReconstructionBackend::Docker;
	Scene.Reconstructor->Parameters.Docker.Port = 0;
	TestTrue(TEXT("Docker transport accepts an asynchronous request"), Scene.Reconstructor->StartReconstruction());
	if (!WaitForReconstructor(*this, *Scene.Reconstructor)) return false;
	TestTrue(TEXT("Docker fails explicitly"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Failed);
	TestTrue(TEXT("Docker explains invalid endpoint settings"), Scene.Reconstructor->StatusMessage.Contains(TEXT("port")));
	Scene.Reconstructor->Parameters.ReconstructionBackend = ECGHReconstructionBackend::CPU;
	Scene.Reconstructor->Parameters.Mode = ECGHReconstructionMode::Camera;
	TestFalse(TEXT("Camera mode cannot start an unsupported optical path"), Scene.Reconstructor->StartReconstruction());
	TestTrue(TEXT("Camera explains missing implementation"), Scene.Reconstructor->StatusMessage.Contains(TEXT("not implemented")));
	Scene.Reconstructor->Parameters.Mode = ECGHReconstructionMode::ObserverPlane;
	Scene.SLM->ClearPhasePattern();
	TestFalse(TEXT("Missing SLM phase rejects reconstruction"), Scene.Reconstructor->StartReconstruction());
	TestEqual(TEXT("Failure never replaces the accepted field"), Scene.Observer->GetComplexFieldRevision(), Revision);
	Scene.SetPhase(*this, 0.7);
	TestTrue(TEXT("A valid CPU request recovers"), Scene.Reconstructor->StartReconstruction());
	if (!WaitForReconstructor(*this, *Scene.Reconstructor)) return false;
	TestTrue(TEXT("CPU recovers after unsupported modes"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Ready);
	const uint64 BeforeDestroy = Scene.Observer->GetComplexFieldRevision();
	Scene.Reconstructor->StartReconstruction();
	Scene.Reconstructor->Destroy();
	TestEqual(TEXT("Destruction cannot publish pending work"), Scene.Observer->GetComplexFieldRevision(), BeforeDestroy);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructorAutoTest,
	"CGH.ReconstructorActor.AutomaticRuntimeUpdatesAndCancellation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructorAutoTest::RunTest(const FString& Parameters)
{
	FCGHReconstructorTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.BeginPlayInTestWorld()) return false;
	Scene.Reconstructor->bAutoReconstruct = true;
	Scene.TickTestWorld();
	if (!WaitForReconstructor(*this, *Scene.Reconstructor, &Scene)) return false;
	TestTrue(TEXT("Runtime ticks publish the initial field"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Ready);
	const int64 InitialJob = Scene.Reconstructor->JobId;
	Scene.Observer->PreviewMode = ECGHObserverPreviewMode::Phase;
	Scene.Reconstructor->Parameters.Docker.Port += 1;
	Scene.Reconstructor->Parameters.Docker.RequestTimeoutSeconds += 1.0;
	Scene.Light->Parameters.PolarizationAngleDeg += 20.0;
	Scene.Light->AddActorWorldOffset(FVector(1.0, 2.0, 3.0));
	for (int32 Tick = 0; Tick < 4; ++Tick) Scene.TickTestWorld();
	TestEqual(TEXT("Presentation, unused Docker settings, and unused plane-wave inputs do not restart CPU work"), Scene.Reconstructor->JobId, InitialJob);
	Scene.SetPhase(*this, 1.0);
	Scene.TickTestWorld();
	if (!WaitForReconstructor(*this, *Scene.Reconstructor, &Scene)) return false;
	TestTrue(TEXT("A phase revision queues automatic reconstruction"), Scene.Reconstructor->JobId > InitialJob);
	Scene.Light->Parameters.Amplitude = 0.3;
	Scene.TickTestWorld();
	const int64 CancelledJob = Scene.Reconstructor->JobId;
	Scene.Reconstructor->CancelReconstruction();
	for (int32 Tick = 0; Tick < 4; ++Tick) Scene.TickTestWorld();
	TestEqual(TEXT("Cancellation suppresses unchanged-input retries"), Scene.Reconstructor->JobId, CancelledJob);
	TestTrue(TEXT("Cancelled automatic work remains idle"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Idle);
	Scene.Observer->Parameters.PixelPitchYUm += 2.0;
	Scene.TickTestWorld();
	if (!WaitForReconstructor(*this, *Scene.Reconstructor, &Scene)) return false;
	TestTrue(TEXT("A new observer input resumes automatic work"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Ready);
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructorDuplicateTest,
	"CGH.ReconstructorActor.DuplicationStartsIdle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructorDuplicateTest::RunTest(const FString& Parameters)
{
	FCGHReconstructorTestScene Scene;
	if (!Scene.Initialize(*this)) return false;
	Scene.Reconstructor->StartReconstruction();
	Scene.Reconstructor->LastComputeSeconds = 7.5;
	const FName Name = MakeUniqueObjectName(Scene.Reconstructor->GetOuter(), Scene.Reconstructor->GetClass(), TEXT("CGHReconstructorDuplicate"));
	TStrongObjectPtr<ACGHReconstructorActor> Duplicate(Cast<ACGHReconstructorActor>(
		StaticDuplicateObject(Scene.Reconstructor, Scene.Reconstructor->GetOuter(), Name)));
	if (!TestNotNull(TEXT("Duplicate actor exists"), Duplicate.Get())) return false;
	TestTrue(TEXT("Duplicate is idle"), Duplicate->JobState == ECGHReconstructionJobState::Idle);
	TestEqual(TEXT("Duplicate has independent request numbering"), Duplicate->JobId, int64(0));
	TestEqual(TEXT("Duplicate has no inherited compute duration"), Duplicate->LastComputeSeconds, 0.0);
	const uint64 Revision = Scene.Observer->GetComplexFieldRevision();
	Duplicate->PollReconstructor();
	TestEqual(TEXT("Duplicate cannot publish source job"), Scene.Observer->GetComplexFieldRevision(), Revision);
	if (!WaitForReconstructor(*this, *Scene.Reconstructor)) return false;
	TestTrue(TEXT("Original job still publishes"), Scene.Reconstructor->JobState == ECGHReconstructionJobState::Ready);
	return true;
}

#endif
