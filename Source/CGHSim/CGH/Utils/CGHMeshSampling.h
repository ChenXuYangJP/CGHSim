#pragma once

#include "CGH/Types/CGHTypes.h"

/** CPU-only surface-contour sampling; no UObject, render-thread or static-mesh dependencies. */
namespace CGHMeshSampling
{
	inline constexpr int32 MaximumSliceCount = 4096;
	inline constexpr int32 MaximumPointCount = 1000000;

	/**
	 * Intersect the mesh with SliceCount evenly spaced planes perpendicular to AxisLocal.
	 * Planes lie at the midpoint of each depth interval; flat meshes use one plane.
	 * Sample every intersection segment with spacing <= PointSpacingM, including endpoints.
	 * Attributes are linearly interpolated, normals normalized, and coincident points merged.
	 * Coplanar triangles contribute only their boundary edges. Degenerate triangles are ignored.
	 * On failure OutPoints is empty and OutError explains the invalid input or exceeded limit.
	 */
	CGHSIM_API bool Sample(const FCGHMeshGeometryResource& Geometry, const FVector3d& AxisLocal,
		int32 SliceCount, double PointSpacingM, int32 MaxPoints, double Amplitude, double PhaseRad,
		TArray<FCGHObjectPoint>& OutPoints, FString& OutError);
}
