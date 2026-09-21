#include "CGH/Solver/CGHDockerSolverBackend.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHSolverActor.h"
#include "CGH/Actors/CGHTargetActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "CGH/Solver/CGHPointFocus.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/AutomationCommon.h"

#include <cmath>

namespace
{
	constexpr double CudaParityTolerance = 1.0e-7;

	int32 CudaTestPort(FAutomationTestBase& Test)
	{
		int32 Port = 0;
		if (!FParse::Value(FCommandLine::Get(), TEXT("CGHCudaTestPort="), Port))
		{
			Test.AddWarning(TEXT("SKIPPED real GPU parity; supply -CGHCudaTestPort=<published CUDA server port> to run it."));
			return 0;
		}
		if (!Test.TestTrue(TEXT("CGHCudaTestPort is a valid TCP port"), Port > 0 && Port <= 65535))
		{
			return -1;
		}
		return Port;
	}

	void CudaSetGrid(FCGHSolverInput& Input, int32 Width, int32 Height)
	{
		FCGHSLMDescription& SLM = Input.Scene.SLM;
		SLM.ResolutionX = Width;
		SLM.ResolutionY = Height;
		SLM.ActiveWidthM = Width * SLM.PixelPitchXM;
		SLM.ActiveHeightM = Height * SLM.PixelPitchYM;
	}

	FCGHSolverInput CudaReferenceInput()
	{
		FCGHSolverInput Input;
		Input.Scene.SLM.PixelPitchXM = 8.0e-6;
		Input.Scene.SLM.PixelPitchYM = 13.0e-6;
		CudaSetGrid(Input, 7, 5);
		Input.Scene.ReconstructionLight.WavelengthM = 532.123456789e-9;
		Input.Scene.ReconstructionLight.Amplitude = 1.0;
		Input.Scene.ReconstructionLight.DirectionSLM = FVector::ForwardVector;
		FCGHTargetDescription Target;
		Target.ResourceId = 1;
		Target.Revision = 1;
		Target.PositionSLMM = FVector(0.021, 0.0017, -0.003);
		Target.Amplitude = 1.0;
		Target.PhaseRad = 1.234;
		Input.Scene.Targets.Add(Target);
		return Input;
	}

	FCGHSolverInput CudaMultipleInput()
	{
		FCGHSolverInput Input = CudaReferenceInput();
		FCGHTargetDescription Second = Input.Scene.Targets[0];
		Second.ResourceId = 2;
		Second.PositionSLMM = FVector(-0.033, -0.0021, 0.0007);
		Second.Amplitude = 0.37;
		Second.PhaseRad = -0.73;
		Input.Scene.Targets.Add(Second);
		FCGHTargetDescription Third = Second;
		Third.ResourceId = 3;
		Third.PositionSLMM = FVector(0.056, 0.0034, 0.0019);
		Third.Amplitude = 1.9;
		Third.PhaseRad = 2.39;
		Input.Scene.Targets.Add(Third);
		return Input;
	}

