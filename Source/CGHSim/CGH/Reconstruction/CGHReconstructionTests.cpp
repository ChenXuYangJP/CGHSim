#include "CGH/Reconstruction/CGHReconstruction.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Reconstruction/CGHCPUReconstructionBackend.h"
#include "CGH/Reconstruction/CGHDockerReconstructionBackend.h"
#include "CGH/Solver/CGHPointFocus.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include <complex>
#include <limits>

namespace
{
	constexpr double ReconstructionTestTwoPi = 2.0 * UE_DOUBLE_PI;

	FCGHReconstructionInput MakeInput()
	{
		FCGHReconstructionInput I;
		I.SLM.ResolutionX = I.SLM.ResolutionY = 1;
		I.SLM.PixelPitchXM = 0.003;
		I.SLM.PixelPitchYM = 0.005;
		I.Light.WavelengthM = 0.2;
		I.Light.Amplitude = 1.7;
		I.Light.InitialPhaseRad = 0.31;
		I.Light.DirectionSLM = FVector(1.0, 0.0, 0.0);
		I.ObserverPlane.ResolutionX = I.ObserverPlane.ResolutionY = 1;
		I.ObserverPlane.PixelPitchXM = 0.01;
		I.ObserverPlane.PixelPitchYM = 0.02;
		I.ObserverPlane.PositionSLMM = FVector(0.7, 0.0, 0.0);
		I.Pattern.ResolutionX = I.Pattern.ResolutionY = 1;
		I.Pattern.PhaseRad = {0.73};
		return I;
	}

	std::complex<double> Complex(const FCGHComplexSample& Sample)
	{
		return {Sample.Real, Sample.Imaginary};
	}

	/** Independent std::complex evaluation of the Green-function normal derivative. */
	std::complex<double> ReferenceField(const FCGHReconstructionInput& I, const FVector& Q)
	{
		const double K = ReconstructionTestTwoPi / I.Light.WavelengthM;
		const double Area = I.SLM.PixelPitchXM * I.SLM.PixelPitchYM;
		std::complex<double> Sum(0.0, 0.0);
		for (int32 Row = 0; Row < I.SLM.ResolutionY; ++Row)
		{
			for (int32 Column = 0; Column < I.SLM.ResolutionX; ++Column)
			{
				const FVector P(0.0,
					(Column - 0.5 * (I.SLM.ResolutionX - 1)) * I.SLM.PixelPitchXM,
					(0.5 * (I.SLM.ResolutionY - 1) - Row) * I.SLM.PixelPitchYM);
				double Amplitude = I.Light.Amplitude;
				double IncidentPhase = I.Light.InitialPhaseRad;
				if (I.Light.SourceType == ECGHSourceType::PointSource)
				{
					const double SourceDistance = FVector::Distance(I.Light.PositionSLMM, P);
					Amplitude /= SourceDistance;
					IncidentPhase += K * SourceDistance;
				}
				else
				{
					IncidentPhase += K * FVector::DotProduct(I.Light.DirectionSLM, P);
				}
				const double R = FVector::Distance(Q, P);
				const std::complex<double> Kernel = Area * Q.X / (ReconstructionTestTwoPi * R * R * R)
					* std::complex<double>(1.0, -K * R) * std::exp(std::complex<double>(0.0, K * R));
				Sum += std::polar(Amplitude, IncidentPhase + I.Pattern.PhaseRad[Row * I.SLM.ResolutionX + Column]) * Kernel;
			}
		}
		return Sum;
	}

