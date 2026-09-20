#include "CGH/Utils/CGHMeshSampling.h"

namespace
{
	struct FSpatialKey
	{
		int64 X = 0;
		int64 Y = 0;
		int64 Z = 0;

		bool operator==(const FSpatialKey& Other) const
		{
			return X == Other.X && Y == Other.Y && Z == Other.Z;
		}

		friend uint32 GetTypeHash(const FSpatialKey& Key)
		{
			return HashCombineFast(HashCombineFast(::GetTypeHash(Key.X), ::GetTypeHash(Key.Y)), ::GetTypeHash(Key.Z));
		}
	};

	/** Neighbor-bin lookup avoids missing duplicates that straddle a quantization boundary. */
	class FPositionLookup
	{
	public:
		FPositionLookup(const FVector3d& InOrigin, double InTolerance)
			: Origin(InOrigin), Tolerance(InTolerance)
		{
		}

		int32 FindOrAdd(const FVector3d& Position, bool& bAdded)
		{
			const FVector3d Relative = (Position - Origin) / Tolerance;
			const FSpatialKey Key { FMath::FloorToInt64(Relative.X), FMath::FloorToInt64(Relative.Y), FMath::FloorToInt64(Relative.Z) };
			for (int64 DX = -1; DX <= 1; ++DX)
			{
				for (int64 DY = -1; DY <= 1; ++DY)
				{
					for (int64 DZ = -1; DZ <= 1; ++DZ)
					{
						if (const TArray<int32>* Candidates = Bins.Find({ Key.X + DX, Key.Y + DY, Key.Z + DZ }))
						{
							for (int32 Candidate : *Candidates)
							{
								if (FVector3d::DistSquared(Positions[Candidate], Position) <= FMath::Square(Tolerance))
								{
									bAdded = false;
									return Candidate;
								}
							}
						}
					}
				}
			}
			bAdded = true;
			const int32 Index = Positions.Add(Position);
			Bins.FindOrAdd(Key).Add(Index);
			return Index;
		}

	private:
		FVector3d Origin;
		double Tolerance;
		TArray<FVector3d> Positions;
		TMap<FSpatialKey, TArray<int32>> Bins;
	};

	struct FSampleVertex
	{
		FVector3d Position;
		FVector3d Normal;
		FVector2d UV;
	};

	FSampleVertex Interpolate(const FSampleVertex& A, const FSampleVertex& B, double Alpha)
	{
		FVector3d Normal = FMath::Lerp(A.Normal, B.Normal, Alpha);
		if (Normal.IsNearlyZero())
		{
			Normal = Alpha <= 0.5 ? A.Normal : B.Normal;
		}
		return { FMath::Lerp(A.Position, B.Position, Alpha), Normal, FMath::Lerp(A.UV, B.UV, Alpha) };
	}

	struct FCoplanarEdgeKey
	{
		int32 Slice;
		int32 A;
		int32 B;

		bool operator==(const FCoplanarEdgeKey& Other) const
		{
			return Slice == Other.Slice && A == Other.A && B == Other.B;
		}

		friend uint32 GetTypeHash(const FCoplanarEdgeKey& Key)
		{
			return HashCombineFast(HashCombineFast(::GetTypeHash(Key.Slice), ::GetTypeHash(Key.A)), ::GetTypeHash(Key.B));
		}
	};

	struct FCoplanarEdge
	{
		FSampleVertex A;
		FSampleVertex B;
		int32 Count = 0;
	};
}

