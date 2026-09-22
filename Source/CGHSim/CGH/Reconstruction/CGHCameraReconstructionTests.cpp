#include "CGH/Reconstruction/CGHReconstruction.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "CGH/Reconstruction/CGHCPUReconstructionBackend.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include <cmath>
#include <complex>
#include <limits>

namespace CGHCameraNumerics
{
	using Complex = std::complex<double>;
	constexpr double Tau = 2.0 * UE_DOUBLE_PI;

	FCGHReconstructionInput Input()
	{
		FCGHReconstructionInput I;
		I.Mode = ECGHReconstructionMode::Camera;
		I.SLM.ResolutionX = I.Pattern.ResolutionX = 3;
		I.SLM.ResolutionY = I.Pattern.ResolutionY = 2;
		I.SLM.PixelPitchXM = 0.003;
		I.SLM.PixelPitchYM = 0.005;
		I.Pattern.PhaseRad = {0.1, 0.9, -0.4, 0.3, 1.1, -0.2};
		I.Light.WavelengthM = 0.025;
		I.Light.Amplitude = 1.3;
		I.Light.InitialPhaseRad = -0.21;
		I.Light.DirectionSLM = FVector(0.8, 0.36, -0.48);
		I.Camera.OpticalPositionSLMM = FVector(0.63, -0.011, 0.008);
		I.Camera.OpticalRotationSLM = FRotator(10.0, 170.0, 23.0).Quaternion();
		I.Camera.FocalLengthM = 0.06;
		I.Camera.FNumber = 2.0;
		I.Camera.FocusDistanceM = 0.63;
		I.Camera.OutputResolutionX = 4;
		I.Camera.OutputResolutionY = 3;
		I.Camera.PixelPitchXM = 0.0004;
		I.Camera.PixelPitchYM = 0.0007;
		I.Camera.PupilResolutionX = 5;
		I.Camera.PupilResolutionY = 7;
		return I;
	}

	Complex Value(const FCGHComplexSample& Sample) { return {Sample.Real, Sample.Imaginary}; }

	// Independent complex Green-function normal derivative, including quadrature area.
	Complex Green(const FVector& Displacement, const FVector& Normal, double Area, double K)
	{
		const double R = Displacement.Size();
		return Area * FVector::DotProduct(Displacement, Normal) / (Tau * R * R * R)
			* Complex(1.0, -K * R) * std::exp(Complex(0.0, K * R));
	}

	/** Independent world-frame double integration. No production geometry/helper is called. */
	TArray<Complex> Oracle(const FCGHReconstructionInput& I)
	{
		const auto& C = I.Camera;
		const double K = Tau / I.Light.WavelengthM;
		const double Diameter = C.FocalLengthM / C.FNumber;
		const double DY = Diameter / C.PupilResolutionX;
		const double DZ = Diameter / C.PupilResolutionY;
		const double V = 1.0 / (1.0 / C.FocalLengthM - 1.0 / C.FocusDistanceM);
		const FVector PropagationNormal = C.OpticalRotationSLM.RotateVector(-FVector::XAxisVector);
		TArray<FVector> LensPositions;
		TArray<Complex> LensFields;
		for (int32 Row = 0; Row < C.PupilResolutionY; ++Row)
		{
			for (int32 Column = 0; Column < C.PupilResolutionX; ++Column)
			{
				const double Y = (Column + 0.5 - C.PupilResolutionX / 2.0) * DY;
				const double Z = (C.PupilResolutionY / 2.0 - Row - 0.5) * DZ;
				if (std::hypot(Y, Z) > Diameter / 2.0) continue;
				const FVector L = C.OpticalPositionSLMM + C.OpticalRotationSLM.RotateVector(FVector(0.0, Y, Z));
				Complex Field(0.0, 0.0);
				for (int32 Index = 0; Index < I.Pattern.PhaseRad.Num(); ++Index)
				{
					const FVector P(0.0,
						(Index % I.SLM.ResolutionX + 0.5 - I.SLM.ResolutionX / 2.0) * I.SLM.PixelPitchXM,
						(I.SLM.ResolutionY / 2.0 - Index / I.SLM.ResolutionX - 0.5) * I.SLM.PixelPitchYM);
					double Amplitude = I.Light.Amplitude;
					double Phase = I.Light.InitialPhaseRad + I.Pattern.PhaseRad[Index];
					if (I.Light.SourceType == ECGHSourceType::PointSource)
					{
						const double R = (P - I.Light.PositionSLMM).Size();
						Amplitude /= R;
						Phase += K * R;
					}
					else Phase += K * FVector::DotProduct(I.Light.DirectionSLM, P);
					Field += std::polar(Amplitude, Phase) * Green(L - P, FVector::XAxisVector,
						I.SLM.PixelPitchXM * I.SLM.PixelPitchYM, K);
				}
				LensPositions.Add(L);
				LensFields.Add(Field * std::exp(Complex(0.0, -K * (Y * Y + Z * Z) / (2.0 * C.FocalLengthM))));
			}
		}
		TArray<Complex> Output;
		for (int32 Row = 0; Row < C.OutputResolutionY; ++Row)
		{
			for (int32 Column = 0; Column < C.OutputResolutionX; ++Column)
			{
				const FVector Q = C.OpticalPositionSLMM + C.OpticalRotationSLM.RotateVector(FVector(-V,
					(Column + 0.5 - C.OutputResolutionX / 2.0) * C.PixelPitchXM,
					(C.OutputResolutionY / 2.0 - Row - 0.5) * C.PixelPitchYM));
				Complex Field(0.0, 0.0);
				for (int32 Index = 0; Index < LensPositions.Num(); ++Index)
				{
					Field += LensFields[Index] * Green(Q - LensPositions[Index], PropagationNormal, DY * DZ, K);
				}
				Output.Add(Field);
			}
		}
		return Output;
	}