	FCGHSolverInput CudaMeshInput()
	{
		FCGHSolverInput Input = CudaMultipleInput();
		Input.Scene.ReconstructionLight.DirectionSLM = FVector(std::sqrt(0.75), 0.3, -0.4);
		Input.Scene.ReconstructionLight.InitialPhaseRad = 2.13;
		FCGHTargetDescription Mesh;
		Mesh.TargetType = ECGHTargetType::Mesh;
		Mesh.ResourceId = 400;
		Mesh.Revision = 7;
		Mesh.PositionSLMM = FVector(0.042, -0.004, 0.0017);
		Mesh.RotationSLM = FQuat(FVector(0.3, -0.4, 0.5).GetSafeNormal(), 0.63);
		// These are intentionally different from samples: amplitudes/phases are already baked into samples.
		Mesh.Amplitude = 53.0;
		Mesh.PhaseRad = -8.0;
		Input.Scene.Targets.Add(Mesh);
		FCGHPointCloudResource Cloud;
		Cloud.ResourceId = Mesh.ResourceId;
		Cloud.Revision = Mesh.Revision;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			FCGHObjectPoint Point;
			Point.PositionLocalM = FVector(0.0003 * Index, 0.0002 * (Index - 2), -0.0004 * (Index + 1));
			Point.NormalLocal = FVector3f(0.0f, 0.0f, 1.0f);
			Point.UV = FVector2f(Index / 4.0f, 0.75f);
			Point.Amplitude = Index == 0 ? 0.0 : 0.17 * Index;
			Point.Phase = -0.19 + 0.27 * Index;
			Cloud.Points.Add(Point);
		}
		Input.PointClouds.Add(Cloud);
		return Input;
	}

	UCGHDockerSolverBackend* CudaBackend(int32 Port)
	{
		UCGHDockerSolverBackend* Backend = NewObject<UCGHDockerSolverBackend>();
		Backend->Settings.Address = TEXT("127.0.0.1");
		Backend->Settings.Port = Port;
		Backend->Settings.ConnectTimeoutSeconds = 5.0;
		Backend->Settings.RequestTimeoutSeconds = 30.0;
		return Backend;
	}

	bool CudaWaitJob(FAutomationTestBase& Test, const TSharedPtr<FCGHSolverJob, ESPMode::ThreadSafe>& Job)
	{
		if (!Test.TestTrue(TEXT("CUDA transport returns an owned job"), Job.IsValid()))
		{
			return false;
		}
		const double Deadline = FPlatformTime::Seconds() + 35.0;
		while (!Job->bFinished.load(std::memory_order_acquire))
		{
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Job->bCancelRequested.store(true, std::memory_order_relaxed);
				Test.AddError(TEXT("CUDA job exceeded the client deadline."));
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return true;
	}

	bool CudaComparePattern(FAutomationTestBase& Test, const TCHAR* Label,
		const FCGHSLMPhasePattern& Actual, const FCGHSLMPhasePattern& Expected)
	{
		if (!Test.TestEqual(TEXT("CUDA preserves reference width"), Actual.ResolutionX, Expected.ResolutionX)
			|| !Test.TestEqual(TEXT("CUDA preserves reference height"), Actual.ResolutionY, Expected.ResolutionY)
			|| !Test.TestEqual(TEXT("CUDA preserves reference phase count"), Actual.PhaseRad.Num(), Expected.PhaseRad.Num()))
		{
			return false;
		}
		double MaximumError = 0.0;
		for (int32 Index = 0; Index < Actual.PhaseRad.Num(); ++Index)
		{
			const double Phase = Actual.PhaseRad[Index];
			if (!Test.TestTrue(TEXT("Received CUDA phases are finite and canonical"),
				std::isfinite(Phase) && Phase >= 0.0 && Phase < 2.0 * UE_DOUBLE_PI))
			{
				return false;
			}
			MaximumError = FMath::Max(MaximumError, std::abs(std::remainder(Phase - Expected.PhaseRad[Index], 2.0 * UE_DOUBLE_PI)));
		}
		Test.AddInfo(FString::Printf(TEXT("%s: maximum wrapped CUDA/reference error %.17g rad (tolerance %.1g)."),
			Label, MaximumError, CudaParityTolerance));
		return Test.TestTrue(FString::Printf(TEXT("%s matches the unchanged CGHPointFocus reference"), Label),
			MaximumError <= CudaParityTolerance);
	}

	bool CudaCompareCase(FAutomationTestBase& Test, UCGHDockerSolverBackend& Backend,
		const TCHAR* Label, FCGHSolverInput Input)
	{
		const std::atomic<bool> NotCancelled{false};
		const FCGHSolverResult Reference = CGHPointFocus::Solve(Input, NotCancelled);
		if (!Test.TestTrue(FString::Printf(TEXT("%s reference succeeds"), Label), Reference.bSucceeded))
		{
			Test.AddError(Reference.Error);
			return false;
		}
		const auto Job = Backend.Submit(MoveTemp(Input));
		if (!CudaWaitJob(Test, Job))
		{
			return false;
		}
		if (!Test.TestTrue(FString::Printf(TEXT("%s CUDA succeeds: %s"), Label, *Job->Result.Error), Job->Result.bSucceeded)
			|| !Test.TestFalse(TEXT("Numerical parity must come from PointFocusSuccess, never DummySuccess"), Job->Result.bIsDummy))
		{
			return false;
		}
		Test.TestTrue(TEXT("CUDA result preserves the propagation convention"), Job->Result.PropagationConvention == Reference.PropagationConvention);
		Test.TestTrue(TEXT("CUDA compute time is finite and nonnegative"), std::isfinite(Job->Result.ComputeSeconds) && Job->Result.ComputeSeconds >= 0.0);
		return CudaComparePattern(Test, Label, Job->Result.Pattern, Reference.Pattern);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCudaReferenceParityTest,
	"CGH.CudaBackend.ReferenceParity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHCudaReferenceParityTest::RunTest(const FString& Parameters)
{
	const int32 Port = CudaTestPort(*this);
	if (Port <= 0)
	{
		return Port == 0;
	}
	UCGHDockerSolverBackend* Backend = CudaBackend(Port);
	bool bPassed = CudaCompareCase(*this, *Backend, TEXT("Single off-axis anisotropic point"), CudaReferenceInput());
	for (const FIntPoint Grid : {FIntPoint(4, 2), FIntPoint(1, 1), FIntPoint(1, 5), FIntPoint(6, 1)})
	{
		FCGHSolverInput Input = CudaReferenceInput();
		CudaSetGrid(Input, Grid.X, Grid.Y);
		Input.Scene.Targets[0].PositionSLMM.X *= -1.0;
		bPassed &= CudaCompareCase(*this, *Backend, TEXT("Even/odd/singleton grid with negative depth"), MoveTemp(Input));
	}
	bPassed &= CudaCompareCase(*this, *Backend, TEXT("Asymmetric complex field from three points"), CudaMultipleInput());
	FCGHSolverInput Batched = CudaReferenceInput();
	CudaSetGrid(Batched, 129, 65); // Cross the 8192-pixel CUDA tile boundary as well as source batches.
	const FCGHTargetDescription BatchFirst = Batched.Scene.Targets[0];
	for (int32 Index = 1; Index < 513; ++Index)
	{
		FCGHTargetDescription Target = BatchFirst;
		Target.ResourceId = static_cast<uint64>(Index + 1);
		Target.PositionSLMM.Y += 1.0e-6 * Index;
		Target.PositionSLMM.Z -= 0.5e-6 * Index;
		Target.Amplitude = 0.03 + 0.005 * (Index % 7);
		Target.PhaseRad = 0.37 * (Index % 19);
		Batched.Scene.Targets.Add(Target);
	}
	bPassed &= CudaCompareCase(*this, *Backend, TEXT("Compensated sum across multiple ordered emitter batches"), MoveTemp(Batched));
	FCGHSolverInput Oblique = CudaMultipleInput();
	Oblique.Scene.ReconstructionLight.DirectionSLM = FVector(std::sqrt(0.75), 0.3, -0.4);
	Oblique.Scene.ReconstructionLight.InitialPhaseRad = 2.13;
	Oblique.Scene.ReconstructionLight.Amplitude = 0.0;
	Oblique.Scene.ReconstructionLight.PolarizationAngleRad = 1.37;
	Oblique.Scene.ReconstructionLight.PositionSLMM = FVector(11.0, -23.0, 17.0);
	bPassed &= CudaCompareCase(*this, *Backend, TEXT("Oblique plane wave with ignored scalar-light fields"), MoveTemp(Oblique));
	bPassed &= CudaCompareCase(*this, *Backend, TEXT("Rigid mesh pose with baked per-sample fields and points"), CudaMeshInput());
	FCGHSolverInput LargeAmplitude = CudaMultipleInput();
	for (FCGHTargetDescription& Target : LargeAmplitude.Scene.Targets)
	{
		Target.Amplitude *= 1.0e300;
	}
	bPassed &= CudaCompareCase(*this, *Backend, TEXT("Global amplitude normalization avoids overflow"), MoveTemp(LargeAmplitude));
	FCGHSolverInput ZeroContributor = CudaReferenceInput();
	FCGHTargetDescription Zero = ZeroContributor.Scene.Targets[0];
	Zero.ResourceId = 2;
	Zero.PositionSLMM = FVector::ZeroVector;
	Zero.Amplitude = 0.0;
	Zero.PhaseRad = -12.4;
	ZeroContributor.Scene.Targets.Add(Zero);
	bPassed &= CudaCompareCase(*this, *Backend, TEXT("Zero-amplitude source on the SLM plane is ignored"), MoveTemp(ZeroContributor));
	FCGHSolverInput Destructive = CudaReferenceInput();
	Destructive.Scene.Targets[0].PhaseRad = 0.0;
	FCGHTargetDescription Opposite = Destructive.Scene.Targets[0];
	Opposite.ResourceId = 2;
	Opposite.PhaseRad = UE_DOUBLE_PI;
	Destructive.Scene.Targets.Add(Opposite);
	bPassed &= CudaCompareCase(*this, *Backend, TEXT("Destructive interference follows the deterministic zero rule"), MoveTemp(Destructive));
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCudaFailureAndCancellationTest,
	"CGH.CudaBackend.ValidationCancellationAndRecovery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHCudaFailureAndCancellationTest::RunTest(const FString& Parameters)
{
	const int32 Port = CudaTestPort(*this);
	if (Port <= 0)
	{
		return Port == 0;
	}
	UCGHDockerSolverBackend* Backend = CudaBackend(Port);
	const std::atomic<bool> NotCancelled{false};
	for (int32 InvalidCase = 0; InvalidCase < 7; ++InvalidCase)
	{
		FCGHSolverInput Input = InvalidCase == 5 ? CudaMeshInput() : CudaReferenceInput();
		switch (InvalidCase)
		{
		case 0: Input.Scene.Targets[0].PositionSLMM.X = 0.0; break;
		case 1: Input.Scene.ReconstructionLight.DirectionSLM = FVector(2.0, 0.0, 0.0); break;
		case 2: Input.Scene.ReconstructionLight.SourceType = ECGHSourceType::PointSource; break;
		case 3: Input.Scene.SLM.ModulationType = ECGHSLMModulationType::Complex; break;
		case 4: Input.Scene.Targets[0].Amplitude = 0.0; break;
		case 5: Input.Scene.Targets.Last().RotationSLM = FQuat(0.0, 0.0, 0.0, 2.0); break;
		case 6: Input.Scene.Targets[0].PhaseRad = 1.7e308; Input.Scene.ReconstructionLight.InitialPhaseRad = -1.7e308; break;
		}
		const FCGHSolverResult Reference = CGHPointFocus::Solve(Input, NotCancelled);
		TestFalse(TEXT("The unchanged reference rejects this numerical input"), Reference.bSucceeded);
		const auto Job = Backend->Submit(MoveTemp(Input));
		if (!CudaWaitJob(*this, Job))
		{
			return false;
		}
		TestFalse(FString::Printf(TEXT("CUDA rejects invalid numerical case %d"), InvalidCase), Job->Result.bSucceeded);
		TestTrue(TEXT("Validation failure comes back from the remote solver"), Job->Result.Error.StartsWith(TEXT("Docker solver server:")));
		TestTrue(TEXT("Failed remote solvers never expose a partial pattern"), Job->Result.Pattern.PhaseRad.IsEmpty());
		TestEqual(TEXT("Failed remote solvers reset output dimensions"), Job->Result.Pattern.ResolutionX, 0);
	}

	// Enough ordered source work to remain active while the client requests cancellation.
	FCGHSolverInput Heavy = CudaReferenceInput();
	CudaSetGrid(Heavy, 512, 512);
	const FCGHTargetDescription First = Heavy.Scene.Targets[0];
	for (int32 Index = 1; Index < 32768; ++Index)
	{
		FCGHTargetDescription Target = First;
		Target.ResourceId = static_cast<uint64>(Index + 1);
		Target.PositionSLMM.Y += 1.0e-7 * Index;
		Target.PhaseRad += 0.01 * (Index % 17);
		Heavy.Scene.Targets.Add(Target);
	}
	const auto Cancelled = Backend->Submit(MoveTemp(Heavy));
	FPlatformProcess::Sleep(0.1f);
	const double CancelStart = FPlatformTime::Seconds();
	TestFalse(TEXT("The cancellation fixture has active work"), Cancelled->bFinished.load(std::memory_order_acquire));
	Cancelled->bCancelRequested.store(true, std::memory_order_relaxed);
	if (!CudaWaitJob(*this, Cancelled))
	{
		return false;
	}
	TestTrue(TEXT("Cancellation releases the client independently of GPU completion"), FPlatformTime::Seconds() - CancelStart < 1.0);
	TestFalse(TEXT("Cancelled CUDA jobs cannot succeed"), Cancelled->Result.bSucceeded);
	TestTrue(TEXT("Cancelled CUDA jobs have an explicit diagnostic"), Cancelled->Result.Error.Contains(TEXT("cancelled")));
	TestTrue(TEXT("Cancelled CUDA jobs expose no partial output"), Cancelled->Result.Pattern.PhaseRad.IsEmpty());
	return CudaCompareCase(*this, *Backend, TEXT("CUDA service recovers after failures and cancellation"), CudaReferenceInput());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCudaActorPublicationTest,
	"CGH.CudaBackend.ActorToSLM", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHCudaActorPublicationTest::RunTest(const FString& Parameters)
{
	const int32 Port = CudaTestPort(*this);
	if (Port <= 0)
	{
		return Port == 0;
	}
	FTestWorldWrapper World;
	if (!World.CreateTestWorld(EWorldType::Game))
	{
		World.ForwardErrorMessages(this);
		return false;
	}
	ACGHSLMActor* SLM = World.GetTestWorld()->SpawnActor<ACGHSLMActor>();
	ACGHReconstructionLightActor* Light = World.GetTestWorld()->SpawnActor<ACGHReconstructionLightActor>();
	ACGHTargetActor* First = World.GetTestWorld()->SpawnActor<ACGHTargetActor>();
	ACGHTargetActor* Second = World.GetTestWorld()->SpawnActor<ACGHTargetActor>();
	ACGHWorkbenchActor* Workbench = World.GetTestWorld()->SpawnActor<ACGHWorkbenchActor>();
	ACGHSolverActor* Solver = World.GetTestWorld()->SpawnActor<ACGHSolverActor>();
	if (!SLM || !Light || !First || !Second || !Workbench || !Solver)
	{
		AddError(TEXT("Could not spawn CUDA actor publication fixture."));
		return false;
	}
	SLM->Parameters.ResolutionX = 7;
	SLM->Parameters.ResolutionY = 5;
	SLM->Parameters.PixelPitchXUm = 8.0;
	SLM->Parameters.PixelPitchYUm = 13.0;
	Light->Parameters.WavelengthNm = 532.123456789;
	Light->Parameters.InitialPhaseRad = 0.91;
	Light->SetActorRotation(FRotator(12.0, 23.0, 0.0));
	First->SetActorLocation(FVector(2.1, 0.17, -0.3));
	First->Parameters.InitialPhaseRad = 1.234;
	Second->SetActorLocation(FVector(-3.3, -0.21, 0.07));
	Second->Parameters.Amplitude = 0.37;
	Second->Parameters.InitialPhaseRad = -0.73;
	Workbench->SLM = SLM;
	Workbench->ReconstructionLight = Light;
	Workbench->Targets = {First, Second};
	Workbench->Solver = Solver;
	Solver->Workbench = Workbench;
	Solver->Parameters.SolverBackend = ECGHSolverBackend::Docker;
	Solver->Parameters.Docker.Port = Port;
	Workbench->UpdateSceneDescription();
	FCGHSolverInput Snapshot;
	Snapshot.Scene = Workbench->SceneDescription;
	const std::atomic<bool> NotCancelled{false};
	const FCGHSolverResult Reference = CGHPointFocus::Solve(Snapshot, NotCancelled);
	if (!TestTrue(TEXT("The actor's exact owned scene succeeds on the reference"), Reference.bSucceeded))
	{
		AddError(Reference.Error);
		return false;
	}
	FCGHSLMPhasePattern Sentinel;
	Sentinel.ResolutionX = 7;
	Sentinel.ResolutionY = 5;
	Sentinel.PhaseRad.Init(0.125, 35);
	if (!TestTrue(TEXT("An existing SLM pattern is available"), SLM->SetPhasePattern(MoveTemp(Sentinel))))
	{
		return false;
	}
	const uint64 Revision = SLM->GetPhasePatternRevision();
	if (!TestTrue(TEXT("Actor queues CUDA through the Docker backend"), Solver->StartSolve()))
	{
		AddError(Solver->StatusMessage);
		return false;
	}
	TestEqual(TEXT("Reception cannot publish before game-thread polling"), SLM->GetPhasePatternRevision(), Revision);
	const double Deadline = FPlatformTime::Seconds() + 35.0;
	while (Solver->JobState == ECGHSolverJobState::Queued || Solver->JobState == ECGHSolverJobState::Running)
	{
		Solver->PollSolver();
		if (FPlatformTime::Seconds() >= Deadline)
		{
			Solver->CancelSolve();
			AddError(TEXT("CUDA actor publication exceeded its deadline."));
			return false;
		}
		FPlatformProcess::Sleep(0.001f);
	}
	if (!TestTrue(FString::Printf(TEXT("CUDA result reaches Ready: %s"), *Solver->StatusMessage), Solver->JobState == ECGHSolverJobState::Ready))
	{
		return false;
	}
	TestTrue(TEXT("The actor identifies the actual CUDA result"), Solver->StatusMessage.Contains(TEXT("CUDA PointFocus")));
	TestFalse(TEXT("A CUDA result is not labelled as a dummy"), Solver->StatusMessage.Contains(TEXT("dummy")));
	TestEqual(TEXT("PollSolver publishes the received phase exactly once"), SLM->GetPhasePatternRevision(), Revision + 1);
	return CudaComparePattern(*this, TEXT("Actor -> Docker backend -> CUDA server -> job -> PollSolver -> SLM"), SLM->GetPhasePattern(), Reference.Pattern);
}

#endif // WITH_DEV_AUTOMATION_TESTS