	bool WaitForJob(FAutomationTestBase& Test, const TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe>& Job)
	{
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		while (!Job->bFinished.load(std::memory_order_acquire))
		{
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Job->bCancelRequested.store(true, std::memory_order_relaxed);
				Test.AddError(TEXT("Reconstruction job did not finish within the test deadline."));
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructionSinglePixelTest,
	"CGH.Reconstruction.SinglePixelAnalyticKernel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructionSinglePixelTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> Cancelled{false};
	const FCGHReconstructionInput I = MakeInput();
	const FCGHReconstructionResult Result = CGHReconstruction::Reconstruct(I, Cancelled);
	if (!TestTrue(TEXT("Single pixel reconstruction succeeds"), Result.bSucceeded))
	{
		AddError(Result.Error);
		return false;
	}
	const double X = I.ObserverPlane.PositionSLMM.X;
	const double K = ReconstructionTestTwoPi / I.Light.WavelengthM;
	const std::complex<double> Expected = I.Light.Amplitude * I.SLM.PixelPitchXM * I.SLM.PixelPitchYM / (ReconstructionTestTwoPi * X * X)
		* std::complex<double>(1.0, -K * X) * std::polar(1.0, I.Light.InitialPhaseRad + I.Pattern.PhaseRad[0] + K * X);
	TestTrue(TEXT("Area weighting, attenuation and near-field term match analytical complex field"),
		std::abs(Complex(Result.Field.Samples[0]) - Expected) < 1.0e-16);
	TestTrue(TEXT("Result preserves the solver propagation convention"), Result.PropagationConvention == ECGHPropagationConvention::ExpPositiveIKR);
	TestTrue(TEXT("Compute duration is finite and nonnegative"), FMath::IsFinite(Result.ComputeSeconds) && Result.ComputeSeconds >= 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructionTiltedPlaneTest,
	"CGH.Reconstruction.TiltedObserverAndObliqueIllumination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructionTiltedPlaneTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> Cancelled{false};
	FCGHReconstructionInput I = MakeInput();
	I.SLM.ResolutionX = I.Pattern.ResolutionX = 4;
	I.SLM.ResolutionY = I.Pattern.ResolutionY = 3;
	I.Pattern.PhaseRad = {0.0, 0.1, 0.3, 0.7, 0.6, -0.1, 0.31, 2.13, -1.2, 0.9, 2.5, -0.3};
	I.Light.DirectionSLM = FVector(0.8, 0.36, 0.48);
	I.ObserverPlane.ResolutionX = 4;
	I.ObserverPlane.ResolutionY = 3;
	I.ObserverPlane.PositionSLMM = FVector(0.47, 0.03, -0.01);
	I.ObserverPlane.RotationSLM = FRotator(21.0, -37.0, 13.0).Quaternion();
	const FCGHReconstructionResult Result = CGHReconstruction::Reconstruct(I, Cancelled);
	if (!TestTrue(TEXT("Tilted displaced receiver reconstructs"), Result.bSucceeded))
	{
		AddError(Result.Error);
		return false;
	}
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Column = 0; Column < 4; ++Column)
		{
			const FVector Local(0.0, (Column - 1.5) * 0.01, (1.0 - Row) * 0.02);
			const FVector Q = I.ObserverPlane.PositionSLMM + I.ObserverPlane.RotationSLM.RotateVector(Local);
			TestTrue(TEXT("Each row-major sample matches independently transformed centered coordinates"),
				std::abs(Complex(Result.Field.Samples[Row * 4 + Column]) - ReferenceField(I, Q)) < 1.0e-14);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructionIlluminationTest,
	"CGH.Reconstruction.PointSourceAndAmplitudeLinearity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructionIlluminationTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> Cancelled{false};
	FCGHReconstructionInput I = MakeInput();
	I.Light.SourceType = ECGHSourceType::PointSource;
	I.Light.PositionSLMM = FVector(-2.0, 0.3, 0.2);
	I.Light.DirectionSLM = FVector::ZeroVector;
	const FCGHReconstructionResult Reference = CGHReconstruction::Reconstruct(I, Cancelled);
	if (!TestTrue(TEXT("Point source reconstructs successfully"), Reference.bSucceeded))
	{
		AddError(Reference.Error);
		return false;
	}
	TestTrue(TEXT("Point source uses 1m/r amplitude and positive propagation phase"),
		std::abs(Complex(Reference.Field.Samples[0]) - ReferenceField(I, I.ObserverPlane.PositionSLMM)) < 1.0e-16);
	I.Light.Amplitude *= 3.0;
	const FCGHReconstructionResult Tripled = CGHReconstruction::Reconstruct(I, Cancelled);
	if (TestTrue(TEXT("Scaled illumination reconstructs"), Tripled.bSucceeded))
	{
		TestTrue(TEXT("Stored complex field preserves linear amplitude scaling"),
			std::abs(Complex(Tripled.Field.Samples[0]) - 3.0 * Complex(Reference.Field.Samples[0])) < 1.0e-16);
	}
	I.Light.Amplitude = 0.0;
	const FCGHReconstructionResult Dark = CGHReconstruction::Reconstruct(I, Cancelled);
	if (TestTrue(TEXT("Zero illumination is a valid dark field"), Dark.bSucceeded))
	{
		TestEqual(TEXT("Dark field is exactly zero"), std::abs(Complex(Dark.Field.Samples[0])), 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructionFocusTest,
	"CGH.Reconstruction.SolverPatternProducesCoherentFocus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructionFocusTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> Cancelled{false};
	FCGHReconstructionInput I = MakeInput();
	I.SLM.ResolutionX = 32;
	I.SLM.ResolutionY = 24;
	I.SLM.PixelPitchXM = I.SLM.PixelPitchYM = 8.0e-6;
	I.Light.WavelengthM = 633.0e-9;
	I.Light.InitialPhaseRad = 1.27;
	I.Light.DirectionSLM = FVector(0.8, 0.6, 0.0);
	I.ObserverPlane.ResolutionX = 5;
	I.ObserverPlane.ResolutionY = 3;
	I.ObserverPlane.PixelPitchXM = I.ObserverPlane.PixelPitchYM = 1.0e-3;
	I.ObserverPlane.PositionSLMM = FVector(0.12, 0.001, -0.002);
	FCGHSolverInput SolverInput;
	SolverInput.Scene.SLM = I.SLM;
	SolverInput.Scene.ReconstructionLight = I.Light;
	FCGHTargetDescription Target;
	Target.PositionSLMM = I.ObserverPlane.PositionSLMM;
	Target.Amplitude = 1.0;
	Target.PhaseRad = 0.73;
	SolverInput.Scene.Targets.Add(Target);
	FCGHSolverResult Solved = CGHPointFocus::Solve(SolverInput, Cancelled);
	if (!TestTrue(TEXT("Existing PointFocus solver creates the phase pattern"), Solved.bSucceeded))
	{
		AddError(Solved.Error);
		return false;
	}
	I.Pattern = MoveTemp(Solved.Pattern);
	const FCGHReconstructionResult Result = CGHReconstruction::Reconstruct(I, Cancelled);
	if (!TestTrue(TEXT("Solver phase reconstructs with matching illumination"), Result.bSucceeded))
	{
		AddError(Result.Error);
		return false;
	}
	std::complex<double> Expected(0.0, 0.0);
	const double K = ReconstructionTestTwoPi / I.Light.WavelengthM;
	const double Area = I.SLM.PixelPitchXM * I.SLM.PixelPitchYM;
	for (int32 Row = 0; Row < I.SLM.ResolutionY; ++Row)
	{
		for (int32 Column = 0; Column < I.SLM.ResolutionX; ++Column)
		{
			const FVector P(0.0, (Column - 15.5) * 8.0e-6, (11.5 - Row) * 8.0e-6);
			const double R = FVector::Distance(Target.PositionSLMM, P);
			Expected += I.Light.Amplitude * Area * Target.PositionSLMM.X / (ReconstructionTestTwoPi * R * R)
				* std::complex<double>(1.0 / R, -K) * std::polar(1.0, Target.PhaseRad);
		}
	}
	const std::complex<double> Center = Complex(Result.Field.Samples[7]);
	TestTrue(TEXT("Focus retains coherent amplitude and the negative imaginary RS kernel factor"),
		std::abs(Center - Expected) < 1.0e-8 * std::abs(Expected));
	const std::complex<double> RelativePhase = Center * std::polar(1.0, -Target.PhaseRad);
	TestTrue(TEXT("Far-field focus approaches -i times desired target field"),
		RelativePhase.imag() < 0.0 && std::abs(RelativePhase.real()) < 1.0e-5 * std::abs(RelativePhase.imag()));
	for (int32 Index = 0; Index < Result.Field.Samples.Num(); ++Index)
	{
		if (Index != 7)
		{
			TestTrue(TEXT("Focus is brighter than each off-focus sample"), std::abs(Center) > 3.0 * std::abs(Complex(Result.Field.Samples[Index])));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructionValidationTest,
	"CGH.Reconstruction.InvalidInputAndCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructionValidationTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> Cancelled{false};
	const auto Reject = [this, &Cancelled](const TCHAR* Label, auto Mutate)
	{
		FCGHReconstructionInput I = MakeInput();
		Mutate(I);
		const FCGHReconstructionResult Result = CGHReconstruction::Reconstruct(I, Cancelled);
		TestFalse(Label, Result.bSucceeded);
		TestFalse(TEXT("Failure supplies a diagnostic"), Result.Error.IsEmpty());
		TestTrue(TEXT("Failure exposes no partial samples"), Result.Field.Samples.IsEmpty());
		TestEqual(TEXT("Failure clears output dimensions"), Result.Field.ResolutionX, 0);
	};
	Reject(TEXT("Camera is unsupported"), [](FCGHReconstructionInput& I) { I.Mode = ECGHReconstructionMode::Camera; });
	Reject(TEXT("Unknown convention is rejected"), [](FCGHReconstructionInput& I) { I.PropagationConvention = static_cast<ECGHPropagationConvention>(255); });
	Reject(TEXT("Complex modulation is rejected"), [](FCGHReconstructionInput& I) { I.SLM.ModulationType = ECGHSLMModulationType::Complex; });
	Reject(TEXT("Missing phase is rejected"), [](FCGHReconstructionInput& I) { I.Pattern.PhaseRad.Reset(); });
	Reject(TEXT("Mismatched phase dimensions are rejected"), [](FCGHReconstructionInput& I) { I.Pattern.ResolutionX = 2; });
	Reject(TEXT("NaN phase is rejected"), [](FCGHReconstructionInput& I) { I.Pattern.PhaseRad[0] = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite phase is rejected"), [](FCGHReconstructionInput& I) { I.Pattern.PhaseRad[0] = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("Zero grid dimension is rejected"), [](FCGHReconstructionInput& I) { I.ObserverPlane.ResolutionY = 0; });
	Reject(TEXT("Oversized product is rejected"), [](FCGHReconstructionInput& I) { I.ObserverPlane.ResolutionX = I.ObserverPlane.ResolutionY = MAX_int32; });
	Reject(TEXT("Excessive count is rejected"), [](FCGHReconstructionInput& I) { I.ObserverPlane.ResolutionX = I.ObserverPlane.ResolutionY = 10000; });
	Reject(TEXT("Negative pitch is rejected"), [](FCGHReconstructionInput& I) { I.ObserverPlane.PixelPitchXM = -1.0; });
	Reject(TEXT("Infinite pitch is rejected"), [](FCGHReconstructionInput& I) { I.SLM.PixelPitchYM = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("Zero wavelength is rejected"), [](FCGHReconstructionInput& I) { I.Light.WavelengthM = 0.0; });
	Reject(TEXT("Overflowing wave number is rejected"), [](FCGHReconstructionInput& I) { I.Light.WavelengthM = std::numeric_limits<double>::min(); });
	Reject(TEXT("Negative amplitude is rejected"), [](FCGHReconstructionInput& I) { I.Light.Amplitude = -1.0; });
	Reject(TEXT("Nonfinite polarization is rejected"), [](FCGHReconstructionInput& I) { I.Light.PolarizationAngleRad = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("Nonunit direction is rejected"), [](FCGHReconstructionInput& I) { I.Light.DirectionSLM *= 2.0; });
	Reject(TEXT("Unknown source is rejected"), [](FCGHReconstructionInput& I) { I.Light.SourceType = static_cast<ECGHSourceType>(255); });
	Reject(TEXT("Nonunit rotation is rejected"), [](FCGHReconstructionInput& I) { I.ObserverPlane.RotationSLM.W = 2.0; });
	Reject(TEXT("Nonfinite pose is rejected"), [](FCGHReconstructionInput& I) { I.ObserverPlane.PositionSLMM.Y = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Aperture-plane observer is rejected"), [](FCGHReconstructionInput& I) { I.ObserverPlane.PositionSLMM.X = 0.0; });
	Reject(TEXT("Backward observer is rejected"), [](FCGHReconstructionInput& I) { I.ObserverPlane.PositionSLMM.X = -0.1; });
	Reject(TEXT("Observer crossing aperture is rejected"), [](FCGHReconstructionInput& I)
	{
		I.ObserverPlane.ResolutionX = 3;
		I.ObserverPlane.PixelPitchXM = 1.0;
		I.ObserverPlane.RotationSLM = FRotator(0.0, 90.0, 0.0).Quaternion();
	});
	Reject(TEXT("Singular point source is rejected"), [](FCGHReconstructionInput& I)
	{
		I.Light.SourceType = ECGHSourceType::PointSource;
		I.Light.PositionSLMM = FVector::ZeroVector;
	});
	Reject(TEXT("Overflowing phase is rejected"), [](FCGHReconstructionInput& I) { I.ObserverPlane.PositionSLMM.X = 1.0e308; });
	const std::atomic<bool> AlreadyCancelled{true};
	const FCGHReconstructionResult CancelResult = CGHReconstruction::Reconstruct(MakeInput(), AlreadyCancelled);
	TestFalse(TEXT("Pre-cancelled job cannot succeed"), CancelResult.bSucceeded);
	TestTrue(TEXT("Cancellation field remains empty"), CancelResult.Field.Samples.IsEmpty());
	TestTrue(TEXT("Cancellation is explicit"), CancelResult.Error.Contains(TEXT("cancelled")));
	FCGHReconstructionInput Small = MakeInput();
	Small.Pattern = FCGHSLMPhasePattern();
	FString Error;
	TestTrue(TEXT("Cheap validation does not require copying phase data"), CGHReconstruction::ValidateScene(Small, Error));
	TestFalse(TEXT("Full validation requires phase data"), CGHReconstruction::ValidateInput(Small, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHReconstructionBackendTest,
	"CGH.Reconstruction.BackendMailboxesAndCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHReconstructionBackendTest::RunTest(const FString& Parameters)
{
	UCGHCPUReconstructionBackend* Backend = NewObject<UCGHCPUReconstructionBackend>();
	FCGHReconstructionInput Input = MakeInput();
	const double* OwnedBuffer = Input.Pattern.PhaseRad.GetData();
	const std::complex<double> Expected = ReferenceField(Input, Input.ObserverPlane.PositionSLMM);
	const TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> Job = Backend->Submit(MoveTemp(Input));
	TestTrue(TEXT("Submit transfers phase ownership into the independent mailbox"), Job->Input.Pattern.PhaseRad.GetData() == OwnedBuffer);
	if (!WaitForJob(*this, Job))
	{
		return false;
	}
	TestTrue(TEXT("Worker marks started"), Job->bStarted.load(std::memory_order_acquire));
	if (TestTrue(TEXT("CPU worker succeeds"), Job->Result.bSucceeded))
	{
		TestTrue(TEXT("Acquired mailbox contains complete expected field"), std::abs(Complex(Job->Result.Field.Samples[0]) - Expected) < 1.0e-16);
	}
	FCGHReconstructionInput Large = MakeInput();
	Large.SLM.ResolutionX = Large.Pattern.ResolutionX = 256;
	Large.SLM.ResolutionY = Large.Pattern.ResolutionY = 256;
	Large.Pattern.PhaseRad.Init(0.0, 256 * 256);
	Large.ObserverPlane.ResolutionX = Large.ObserverPlane.ResolutionY = 256;
	const TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> CancelJob = Backend->Submit(MoveTemp(Large));
	// Let the worker begin; production cancellation must also interrupt the inner source loop.
	FPlatformProcess::Sleep(0.03f);
	CancelJob->bCancelRequested.store(true, std::memory_order_relaxed);
	if (!WaitForJob(*this, CancelJob))
	{
		return false;
	}
	TestFalse(TEXT("Cancelled worker does not succeed"), CancelJob->Result.bSucceeded);
	TestTrue(TEXT("No partial field escapes cancellation"), CancelJob->Result.Field.Samples.IsEmpty());
	TestTrue(TEXT("Worker identifies cancellation"), CancelJob->Result.Error.Contains(TEXT("cancelled")));
	UCGHDockerReconstructionBackend* Docker = NewObject<UCGHDockerReconstructionBackend>();
	Docker->Settings.Port = 0; // An invalid endpoint fails asynchronously without a remote service.
	const TSharedPtr<FCGHReconstructionJob, ESPMode::ThreadSafe> Rejected = Docker->Submit(MakeInput());
	if (!WaitForJob(*this, Rejected))
	{
		return false;
	}
	TestTrue(TEXT("Docker worker marks started before completing its mailbox"), Rejected->bStarted.load(std::memory_order_acquire));
	TestFalse(TEXT("Invalid Docker endpoint never fabricates success"), Rejected->Result.bSucceeded);
	TestTrue(TEXT("Docker identifies invalid endpoint settings"), Rejected->Result.Error.Contains(TEXT("port")));
	TestTrue(TEXT("Failed Docker job returns no fake samples"), Rejected->Result.Field.Samples.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