	bool Check(FAutomationTestBase& Test, const FCGHReconstructionInput& I)
	{
		const std::atomic<bool> Cancel{false};
		const auto Result = CGHReconstruction::Reconstruct(I, Cancel);
		if (!Test.TestTrue(FString::Printf(TEXT("Thin-lens reconstruction succeeds: %s"), *Result.Error), Result.bSucceeded)) return false;
		const auto Expected = Oracle(I);
		if (!Test.TestEqual(TEXT("The field contains every sensor pixel"), Result.Field.Samples.Num(), Expected.Num())) return false;
		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			const double Error = std::abs(Value(Result.Field.Samples[Index]) - Expected[Index]);
			Test.TestTrue(TEXT("World-frame pupil/sensor integral agrees with shared two-pass implementation"), Error <= 1.e-11 * std::abs(Expected[Index]) + 1.e-18);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraAnalyticTest, "CGH.Reconstruction.Camera.SinglePupilAnalyticAndFieldUnits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCGHCameraAnalyticTest::RunTest(const FString& Parameters)
{
	using namespace CGHCameraNumerics;
	auto I = Input();
	I.SLM.ResolutionX = I.SLM.ResolutionY = I.Pattern.ResolutionX = I.Pattern.ResolutionY = 1;
	I.Pattern.PhaseRad = {0.37};
	I.Camera.PupilResolutionX = I.Camera.PupilResolutionY = 1;
	I.Camera.OutputResolutionX = I.Camera.OutputResolutionY = 1;
	I.Camera.OpticalPositionSLMM = FVector(0.5, 0.0, 0.0);
	I.Camera.OpticalRotationSLM = FRotator(0.0, 180.0, 0.0).Quaternion();
	I.Light.DirectionSLM = FVector::XAxisVector;
	const std::atomic<bool> Cancel{false};
	const auto Result = CGHReconstruction::Reconstruct(I, Cancel);
	if (!TestTrue(TEXT("Analytical one-source/one-pupil/one-sensor path succeeds"), Result.bSucceeded)) return false;
	const double K = Tau / I.Light.WavelengthM;
	const double Diameter = I.Camera.FocalLengthM / I.Camera.FNumber;
	const double V = 1.0 / (1.0 / I.Camera.FocalLengthM - 1.0 / I.Camera.FocusDistanceM);
	const Complex Expected = std::polar(I.Light.Amplitude, I.Light.InitialPhaseRad + 0.37)
		* Green(FVector(0.5, 0.0, 0.0), FVector::XAxisVector, I.SLM.PixelPitchXM * I.SLM.PixelPitchYM, K)
		* Green(FVector(V, 0.0, 0.0), FVector::XAxisVector, Diameter * Diameter, K);
	TestTrue(TEXT("Two area weights and thin-lens image distance preserve unnormalized field units"),
		std::abs(Value(Result.Field.Samples[0]) - Expected) < std::abs(Expected) * 1.e-12);
	I.Light.Amplitude *= 3.0;
	const auto Tripled = CGHReconstruction::Reconstruct(I, Cancel);
	TestTrue(TEXT("Camera amplitude is linear in illumination; intensity scales quadratically"), Tripled.bSucceeded
		&& std::abs(Value(Tripled.Field.Samples[0]) - 3.0 * Expected) < std::abs(Expected) * 1.e-11);
	I.Light.Amplitude = 0.0;
	const auto Dark = CGHReconstruction::Reconstruct(I, Cancel);
	TestTrue(TEXT("Dark illumination yields exact dark sensor samples"), Dark.bSucceeded && Value(Dark.Field.Samples[0]) == Complex(0.0, 0.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraIntegralTest, "CGH.Reconstruction.Camera.IndependentTiltedRolledAndPointSourceIntegral",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCGHCameraIntegralTest::RunTest(const FString& Parameters)
{
	using namespace CGHCameraNumerics;
	auto I = Input();
	if (!Check(*this, I)) return false;
	I.Light.SourceType = ECGHSourceType::PointSource;
	I.Light.PositionSLMM = FVector(-0.7, 0.04, -0.08);
	if (!Check(*this, I)) return false;
	const std::atomic<bool> Cancel{false};
	const auto Original = CGHReconstruction::Reconstruct(I, Cancel);
	for (int32 Change = 0; Change < 6; ++Change)
	{
		auto Changed = I;
		switch (Change)
		{
		case 0: Changed.Camera.FocalLengthM *= 0.8; break;
		case 1: Changed.Camera.FNumber *= 1.5; break;
		case 2: Changed.Camera.FocusDistanceM *= 1.4; break;
		case 3: Changed.Camera.PixelPitchXM *= 3.0; break;
		case 4: Changed.Camera.OpticalRotationSLM = FRotator(10.0, 170.0, 77.0).Quaternion(); break;
		case 5: Changed.Camera.OpticalPositionSLMM.Y += 0.03; break;
		}
		if (!Check(*this, Changed)) return false;
		const auto Result = CGHReconstruction::Reconstruct(Changed, Cancel);
		TestTrue(TEXT("Focal length, aperture, focus, sensor pitch, roll and lens pose all affect the optical field"),
			Result.bSucceeded && std::abs(Value(Result.Field.Samples[0]) - Value(Original.Field.Samples[0])) > 1.e-15);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraFocusTest, "CGH.Reconstruction.Camera.ThinLensImagePositionFocusAndPupilConvergence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCGHCameraFocusTest::RunTest(const FString& Parameters)
{
	using namespace CGHCameraNumerics;
	auto I = Input();
	I.SLM.ResolutionX = I.SLM.ResolutionY = I.Pattern.ResolutionX = I.Pattern.ResolutionY = 1;
	I.SLM.PixelPitchXM = I.SLM.PixelPitchYM = 1.e-5;
	I.Pattern.PhaseRad = {0.0};
	I.Light.WavelengthM = 2.e-6;
	I.Light.DirectionSLM = FVector::XAxisVector;
	I.Camera.OpticalPositionSLMM = FVector(0.5, -0.004, 0.0);
	I.Camera.OpticalRotationSLM = FRotator(0.0, 180.0, 0.0).Quaternion();
	I.Camera.FocalLengthM = 0.05;
	I.Camera.FNumber = 25.0;
	I.Camera.FocusDistanceM = 0.5;
	I.Camera.OutputResolutionX = 33;
	I.Camera.OutputResolutionY = 5;
	I.Camera.PixelPitchXM = I.Camera.PixelPitchYM = 0.00005;
	I.Camera.PupilResolutionX = I.Camera.PupilResolutionY = 64;
	const std::atomic<bool> Cancel{false};
	const auto Focused = CGHReconstruction::Reconstruct(I, Cancel);
	if (!TestTrue(TEXT("Focused off-axis camera fixture succeeds"), Focused.bSucceeded)) return false;
	int32 Peak = 0;
	for (int32 Index = 1; Index < Focused.Field.Samples.Num(); ++Index)
	{
		if (std::norm(Value(Focused.Field.Samples[Index])) > std::norm(Value(Focused.Field.Samples[Peak]))) Peak = Index;
	}
	const double V = I.Camera.FocalLengthM / (1.0 - I.Camera.FocalLengthM / I.Camera.FocusDistanceM);
	const double PredictedColumn = 16.0 + V / 0.5 * 0.004 / I.Camera.PixelPitchXM;
	TestTrue(TEXT("The raw sensor image peaks at the inverted thin-lens image position"),
		Peak / 33 == 2 && FMath::Abs(Peak % 33 - PredictedColumn) < 1.0);
	I.Camera.PupilResolutionX = I.Camera.PupilResolutionY = 128;
	const auto Refined = CGHReconstruction::Reconstruct(I, Cancel);
	if (!TestTrue(TEXT("Refined pupil fixture succeeds"), Refined.bSucceeded)) return false;
	const double PeakPower = std::norm(Value(Focused.Field.Samples[Peak]));
	const double RefinedPower = std::norm(Value(Refined.Field.Samples[Peak]));
	TestTrue(TEXT("Peak intensity converges when pupil sampling is refined in this resolved fixture"),
		std::abs(PeakPower - RefinedPower) / RefinedPower < 0.025);
	I.Camera.FocusDistanceM = 1.0;
	const auto Defocused = CGHReconstruction::Reconstruct(I, Cancel);
	if (!TestTrue(TEXT("Changing focus distance moves the sensor without rejecting the scene"), Defocused.bSucceeded)) return false;
	double DefocusedPeak = 0.0;
	for (const auto& Sample : Defocused.Field.Samples) DefocusedPeak = FMath::Max(DefocusedPeak, std::norm(Value(Sample)));
	TestTrue(TEXT("Focusing at the actual object distance produces a brighter peak than defocus"), RefinedPower > 1.4 * DefocusedPeak);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraValidationTest, "CGH.Reconstruction.Camera.InvalidLensGeometryAndSampling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCGHCameraValidationTest::RunTest(const FString& Parameters)
{
	using namespace CGHCameraNumerics;
	const std::atomic<bool> Cancel{false};
	for (int32 Case = 0; Case < 15; ++Case)
	{
		auto I = Input();
		switch (Case)
		{
		case 0: I.Camera.FocalLengthM = 0.0; break;
		case 1: I.Camera.FNumber = -1.0; break;
		case 2: I.Camera.FocusDistanceM = I.Camera.FocalLengthM; break;
		case 3: I.Camera.FocusDistanceM = std::numeric_limits<double>::infinity(); break;
		case 4: I.Camera.PixelPitchXM = std::numeric_limits<double>::quiet_NaN(); break;
		case 5: I.Camera.OutputResolutionX = 0; break;
		case 6: I.Camera.PupilResolutionY = 2049; break;
		case 7: I.Camera.PupilResolutionX = 0; break;
		case 8: I.Camera.OpticalRotationSLM = FQuat(0.0, 0.0, 0.0, 2.0); break;
		case 9: I.Camera.OpticalRotationSLM = FQuat::Identity; break;
		case 10: I.Camera.OpticalPositionSLMM.X = -0.3; break;
		case 11: I.Camera.FNumber = std::numeric_limits<double>::denorm_min(); break;
		case 12: I.Camera.OpticalPositionSLMM.X = 0.0001; break; // Tilted pupil crosses SLM.
		case 13: I.Pattern.PhaseRad[0] = std::numeric_limits<double>::infinity(); break;
		case 14: I.Camera.OutputResolutionX = I.Camera.OutputResolutionY = 10000; break;
		}
		const auto Result = CGHReconstruction::Reconstruct(I, Cancel);
		TestFalse(FString::Printf(TEXT("Invalid camera input %d is rejected"), Case), Result.bSucceeded);
		TestFalse(TEXT("Invalid camera supplies a diagnostic"), Result.Error.IsEmpty());
		TestTrue(TEXT("Invalid camera publishes no partial sensor field"), Result.Field.Samples.IsEmpty() && Result.Field.ResolutionX == 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraCancellationTest, "CGH.Reconstruction.Camera.CancellationAcrossBothPropagationStages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCGHCameraCancellationTest::RunTest(const FString& Parameters)
{
	using namespace CGHCameraNumerics;
	for (bool LargeSLM : {true, false})
	{
		auto I = Input();
		I.SLM.ResolutionX = I.Pattern.ResolutionX = LargeSLM ? 256 : 1;
		I.SLM.ResolutionY = I.Pattern.ResolutionY = LargeSLM ? 256 : 1;
		I.SLM.PixelPitchXM = I.SLM.PixelPitchYM = 8.e-6;
		I.Pattern.PhaseRad.Init(0.3, I.SLM.ResolutionX * I.SLM.ResolutionY);
		I.Camera.PupilResolutionX = I.Camera.PupilResolutionY = 64;
		I.Camera.OutputResolutionX = I.Camera.OutputResolutionY = LargeSLM ? 2 : 1024;
		I.Camera.PixelPitchXM = I.Camera.PixelPitchYM = 8.e-6;
		std::atomic<bool> Cancel{false}, Started{false};
		auto Future = Async(EAsyncExecution::Thread, [&]()
		{
			Started.store(true, std::memory_order_release);
			return CGHReconstruction::Reconstruct(I, Cancel);
		});
		while (!Started.load(std::memory_order_acquire)) FPlatformProcess::SleepNoStats(0.001f);
		FPlatformProcess::SleepNoStats(0.03f);
		const double Begin = FPlatformTime::Seconds();
		Cancel.store(true, std::memory_order_relaxed);
		const auto Result = Future.Get();
		TestFalse(TEXT("Cancellation suppresses both the intermediate pupil and final sensor field"), Result.bSucceeded);
		TestTrue(TEXT("Cancelled camera job has empty output"), Result.Field.Samples.IsEmpty());
		TestTrue(TEXT("Camera cancellation remains bounded within inner source work"), FPlatformTime::Seconds() - Begin < 3.0);
		TestTrue(TEXT("Camera cancellation reports its cause"), Result.Error.Contains(TEXT("cancelled")));
	}
	return true;
}

#endif
