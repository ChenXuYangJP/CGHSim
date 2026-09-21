#include "CGH/Actors/CGHSolverActor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHTargetActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

#include <limits>

#if WITH_EDITOR
#include "CGH/Components/CGHSLMPreviewComponent.h"
#include "Engine/Texture2D.h"
#endif

namespace
{
	/** A small, asymmetric fixture, independent of the user's map and saved assets. */
	struct FCGHSolverTestScene : FTestWorldWrapper
	{
		ACGHSLMActor* SLM = nullptr;
		ACGHReconstructionLightActor* Light = nullptr;
		ACGHTargetActor* Target = nullptr;
		ACGHWorkbenchActor* Workbench = nullptr;
		ACGHSolverActor* Solver = nullptr;

		bool Initialize(FAutomationTestBase& Test, EWorldType::Type WorldType = EWorldType::Game)
		{
			if (!CreateTestWorld(WorldType))
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
				Test.AddError(TEXT("Could not spawn solver fixture actors."));
				return false;
			}
			SLM->Parameters.ResolutionX = 4;
			SLM->Parameters.ResolutionY = 3;
			SLM->Parameters.PixelPitchXUm = 80.0;
			SLM->Parameters.PixelPitchYUm = 110.0;
			SLM->SetActorLocationAndRotation(FVector(120.0, -70.0, 35.0), FRotator(20.0, 40.0, -15.0));
			SLM->ClearPhasePattern();
			Light->Parameters.WavelengthNm = 633.123456789;
			Light->SetActorRotation(SLM->GetActorRotation());
			Target->SetActorLocation(SLM->GetActorTransform().TransformPositionNoScale(FVector(50.0, 1.5, -2.4)));
			Target->Parameters.InitialPhaseRad = 0.73;
			Workbench->SLM = SLM;
			Workbench->ReconstructionLight = Light;
			Workbench->Targets = {Target};
			Workbench->Solver = Solver;
			Solver->Workbench = Workbench;
			return true;
		}

