#include "CGH/Solver/CGHPointFocus.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Utils/CGHPhasePreview.h"
#include "Misc/AutomationTest.h"
#include <cmath>
#include <complex>
#include <limits>

namespace
{
	constexpr double PointFocusTestTwoPi = 2.0 * UE_DOUBLE_PI;

	FCGHSolverInput MakePointFocusInput()
	{
		FCGHSolverInput Input;
		Input.Scene.SLM.ResolutionX = 4;
		Input.Scene.SLM.ResolutionY = 3;
		Input.Scene.SLM.PixelPitchXM = 8.0e-6;
		Input.Scene.SLM.PixelPitchYM = 13.0e-6;
		Input.Scene.ReconstructionLight.WavelengthM = 532.0e-9;
		Input.Scene.ReconstructionLight.DirectionSLM = FVector::ForwardVector;
		Input.Scene.ReconstructionLight.Amplitude = 1.0;
		FCGHTargetDescription Target;
		Target.TargetType = ECGHTargetType::Point;
		Target.PositionSLMM = FVector(0.021, 0.0017, -0.003);
		Target.PhaseRad = 1.234;
		Target.Amplitude = 1.0;
		Input.Scene.Targets.Add(Target);
		return Input;
	}

	double CircularDifference(double First, double Second)
	{
		return std::abs(std::remainder(First - Second, PointFocusTestTwoPi));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusPropagationTest,
	"CGH.Solver.PointFocus.OffAxisAnisotropicPropagation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusPropagationTest::RunTest(const FString& Parameters)
{
	const FCGHSolverInput Input = MakePointFocusInput();
	const std::atomic<bool> CancelRequested{false};
	const FCGHSolverResult Result = CGHPointFocus::Solve(Input, CancelRequested);
	if (!TestTrue(TEXT("A single off-axis point produces a phase pattern"), Result.bSucceeded))
	{
		AddError(Result.Error);
		return false;
	}
	TestTrue(TEXT("The result explicitly declares exp(+i*k*r) propagation"),
		Result.PropagationConvention == ECGHPropagationConvention::ExpPositiveIKR);
	TestEqual(TEXT("Columns retain horizontal resolution"), Result.Pattern.ResolutionX, 4);
	TestEqual(TEXT("Rows retain vertical resolution"), Result.Pattern.ResolutionY, 3);
	if (!TestEqual(TEXT("One phase is returned per pixel"), Result.Pattern.PhaseRad.Num(), 12))
	{
		return false;
	}
	FString Error;
	TestTrue(TEXT("Output passes the SLM publication validator"), CGHPhasePreview::ValidatePattern(Result.Pattern, 4, 3, Error));
	TestTrue(TEXT("Compute duration is finite and nonnegative"), std::isfinite(Result.ComputeSeconds) && Result.ComputeSeconds >= 0.0);
	TestTrue(TEXT("Success leaves no diagnostic"), Result.Error.IsEmpty());

	// Explicit pixel positions independently fix centering, pitch mapping, row order and column order.
	const double ColumnY[] = {-12.0e-6, -4.0e-6, 4.0e-6, 12.0e-6};
	const double RowZ[] = {13.0e-6, 0.0, -13.0e-6};
	const FVector Target = Input.Scene.Targets[0].PositionSLMM;
	const std::complex<double> Desired = std::polar(1.0, Input.Scene.Targets[0].PhaseRad);
	std::complex<double> Sum(0.0, 0.0);
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Column = 0; Column < 4; ++Column)
		{
			const double Phase = Result.Pattern.PhaseRad[Row * 4 + Column];
			TestTrue(TEXT("Every phase lies in the half-open [0, 2pi) interval"), Phase >= 0.0 && Phase < PointFocusTestTwoPi);
			const double DY = Target.Y - ColumnY[Column];
			const double DZ = Target.Z - RowZ[Row];
			const double R = std::sqrt(Target.X * Target.X + DY * DY + DZ * DZ);
			// Propagate independent unit complex fields using the declared positive propagation sign.
			const std::complex<double> Field = std::polar(1.0, Phase)
				* std::polar(1.0, PointFocusTestTwoPi * R / Input.Scene.ReconstructionLight.WavelengthM);
			TestTrue(TEXT("Each pixel arrives with the desired target phase"), std::abs(Field - Desired) < 2.0e-9);
			Sum += Field;
		}
	}
	TestTrue(TEXT("All unit-amplitude pixel fields add coherently at the target"), std::abs(Sum - 12.0 * Desired) < 2.0e-8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusCenteringTest,
	"CGH.Solver.PointFocus.EvenOddAndSingletonCenters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusCenteringTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> CancelRequested{false};
	for (const FIntPoint Dimensions : {FIntPoint(4, 2), FIntPoint(3, 3), FIntPoint(1, 1), FIntPoint(1, 5), FIntPoint(5, 1)})
	{
		FCGHSolverInput Input = MakePointFocusInput();
		Input.Scene.SLM.ResolutionX = Dimensions.X;
		Input.Scene.SLM.ResolutionY = Dimensions.Y;
		Input.Scene.SLM.PixelPitchXM = 0.013;
		Input.Scene.SLM.PixelPitchYM = 0.021;
		Input.Scene.ReconstructionLight.WavelengthM = 0.4;
		Input.Scene.Targets[0].PositionSLMM = FVector(0.2, 0.0, 0.0);
		Input.Scene.Targets[0].PhaseRad = 0.3;
		const FCGHSolverResult Result = CGHPointFocus::Solve(Input, CancelRequested);
		if (!TestTrue(TEXT("Even, odd and singleton grids are supported"), Result.bSucceeded))
		{
			return false;
		}
		for (int32 Row = 0; Row < Dimensions.Y; ++Row)
		{
			for (int32 Column = 0; Column < Dimensions.X; ++Column)
			{
				const double Phase = Result.Pattern.PhaseRad[Row * Dimensions.X + Column];
				TestEqual(TEXT("On-axis focus is horizontally symmetric about pixel centers"), Phase,
					Result.Pattern.PhaseRad[Row * Dimensions.X + Dimensions.X - 1 - Column], 1.0e-12);
				TestEqual(TEXT("On-axis focus is vertically symmetric about pixel centers"), Phase,
					Result.Pattern.PhaseRad[(Dimensions.Y - 1 - Row) * Dimensions.X + Column], 1.0e-12);
			}
		}
		if (Dimensions.X % 2 == 1 && Dimensions.Y % 2 == 1)
		{
			const double CenterPhase = Result.Pattern.PhaseRad[(Dimensions.Y / 2) * Dimensions.X + Dimensions.X / 2];
			TestTrue(TEXT("An odd grid's central pixel sits exactly at the SLM origin"),
				CircularDifference(CenterPhase, 0.3 - PointFocusTestTwoPi * 0.2 / 0.4) < 1.0e-12);
		}
		if (Dimensions == FIntPoint(4, 2))
		{
			const double CornerDistance = std::sqrt(0.2 * 0.2 + 0.0195 * 0.0195 + 0.0105 * 0.0105);
			TestTrue(TEXT("Even grids use half-pixel centers on both axes"),
				CircularDifference(Result.Pattern.PhaseRad[0], 0.3 - PointFocusTestTwoPi * CornerDistance / 0.4) < 1.0e-12);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusDirectionsTest,
	"CGH.Solver.PointFocus.PropagationSignAndPixelDirections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusDirectionsTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> CancelRequested{false};
	FCGHSolverInput Input = MakePointFocusInput();
	Input.Scene.SLM.PixelPitchXM = 0.01;
	Input.Scene.SLM.PixelPitchYM = 0.02;
	Input.Scene.ReconstructionLight.WavelengthM = 1.0;
	Input.Scene.Targets[0].PositionSLMM = FVector(0.4, 0.15, 0.13);
	Input.Scene.Targets[0].PhaseRad = 5.0;
	const FCGHSolverResult Positive = CGHPointFocus::Solve(Input, CancelRequested);
	if (!TestTrue(TEXT("Direction fixture computes successfully"), Positive.bSucceeded))
	{
		return false;
	}
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Column = 0; Column < 4; ++Column)
		{
			const double Phase = Positive.Pattern.PhaseRad[Row * 4 + Column];
			if (Column < 3)
			{
				TestTrue(TEXT("Increasing column approaches a +Y target and increases phase for exp(+ikr)"),
					Positive.Pattern.PhaseRad[Row * 4 + Column + 1] > Phase);
			}
			if (Row < 2)
			{
				TestTrue(TEXT("Increasing row moves toward -Z, away from a +Z target, and decreases phase"),
					Positive.Pattern.PhaseRad[(Row + 1) * 4 + Column] < Phase);
			}
		}
	}
	Input.Scene.Targets[0].PositionSLMM.X *= -1.0;
	const FCGHSolverResult Negative = CGHPointFocus::Solve(Input, CancelRequested);
	TestTrue(TEXT("Signed target depth on either side of the SLM is supported"), Negative.bSucceeded);
	TestTrue(TEXT("Mirroring target X preserves all source-to-target distances"), Negative.Pattern.PhaseRad == Positive.Pattern.PhaseRad);
	Input.Scene.Targets[0].PhaseRad += 10.0 * PointFocusTestTwoPi + 0.7;
	const FCGHSolverResult Shifted = CGHPointFocus::Solve(Input, CancelRequested);
	if (!TestTrue(TEXT("Target phase offset computes successfully"), Shifted.bSucceeded))
	{
		return false;
	}
	for (int32 Index = 0; Index < Shifted.Pattern.PhaseRad.Num(); ++Index)
	{
		TestTrue(TEXT("Target phase offsets transfer modulo 2pi to every pixel"),
			CircularDifference(Shifted.Pattern.PhaseRad[Index], Positive.Pattern.PhaseRad[Index] + 0.7) < 1.0e-12);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusValidationTest,
	"CGH.Solver.PointFocus.InvalidInputAndCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusValidationTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> CancelRequested{false};
	const auto Reject = [this, &CancelRequested](const TCHAR* Label, auto Mutate)
	{
		FCGHSolverInput Input = MakePointFocusInput();
		Mutate(Input);
		FString Error;
		TestFalse(Label, CGHPointFocus::ValidateInput(Input, Error));
		TestFalse(TEXT("Validation supplies a diagnostic"), Error.IsEmpty());
		const FCGHSolverResult Result = CGHPointFocus::Solve(Input, CancelRequested);
		TestFalse(TEXT("Invalid input cannot report a successful solve"), Result.bSucceeded);
		TestFalse(TEXT("Failed solve supplies a diagnostic"), Result.Error.IsEmpty());
		TestTrue(TEXT("Failed solve never exposes a partial phase buffer"), Result.Pattern.PhaseRad.IsEmpty());
		TestEqual(TEXT("Failed solve resets output dimensions"), Result.Pattern.ResolutionX, 0);
	};
	Reject(TEXT("Unknown scene schema is rejected"), [](FCGHSolverInput& I) { I.Scene.SchemaVersion = 1; });
	Reject(TEXT("Unknown algorithm is rejected"), [](FCGHSolverInput& I) { I.Algorithm = static_cast<ECGHSolverAlgorithm>(255); });
	Reject(TEXT("An unrecognized/opposite propagation convention cannot silently change phase sign"),
		[](FCGHSolverInput& I) { I.PropagationConvention = static_cast<ECGHPropagationConvention>(1); });
	Reject(TEXT("Complex modulation is unsupported"), [](FCGHSolverInput& I) { I.Scene.SLM.ModulationType = ECGHSLMModulationType::Complex; });
	Reject(TEXT("Unknown modulation is rejected"), [](FCGHSolverInput& I) { I.Scene.SLM.ModulationType = static_cast<ECGHSLMModulationType>(255); });
	Reject(TEXT("Empty target list is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets.Reset(); });
	Reject(TEXT("A mesh target without its resource is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].TargetType = ECGHTargetType::Mesh; });
	Reject(TEXT("Unknown target type is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].TargetType = static_cast<ECGHTargetType>(255); });
	Reject(TEXT("Zero dimensions are rejected"), [](FCGHSolverInput& I) { I.Scene.SLM.ResolutionX = 0; });
	Reject(TEXT("Negative dimensions are rejected"), [](FCGHSolverInput& I) { I.Scene.SLM.ResolutionY = -1; });
	Reject(TEXT("Oversized dimensions are rejected before multiplication or allocation"),
		[](FCGHSolverInput& I) { I.Scene.SLM.ResolutionX = MAX_int32; I.Scene.SLM.ResolutionY = MAX_int32; });
	Reject(TEXT("Pixel count above the shared limit is rejected"),
		[](FCGHSolverInput& I) { I.Scene.SLM.ResolutionX = 10000; I.Scene.SLM.ResolutionY = 10000; });
	Reject(TEXT("Zero pitch is rejected"), [](FCGHSolverInput& I) { I.Scene.SLM.PixelPitchXM = 0.0; });
	Reject(TEXT("Negative pitch is rejected"), [](FCGHSolverInput& I) { I.Scene.SLM.PixelPitchYM = -1.0; });
	Reject(TEXT("NaN pitch is rejected"), [](FCGHSolverInput& I) { I.Scene.SLM.PixelPitchXM = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite pitch is rejected"), [](FCGHSolverInput& I) { I.Scene.SLM.PixelPitchYM = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("Zero wavelength is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.WavelengthM = 0.0; });
	Reject(TEXT("Negative wavelength is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.WavelengthM = -1.0; });
	Reject(TEXT("NaN wavelength is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.WavelengthM = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite wavelength is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.WavelengthM = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("An overflowing wave number is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.WavelengthM = std::numeric_limits<double>::min(); });
	Reject(TEXT("PointSource illumination is unsupported"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.SourceType = ECGHSourceType::PointSource; });
	Reject(TEXT("Unknown illumination source type is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.SourceType = static_cast<ECGHSourceType>(255); });
	Reject(TEXT("NaN incident phase is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.InitialPhaseRad = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite incident phase is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.InitialPhaseRad = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("Zero plane-wave direction is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.DirectionSLM = FVector::ZeroVector; });
	Reject(TEXT("Nonunit plane-wave direction is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.DirectionSLM = FVector(2.0, 0.0, 0.0); });
	Reject(TEXT("A direction outside the declared squared-norm tolerance is rejected"),
		[](FCGHSolverInput& I) { I.Scene.ReconstructionLight.DirectionSLM = FVector(std::sqrt(1.0 + 2.0e-6), 0.0, 0.0); });
	Reject(TEXT("NaN direction is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.DirectionSLM.Y = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite direction is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.DirectionSLM.Z = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("Negative light amplitude is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.Amplitude = -1.0; });
	Reject(TEXT("NaN light amplitude is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.Amplitude = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite light amplitude is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.Amplitude = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("NaN polarization is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.PolarizationAngleRad = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite polarization is rejected"), [](FCGHSolverInput& I) { I.Scene.ReconstructionLight.PolarizationAngleRad = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("Plane-wave aperture-corner phase overflow is rejected before allocation"), [](FCGHSolverInput& I)
	{
		I.Scene.SLM.PixelPitchXM = 1.0e300;
		I.Scene.ReconstructionLight.DirectionSLM = FVector(0.0, 1.0, 0.0);
		I.Scene.ReconstructionLight.InitialPhaseRad = std::numeric_limits<double>::max();
	});
	Reject(TEXT("Finite incident and target phases with overflowing phase difference are rejected"), [](FCGHSolverInput& I)
	{
		I.Scene.Targets[0].PhaseRad = std::numeric_limits<double>::max();
		I.Scene.ReconstructionLight.InitialPhaseRad = -std::numeric_limits<double>::max();
	});
	Reject(TEXT("The SLM plane is not a supported focus depth"), [](FCGHSolverInput& I) { I.Scene.Targets[0].PositionSLMM.X = 0.0; });
	Reject(TEXT("NaN target position is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].PositionSLMM.Y = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite target position is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].PositionSLMM.Z = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("NaN target phase is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].PhaseRad = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite target phase is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].PhaseRad = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("An overflowing aperture is rejected before allocation"), [](FCGHSolverInput& I) { I.Scene.SLM.PixelPitchXM = std::numeric_limits<double>::max(); });
	Reject(TEXT("Finite geometry with overflowing propagation phase is rejected"),
		[](FCGHSolverInput& I) { I.Scene.Targets[0].PositionSLMM.X = std::numeric_limits<double>::max() / 4.0; });

	const std::atomic<bool> AlreadyCancelled{true};
	const FCGHSolverResult Cancelled = CGHPointFocus::Solve(MakePointFocusInput(), AlreadyCancelled);
	TestFalse(TEXT("Pre-cancelled jobs do not succeed"), Cancelled.bSucceeded);
	TestTrue(TEXT("Cancellation does not allocate or publish a pattern"), Cancelled.Pattern.PhaseRad.IsEmpty());
	TestTrue(TEXT("Cancellation is identified in the error"), Cancelled.Error.Contains(TEXT("cancelled")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusPlaneWaveTest,
	"CGH.Solver.PointFocus.ObliquePlaneWavePhaseCompensation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusPlaneWaveTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> CancelRequested{false};
	FCGHSolverInput Input = MakePointFocusInput();
	Input.Scene.ReconstructionLight.DirectionSLM = FVector(std::sqrt(0.75), 0.3, -0.4);
	Input.Scene.ReconstructionLight.InitialPhaseRad = 2.13;
	const FCGHSolverResult Result = CGHPointFocus::Solve(Input, CancelRequested);
	if (!TestTrue(TEXT("An oblique plane wave with nonzero origin phase is supported"), Result.bSucceeded))
	{
		AddError(Result.Error);
		return false;
	}
	const double ColumnY[] = {-12.0e-6, -4.0e-6, 4.0e-6, 12.0e-6};
	const double RowZ[] = {13.0e-6, 0.0, -13.0e-6};
	const FVector Target = Input.Scene.Targets[0].PositionSLMM;
	const double K = PointFocusTestTwoPi / Input.Scene.ReconstructionLight.WavelengthM;
	const std::complex<double> Desired = std::polar(1.0, Input.Scene.Targets[0].PhaseRad);
	std::complex<double> Sum(0.0, 0.0);
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Column = 0; Column < 4; ++Column)
		{
			const double DY = Target.Y - ColumnY[Column];
			const double DZ = Target.Z - RowZ[Row];
			const double R = std::sqrt(Target.X * Target.X + DY * DY + DZ * DZ);
			const double Incident = 2.13 + K * (0.3 * ColumnY[Column] - 0.4 * RowZ[Row]);
			// Independently illuminate, modulate, and propagate each unit complex field.
			const std::complex<double> Field = std::polar(1.0, Incident)
				* std::polar(1.0, Result.Pattern.PhaseRad[Row * 4 + Column])
				* std::polar(1.0, K * R);
			TestTrue(TEXT("Incident plus SLM plus positive propagation phase reaches the requested target phase"),
				std::abs(Field - Desired) < 2.0e-9);
			Sum += Field;
		}
	}
	TestTrue(TEXT("The obliquely illuminated aperture sums coherently at the target"), std::abs(Sum - 12.0 * Desired) < 2.0e-8);
	Input.Scene.ReconstructionLight.InitialPhaseRad += 0.75;
	const FCGHSolverResult Shifted = CGHPointFocus::Solve(Input, CancelRequested);
	if (!TestTrue(TEXT("Changing the incident origin phase computes successfully"), Shifted.bSucceeded))
	{
		return false;
	}
	for (int32 Index = 0; Index < Shifted.Pattern.PhaseRad.Num(); ++Index)
	{
		TestTrue(TEXT("Increasing incident phase decreases SLM phase by the same amount modulo 2pi"),
			CircularDifference(Shifted.Pattern.PhaseRad[Index], Result.Pattern.PhaseRad[Index] - 0.75) < 1.0e-9);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusIlluminationScopeTest,
	"CGH.Solver.PointFocus.PlaneWaveIlluminationContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusIlluminationScopeTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> CancelRequested{false};
	FCGHSolverInput Input = MakePointFocusInput();
	Input.Scene.ReconstructionLight.DirectionSLM = FVector(0.8, 0.6, 0.0);
	Input.Scene.ReconstructionLight.InitialPhaseRad = 0.91;
	const FCGHSolverResult Reference = CGHPointFocus::Solve(Input, CancelRequested);
	Input.Scene.ReconstructionLight.PositionSLMM = FVector(10.0, -20.0, 30.0);
	Input.Scene.ReconstructionLight.PolarizationAngleRad = 1.3;
	Input.Scene.ReconstructionLight.Amplitude = 0.0;
	Input.Scene.Targets[0].Amplitude = 3.0;
	const FCGHSolverResult UnsimulatedFields = CGHPointFocus::Solve(Input, CancelRequested);
	TestTrue(TEXT("Finite polarization and zero illumination amplitude are accepted by the phase-only calculation"),
		Reference.bSucceeded && UnsimulatedFields.bSucceeded);
	TestTrue(TEXT("Plane-wave position, polarization and light amplitude, and positive single-point amplitude do not affect phase"),
		Reference.Pattern.PhaseRad == UnsimulatedFields.Pattern.PhaseRad);
	Input.Scene.ReconstructionLight.Amplitude = 3.75;
	const FCGHSolverResult PositiveAmplitude = CGHPointFocus::Solve(Input, CancelRequested);
	TestTrue(TEXT("A nonunit positive illumination amplitude still leaves phase unchanged"),
		PositiveAmplitude.bSucceeded && PositiveAmplitude.Pattern.PhaseRad == Reference.Pattern.PhaseRad);
	Input.Scene.ReconstructionLight.WavelengthM = 633.0e-9;
	const FCGHSolverResult NewWavelength = CGHPointFocus::Solve(Input, CancelRequested);
	TestTrue(TEXT("Changing the reconstruction wavelength changes the phase pattern"),
		NewWavelength.bSucceeded && NewWavelength.Pattern.PhaseRad != Reference.Pattern.PhaseRad);

	Input = MakePointFocusInput();
	const FCGHSolverResult Normal = CGHPointFocus::Solve(Input, CancelRequested);
	Input.Scene.ReconstructionLight.DirectionSLM = FVector(-1.0, 0.0, 0.0);
	const FCGHSolverResult OppositeNormal = CGHPointFocus::Solve(Input, CancelRequested);
	TestTrue(TEXT("Either normal direction has the same incident phase on the X=0 plane"),
		Normal.bSucceeded && OppositeNormal.bSucceeded && Normal.Pattern.PhaseRad == OppositeNormal.Pattern.PhaseRad);
	Input.Scene.ReconstructionLight.DirectionSLM = FVector(std::sqrt(1.0 + 0.5e-6), 0.0, 0.0);
	const FCGHSolverResult WithinTolerance = CGHPointFocus::Solve(Input, CancelRequested);
	TestTrue(TEXT("Direction normalization error within the declared squared-norm tolerance is accepted"), WithinTolerance.bSucceeded);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