bool CGHMeshSampling::Sample(const FCGHMeshGeometryResource& Geometry, const FVector3d& AxisLocal,
	int32 SliceCount, double PointSpacingM, int32 MaxPoints, double Amplitude, double PhaseRad,
	TArray<FCGHObjectPoint>& OutPoints, FString& OutError)
{
	OutPoints.Reset();
	OutError.Reset();
	const auto Fail = [&OutPoints, &OutError](const TCHAR* Message)
	{
		OutPoints.Reset();
		OutError = Message;
		return false;
	};
	if (SliceCount < 1 || SliceCount > MaximumSliceCount || MaxPoints < 1 || MaxPoints > MaximumPointCount)
	{
		return Fail(TEXT("Slice count must be 1..4096 and maximum point count must be 1..1000000."));
	}
	if (!FMath::IsFinite(PointSpacingM) || PointSpacingM <= 0.0 ||
		!FMath::IsFinite(Amplitude) || Amplitude < 0.0 || !FMath::IsFinite(PhaseRad))
	{
		return Fail(TEXT("Point spacing must be finite and positive; amplitude must be finite and nonnegative; phase must be finite."));
	}
	if (AxisLocal.ContainsNaN() || !FMath::IsFinite(AxisLocal.SizeSquared()) || AxisLocal.IsNearlyZero())
	{
		return Fail(TEXT("The sampling axis must be finite and nonzero."));
	}
	if (Geometry.VerticesM.Num() < 3 || Geometry.Indices.Num() < 3 || Geometry.Indices.Num() % 3 != 0)
	{
		return Fail(TEXT("Mesh geometry requires vertices and a nonempty triangle index list."));
	}
	if ((!Geometry.Normals.IsEmpty() && Geometry.Normals.Num() != Geometry.VerticesM.Num()) ||
		(!Geometry.UVs.IsEmpty() && Geometry.UVs.Num() != Geometry.VerticesM.Num()))
	{
		return Fail(TEXT("Normals and UVs must be empty or have one value per vertex."));
	}

	FBox3d Bounds(ForceInit);
	for (const FVector3d& Vertex : Geometry.VerticesM)
	{
		if (Vertex.ContainsNaN())
		{
			return Fail(TEXT("Mesh vertices must contain only finite coordinates."));
		}
		Bounds += Vertex;
	}
	for (uint32 Index : Geometry.Indices)
	{
		if (Index >= static_cast<uint32>(Geometry.VerticesM.Num()))
		{
			return Fail(TEXT("A mesh triangle references a vertex outside the vertex array."));
		}
	}
	for (const FVector3f& Normal : Geometry.Normals)
	{
		if (Normal.ContainsNaN())
		{
			return Fail(TEXT("Mesh normals must contain only finite values."));
		}
	}
	for (const FVector2f& UV : Geometry.UVs)
	{
		if (UV.ContainsNaN())
		{
			return Fail(TEXT("Mesh UVs must contain only finite values."));
		}
	}

	const double Diagonal = Bounds.GetSize().Length();
	if (!FMath::IsFinite(Diagonal) || Diagonal <= 1.0e-12)
	{
		return Fail(TEXT("Mesh geometry has no finite, nonzero extent."));
	}
	// Keep bins in int64 range and reject spacing that cannot be distinguished at this mesh scale.
	const double Tolerance = FMath::Max(Diagonal * 1.0e-10, 1.0e-12);
	if (PointSpacingM < Tolerance * 10.0)
	{
		return Fail(TEXT("Point spacing is too small relative to mesh extent for reliable sampling."));
	}
	const FVector3d Axis = AxisLocal.GetSafeNormal();
	TArray<double> Depths;
	Depths.Reserve(Geometry.VerticesM.Num());
	double MinDepth = TNumericLimits<double>::Max();
	double MaxDepth = TNumericLimits<double>::Lowest();
	for (const FVector3d& Vertex : Geometry.VerticesM)
	{
		const double Depth = FVector3d::DotProduct(Vertex - Bounds.Min, Axis);
		Depths.Add(Depth);
		MinDepth = FMath::Min(MinDepth, Depth);
		MaxDepth = FMath::Max(MaxDepth, Depth);
	}
	const bool bFlat = MaxDepth - MinDepth <= Tolerance;
	const int32 PlaneCount = bFlat ? 1 : SliceCount;
	const double SliceStep = bFlat ? 0.0 : (MaxDepth - MinDepth) / PlaneCount;
	if (!bFlat && SliceStep <= Tolerance * 2.0)
	{
		return Fail(TEXT("Slice spacing is too small relative to mesh extent; reduce the slice count."));
	}
	const double FirstPlane = bFlat ? (MinDepth + MaxDepth) * 0.5 : MinDepth + SliceStep * 0.5;

	FPositionLookup PointLookup(Bounds.Min, Tolerance);
	FPositionLookup VertexLookup(Bounds.Min, Tolerance);
	TArray<int32> CanonicalVertices;
	CanonicalVertices.Reserve(Geometry.VerticesM.Num());
	for (const FVector3d& Position : Geometry.VerticesM)
	{
		bool bAdded;
		CanonicalVertices.Add(VertexLookup.FindOrAdd(Position, bAdded));
	}
	TMap<FCoplanarEdgeKey, FCoplanarEdge> CoplanarEdges;
	// Maps preserve identity; an ordered key list keeps output independent of hash-table layout.
	TArray<FCoplanarEdgeKey> CoplanarOrder;
	int64 IntersectionTests = 0;
	int64 SampleAttempts = 0;
	constexpr int64 MaximumIntersectionTests = 20000000;

	const auto AddPoint = [&](const FSampleVertex& Vertex)
	{
		if (++SampleAttempts > MaximumIntersectionTests)
		{
			return Fail(TEXT("Point sampling exceeds the processing limit; simplify the mesh or increase point spacing."));
		}
		bool bAdded;
		PointLookup.FindOrAdd(Vertex.Position, bAdded);
		if (!bAdded)
		{
			return true;
		}
		if (OutPoints.Num() >= MaxPoints)
		{
			return Fail(TEXT("Point cloud exceeds the maximum point count; increase point spacing or reduce slice count."));
		}
		FCGHObjectPoint& Point = OutPoints.AddDefaulted_GetRef();
		Point.PositionLocalM = Vertex.Position;
		Point.NormalLocal = FVector3f(Vertex.Normal.GetSafeNormal(0.0));
		Point.UV = FVector2f(Vertex.UV);
		Point.Amplitude = Amplitude;
		Point.Phase = PhaseRad;
		return true;
	};
	const auto AddSegment = [&](const FSampleVertex& A, const FSampleVertex& B)
	{
		const double Length = FVector3d::Distance(A.Position, B.Position);
		if (Length <= Tolerance)
		{
			return AddPoint(A);
		}
		const double RequiredSteps = FMath::CeilToDouble(Length / PointSpacingM);
		if (!FMath::IsFinite(RequiredSteps) || RequiredSteps >= MaxPoints)
		{
			return Fail(TEXT("A sampled edge alone exceeds the maximum point count; increase point spacing."));
		}
		const int32 Steps = FMath::Max(1, static_cast<int32>(RequiredSteps));
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			if (!AddPoint(Interpolate(A, B, static_cast<double>(Step) / Steps)))
			{
				return false;
			}
		}
		return true;
	};

	for (int32 Triangle = 0; Triangle < Geometry.Indices.Num(); Triangle += 3)
	{
		uint32 Indices[3] = { Geometry.Indices[Triangle], Geometry.Indices[Triangle + 1], Geometry.Indices[Triangle + 2] };
		const FVector3d FaceCross = FVector3d::CrossProduct(
			Geometry.VerticesM[Indices[1]] - Geometry.VerticesM[Indices[0]],
			Geometry.VerticesM[Indices[2]] - Geometry.VerticesM[Indices[0]]);
		if (!FMath::IsFinite(FaceCross.SizeSquared()))
		{
			return Fail(TEXT("Mesh coordinates exceed the numerical range for triangle intersection."));
		}
		if (FaceCross.SizeSquared() <= FMath::Pow(Tolerance, 4.0))
		{
			continue;
		}
		const FVector3d FaceNormal = FaceCross.GetSafeNormal(0.0);
		FSampleVertex Vertices[3];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const uint32 Index = Indices[Corner];
			Vertices[Corner] = { Geometry.VerticesM[Index], Geometry.Normals.IsEmpty() ? FaceNormal : FVector3d(Geometry.Normals[Index]),
				Geometry.UVs.IsEmpty() ? FVector2d::ZeroVector : FVector2d(Geometry.UVs[Index]) };
			if (Vertices[Corner].Normal.IsNearlyZero())
			{
				Vertices[Corner].Normal = FaceNormal;
			}
		}
		const double TriangleMin = FMath::Min3(Depths[Indices[0]], Depths[Indices[1]], Depths[Indices[2]]);
		const double TriangleMax = FMath::Max3(Depths[Indices[0]], Depths[Indices[1]], Depths[Indices[2]]);
		const int32 StartSlice = bFlat ? 0 : FMath::Max(0, FMath::CeilToInt32((TriangleMin - FirstPlane - Tolerance) / SliceStep));
		const int32 EndSlice = bFlat ? 0 : FMath::Min(PlaneCount - 1, FMath::FloorToInt32((TriangleMax - FirstPlane + Tolerance) / SliceStep));
		for (int32 Slice = StartSlice; Slice <= EndSlice; ++Slice)
		{
			if (++IntersectionTests > MaximumIntersectionTests)
			{
				return Fail(TEXT("Mesh slicing exceeds the processing limit; reduce slice count or mesh complexity."));
			}
			const double Plane = FirstPlane + Slice * SliceStep;
			double Distances[3] = { Depths[Indices[0]] - Plane, Depths[Indices[1]] - Plane, Depths[Indices[2]] - Plane };
			bool OnPlane[3] = { FMath::Abs(Distances[0]) <= Tolerance, FMath::Abs(Distances[1]) <= Tolerance,
				FMath::Abs(Distances[2]) <= Tolerance };
			if (OnPlane[0] && OnPlane[1] && OnPlane[2])
			{
				for (int32 Edge = 0; Edge < 3; ++Edge)
				{
					const int32 Next = (Edge + 1) % 3;
					const int32 A = CanonicalVertices[Indices[Edge]];
					const int32 B = CanonicalVertices[Indices[Next]];
					const FCoplanarEdgeKey Key { Slice, FMath::Min(A, B), FMath::Max(A, B) };
					FCoplanarEdge& Entry = CoplanarEdges.FindOrAdd(Key);
					if (Entry.Count++ == 0)
					{
						Entry.A = Vertices[Edge];
						Entry.B = Vertices[Next];
						CoplanarOrder.Add(Key);
						if (CoplanarOrder.Num() > 3000000)
						{
							return Fail(TEXT("Coplanar boundary extraction exceeds the processing limit; simplify the mesh."));
						}
					}
				}
				continue;
			}

			TArray<FSampleVertex, TInlineAllocator<3>> Intersections;
			const auto AddIntersection = [&](const FSampleVertex& Vertex)
			{
				for (const FSampleVertex& Existing : Intersections)
				{
					if (FVector3d::DistSquared(Existing.Position, Vertex.Position) <= FMath::Square(Tolerance))
					{
						return;
					}
				}
				Intersections.Add(Vertex);
			};
			for (int32 Edge = 0; Edge < 3; ++Edge)
			{
				const int32 Next = (Edge + 1) % 3;
				if (OnPlane[Edge])
				{
					AddIntersection(Vertices[Edge]);
				}
				if (!OnPlane[Edge] && !OnPlane[Next] && (Distances[Edge] < 0.0) != (Distances[Next] < 0.0))
				{
					AddIntersection(Interpolate(Vertices[Edge], Vertices[Next], Distances[Edge] / (Distances[Edge] - Distances[Next])));
				}
			}
			if (Intersections.Num() == 1 && !AddPoint(Intersections[0]))
			{
				return false;
			}
			if (Intersections.Num() >= 2 && !AddSegment(Intersections[0], Intersections[1]))
			{
				return false;
			}
		}
	}
	for (const FCoplanarEdgeKey& Key : CoplanarOrder)
	{
		const FCoplanarEdge& Edge = CoplanarEdges.FindChecked(Key);
		if (Edge.Count == 1 && !AddSegment(Edge.A, Edge.B))
		{
			return false;
		}
	}
	if (OutPoints.IsEmpty())
	{
		return Fail(TEXT("The selected slice planes do not intersect any nondegenerate mesh surfaces."));
	}
	return true;
}
