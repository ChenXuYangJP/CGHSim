#include "CGH/Solver/CGHPointFocus.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include <cmath>
#include <complex>
#include <limits>

namespace
{
	constexpr double SuperpositionTwoPi = 2.0 * UE_DOUBLE_PI;

	struct FExpectedSource
	{
		FVector Position;
		double Amplitude;
		double Phase;
	};

	FCGHSolverInput MakeSuperpositionInput()
	{
		FCGHSolverInput Input;
		Input.Scene.SLM.ResolutionX = 4;
		Input.Scene.SLM.ResolutionY = 3;
		Input.Scene.SLM.PixelPitchXM = 0.03;
		Input.Scene.SLM.PixelPitchYM = 0.05;
		Input.Scene.ReconstructionLight.WavelengthM = 0.47;
		Input.Scene.ReconstructionLight.DirectionSLM = FVector(0.8, 0.6, 0.0);
		Input.Scene.ReconstructionLight.InitialPhaseRad = 0.29;
		Input.Scene.ReconstructionLight.Amplitude = 1.0;
		FCGHTargetDescription Point;
		Point.TargetType = ECGHTargetType::Point;
		Point.PositionSLMM = FVector(0.4, 0.11, -0.06);
		Point.Amplitude = 1.0;
		Point.PhaseRad = -0.7;
		Input.Scene.Targets.Add(Point);
		return Input;
	}

	FCGHObjectPoint MakeSample(const FVector& Position, double Amplitude, double Phase)
	{
		FCGHObjectPoint Sample;
		Sample.PositionLocalM = Position;
		Sample.Amplitude = Amplitude;
		Sample.Phase = Phase;
		return Sample;
	}

	FCGHSolverInput MakeMeshInput()
	{
		FCGHSolverInput Input = MakeSuperpositionInput();
		FCGHTargetDescription& Mesh = Input.Scene.Targets[0];
		Mesh.TargetType = ECGHTargetType::Mesh;
		Mesh.ResourceId = 17;
		Mesh.Revision = 3;
		Mesh.PositionSLMM = FVector(0.5, -0.04, 0.08);
		Mesh.RotationSLM = FQuat(FVector::ForwardVector, UE_DOUBLE_PI / 2.0);
		// Deliberately distinct from samples: these properties have already been baked into them.
		Mesh.Amplitude = 8.0;
		Mesh.PhaseRad = 2.3;
		FCGHPointCloudResource Cloud;
		Cloud.ResourceId = Mesh.ResourceId;
		Cloud.Revision = Mesh.Revision;
		Cloud.Points.Add(MakeSample(FVector(0.03, 0.02, -0.07), 2.0, -0.4));
		Cloud.Points.Add(MakeSample(FVector(-0.02, -0.09, 0.03), 0.6, 1.7));
		Input.PointClouds.Add(MoveTemp(Cloud));
		return Input;
	}

	double CircularError(double Actual, double Expected)
	{
		return std::abs(std::remainder(Actual - Expected, SuperpositionTwoPi));
	}

