#include "CGH/Utils/CGHMeshSampling.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include <limits>

namespace
{
	FCGHMeshGeometryResource MakeUnitCube()
	{
		FCGHMeshGeometryResource Geometry;
		Geometry.VerticesM = {
			{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
			{0.0, 0.0, 1.0}, {1.0, 0.0, 1.0}, {1.0, 1.0, 1.0}, {0.0, 1.0, 1.0}
		};
		Geometry.Indices = {
			0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
			0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
			0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5
		};
		for (const FVector& Position : Geometry.VerticesM)
		{
			Geometry.Normals.Add(FVector3f(0.0f, Position.Y + 1.0, Position.Z + 1.0));
			Geometry.UVs.Add(FVector2f(Position.Y, Position.Z));
		}
		return Geometry;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHMeshSlicesTest,
	"CGH.MeshSampling.CubeSlicesAndAttributes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHMeshSlicesTest::RunTest(const FString& Parameters)
{
	const FCGHMeshGeometryResource Geometry = MakeUnitCube();
	TArray<FCGHObjectPoint> Points;
	FString Error;
	if (!TestTrue(TEXT("A cube produces two sampled contours"), CGHMeshSampling::Sample(
		Geometry, FVector3d::XAxisVector, 2, 0.25, 1000, 0.75, -0.4, Points, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Each unit-square contour has 16 unique points at 0.25m spacing"), Points.Num(), 32);
	TestTrue(TEXT("Successful sampling clears the error"), Error.IsEmpty());
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		const FCGHObjectPoint& Point = Points[Index];
		const FVector& P = Point.PositionLocalM;
		TestTrue(TEXT("Samples lie on slice midplanes"), FMath::IsNearlyEqual(P.X, 0.25) || FMath::IsNearlyEqual(P.X, 0.75));
		TestTrue(TEXT("Samples lie on the cross-section perimeter"),
			FMath::IsNearlyZero(P.Y) || FMath::IsNearlyEqual(P.Y, 1.0) || FMath::IsNearlyZero(P.Z) || FMath::IsNearlyEqual(P.Z, 1.0));
		TestEqual(TEXT("UV Y is interpolated from mesh vertices"), static_cast<double>(Point.UV.X), P.Y, 1.0e-6);
		TestEqual(TEXT("UV Z is interpolated from mesh vertices"), static_cast<double>(Point.UV.Y), P.Z, 1.0e-6);
		const FVector3d ExpectedNormal = FVector3d(0.0, P.Y + 1.0, P.Z + 1.0).GetSafeNormal();
		TestTrue(TEXT("Normals are interpolated and normalized"), FVector3d(Point.NormalLocal).Equals(ExpectedNormal, 1.0e-6));
		TestEqual(TEXT("Sample amplitude"), Point.Amplitude, 0.75);
		TestEqual(TEXT("Sample phase"), Point.Phase, -0.4);
		for (int32 Other = Index + 1; Other < Points.Num(); ++Other)
		{
			TestFalse(TEXT("Shared triangle endpoints appear only once"), P.Equals(Points[Other].PositionLocalM, 1.0e-8));
		}
	}
	TestTrue(TEXT("Changing the sampling axis succeeds"), CGHMeshSampling::Sample(
		Geometry, FVector3d::YAxisVector, 2, 0.25, 1000, 1.0, 0.0, Points, Error));
	TestEqual(TEXT("Rotated slicing has the same contour size"), Points.Num(), 32);
	for (const FCGHObjectPoint& Point : Points)
	{
		TestTrue(TEXT("Changed axis changes the slice planes"),
			FMath::IsNearlyEqual(Point.PositionLocalM.Y, 0.25) || FMath::IsNearlyEqual(Point.PositionLocalM.Y, 0.75));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHMeshPlanarTest,
	"CGH.MeshSampling.CoplanarBoundariesAndDegenerates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHMeshPlanarTest::RunTest(const FString& Parameters)
{
	FCGHMeshGeometryResource Geometry;
	// Separate coincident seam vertices exercise geometry-based boundary welding.
	Geometry.VerticesM = { {0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 0}, {0, 1, 1}, {0, 0, 1} };
	Geometry.Indices = { 0, 1, 2, 3, 4, 5, 0, 0, 1 };
	TArray<FCGHObjectPoint> Points;
	FString Error;
	if (!TestTrue(TEXT("A zero-depth mesh uses one coplanar slice"), CGHMeshSampling::Sample(
		Geometry, FVector3d::XAxisVector, 16, 0.25, 1000, 1.0, 0.0, Points, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Only square boundary edges contribute points"), Points.Num(), 16);
	for (const FCGHObjectPoint& Point : Points)
	{
		const FVector& P = Point.PositionLocalM;
		TestTrue(TEXT("Internal triangulation diagonal is excluded"),
			FMath::IsNearlyZero(P.Y) || FMath::IsNearlyEqual(P.Y, 1.0) || FMath::IsNearlyZero(P.Z) || FMath::IsNearlyEqual(P.Z, 1.0));
		TestTrue(TEXT("Missing normals fall back to the face normal"), FVector3d(Point.NormalLocal).Equals(FVector3d::XAxisVector, 1.0e-6));
		TestTrue(TEXT("Missing UVs default to zero"), Point.UV.IsNearlyZero());
	}
	Geometry.Indices = { 0, 0, 1 };
	TestFalse(TEXT("An entirely degenerate mesh reports an error"), CGHMeshSampling::Sample(
		Geometry, FVector3d::XAxisVector, 1, 0.25, 1000, 1.0, 0.0, Points, Error));
	TestTrue(TEXT("Degenerate failure clears previous points"), Points.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHMeshSamplingLimitsTest,
	"CGH.MeshSampling.InvalidInputsAndPointLimits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHMeshSamplingLimitsTest::RunTest(const FString& Parameters)
{
	FCGHMeshGeometryResource Geometry = MakeUnitCube();
	TArray<FCGHObjectPoint> Points;
	FString Error;
	const auto ExpectFailure = [&](const TCHAR* Label, const FVector3d& Axis, int32 Slices, double Spacing, int32 Limit)
	{
		Points.AddDefaulted();
		TestFalse(Label, CGHMeshSampling::Sample(Geometry, Axis, Slices, Spacing, Limit, 1.0, 0.0, Points, Error));
		TestTrue(TEXT("Failed sampling clears partial or stale points"), Points.IsEmpty());
		TestFalse(TEXT("Failed sampling explains the error"), Error.IsEmpty());
	};
	ExpectFailure(TEXT("Output limit is enforced after multiple segments"), FVector3d::XAxisVector, 2, 0.25, 20);
	ExpectFailure(TEXT("A dense segment is rejected before allocating its samples"), FVector3d::XAxisVector, 2, 0.0001, 100);
	ExpectFailure(TEXT("Zero spacing is rejected"), FVector3d::XAxisVector, 2, 0.0, 1000);
	ExpectFailure(TEXT("NaN spacing is rejected"), FVector3d::XAxisVector, 2, std::numeric_limits<double>::quiet_NaN(), 1000);
	ExpectFailure(TEXT("Zero axis is rejected"), FVector3d::ZeroVector, 2, 0.25, 1000);
	ExpectFailure(TEXT("Unbounded slice count is rejected"), FVector3d::XAxisVector, 4097, 0.25, 1000);
	ExpectFailure(TEXT("Unbounded output count is rejected"), FVector3d::XAxisVector, 2, 0.25, 1000001);
	Geometry.Indices[0] = 999;
	ExpectFailure(TEXT("Invalid triangle indices are rejected"), FVector3d::XAxisVector, 2, 0.25, 1000);
	Geometry = MakeUnitCube();
	Geometry.Normals.Pop();
	ExpectFailure(TEXT("Mismatched attributes are rejected"), FVector3d::XAxisVector, 2, 0.25, 1000);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
