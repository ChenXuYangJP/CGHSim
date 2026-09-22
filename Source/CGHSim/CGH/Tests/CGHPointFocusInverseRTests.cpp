#include "CGH/Solver/CGHPointFocus.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"
#include <cmath>
#include <complex>
#include <limits>

namespace CGHInverseRTests
{
	struct FSource { FVector Position; double Amplitude; double Phase; };

	FCGHSolverInput Input()
	{
		FCGHSolverInput Result;
		Result.Algorithm = ECGHSolverAlgorithm::PointFocusInverseR;
		Result.Scene.SLM.ResolutionX = 4;
		Result.Scene.SLM.ResolutionY = 3;
		Result.Scene.SLM.PixelPitchXM = 0.03;
		Result.Scene.SLM.PixelPitchYM = 0.05;
		Result.Scene.ReconstructionLight.WavelengthM = 0.47;
		Result.Scene.ReconstructionLight.DirectionSLM = FVector(0.8, 0.6, 0.0);
		Result.Scene.ReconstructionLight.InitialPhaseRad = 0.29;
		return Result;
	}

	void SetSources(FCGHSolverInput& Input, const TArray<FSource>& Sources)
	{
		Input.Scene.Targets.Reset();
		for (const FSource& Source : Sources)
		{
			FCGHTargetDescription Target;
			Target.PositionSLMM = Source.Position;
			Target.Amplitude = Source.Amplitude;
			Target.PhaseRad = Source.Phase;
			Input.Scene.Targets.Add(Target);
		}
	}

	double PhaseError(double A, double B) { return std::abs(std::remainder(A - B, UE_DOUBLE_TWO_PI)); }