	bool CheckIndependentComplexField(FAutomationTestBase& Test, const FCGHSolverInput& Input,
		const FCGHSolverResult& Result, const TArray<FExpectedSource>& Sources)
	{
		if (!Test.TestTrue(TEXT("The fixture computes successfully"), Result.bSucceeded))
		{
			Test.AddError(Result.Error);
			return false;
		}
		const int32 NX = Input.Scene.SLM.ResolutionX;
		const int32 NY = Input.Scene.SLM.ResolutionY;
		if (!Test.TestEqual(TEXT("Exactly one phase is produced per pixel"), Result.Pattern.PhaseRad.Num(), NX * NY))
		{
			return false;
		}
		const double K = SuperpositionTwoPi / Input.Scene.ReconstructionLight.WavelengthM;
		for (int32 Row = 0; Row < NY; ++Row)
		{
			for (int32 Column = 0; Column < NX; ++Column)
			{
				const double Y = (2.0 * Column - NX + 1.0) * Input.Scene.SLM.PixelPitchXM / 2.0;
				const double Z = (NY - 1.0 - 2.0 * Row) * Input.Scene.SLM.PixelPitchYM / 2.0;
				std::complex<double> Field(0.0, 0.0);
				for (const FExpectedSource& Source : Sources)
				{
					const double DX = Source.Position.X;
					const double DY = Source.Position.Y - Y;
					const double DZ = Source.Position.Z - Z;
					const double Distance = std::sqrt(DX * DX + DY * DY + DZ * DZ);
					Field += std::polar(Source.Amplitude, Source.Phase - K * Distance);
				}
				const double Incident = Input.Scene.ReconstructionLight.InitialPhaseRad
					+ K * (Input.Scene.ReconstructionLight.DirectionSLM.Y * Y + Input.Scene.ReconstructionLight.DirectionSLM.Z * Z);
				const double Actual = Result.Pattern.PhaseRad[Row * NX + Column];
				Test.TestTrue(TEXT("Each pixel uses the argument of the independently summed complex field"),
					CircularError(Actual, std::arg(Field) - Incident) < 2.0e-12);
				Test.TestTrue(TEXT("Complex result remains in the canonical half-open phase interval"), Actual >= 0.0 && Actual < SuperpositionTwoPi);
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusMultiplePointsTest,
	"CGH.Solver.PointFocus.MultiplePointsUseWeightedComplexSum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusMultiplePointsTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> Cancel{false};
	FCGHSolverInput Input = MakeSuperpositionInput();
	FCGHTargetDescription Second = Input.Scene.Targets[0];
	Second.PositionSLMM = FVector(-0.61, -0.13, 0.07);
	Second.Amplitude = 0.3;
	Second.PhaseRad = 1.8 + 8.0 * SuperpositionTwoPi;
	Input.Scene.Targets.Add(Second);
	const FCGHSolverResult Result = CGHPointFocus::Solve(Input, Cancel);
	if (!CheckIndependentComplexField(*this, Input, Result, {
		{FVector(0.4, 0.11, -0.06), 1.0, -0.7}, {FVector(-0.61, -0.13, 0.07), 0.3, Second.PhaseRad}}))
	{
		return false;
	}
	FCGHSolverInput FirstOnly = Input;
	FirstOnly.Scene.Targets.SetNum(1);
	FCGHSolverInput SecondOnly = Input;
	SecondOnly.Scene.Targets.RemoveAt(0);
	const FCGHSolverResult FirstPattern = CGHPointFocus::Solve(FirstOnly, Cancel);
	const FCGHSolverResult SecondPattern = CGHPointFocus::Solve(SecondOnly, Cancel);
	TestTrue(TEXT("Adding individual phase patterns gives a different, incorrect result"),
		CircularError(Result.Pattern.PhaseRad[0], FirstPattern.Pattern.PhaseRad[0] + SecondPattern.Pattern.PhaseRad[0]) > 0.1);
	Input.Scene.Targets[1].Amplitude = 1.0;
	const FCGHSolverResult EqualWeights = CGHPointFocus::Solve(Input, Cancel);
	if (!CheckIndependentComplexField(*this, Input, EqualWeights, {
		{FVector(0.4, 0.11, -0.06), 1.0, -0.7}, {FVector(-0.61, -0.13, 0.07), 1.0, Second.PhaseRad}}))
	{
		return false;
	}
	double MaximumAmplitudeInducedPhaseChange = 0.0;
	for (int32 Index = 0; Index < Result.Pattern.PhaseRad.Num(); ++Index)
	{
		MaximumAmplitudeInducedPhaseChange = FMath::Max(MaximumAmplitudeInducedPhaseChange,
			CircularError(EqualWeights.Pattern.PhaseRad[Index], Result.Pattern.PhaseRad[Index]));
	}
	// The independent fields differ by up to approximately 0.19846 radians over this aperture.
	TestTrue(TEXT("Relative target amplitude changes the multiple-target phase pattern across the aperture"),
		MaximumAmplitudeInducedPhaseChange > 0.1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusMeshTransformTest,
	"CGH.Solver.PointFocus.MeshRigidPoseAndBakedSampleFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusMeshTransformTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> Cancel{false};
	FCGHSolverInput Input = MakeMeshInput();
	const FCGHSolverResult Mesh = CGHPointFocus::Solve(Input, Cancel);
	// A +90-degree X quaternion maps local (x,y,z) to (x,-z,y), followed by target translation.
	if (!CheckIndependentComplexField(*this, Input, Mesh, {
		{FVector(0.53, 0.03, 0.10), 2.0, -0.4}, {FVector(0.48, -0.07, -0.01), 0.6, 1.7}}))
	{
		return false;
	}
	Input.Scene.Targets[0].Amplitude = 0.0;
	Input.Scene.Targets[0].PhaseRad = -17.0;
	Input.PointClouds[0].Points[0].NormalLocal.X = std::numeric_limits<float>::quiet_NaN();
	Input.PointClouds[0].Points[1].UV.Y = std::numeric_limits<float>::infinity();
	const FCGHSolverResult DescriptiveChange = CGHPointFocus::Solve(Input, Cancel);
	TestTrue(TEXT("Mesh amplitude/phase are not applied twice, and scalar fields ignore normals/UVs"),
		DescriptiveChange.bSucceeded && DescriptiveChange.Pattern.PhaseRad == Mesh.Pattern.PhaseRad);

	FCGHTargetDescription Point;
	Point.PositionSLMM = FVector(-0.35, 0.03, 0.04);
	Point.Amplitude = 0.8;
	Point.PhaseRad = -2.0;
	Input.Scene.Targets.Add(Point);
	FCGHTargetDescription OtherMesh = Input.Scene.Targets[0];
	OtherMesh.ResourceId = 19;
	OtherMesh.Revision = 5;
	OtherMesh.RotationSLM = FQuat::Identity;
	OtherMesh.PositionSLMM = FVector(-0.6, 0.01, -0.02);
	Input.Scene.Targets.Add(OtherMesh);
	FCGHPointCloudResource OtherCloud;
	OtherCloud.ResourceId = 19;
	OtherCloud.Revision = 5;
	OtherCloud.Points.Add(MakeSample(FVector(0.05, -0.02, 0.01), 1.1, 0.6));
	Input.PointClouds.Insert(MoveTemp(OtherCloud), 0); // Resource order deliberately differs from target order.
	return CheckIndependentComplexField(*this, Input, CGHPointFocus::Solve(Input, Cancel), {
		{FVector(0.53, 0.03, 0.10), 2.0, -0.4}, {FVector(0.48, -0.07, -0.01), 0.6, 1.7},
		{FVector(-0.35, 0.03, 0.04), 0.8, -2.0}, {FVector(-0.55, -0.01, -0.01), 1.1, 0.6}});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusAmplitudeAndCancellationTest,
	"CGH.Solver.PointFocus.AmplitudeScalingZeroSourcesAndDestructiveInterference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusAmplitudeAndCancellationTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> Cancel{false};
	FCGHSolverInput Input = MakeSuperpositionInput();
	const FCGHSolverResult Single = CGHPointFocus::Solve(Input, Cancel);
	FCGHTargetDescription Zero = Input.Scene.Targets[0];
	Zero.Amplitude = 0.0;
	Zero.PositionSLMM = FVector::ZeroVector;
	Zero.PhaseRad = 9.0;
	Input.Scene.Targets.Add(Zero);
	const FCGHSolverResult Ignored = CGHPointFocus::Solve(Input, Cancel);
	TestTrue(TEXT("A zero-amplitude point contributes nothing, including on the SLM plane"),
		Ignored.bSucceeded && Ignored.Pattern.PhaseRad == Single.Pattern.PhaseRad);
	Input.Scene.Targets[1] = Input.Scene.Targets[0];
	Input.Scene.Targets[1].Amplitude = 0.25;
	Input.Scene.Targets[1].PhaseRad += 1.2;
	const FCGHSolverResult Regular = CGHPointFocus::Solve(Input, Cancel);
	Input.Scene.Targets[0].Amplitude = std::numeric_limits<double>::max();
	Input.Scene.Targets[1].Amplitude = std::numeric_limits<double>::max() / 4.0;
	const FCGHSolverResult Huge = CGHPointFocus::Solve(Input, Cancel);
	TestTrue(TEXT("Global amplitude normalization prevents overflow without changing phase"),
		Huge.bSucceeded && Huge.Pattern.PhaseRad == Regular.Pattern.PhaseRad);
	Input.Scene.Targets[0].Amplitude = std::numeric_limits<double>::min();
	Input.Scene.Targets[1].Amplitude = std::numeric_limits<double>::min() / 4.0;
	const FCGHSolverResult Tiny = CGHPointFocus::Solve(Input, Cancel);
	TestTrue(TEXT("A uniformly tiny positive field has the same phase"),
		Tiny.bSucceeded && Tiny.Pattern.PhaseRad == Regular.Pattern.PhaseRad);

	Input = MakeSuperpositionInput();
	Input.Scene.SLM.ResolutionX = 1;
	Input.Scene.SLM.ResolutionY = 1;
	Input.Scene.ReconstructionLight.WavelengthM = SuperpositionTwoPi;
	Input.Scene.ReconstructionLight.InitialPhaseRad = 0.93;
	Input.Scene.Targets[0].PositionSLMM = FVector(1.0, 0.0, 0.0);
	Input.Scene.Targets[0].PhaseRad = 1.0;
	const FCGHTargetDescription Opposing = Input.Scene.Targets[0];
	Input.Scene.Targets.Add(Opposing);
	Input.Scene.Targets[1].PhaseRad += UE_DOUBLE_PI;
	const FCGHSolverResult Destructive = CGHPointFocus::Solve(Input, Cancel);
	if (!TestTrue(TEXT("Destructive interference is a valid result"), Destructive.bSucceeded))
	{
		return false;
	}
	TestEqual(TEXT("Undefined phase at a destructive field zero uses zero SLM modulation, regardless of incident phase"),
		Destructive.Pattern.PhaseRad[0], 0.0);
	Input.Scene.ReconstructionLight.WavelengthM = 532.0e-9;
	Input.Scene.SLM.ResolutionX = 4;
	Input.Scene.SLM.ResolutionY = 3;
	const FCGHSolverResult OpticalDestructive = CGHPointFocus::Solve(Input, Cancel);
	TestTrue(TEXT("Colocated opposite fields also cancel at optical wavelengths"), OpticalDestructive.bSucceeded);
	for (double Phase : OpticalDestructive.Pattern.PhaseRad)
	{
		TestEqual(TEXT("A large propagation phase does not destroy an opposing source phase relationship"), Phase, 0.0);
	}
	Input.Scene.ReconstructionLight.WavelengthM = SuperpositionTwoPi;
	Input.Scene.SLM.ResolutionX = 1;
	Input.Scene.SLM.ResolutionY = 1;
	Input.Scene.Targets[1].Amplitude -= 1.0e-10;
	const FCGHSolverResult Resolved = CGHPointFocus::Solve(Input, Cancel);
	TestTrue(TEXT("A small but resolved field above the cancellation threshold retains its phase"),
		Resolved.bSucceeded && CircularError(Resolved.Pattern.PhaseRad[0], -0.93) < 2.0e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusMeshValidationTest,
	"CGH.Solver.PointFocus.MeshResourceAndSampleValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusMeshValidationTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> Cancel{false};
	const auto Reject = [this, &Cancel](const TCHAR* Label, auto Mutate)
	{
		FCGHSolverInput Input = MakeMeshInput();
		Mutate(Input);
		FString Error;
		TestFalse(Label, CGHPointFocus::ValidateInput(Input, Error));
		TestFalse(TEXT("Invalid resource diagnostic is populated"), Error.IsEmpty());
		const FCGHSolverResult Result = CGHPointFocus::Solve(Input, Cancel);
		TestFalse(TEXT("Invalid resources cannot be published"), Result.bSucceeded);
		TestTrue(TEXT("Validation failure leaves no partial pattern"), Result.Pattern.PhaseRad.IsEmpty());
		TestEqual(TEXT("Failure resets dimensions"), Result.Pattern.ResolutionX, 0);
	};
	Reject(TEXT("Missing mesh cloud is rejected"), [](FCGHSolverInput& I) { I.PointClouds.Reset(); });
	Reject(TEXT("Stale mesh cloud revision is rejected"), [](FCGHSolverInput& I) { ++I.PointClouds[0].Revision; });
	Reject(TEXT("Wrong mesh cloud ID is rejected"), [](FCGHSolverInput& I) { ++I.PointClouds[0].ResourceId; });
	Reject(TEXT("Zero mesh description ID is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].ResourceId = 0; });
	Reject(TEXT("Zero mesh description revision is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].Revision = 0; });
	Reject(TEXT("Zero cloud ID is rejected"), [](FCGHSolverInput& I) { I.PointClouds[0].ResourceId = 0; });
	Reject(TEXT("Zero cloud revision is rejected"), [](FCGHSolverInput& I) { I.PointClouds[0].Revision = 0; });
	Reject(TEXT("Duplicate cloud ID is rejected"), [](FCGHSolverInput& I) { const FCGHPointCloudResource Copy = I.PointClouds[0]; I.PointClouds.Add(Copy); });
	Reject(TEXT("Duplicate mesh description ID is rejected"), [](FCGHSolverInput& I) { const FCGHTargetDescription Copy = I.Scene.Targets[0]; I.Scene.Targets.Add(Copy); });
	Reject(TEXT("Empty clouds are rejected"), [](FCGHSolverInput& I) { I.PointClouds[0].Points.Reset(); });
	Reject(TEXT("Unexpected cloud is rejected"), [](FCGHSolverInput& I)
	{
		FCGHPointCloudResource Extra = I.PointClouds[0];
		++Extra.ResourceId;
		I.PointClouds.Add(MoveTemp(Extra));
	});
	Reject(TEXT("A point target cannot consume a mesh cloud"), [](FCGHSolverInput& I) { I.Scene.Targets[0].TargetType = ECGHTargetType::Point; });
	Reject(TEXT("Nonunit mesh quaternion is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].RotationSLM.W = 2.0; });
	Reject(TEXT("NaN mesh quaternion is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].RotationSLM.X = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite mesh position is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].PositionSLMM.Y = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("Negative mesh description amplitude is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].Amplitude = -1.0; });
	Reject(TEXT("NaN mesh description phase is rejected"), [](FCGHSolverInput& I) { I.Scene.Targets[0].PhaseRad = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("NaN sample position is rejected"), [](FCGHSolverInput& I) { I.PointClouds[0].Points[0].PositionLocalM.Z = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite sample position is rejected"), [](FCGHSolverInput& I) { I.PointClouds[0].Points[0].PositionLocalM.Z = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("Negative sample amplitude is rejected"), [](FCGHSolverInput& I) { I.PointClouds[0].Points[0].Amplitude = -0.1; });
	Reject(TEXT("NaN sample amplitude is rejected"), [](FCGHSolverInput& I) { I.PointClouds[0].Points[0].Amplitude = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite sample amplitude is rejected"), [](FCGHSolverInput& I) { I.PointClouds[0].Points[0].Amplitude = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("NaN sample phase is rejected"), [](FCGHSolverInput& I) { I.PointClouds[0].Points[0].Phase = std::numeric_limits<double>::quiet_NaN(); });
	Reject(TEXT("Infinite sample phase is rejected"), [](FCGHSolverInput& I) { I.PointClouds[0].Points[0].Phase = std::numeric_limits<double>::infinity(); });
	Reject(TEXT("All-zero cloud cannot define a phase"), [](FCGHSolverInput& I)
	{
		for (FCGHObjectPoint& Point : I.PointClouds[0].Points) { Point.Amplitude = 0.0; }
	});
	Reject(TEXT("A contributing transformed sample on the SLM plane is rejected"), [](FCGHSolverInput& I)
	{
		I.Scene.Targets[0].RotationSLM = FQuat::Identity;
		I.PointClouds[0].Points[0].PositionLocalM.X = -I.Scene.Targets[0].PositionSLMM.X;
	});
	Reject(TEXT("Finite positions causing transform overflow are rejected"), [](FCGHSolverInput& I)
	{
		I.Scene.Targets[0].RotationSLM = FQuat::Identity;
		I.Scene.Targets[0].PositionSLMM.X = std::numeric_limits<double>::max();
		I.PointClouds[0].Points[0].PositionLocalM.X = std::numeric_limits<double>::max();
	});
	Reject(TEXT("Sample propagation phase overflow is rejected"), [](FCGHSolverInput& I)
	{
		I.Scene.Targets[0].RotationSLM = FQuat::Identity;
		I.PointClouds[0].Points[0].PositionLocalM.X = std::numeric_limits<double>::max() / 2.0;
	});
	FCGHSolverInput Input = MakeMeshInput();
	Input.PointClouds.Reset();
	FString Error;
	TestTrue(TEXT("Cheap scene validation accepts valid mesh descriptions without copying or inspecting resources"), CGHPointFocus::ValidateScene(Input, Error));
	TestFalse(TEXT("Full validation still requires owned resources"), CGHPointFocus::ValidateInput(Input, Error));
	Input = MakeSuperpositionInput();
	Input.Scene.Targets[0].Amplitude = 0.0;
	TestFalse(TEXT("An all-zero point scene is rejected early"), CGHPointFocus::ValidateScene(Input, Error));
	Input = MakeMeshInput();
	Input.PointClouds[0].Points.SetNum(CGHPointFocus::MaximumEmitterCount);
	FCGHTargetDescription ExtraPoint = MakeSuperpositionInput().Scene.Targets[0];
	Input.Scene.Targets.Add(ExtraPoint);
	TestFalse(TEXT("The aggregate limit includes mesh samples plus separate point targets"), CGHPointFocus::ValidateInput(Input, Error));
	TestTrue(TEXT("Emitter cap failure is explicit"), Error.Contains(TEXT("1,000,000")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPointFocusOwnedCloudTest,
	"CGH.Solver.PointFocus.OwnedCloudSnapshotAndCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPointFocusOwnedCloudTest::RunTest(const FString& Parameters)
{
	const std::atomic<bool> Cancel{false};
	FCGHSolverInput Source = MakeMeshInput();
	FCGHSolverInput Snapshot = Source;
	FCGHSolverJob Job(MoveTemp(Snapshot));
	const FCGHSolverResult Reference = CGHPointFocus::Solve(Source, Cancel);
	Source.PointClouds[0].Points[0].Phase += 0.9;
	Source.PointClouds[0].Points[0].PositionLocalM.Y += 0.1;
	Source.PointClouds[0].Points[1].Amplitude *= 4.0;
	++Source.PointClouds[0].Revision;
	const FCGHSolverResult Owned = CGHPointFocus::Solve(Job.Input, Cancel);
	TestTrue(TEXT("A submitted cloud owns all values independently of subsequent source edits"),
		Owned.bSucceeded && Reference.bSucceeded && Owned.Pattern.PhaseRad == Reference.Pattern.PhaseRad);

	FCGHSolverInput Large = MakeMeshInput();
	Large.Scene.SLM.ResolutionX = 16384;
	Large.Scene.SLM.ResolutionY = 1;
	const FCGHObjectPoint RepeatedSample = Large.PointClouds[0].Points[0];
	Large.PointClouds[0].Points.Init(RepeatedSample, 4096);
	std::atomic<bool> Requested{false};
	std::atomic<bool> Started{false};
	TFuture<FCGHSolverResult> Future = Async(EAsyncExecution::Thread, [&Large, &Requested, &Started]()
	{
		Started.store(true, std::memory_order_release);
		return CGHPointFocus::Solve(Large, Requested);
	});
	while (!Started.load(std::memory_order_acquire)) { FPlatformProcess::SleepNoStats(0.001f); }
	FPlatformProcess::SleepNoStats(0.02f);
	const double CancelStart = FPlatformTime::Seconds();
	Requested.store(true, std::memory_order_relaxed);
	const FCGHSolverResult Cancelled = Future.Get();
	TestFalse(TEXT("A running multiple-source solve cooperatively cancels"), Cancelled.bSucceeded);
	TestTrue(TEXT("Cancellation clears partially calculated pixels"), Cancelled.Pattern.PhaseRad.IsEmpty());
	TestTrue(TEXT("Cancellation is observed within the large single row"), FPlatformTime::Seconds() - CancelStart < 3.0);
	TestTrue(TEXT("Cancellation identifies its cause"), Cancelled.Error.Contains(TEXT("cancelled")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