		bool PublishSentinel(FAutomationTestBase& Test, double Phase = 0.125)
		{
			FCGHSLMPhasePattern Pattern;
			Pattern.ResolutionX = SLM->Parameters.ResolutionX;
			Pattern.ResolutionY = SLM->Parameters.ResolutionY;
			Pattern.PhaseRad.Init(Phase, Pattern.ResolutionX * Pattern.ResolutionY);
			return Test.TestTrue(TEXT("Fixture publishes a valid previous phase pattern"), SLM->SetPhasePattern(Pattern));
		}
	};

	bool WaitForCGHSolver(FAutomationTestBase& Test, ACGHSolverActor& Solver, FTestWorldWrapper* TickScene = nullptr)
	{
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		do
		{
			if (TickScene)
			{
				TickScene->TickTestWorld();
			}
			else
			{
				Solver.PollSolver();
			}
			if (Solver.JobState != ECGHSolverJobState::Queued && Solver.JobState != ECGHSolverJobState::Running)
			{
				return true;
			}
			// This yield belongs only to the test; production polling must never wait.
			FPlatformProcess::Sleep(0.001f);
		} while (FPlatformTime::Seconds() < Deadline);
		Test.AddError(FString::Printf(TEXT("Solver did not settle: %s"), *Solver.StatusMessage));
		Solver.CancelSolve();
		return false;
	}

	/** Verify propagation independently using complex phase, including row direction and SI conversion. */
	bool CheckCGHPointFocus(FAutomationTestBase& Test, const FCGHSolverTestScene& Scene)
	{
		if (!Test.TestTrue(TEXT("Solver publishes a valid SLM pattern"), Scene.SLM->HasValidPhasePattern()))
		{
			return false;
		}
		const FCGHSLMPhasePattern& Pattern = Scene.SLM->GetPhasePattern();
		const FVector TargetM = Scene.SLM->GetActorTransform().InverseTransformPositionNoScale(
			Scene.Target->GetActorLocation()) * 0.01;
		const double WaveNumber = 2.0 * UE_DOUBLE_PI / (Scene.Light->Parameters.WavelengthNm * 1.0e-9);
		const FVector IncidentDirection = Scene.SLM->GetActorTransform().InverseTransformVectorNoScale(
			Scene.Light->GetPropagationDirection()).GetSafeNormal();
		Test.TestEqual(TEXT("SLM output has configured columns"), Pattern.ResolutionX, Scene.SLM->Parameters.ResolutionX);
		Test.TestEqual(TEXT("SLM output has configured rows"), Pattern.ResolutionY, Scene.SLM->Parameters.ResolutionY);
		double CoherentReal = 0.0;
		double CoherentImaginary = 0.0;
		for (int32 Row = 0; Row < Pattern.ResolutionY; ++Row)
		{
			for (int32 Column = 0; Column < Pattern.ResolutionX; ++Column)
			{
				const FVector PixelM(0.0,
					(Column - (Pattern.ResolutionX - 1) / 2.0) * Scene.SLM->Parameters.PixelPitchXUm * 1.0e-6,
					((Pattern.ResolutionY - 1) / 2.0 - Row) * Scene.SLM->Parameters.PixelPitchYUm * 1.0e-6);
				const double Phase = Pattern.PhaseRad[Row * Pattern.ResolutionX + Column];
				Test.TestTrue(TEXT("SLM phases are finite and wrapped into [0, 2*pi)"),
					FMath::IsFinite(Phase) && Phase >= 0.0 && Phase < 2.0 * UE_DOUBLE_PI);
				const double IncidentPhase = Scene.Light->Parameters.InitialPhaseRad
					+ WaveNumber * FVector::DotProduct(IncidentDirection, PixelM);
				const double PropagatedError = IncidentPhase + Phase + WaveNumber * FVector::Distance(TargetM, PixelM)
					- Scene.Target->Parameters.InitialPhaseRad;
				Test.TestEqual(TEXT("Incident wave, SLM phase and exp(+i*k*r) propagation reach the requested target phase"),
					FMath::Cos(PropagatedError), 1.0, 1.0e-12);
				Test.TestEqual(TEXT("Propagated phase has no residual imaginary component"),
					FMath::Sin(PropagatedError), 0.0, 2.0e-8);
				CoherentReal += FMath::Cos(PropagatedError);
				CoherentImaginary += FMath::Sin(PropagatedError);
			}
		}
		Test.TestEqual(TEXT("All phase-only pixel contributions add coherently at the point"),
			FMath::Sqrt(CoherentReal * CoherentReal + CoherentImaginary * CoherentImaginary),
			static_cast<double>(Pattern.PhaseRad.Num()), 1.0e-10);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSolverPublicationTest,
	"CGH.SolverActor.PointFocusPublishesFromSceneToPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSolverPublicationTest::RunTest(const FString& Parameters)
{
	for (EWorldType::Type WorldType : {EWorldType::Game, EWorldType::Editor})
	{
		FCGHSolverTestScene Scene;
		if (!Scene.Initialize(*this, WorldType))
		{
			return false;
		}
		TestFalse(TEXT("Automatic solving is opt-in"), Scene.Solver->bAutoSolve);
		const uint64 BeforeRevision = Scene.SLM->GetPhasePatternRevision();
		Scene.Workbench->SolvePhasePattern();
		TestFalse(TEXT("Point focus does not need a camera or a complete camera snapshot"), Scene.Workbench->bSceneDescriptionComplete);
		TestEqual(TEXT("Workers do not publish before game-thread polling"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
		if (!WaitForCGHSolver(*this, *Scene.Solver))
		{
			return false;
		}
		TestTrue(TEXT("Finished request is ready"), Scene.Solver->JobState == ECGHSolverJobState::Ready);
		TestTrue(TEXT("Publication advances the SLM revision"), Scene.SLM->GetPhasePatternRevision() > BeforeRevision);
		TestFalse(TEXT("Solved data is not marked as a preview fixture"), Scene.SLM->bIsPreviewPhasePattern);
		TestTrue(TEXT("Compute duration is finite and nonnegative"),
			FMath::IsFinite(Scene.Solver->LastComputeSeconds) && Scene.Solver->LastComputeSeconds >= 0.0);
		CheckCGHPointFocus(*this, Scene);
		Scene.Workbench->Tick(0.0f);
		TestTrue(TEXT("Workbench mirrors ready solver status"), Scene.Workbench->SolverJobState == ECGHSolverJobState::Ready);
		TestEqual(TEXT("Workbench mirrors the solver status message"), Scene.Workbench->SolverStatusMessage, Scene.Solver->StatusMessage);
#if WITH_EDITOR
		UCGHSLMPreviewComponent* Preview = Scene.SLM->FindComponentByClass<UCGHSLMPreviewComponent>();
		if (!TestNotNull(TEXT("SLM has its preview component"), Preview))
		{
			return false;
		}
		Preview->RefreshPreviewTexture();
		UTexture2D* Texture = Preview->GetPreviewTexture();
		if (!TestNotNull(TEXT("Solver publication produces the SLM preview image"), Texture))
		{
			AddError(Preview->GetPreviewError());
			return false;
		}
		TestEqual(TEXT("Preview preserves the asymmetric output width"), Texture->GetSizeX(), 4);
		TestEqual(TEXT("Preview preserves the asymmetric output height"), Texture->GetSizeY(), 3);
		TestEqual(TEXT("Preview uses the published solver revision"), Preview->GetPreviewRevision(), Scene.SLM->GetPhasePatternRevision());
#endif
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSolverObliqueIlluminationTest,
	"CGH.SolverActor.ObliquePlaneWaveFocusesInRotatedSLMFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSolverObliqueIlluminationTest::RunTest(const FString& Parameters)
{
	FCGHSolverTestScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	const FVector LocalDirection(0.8, 0.36, -0.48);
	const FVector WorldDirection = Scene.SLM->GetActorTransform().TransformVectorNoScale(LocalDirection);
	Scene.Light->SetActorRotation(WorldDirection.Rotation());
	Scene.Light->SetActorLocation(FVector(-110.0, 73.0, 21.0));
	Scene.Light->Parameters.InitialPhaseRad = -0.91;
	Scene.Light->Parameters.Amplitude = 0.65;
	Scene.Light->Parameters.PolarizationAngleDeg = 27.0;
	TestTrue(TEXT("An oblique plane-wave request starts"), Scene.Solver->StartSolve());
	TestEqual(TEXT("Workbench exports the wave direction in the rotated SLM frame"),
		Scene.Workbench->SceneDescription.ReconstructionLight.DirectionSLM, LocalDirection, 1.0e-12f);
	if (!WaitForCGHSolver(*this, *Scene.Solver) || !CheckCGHPointFocus(*this, Scene))
	{
		return false;
	}
	TestTrue(TEXT("Oblique illumination reaches ready"), Scene.Solver->JobState == ECGHSolverJobState::Ready);
	const TArray<double> Published = Scene.SLM->GetPhasePattern().PhaseRad;
	const uint64 BeforePositionRevision = Scene.SLM->GetPhasePatternRevision();
	Scene.Light->AddActorWorldOffset(FVector(500.0, -200.0, 30.0));
	Scene.Light->Parameters.Amplitude = 0.3;
	Scene.Light->Parameters.PolarizationAngleDeg = 48.0;
	TestTrue(TEXT("The plane wave can be resolved after display position and unused optical fields change"), Scene.Solver->StartSolve());
	if (!WaitForCGHSolver(*this, *Scene.Solver))
	{
		return false;
	}
	TestTrue(TEXT("Plane-wave phase is referenced to the SLM origin and independent of actor position"),
		Scene.SLM->GetPhasePattern().PhaseRad == Published);
	TestEqual(TEXT("Unused valid illumination edits preserve the identical phase revision"),
		Scene.SLM->GetPhasePatternRevision(), BeforePositionRevision);
	return CheckCGHPointFocus(*this, Scene);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSolverReplacementTest,
	"CGH.SolverActor.CancellationAndLatestRequestWin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSolverReplacementTest::RunTest(const FString& Parameters)
{
	FCGHSolverTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.PublishSentinel(*this))
	{
		return false;
	}
	const uint64 BeforeRevision = Scene.SLM->GetPhasePatternRevision();
	TestTrue(TEXT("Initial request starts"), Scene.Solver->StartSolve());
	Scene.Workbench->CancelSolve();
	TestTrue(TEXT("Workbench cancel makes the job idle"), Scene.Solver->JobState == ECGHSolverJobState::Idle);
	Scene.Solver->PollSolver();
	TestEqual(TEXT("Cancellation preserves the previous phase revision"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
	TestEqual(TEXT("Cancellation preserves the previous phase samples"), Scene.SLM->GetPhasePattern().PhaseRad[0], 0.125);

	TestTrue(TEXT("A request after cancellation is accepted"), Scene.Solver->StartSolve());
	const int64 FirstJob = Scene.Solver->JobId;
	Scene.Target->Parameters.InitialPhaseRad = 1.25;
	TestTrue(TEXT("A replacement request is accepted"), Scene.Solver->StartSolve());
	Scene.Target->Parameters.InitialPhaseRad = -0.31;
	TestTrue(TEXT("Only the newest pending request is retained"), Scene.Solver->StartSolve());
	TestTrue(TEXT("Requests have advancing identities"), Scene.Solver->JobId > FirstJob);
	TestEqual(TEXT("Superseded requests cannot publish without polling"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
	if (!WaitForCGHSolver(*this, *Scene.Solver))
	{
		return false;
	}
	TestTrue(TEXT("Newest request reaches ready"), Scene.Solver->JobState == ECGHSolverJobState::Ready);
	TestEqual(TEXT("Only the newest result advances SLM publication"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision + 1);
	return CheckCGHPointFocus(*this, Scene);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSolverStaleInputsTest,
	"CGH.SolverActor.ChangedInputsCannotOverwritePublishedPattern",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSolverStaleInputsTest::RunTest(const FString& Parameters)
{
	for (int32 Mutation = 0; Mutation < 11; ++Mutation)
	{
		FCGHSolverTestScene Scene;
		if (!Scene.Initialize(*this) || !Scene.PublishSentinel(*this))
		{
			return false;
		}
		const uint64 BeforeRevision = Scene.SLM->GetPhasePatternRevision();
		TestTrue(TEXT("Request snapshots the original inputs"), Scene.Solver->StartSolve());
		// Mutate before the first poll, even if the small worker already finished.
		switch (Mutation)
		{
		case 0: Scene.Target->Parameters.InitialPhaseRad += 0.5; break;
		case 1: Scene.Target->AddActorWorldOffset(FVector(0.0, 1.0, 0.0)); break;
		case 2: Scene.SLM->Parameters.PixelPitchXUm += 5.0; break;
		case 3: Scene.Light->Parameters.WavelengthNm += 10.0; break;
		case 4: Scene.Target->Destroy(); break;
		case 5: Scene.SLM->AddActorWorldOffset(FVector(0.5, 0.0, 0.0)); break;
		case 6: Scene.Light->Parameters.InitialPhaseRad += 0.4; break;
		case 7: Scene.Light->SetActorRotation(Scene.Light->GetActorRotation() + FRotator(3.0, 7.0, 0.0)); break;
		case 8: Scene.Light->Parameters.PolarizationAngleDeg = std::numeric_limits<double>::quiet_NaN(); break;
		case 9: Scene.Light->Parameters.SourceType = ECGHSourceType::PointSource; break;
		case 10: Scene.Light->Parameters.Amplitude = -0.25; break;
		}
		if (!WaitForCGHSolver(*this, *Scene.Solver))
		{
			return false;
		}
		TestFalse(TEXT("An obsolete job is never reported ready"), Scene.Solver->JobState == ECGHSolverJobState::Ready);
		TestEqual(TEXT("Changed inputs preserve the previously published revision"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
		TestEqual(TEXT("Changed inputs preserve the previously published phase"), Scene.SLM->GetPhasePattern().PhaseRad[0], 0.125);
		TestFalse(TEXT("Discarding obsolete results explains the state"), Scene.Solver->StatusMessage.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSolverExternalPublicationTest,
	"CGH.SolverActor.ExternalPublicationAndReferenceReplacementWin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSolverExternalPublicationTest::RunTest(const FString& Parameters)
{
	FCGHSolverTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.PublishSentinel(*this))
	{
		return false;
	}
	TestTrue(TEXT("Request starts before an external publication"), Scene.Solver->StartSolve());
	if (!Scene.PublishSentinel(*this, 2.5))
	{
		return false;
	}
	const uint64 ExternalRevision = Scene.SLM->GetPhasePatternRevision();
	if (!WaitForCGHSolver(*this, *Scene.Solver))
	{
		return false;
	}
	TestEqual(TEXT("An in-flight solve cannot overwrite a newer manual publication"), Scene.SLM->GetPhasePatternRevision(), ExternalRevision);
	TestEqual(TEXT("The newer manual phase remains visible"), Scene.SLM->GetPhasePattern().PhaseRad[0], 2.5);

	TestTrue(TEXT("Another valid request starts"), Scene.Solver->StartSolve());
	ACGHTargetActor* Replacement = Scene.GetTestWorld()->SpawnActor<ACGHTargetActor>();
	if (!TestNotNull(TEXT("A replacement target can be spawned"), Replacement))
	{
		return false;
	}
	Replacement->SetActorTransform(Scene.Target->GetActorTransform());
	Replacement->Parameters = Scene.Target->Parameters;
	Scene.Workbench->Targets = {Replacement};
	if (!WaitForCGHSolver(*this, *Scene.Solver))
	{
		return false;
	}
	TestEqual(TEXT("Numerically identical replacement actor invalidates the old identity"), Scene.SLM->GetPhasePatternRevision(), ExternalRevision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSolverInputValidationTest,
	"CGH.SolverActor.RejectsUnsupportedOrUnavailableSceneInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSolverInputValidationTest::RunTest(const FString& Parameters)
{
	FCGHSolverTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.PublishSentinel(*this))
	{
		return false;
	}
	const uint64 BeforeRevision = Scene.SLM->GetPhasePatternRevision();
	const auto CheckRejected = [this, &Scene, BeforeRevision](const TCHAR* Label)
	{
		TestFalse(Label, Scene.Solver->StartSolve());
		TestTrue(TEXT("Rejected input sets failed job state"), Scene.Solver->JobState == ECGHSolverJobState::Failed);
		TestFalse(TEXT("Rejected input has a diagnostic"), Scene.Solver->StatusMessage.IsEmpty());
		TestEqual(TEXT("Rejected input preserves the previous SLM pattern"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
	};
	Scene.Solver->Parameters.SolverBackend = ECGHSolverBackend::Docker;
	CheckRejected(TEXT("Docker is explicitly unsupported in the CPU reference implementation"));
	Scene.Solver->Parameters.SolverBackend = ECGHSolverBackend::CPU;
	Scene.Light->Parameters.SourceType = ECGHSourceType::PointSource;
	CheckRejected(TEXT("Point-source illumination is unsupported by this plane-wave solver"));
	Scene.Light->Parameters.SourceType = ECGHSourceType::PlaneWave;
	Scene.Workbench->Targets.Reset();
	CheckRejected(TEXT("Missing point target is rejected"));
	Scene.Workbench->Targets = {Scene.Target, Scene.Target};
	CheckRejected(TEXT("Multiple target entries are rejected"));
	Scene.Workbench->Targets = {Scene.Target};
	Scene.Target->Parameters.TargetType = ECGHTargetType::Mesh;
	CheckRejected(TEXT("Mesh targets are rejected"));
	Scene.Target->Parameters.TargetType = ECGHTargetType::Point;
	Scene.Workbench->ReconstructionLight = nullptr;
	CheckRejected(TEXT("Missing wavelength source is rejected"));
	Scene.Workbench->ReconstructionLight = Scene.Light;
	Scene.Workbench->SLM = nullptr;
	CheckRejected(TEXT("Missing publication SLM is rejected"));
	Scene.Workbench->SLM = Scene.SLM;
	ACGHSLMActor* OtherSLM = Scene.GetTestWorld()->SpawnActor<ACGHSLMActor>();
	if (!TestNotNull(TEXT("Alternate SLM exists"), OtherSLM))
	{
		return false;
	}
	Scene.Target->SLM = OtherSLM;
	CheckRejected(TEXT("Target with a different explicit SLM reference is rejected"));
	Scene.Target->SLM = nullptr;
	Scene.Solver->Workbench = nullptr;
	CheckRejected(TEXT("Missing workbench is rejected"));
	Scene.Solver->Workbench = Scene.Workbench;

	FCGHSolverTestScene ForeignScene;
	if (!ForeignScene.Initialize(*this))
	{
		return false;
	}
	Scene.Workbench->Targets = {ForeignScene.Target};
	CheckRejected(TEXT("A target from another world is rejected"));
	Scene.Workbench->Targets = {Scene.Target};
	TestTrue(TEXT("Valid input can recover after rejection"), Scene.Solver->StartSolve());
	return WaitForCGHSolver(*this, *Scene.Solver) && CheckCGHPointFocus(*this, Scene);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSolverAutoRefreshTest,
	"CGH.SolverActor.AutoSolveTracksConsumedInputsWithoutRepeatedJobs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSolverAutoRefreshTest::RunTest(const FString& Parameters)
{
	FCGHSolverTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.BeginPlayInTestWorld())
	{
		Scene.ForwardErrorMessages(this);
		return false;
	}
	Scene.Solver->bAutoSolve = true;
	Scene.TickTestWorld();
	if (!WaitForCGHSolver(*this, *Scene.Solver, &Scene) || !CheckCGHPointFocus(*this, Scene))
	{
		return false;
	}
	const int64 InitialJob = Scene.Solver->JobId;
	const uint64 InitialRevision = Scene.SLM->GetPhasePatternRevision();
	for (int32 Poll = 0; Poll < 8; ++Poll)
	{
		Scene.TickTestWorld();
	}
	TestEqual(TEXT("Unchanged auto inputs do not launch repeated jobs"), Scene.Solver->JobId, InitialJob);
	TestEqual(TEXT("Unchanged auto inputs do not republish phase"), Scene.SLM->GetPhasePatternRevision(), InitialRevision);

	Scene.Target->Parameters.Amplitude = 0.25;
	Scene.Target->SetActorRotation(FRotator(12.0, -30.0, 4.0));
	Scene.Light->Parameters.Amplitude = 0.5;
	Scene.Light->Parameters.PolarizationAngleDeg = 65.0;
	Scene.Light->AddActorWorldOffset(FVector(23.0, -17.0, 5.0));
	Scene.TickTestWorld();
	TestEqual(TEXT("Finite amplitude/polarization, point orientation and plane-wave position do not trigger a point-focus job"), Scene.Solver->JobId, InitialJob);

	Scene.Light->Parameters.InitialPhaseRad = 1.25;
	Scene.TickTestWorld();
	if (!WaitForCGHSolver(*this, *Scene.Solver, &Scene) || !CheckCGHPointFocus(*this, Scene))
	{
		return false;
	}
	TestTrue(TEXT("Changing incident phase launches a new job"), Scene.Solver->JobId > InitialJob);
	TestTrue(TEXT("Changing incident phase publishes compensation on the SLM"), Scene.SLM->GetPhasePatternRevision() > InitialRevision);
	const int64 BeforeDirectionJob = Scene.Solver->JobId;
	const uint64 BeforeDirectionRevision = Scene.SLM->GetPhasePatternRevision();
	Scene.Light->SetActorRotation(Scene.Light->GetActorRotation() + FRotator(3.0, 7.0, 0.0));
	Scene.TickTestWorld();
	if (!WaitForCGHSolver(*this, *Scene.Solver, &Scene) || !CheckCGHPointFocus(*this, Scene))
	{
		return false;
	}
	TestTrue(TEXT("Changing incident direction launches a new job"), Scene.Solver->JobId > BeforeDirectionJob);
	TestTrue(TEXT("Changing incident direction publishes wavefront compensation"), Scene.SLM->GetPhasePatternRevision() > BeforeDirectionRevision);

	const int64 BeforeTargetJob = Scene.Solver->JobId;
	const uint64 BeforeTargetRevision = Scene.SLM->GetPhasePatternRevision();
	Scene.Target->Parameters.InitialPhaseRad += 0.45;
	Scene.TickTestWorld();
	if (!WaitForCGHSolver(*this, *Scene.Solver, &Scene))
	{
		return false;
	}
	TestTrue(TEXT("A consumed target phase edit launches a new job"), Scene.Solver->JobId > BeforeTargetJob);
	TestTrue(TEXT("Auto solving publishes the changed phase"), Scene.SLM->GetPhasePatternRevision() > BeforeTargetRevision);
	if (!CheckCGHPointFocus(*this, Scene))
	{
		return false;
	}
	const uint64 BeforeCancelRevision = Scene.SLM->GetPhasePatternRevision();
	Scene.Target->Parameters.InitialPhaseRad += 0.25;
	Scene.TickTestWorld();
	const int64 CancelledJob = Scene.Solver->JobId;
	Scene.Solver->CancelSolve();
	for (int32 Poll = 0; Poll < 8; ++Poll)
	{
		FPlatformProcess::Sleep(0.001f);
		Scene.TickTestWorld();
	}
	TestTrue(TEXT("Automatic mode respects cancellation of unchanged input"), Scene.Solver->JobState == ECGHSolverJobState::Idle);
	TestEqual(TEXT("Cancellation does not trigger an automatic retry loop"), Scene.Solver->JobId, CancelledJob);
	TestEqual(TEXT("Cancelled automatic work retains the previous phase"), Scene.SLM->GetPhasePatternRevision(), BeforeCancelRevision);

	Scene.Workbench->Targets.Reset();
	Scene.TickTestWorld();
	TestTrue(TEXT("An unavailable target makes automatic solving fail"), Scene.Solver->JobState == ECGHSolverJobState::Failed);
	TestEqual(TEXT("Unavailable target preserves the last published phase"), Scene.SLM->GetPhasePatternRevision(), BeforeCancelRevision);
	Scene.Workbench->Targets = {Scene.Target};
	Scene.TickTestWorld();
	if (!WaitForCGHSolver(*this, *Scene.Solver, &Scene) || !CheckCGHPointFocus(*this, Scene))
	{
		return false;
	}
	TestTrue(TEXT("Restoring the same target resumes automatic solving"), Scene.Solver->JobId > CancelledJob);
	TestTrue(TEXT("Restored reference can reach ready again"), Scene.Solver->JobState == ECGHSolverJobState::Ready);

	const uint64 BeforeInvalidRevision = Scene.SLM->GetPhasePatternRevision();
	const double ValidWavelengthNm = Scene.Light->Parameters.WavelengthNm;
	Scene.Light->Parameters.WavelengthNm = std::numeric_limits<double>::quiet_NaN();
	Scene.TickTestWorld();
	TestTrue(TEXT("A NaN wavelength is rejected by automatic solving"), Scene.Solver->JobState == ECGHSolverJobState::Failed);
	const int64 InvalidJob = Scene.Solver->JobId;
	for (int32 Tick = 0; Tick < 8; ++Tick)
	{
		Scene.TickTestWorld();
	}
	TestEqual(TEXT("An unchanged NaN input does not launch a new failed request every tick"), Scene.Solver->JobId, InvalidJob);
	TestEqual(TEXT("Invalid numerical input preserves the published phase"), Scene.SLM->GetPhasePatternRevision(), BeforeInvalidRevision);
	Scene.Light->Parameters.WavelengthNm = ValidWavelengthNm;
	Scene.TickTestWorld();
	if (!WaitForCGHSolver(*this, *Scene.Solver, &Scene))
	{
		return false;
	}
	TestTrue(TEXT("Correcting invalid numerical input resumes automatic solving"), Scene.Solver->JobId > InvalidJob);
	Scene.ForwardErrorMessages(this);
	return CheckCGHPointFocus(*this, Scene);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSolverMovePublicationTest,
	"CGH.SolverActor.MovePublicationPreservesOwnershipAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSolverMovePublicationTest::RunTest(const FString& Parameters)
{
	FCGHSolverTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.PublishSentinel(*this))
	{
		return false;
	}
	const uint64 BeforeRevision = Scene.SLM->GetPhasePatternRevision();
	FCGHSLMPhasePattern Incoming;
	Incoming.ResolutionX = Scene.SLM->Parameters.ResolutionX;
	Incoming.ResolutionY = Scene.SLM->Parameters.ResolutionY;
	Incoming.PhaseRad.Init(1.75, Incoming.ResolutionX * Incoming.ResolutionY);
	Incoming.Revision = MAX_uint64 - 17;
	const double* WorkerBuffer = Incoming.PhaseRad.GetData();
	if (!TestTrue(TEXT("A complete owned worker output can be published"), Scene.SLM->SetPhasePattern(MoveTemp(Incoming))))
	{
		return false;
	}
	TestTrue(TEXT("Publication transfers the worker allocation directly to the SLM"),
		Scene.SLM->GetPhasePattern().PhaseRad.GetData() == WorkerBuffer);
	TestTrue(TEXT("Moved worker output relinquishes its phase allocation"), Incoming.PhaseRad.IsEmpty());
	TestEqual(TEXT("Incoming revision is ignored in favor of the SLM's next revision"),
		Scene.SLM->GetPhasePatternRevision(), BeforeRevision + 1);
	TestEqual(TEXT("Moved samples are intact"), Scene.SLM->GetPhasePattern().PhaseRad[0], 1.75);
	const uint64 PublishedRevision = Scene.SLM->GetPhasePatternRevision();

	FCGHSLMPhasePattern Identical = Scene.SLM->GetPhasePattern();
	Identical.Revision = 998;
	TestTrue(TEXT("An identical owned pattern is accepted"), Scene.SLM->SetPhasePattern(MoveTemp(Identical)));
	TestEqual(TEXT("Identical owned publication preserves the SLM revision"), Scene.SLM->GetPhasePatternRevision(), PublishedRevision);
	TestTrue(TEXT("Identical owned publication preserves the existing allocation"),
		Scene.SLM->GetPhasePattern().PhaseRad.GetData() == WorkerBuffer);

	for (int32 InvalidCase = 0; InvalidCase < 3; ++InvalidCase)
	{
		FCGHSLMPhasePattern Invalid = Scene.SLM->GetPhasePattern();
		if (InvalidCase == 0)
		{
			Invalid.ResolutionX += 1;
			Invalid.PhaseRad.Init(0.5, Invalid.ResolutionX * Invalid.ResolutionY);
		}
		else if (InvalidCase == 1)
		{
			Invalid.PhaseRad.Pop();
		}
		else
		{
			Invalid.PhaseRad[0] = std::numeric_limits<double>::quiet_NaN();
		}
		const double* InvalidBuffer = Invalid.PhaseRad.GetData();
		TestFalse(TEXT("Mismatched, incomplete, or nonfinite owned output is rejected"),
			Scene.SLM->SetPhasePattern(MoveTemp(Invalid)));
		TestFalse(TEXT("Rejected owned output supplies a diagnostic"), Scene.SLM->PhasePatternError.IsEmpty());
		TestEqual(TEXT("Rejected owned output preserves the previous revision"), Scene.SLM->GetPhasePatternRevision(), PublishedRevision);
		TestTrue(TEXT("Rejected owned output preserves the previous allocation"),
			Scene.SLM->GetPhasePattern().PhaseRad.GetData() == WorkerBuffer);
		TestEqual(TEXT("Rejected owned output preserves the previous samples"), Scene.SLM->GetPhasePattern().PhaseRad[0], 1.75);
		TestTrue(TEXT("Validation happens before taking ownership of rejected output"), Invalid.PhaseRad.GetData() == InvalidBuffer);
	}

	FCGHSLMPhasePattern Recovery = Scene.SLM->GetPhasePattern();
	Recovery.PhaseRad[0] = 2.25;
	const double* RecoveryBuffer = Recovery.PhaseRad.GetData();
	TestTrue(TEXT("Valid owned output can recover from publication errors"), Scene.SLM->SetPhasePattern(MoveTemp(Recovery)));
	TestTrue(TEXT("Recovery clears the publication error"), Scene.SLM->PhasePatternError.IsEmpty());
	TestEqual(TEXT("Recovery advances the SLM revision once"), Scene.SLM->GetPhasePatternRevision(), PublishedRevision + 1);
	TestTrue(TEXT("Recovery also transfers the allocation without copying"), Scene.SLM->GetPhasePattern().PhaseRad.GetData() == RecoveryBuffer);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSolverDestroyedActorTest,
	"CGH.SolverActor.DestroyedActorAndWorldDoNotPublishWorkers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSolverDestroyedActorTest::RunTest(const FString& Parameters)
{
	FCGHSolverTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.PublishSentinel(*this) || !Scene.BeginPlayInTestWorld())
	{
		Scene.ForwardErrorMessages(this);
		return false;
	}
	const uint64 BeforeRevision = Scene.SLM->GetPhasePatternRevision();
	TestTrue(TEXT("Request starts before removal from a streamed level"), Scene.Solver->StartSolve());
	Scene.Solver->RouteEndPlay(EEndPlayReason::RemovedFromWorld);
	TestTrue(TEXT("EndPlay clears the public running state along with the worker buffers"),
		Scene.Solver->JobState == ECGHSolverJobState::Idle);
	TestEqual(TEXT("EndPlay preserves the published SLM phase"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
	TestTrue(TEXT("Request starts before actor destruction"), Scene.Solver->StartSolve());
	TestTrue(TEXT("An active solver actor can be destroyed"), Scene.Solver->Destroy());
	Scene.Solver = nullptr;
	// Let any completed task finish while the surviving world ticks its actors.
	for (int32 Tick = 0; Tick < 4; ++Tick)
	{
		FPlatformProcess::Sleep(0.001f);
		Scene.TickTestWorld();
	}
	TestEqual(TEXT("A destroyed solver cannot publish to the surviving SLM"), Scene.SLM->GetPhasePatternRevision(), BeforeRevision);
	{
		FCGHSolverTestScene ShortLivedScene;
		if (!ShortLivedScene.Initialize(*this) || !ShortLivedScene.BeginPlayInTestWorld())
		{
			ShortLivedScene.ForwardErrorMessages(this);
			return false;
		}
		TestTrue(TEXT("Request starts immediately before its world is torn down"), ShortLivedScene.Solver->StartSolve());
	}
	// Exercise task completion after teardown without accessing destroyed UObjects.
	FPlatformProcess::Sleep(0.005f);
	Scene.TickTestWorld();
	Scene.ForwardErrorMessages(this);
	return true;
}

#endif