	// Independent long-double complex arithmetic permits direct A/r even when binary64 A/r overflows or underflows.
	bool CheckOracle(FAutomationTestBase& Test, const FCGHSolverInput& Input, const TArray<FSource>& Sources)
	{
		const std::atomic<bool> Cancel{false};
		const FCGHSolverResult Result = CGHPointFocus::Solve(Input, Cancel);
		if (!Test.TestTrue(TEXT("Inverse-distance CPU solve succeeds"), Result.bSucceeded)) { Test.AddError(Result.Error); return false; }
		const auto& SLM = Input.Scene.SLM;
		const auto& Light = Input.Scene.ReconstructionLight;
		const double K = UE_DOUBLE_TWO_PI / Light.WavelengthM;
		if (!Test.TestEqual(TEXT("Inverse-distance output contains every pixel"), Result.Pattern.PhaseRad.Num(), SLM.ResolutionX * SLM.ResolutionY)) return false;
		for (int32 Row = 0; Row < SLM.ResolutionY; ++Row)
		{
			for (int32 Column = 0; Column < SLM.ResolutionX; ++Column)
			{
				const double Y = (2.0 * Column - SLM.ResolutionX + 1.0) * SLM.PixelPitchXM / 2.0;
				const double Z = (SLM.ResolutionY - 1.0 - 2.0 * Row) * SLM.PixelPitchYM / 2.0;
				std::complex<long double> Field(0.0L, 0.0L);
				for (const FSource& Source : Sources)
				{
					const long double X = Source.Position.X;
					const long double DY = static_cast<long double>(Source.Position.Y) - Y;
					const long double DZ = static_cast<long double>(Source.Position.Z) - Z;
					const long double R = std::sqrt(X * X + DY * DY + DZ * DZ);
					Field += std::polar(static_cast<long double>(Source.Amplitude) / R, static_cast<long double>(Source.Phase) - K * R);
				}
				const double Incident = Light.InitialPhaseRad + K * (Light.DirectionSLM.Y * Y + Light.DirectionSLM.Z * Z);
				const double Actual = Result.Pattern.PhaseRad[Row * SLM.ResolutionX + Column];
				Test.TestTrue(TEXT("Phase matches independently summed A/r complex field and incident compensation"),
					PhaseError(Actual, static_cast<double>(std::arg(Field)) - Incident) < 2.e-12);
				Test.TestTrue(TEXT("Inverse-distance phase uses the canonical half-open interval"), Actual >= 0.0 && Actual < UE_DOUBLE_TWO_PI);
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHInverseRReferenceTest,
	"CGH.Solver.PointFocusInverseR.ReferenceAndCompatibility", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHInverseRReferenceTest::RunTest(const FString& Parameters)
{
	using namespace CGHInverseRTests;
	const std::atomic<bool> Cancel{false};
	FCGHSolverInput Scene = Input();
	const TArray<FSource> Sources = {{FVector(0.2, 0.11, -0.06), 1.0, -0.7},
		{FVector(-0.81, -0.13, 0.07), 0.3, 1.8}, {FVector(0.47, 0.04, 0.08), 1.7, -2.2}};
	SetSources(Scene, Sources);
	if (!CheckOracle(*this, Scene, Sources)) return false;
	const FCGHSolverResult Weighted = CGHPointFocus::Solve(Scene, Cancel);
	Scene.Algorithm = ECGHSolverAlgorithm::PointFocus;
	const FCGHSolverResult Legacy = CGHPointFocus::Solve(Scene, Cancel);
	TestTrue(TEXT("Different source distances materially change the phase"),
		Legacy.bSucceeded && PhaseError(Weighted.Pattern.PhaseRad[0], Legacy.Pattern.PhaseRad[0]) > 0.1);
	Scene.Scene.Targets.SetNum(1);
	const FCGHSolverResult LegacySingle = CGHPointFocus::Solve(Scene, Cancel);
	Scene.Algorithm = ECGHSolverAlgorithm::PointFocusInverseR;
	for (const double Amplitude : {1.0, std::numeric_limits<double>::max(), std::numeric_limits<double>::min()})
	{
		Scene.Scene.Targets[0].Amplitude = Amplitude;
		const FCGHSolverResult Single = CGHPointFocus::Solve(Scene, Cancel);
		TestTrue(TEXT("Single positive emitter keeps the original analytical phase exactly"), Single.bSucceeded && Single.Pattern.PhaseRad == LegacySingle.Pattern.PhaseRad);
	}
	const TArray<FSource> Equidistant = {{FVector(0.4, 0.0, 0.0), 0.7, 0.4}, {FVector(-0.4, 0.0, 0.0), 1.3, 1.2}};
	SetSources(Scene, Equidistant);
	const FCGHSolverResult EqualR = CGHPointFocus::Solve(Scene, Cancel);
	Scene.Algorithm = ECGHSolverAlgorithm::PointFocus;
	const FCGHSolverResult EqualRLegacy = CGHPointFocus::Solve(Scene, Cancel);
	if (!TestTrue(TEXT("Equal-distance fixtures succeed in both modes"), EqualR.bSucceeded && EqualRLegacy.bSucceeded)) return false;
	for (int32 Index = 0; Index < EqualR.Pattern.PhaseRad.Num(); ++Index)
	{
		TestTrue(TEXT("Common inverse-distance factor leaves the phase unchanged"), PhaseError(EqualR.Pattern.PhaseRad[Index], EqualRLegacy.Pattern.PhaseRad[Index]) < 2.e-12);
	}
	TestEqual(TEXT("Original enum ordinal is unchanged"), static_cast<uint8>(ECGHSolverAlgorithm::PointFocus), uint8(0));
	TestTrue(TEXT("Original algorithm remains the parameter default"), FCGHSolverParameters().Algorithm == ECGHSolverAlgorithm::PointFocus);
	const FProperty* Property = FindFProperty<FProperty>(FCGHSolverParameters::StaticStruct(), TEXT("Algorithm"));
	if (TestNotNull(TEXT("Algorithm is reflected"), Property))
	{
		TestTrue(TEXT("Details can edit the algorithm"), Property->HasAnyPropertyFlags(CPF_Edit));
		TestTrue(TEXT("Blueprint can select the algorithm"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible) && !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
	}
	TestEqual(TEXT("New mode is reflected in the enum"), StaticEnum<ECGHSolverAlgorithm>()->GetValueByNameString(TEXT("PointFocusInverseR")), int64(1));
#if WITH_EDITOR
	TestEqual(TEXT("New mode has the requested Details label"), StaticEnum<ECGHSolverAlgorithm>()->GetDisplayNameTextByValue(1).ToString(), FString(TEXT("Point Focus (1/r Amplitude)")));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHInverseRMeshTest,
	"CGH.Solver.PointFocusInverseR.MeshSamplesUseDistanceAndBakedAmplitude", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHInverseRMeshTest::RunTest(const FString& Parameters)
{
	using namespace CGHInverseRTests;
	FCGHSolverInput Scene = Input();
	FCGHTargetDescription Mesh;
	Mesh.TargetType = ECGHTargetType::Mesh;
	Mesh.ResourceId = 17; Mesh.Revision = 3;
	Mesh.PositionSLMM = FVector(0.5, -0.04, 0.08);
	Mesh.RotationSLM = FQuat(FVector::ForwardVector, UE_DOUBLE_PI / 2.0);
	Mesh.Amplitude = 83.0; Mesh.PhaseRad = -7.8;
	Scene.Scene.Targets.Add(Mesh);
	FCGHPointCloudResource Cloud;
	Cloud.ResourceId = Mesh.ResourceId; Cloud.Revision = Mesh.Revision;
	FCGHObjectPoint First, Second;
	First.PositionLocalM = FVector(0.03, 0.02, -0.07); First.Amplitude = 2.0; First.Phase = -0.4;
	Second.PositionLocalM = FVector(-0.02, -0.09, 0.03); Second.Amplitude = 0.6; Second.Phase = 1.7;
	Cloud.Points = {First, Second};
	Scene.PointClouds.Add(Cloud);
	return CheckOracle(*this, Scene, {{FVector(0.53, 0.03, 0.10), 2.0, -0.4}, {FVector(0.48, -0.07, -0.01), 0.6, 1.7}});
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHInverseRExtremeTest,
	"CGH.Solver.PointFocusInverseR.ExtremeAmplitudesAndDistances", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHInverseRExtremeTest::RunTest(const FString& Parameters)
{
	using namespace CGHInverseRTests;
	FCGHSolverInput Scene = Input();
	Scene.Scene.SLM.ResolutionX = 1; Scene.Scene.SLM.ResolutionY = 1;
	Scene.Scene.ReconstructionLight.WavelengthM = std::numeric_limits<double>::max();
	const double Maximum = std::numeric_limits<double>::max();
	const double Minimum = std::numeric_limits<double>::min();
	const TArray<TArray<FSource>> Cases = {
		{{FVector(1.e-200, 0.0, 0.0), 1.e308, 0.5}, {FVector(2.e-200, 0.0, 0.0), 2.5e307, -1.0}},
		{{FVector(1.e300, 0.0, 0.0), 1.e-300, 0.5}, {FVector(5.e299, 0.0, 0.0), 5.e-301, -1.0}},
		{{FVector(Maximum / 4.0, 0.0, 0.0), Maximum, 0.2}, {FVector(Minimum, 0.0, 0.0), Minimum, 1.4}}
	};
	for (const TArray<FSource>& Sources : Cases)
	{
		SetSources(Scene, Sources);
		if (!CheckOracle(*this, Scene, Sources)) return false;
		const TArray<FSource> Reversed = {Sources[1], Sources[0]};
		SetSources(Scene, Reversed);
		if (!CheckOracle(*this, Scene, Reversed)) return false;
	}
	// Some standard libraries underflow the three-component hypot to zero for a positive subnormal X.
	volatile double TinyX = 1.e-323;
	SetSources(Scene, {{FVector(TinyX, 0.0, 0.0), 1.0, 0.7}});
	const double NumericalDistance = std::hypot(Scene.Scene.Targets[0].PositionSLMM.X, 0.0, 0.0);
	const std::atomic<bool> Cancel{false};
	const FCGHSolverResult TinyInverse = CGHPointFocus::Solve(Scene, Cancel);
	Scene.Algorithm = ECGHSolverAlgorithm::PointFocus;
	const FCGHSolverResult TinyLegacy = CGHPointFocus::Solve(Scene, Cancel);
	TestTrue(TEXT("Original single-source mode retains its positive subnormal-distance behavior"), TinyLegacy.bSucceeded);
	if (NumericalDistance == 0.0)
	{
		TestFalse(TEXT("Inverse-distance single-source mode rejects a numerical zero distance"), TinyInverse.bSucceeded);
		TestTrue(TEXT("Rejected zero distance exposes no partial pattern"), TinyInverse.Pattern.PhaseRad.IsEmpty());
		TestEqual(TEXT("Rejected zero distance clears output dimensions"), TinyInverse.Pattern.ResolutionX, 0);
		TestTrue(TEXT("Zero-distance rejection explains the required positive distance"), TinyInverse.Error.Contains(TEXT("positive")));
	}
	else
	{
		TestTrue(TEXT("A representable positive subnormal distance retains the original single-source phase"),
			TinyInverse.bSucceeded && TinyInverse.Pattern.PhaseRad == TinyLegacy.Pattern.PhaseRad);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHInverseRZeroCancellationTest,
	"CGH.Solver.PointFocusInverseR.ZeroFieldsValidationAndCancellation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHInverseRZeroCancellationTest::RunTest(const FString& Parameters)
{
	using namespace CGHInverseRTests;
	FCGHSolverInput Scene = Input();
	Scene.Scene.SLM.ResolutionX = 1; Scene.Scene.SLM.ResolutionY = 1;
	Scene.Scene.ReconstructionLight.WavelengthM = UE_DOUBLE_TWO_PI;
	Scene.Scene.ReconstructionLight.InitialPhaseRad = 0.93;
	SetSources(Scene, {{FVector(1.0, 0.0, 0.0), 1.0, 1.0}, {FVector(2.0, 0.0, 0.0), 2.0, 2.0 + UE_DOUBLE_PI}});
	std::atomic<bool> Cancel{false};
	const FCGHSolverResult Zero = CGHPointFocus::Solve(Scene, Cancel);
	if (!TestTrue(TEXT("Equal inverse-distance opposing fields produce a valid zero"), Zero.bSucceeded)) return false;
	TestEqual(TEXT("Undefined phase becomes exact zero independently of incident phase"), Zero.Pattern.PhaseRad[0], 0.0);
	Scene.Scene.Targets[1].Amplitude *= 1.0 - 1.e-10;
	const FCGHSolverResult Resolved = CGHPointFocus::Solve(Scene, Cancel);
	TestTrue(TEXT("Resolved residual above the weighted cancellation threshold retains its phase"),
		Resolved.bSucceeded && PhaseError(Resolved.Pattern.PhaseRad[0], -0.93) < 2.e-5);
	Scene.Scene.Targets[0].PositionSLMM = FVector::ZeroVector;
	const FCGHSolverResult Invalid = CGHPointFocus::Solve(Scene, Cancel);
	TestTrue(TEXT("Contributing points on the SLM plane are rejected without partial output"), !Invalid.bSucceeded && Invalid.Pattern.PhaseRad.IsEmpty());
	Scene.Scene.Targets[0].Amplitude = 0.0;
	TestTrue(TEXT("A zero-amplitude point on the SLM plane is ignored"), CGHPointFocus::Solve(Scene, Cancel).bSucceeded);
	Scene.Scene.Targets[1].Amplitude = 0.0;
	TestFalse(TEXT("All-zero emitters cannot define a hologram"), CGHPointFocus::Solve(Scene, Cancel).bSucceeded);
	Scene = Input();
	SetSources(Scene, {{FVector(0.3, 0.01, -0.02), 1.0, 0.7}});
	const FCGHTargetDescription Source = Scene.Scene.Targets[0];
	Scene.Scene.Targets.Init(Source, 4096);
	Scene.Scene.SLM.ResolutionX = 16384; Scene.Scene.SLM.ResolutionY = 1;
	std::atomic<bool> Started{false};
	TFuture<FCGHSolverResult> Future = Async(EAsyncExecution::Thread, [&]()
	{
		Started.store(true, std::memory_order_release);
		return CGHPointFocus::Solve(Scene, Cancel);
	});
	while (!Started.load(std::memory_order_acquire)) FPlatformProcess::SleepNoStats(0.001f);
	FPlatformProcess::SleepNoStats(0.02f);
	const double CancelStart = FPlatformTime::Seconds();
	Cancel.store(true, std::memory_order_relaxed);
	const FCGHSolverResult Cancelled = Future.Get();
	TestTrue(TEXT("A running inverse-distance accumulation cancels without publishing partial pixels"), !Cancelled.bSucceeded && Cancelled.Pattern.PhaseRad.IsEmpty());
	TestTrue(TEXT("Inner emitter loop observes cancellation promptly"), FPlatformTime::Seconds() - CancelStart < 3.0);
	TestTrue(TEXT("Cancellation supplies its cause"), Cancelled.Error.Contains(TEXT("cancelled")));
	return true;
}

#endif
