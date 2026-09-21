#include "CGH/Solver/CGHPointFocus.h"

#include "CGH/Utils/CGHPhasePreview.h"
#include "HAL/PlatformTime.h"
#include <cmath>
#include <limits>

namespace
{
	constexpr double TwoPi = 2.0 * UE_DOUBLE_PI;
	constexpr double CancellationTolerance = 32.0 * std::numeric_limits<double>::epsilon();

	struct FEmitter
	{
		FVector Position;
		double Amplitude;
		double Phase;
		double WeightedReal = 0.0;
		double WeightedImaginary = 0.0;
	};

	struct FAperture
	{
		double HalfY;
		double HalfZ;
		double K;
		double MinimumIncident;
		double MaximumIncident;
	};

	// Neumaier summation retains small contributions even when large complex fields cancel.
	struct FCompensatedSum
	{
		double Sum = 0.0;
		double Correction = 0.0;
		void Add(double Value)
		{
			const double Next = Sum + Value;
			Correction += std::abs(Sum) >= std::abs(Value) ? (Sum - Next) + Value : (Value - Next) + Sum;
			Sum = Next;
		}
		double Value() const { return Sum + Correction; }
	};

	bool Fail(FString& Error, const TCHAR* Message)
	{
		Error = Message;
		return false;
	}

	bool CheckCancelled(const std::atomic<bool>* CancelRequested, FString& Error)
	{
		return CancelRequested && CancelRequested->load(std::memory_order_relaxed)
			? Fail(Error, TEXT("PointFocus job cancelled.")) : true;
	}

	bool IsPositiveFinite(double Value)
	{
		return std::isfinite(Value) && Value > 0.0;
	}

	bool IsFinite(const FVector& Value)
	{
		return std::isfinite(Value.X) && std::isfinite(Value.Y) && std::isfinite(Value.Z);
	}

	double WrapToTwoPi(double Phase)
	{
		double Wrapped = std::fmod(Phase, TwoPi);
		if (Wrapped < 0.0)
		{
			Wrapped += TwoPi;
		}
		return Wrapped >= TwoPi || Wrapped == 0.0 ? 0.0 : Wrapped;
	}

	bool ValidateEmitter(const FVector& Position, double Amplitude, double Phase,
		const FAperture& Aperture, FString& Error)
	{
		if (!IsFinite(Position) || !std::isfinite(Phase) || !std::isfinite(Amplitude) || Amplitude < 0.0)
		{
			return Fail(Error, TEXT("Target/sample positions and phases must be finite; amplitudes must be finite and nonnegative."));
		}
		if (Amplitude == 0.0)
		{
			return true;
		}
		if (Position.X == 0.0)
		{
			return Fail(Error, TEXT("Every contributing point must lie off the SLM plane (SLM-local X must be nonzero)."));
		}
		// Check the farthest corner before phase allocation. hypot avoids squared-distance overflow.
		const double MaximumDistance = std::hypot(Position.X, std::abs(Position.Y) + Aperture.HalfY,
			std::abs(Position.Z) + Aperture.HalfZ);
		const double MaximumPropagationPhase = Aperture.K * MaximumDistance;
		const double MinimumPhase = Phase - MaximumPropagationPhase;
		if (!std::isfinite(MaximumDistance) || !std::isfinite(MaximumPropagationPhase)
			|| !std::isfinite(MinimumPhase) || !std::isfinite(MinimumPhase - Aperture.MaximumIncident)
			|| !std::isfinite(Phase - Aperture.MinimumIncident))
		{
			return Fail(Error, TEXT("The scene geometry, propagation phase or incident plane-wave phase exceeds finite double-precision bounds."));
		}
		return true;
	}

	bool ValidateSceneInternal(const FCGHSolverInput& Input, FAperture& Aperture, FString& Error,
		const std::atomic<bool>* CancelRequested)
	{
		Error.Reset();
		if (!CheckCancelled(CancelRequested, Error))
		{
			return false;
		}
		if (Input.Scene.SchemaVersion != 2)
		{
			return Fail(Error, TEXT("PointFocus requires scene schema version 2."));
		}
		if (Input.Algorithm != ECGHSolverAlgorithm::PointFocus)
		{
			return Fail(Error, TEXT("The CPU backend supports only the PointFocus algorithm."));
		}
		if (Input.PropagationConvention != ECGHPropagationConvention::ExpPositiveIKR)
		{
			return Fail(Error, TEXT("PointFocus requires the exp(+i*k*r) propagation convention."));
		}
		const FCGHSLMDescription& SLM = Input.Scene.SLM;
		if (SLM.ModulationType != ECGHSLMModulationType::PhaseOnly)
		{
			return Fail(Error, TEXT("PointFocus requires a phase-only SLM."));
		}
		if (SLM.ResolutionX <= 0 || SLM.ResolutionY <= 0
			|| SLM.ResolutionX > CGHPhasePreview::MaximumAxisResolution
			|| SLM.ResolutionY > CGHPhasePreview::MaximumAxisResolution
			|| static_cast<int64>(SLM.ResolutionX) * SLM.ResolutionY > CGHPhasePreview::MaximumPixelCount)
		{
			return Fail(Error, TEXT("SLM dimensions exceed the supported phase-pattern bounds."));
		}
		if (!IsPositiveFinite(SLM.PixelPitchXM) || !IsPositiveFinite(SLM.PixelPitchYM))
		{
			return Fail(Error, TEXT("SLM pixel pitches must be finite and positive, in meters."));
		}
		const FCGHReconstructionLightDescription& Light = Input.Scene.ReconstructionLight;
		if (Light.SourceType != ECGHSourceType::PlaneWave)
		{
			return Fail(Error, TEXT("PointFocus supports only PlaneWave reconstruction illumination."));
		}
		if (!IsPositiveFinite(Light.WavelengthM) || !std::isfinite(TwoPi / Light.WavelengthM))
		{
			return Fail(Error, TEXT("The reconstruction wavelength must be finite and positive with a finite wave number."));
		}
		if (!std::isfinite(Light.InitialPhaseRad))
		{
			return Fail(Error, TEXT("The plane-wave phase at the SLM origin must be finite."));
		}
		const FVector& Direction = Light.DirectionSLM;
		const double DirectionNormSquared = Direction.SizeSquared();
		if (!IsFinite(Direction) || !std::isfinite(DirectionNormSquared) || std::abs(DirectionNormSquared - 1.0) > 1.0e-6)
		{
			return Fail(Error, TEXT("Plane-wave DirectionSLM must be finite and unit length (squared-norm tolerance 1e-6)."));
		}
		if (!std::isfinite(Light.Amplitude) || Light.Amplitude < 0.0)
		{
			return Fail(Error, TEXT("The reconstruction amplitude must be finite and nonnegative."));
		}
		if (!std::isfinite(Light.PolarizationAngleRad))
		{
			return Fail(Error, TEXT("The reconstruction polarization angle must be finite."));
		}
		Aperture.HalfY = (SLM.ResolutionX - 1) / 2.0 * SLM.PixelPitchXM;
		Aperture.HalfZ = (SLM.ResolutionY - 1) / 2.0 * SLM.PixelPitchYM;
		Aperture.K = TwoPi / Light.WavelengthM;
		const double MaximumIncidentSpatialPhase = Aperture.K
			* (std::abs(Direction.Y) * Aperture.HalfY + std::abs(Direction.Z) * Aperture.HalfZ);
		Aperture.MinimumIncident = Light.InitialPhaseRad - MaximumIncidentSpatialPhase;
		Aperture.MaximumIncident = Light.InitialPhaseRad + MaximumIncidentSpatialPhase;
		if (!std::isfinite(Aperture.HalfY) || !std::isfinite(Aperture.HalfZ)
			|| !std::isfinite(MaximumIncidentSpatialPhase) || !std::isfinite(Aperture.MinimumIncident)
			|| !std::isfinite(Aperture.MaximumIncident))
		{
			return Fail(Error, TEXT("The SLM aperture or incident plane-wave phase exceeds finite double-precision bounds."));
		}
		if (Input.Scene.Targets.IsEmpty() || Input.Scene.Targets.Num() > CGHPointFocus::MaximumEmitterCount)
		{
			return Fail(Error, TEXT("PointFocus requires targets and supports at most 1,000,000 total source points."));
		}
		TSet<uint64> MeshResourceIds;
		bool bHasPotentialContribution = false;
		for (int32 Index = 0; Index < Input.Scene.Targets.Num(); ++Index)
		{
			if ((Index & 255) == 0 && !CheckCancelled(CancelRequested, Error))
			{
				return false;
			}
			const FCGHTargetDescription& Target = Input.Scene.Targets[Index];
			if (!IsFinite(Target.PositionSLMM) || !std::isfinite(Target.PhaseRad)
				|| !std::isfinite(Target.Amplitude) || Target.Amplitude < 0.0)
			{
				return Fail(Error, TEXT("Target positions and phases must be finite; amplitudes must be finite and nonnegative."));
			}
			switch (Target.TargetType)
			{
			case ECGHTargetType::Point:
				if (!ValidateEmitter(Target.PositionSLMM, Target.Amplitude, Target.PhaseRad, Aperture, Error))
				{
					return false;
				}
				bHasPotentialContribution |= Target.Amplitude > 0.0;
				break;
			case ECGHTargetType::Mesh:
			{
				if (Target.ResourceId == 0 || Target.Revision == 0 || MeshResourceIds.Contains(Target.ResourceId))
				{
					return Fail(Error, TEXT("Mesh targets require unique nonzero resource IDs and nonzero revisions."));
				}
				MeshResourceIds.Add(Target.ResourceId);
				const FQuat& Rotation = Target.RotationSLM;
				const double NormSquared = Rotation.SizeSquared();
				if (!std::isfinite(Rotation.X) || !std::isfinite(Rotation.Y) || !std::isfinite(Rotation.Z)
					|| !std::isfinite(Rotation.W) || !std::isfinite(NormSquared) || std::abs(NormSquared - 1.0) > 1.0e-6)
				{
					return Fail(Error, TEXT("Mesh RotationSLM must be finite and unit length (squared-norm tolerance 1e-6)."));
				}
				bHasPotentialContribution = true; // Sample amplitudes are resolved only with the cloud.
				break;
			}
			default:
				return Fail(Error, TEXT("PointFocus supports only Point and Mesh targets."));
			}
		}
		return bHasPotentialContribution || Fail(Error, TEXT("PointFocus requires at least one source point with positive amplitude."));
	}

	bool GatherEmitters(const FCGHSolverInput& Input, TArray<FEmitter>* Emitters,
		double& MaximumAmplitude, FAperture& Aperture, FString& Error, const std::atomic<bool>* CancelRequested)
	{
		if (!ValidateSceneInternal(Input, Aperture, Error, CancelRequested))
		{
			return false;
		}
		// This table only references the immutable input; numerical workers never access actors/resources.
		TMap<uint64, const FCGHPointCloudResource*> Clouds;
		int64 TotalPoints = 0;
		int32 CloudIndex = 0;
		for (const FCGHPointCloudResource& Cloud : Input.PointClouds)
		{
			if ((CloudIndex++ & 255) == 0 && !CheckCancelled(CancelRequested, Error))
			{
				return false;
			}
			if (Cloud.ResourceId == 0 || Cloud.Revision == 0 || Clouds.Contains(Cloud.ResourceId) || Cloud.Points.IsEmpty())
			{
				return Fail(Error, TEXT("Mesh point clouds must be nonempty with unique nonzero resource IDs and nonzero revisions."));
			}
			TotalPoints += Cloud.Points.Num();
			if (TotalPoints > CGHPointFocus::MaximumEmitterCount)
			{
				return Fail(Error, TEXT("PointFocus supports at most 1,000,000 total source points."));
			}
			Clouds.Add(Cloud.ResourceId, &Cloud);
		}
		int32 MeshCount = 0;
		for (int32 Index = 0; Index < Input.Scene.Targets.Num(); ++Index)
		{
			if ((Index & 255) == 0 && !CheckCancelled(CancelRequested, Error))
			{
				return false;
			}
			const FCGHTargetDescription& Target = Input.Scene.Targets[Index];
			if (Target.TargetType == ECGHTargetType::Mesh)
			{
				const FCGHPointCloudResource* const* Found = Clouds.Find(Target.ResourceId);
				if (!Found || (*Found)->Revision != Target.Revision)
				{
					return Fail(Error, TEXT("Each mesh target requires a point cloud with matching ResourceId and Revision."));
				}
				++MeshCount;
			}
			else
			{
				++TotalPoints;
			}
		}
		if (Clouds.Num() != MeshCount)
		{
			return Fail(Error, TEXT("Solver input contains unexpected or stale point-cloud resources."));
		}
		if (TotalPoints > CGHPointFocus::MaximumEmitterCount)
		{
			return Fail(Error, TEXT("PointFocus supports at most 1,000,000 total source points."));
		}
		if (!CheckCancelled(CancelRequested, Error))
		{
			return false;
		}
		if (Emitters)
		{
			Emitters->Reserve(static_cast<int32>(TotalPoints));
		}
		MaximumAmplitude = 0.0;
		const auto Add = [Emitters, &MaximumAmplitude](const FVector& Position, double Amplitude, double Phase)
		{
			if (Amplitude > 0.0)
			{
				MaximumAmplitude = FMath::Max(MaximumAmplitude, Amplitude);
				if (Emitters)
				{
					Emitters->Add({Position, Amplitude, Phase});
				}
			}
		};
		int32 EmitterIndex = 0;
		for (const FCGHTargetDescription& Target : Input.Scene.Targets)
		{
			if (!CheckCancelled(CancelRequested, Error))
			{
				return false;
			}
			if (Target.TargetType == ECGHTargetType::Point)
			{
				Add(Target.PositionSLMM, Target.Amplitude, Target.PhaseRad);
				continue;
			}
			const FCGHPointCloudResource& Cloud = *Clouds.FindChecked(Target.ResourceId);
			for (const FCGHObjectPoint& Sample : Cloud.Points)
			{
				if ((EmitterIndex++ & 255) == 0 && !CheckCancelled(CancelRequested, Error))
				{
					return false;
				}
				if (!IsFinite(Sample.PositionLocalM))
				{
					return Fail(Error, TEXT("Mesh sample positions must be finite."));
				}
				// The sampler already bakes target amplitude/phase and scale into the local resource.
				const FVector Position = Target.PositionSLMM + Target.RotationSLM.RotateVector(Sample.PositionLocalM);
				if (!ValidateEmitter(Position, Sample.Amplitude, Sample.Phase, Aperture, Error))
				{
					return false;
				}
				Add(Position, Sample.Amplitude, Sample.Phase);
			}
		}
		return MaximumAmplitude > 0.0 || Fail(Error, TEXT("PointFocus requires at least one source point with positive amplitude."));
	}
}

bool CGHPointFocus::ValidateScene(const FCGHSolverInput& Input, FString& OutError)
{
	FAperture Aperture;
	return ValidateSceneInternal(Input, Aperture, OutError, nullptr);
}

bool CGHPointFocus::ValidateInput(const FCGHSolverInput& Input, FString& OutError)
{
	FAperture Aperture;
	double MaximumAmplitude;
	return GatherEmitters(Input, nullptr, MaximumAmplitude, Aperture, OutError, nullptr);
}

FCGHSolverResult CGHPointFocus::Solve(const FCGHSolverInput& Input, const std::atomic<bool>& CancelRequested)
{
	const double StartSeconds = FPlatformTime::Seconds();
	FCGHSolverResult Result;
	const auto Failed = [&Result, StartSeconds](FString Error) -> FCGHSolverResult
	{
		Result.Pattern = FCGHSLMPhasePattern();
		Result.Error = MoveTemp(Error);
		Result.ComputeSeconds = FPlatformTime::Seconds() - StartSeconds;
		return MoveTemp(Result);
	};
	FAperture Aperture;
	TArray<FEmitter> Emitters;
	double MaximumAmplitude;
	if (!GatherEmitters(Input, &Emitters, MaximumAmplitude, Aperture, Result.Error, &CancelRequested))
	{
		return Failed(MoveTemp(Result.Error));
	}
	FCompensatedSum WeightSum;
	for (int32 Index = 0; Index < Emitters.Num(); ++Index)
	{
		if ((Index & 255) == 0 && CancelRequested.load(std::memory_order_relaxed))
		{
			return Failed(TEXT("PointFocus job cancelled."));
		}
		FEmitter& Emitter = Emitters[Index];
		Emitter.Amplitude /= MaximumAmplitude;
		Emitter.WeightedReal = Emitter.Amplitude * std::cos(Emitter.Phase);
		Emitter.WeightedImaginary = Emitter.Amplitude * std::sin(Emitter.Phase);
		WeightSum.Add(Emitter.Amplitude);
	}
	const double ZeroFieldThreshold = CancellationTolerance * WeightSum.Value();
	const FCGHSLMDescription& SLM = Input.Scene.SLM;
	const FCGHReconstructionLightDescription& Light = Input.Scene.ReconstructionLight;
	const double CenterColumn = (SLM.ResolutionX - 1) / 2.0;
	const double CenterRow = (SLM.ResolutionY - 1) / 2.0;
	Result.Pattern.ResolutionX = SLM.ResolutionX;
	Result.Pattern.ResolutionY = SLM.ResolutionY;
	Result.Pattern.PhaseRad.SetNumUninitialized(SLM.ResolutionX * SLM.ResolutionY);
	for (int32 Row = 0; Row < SLM.ResolutionY; ++Row)
	{
		const double Zp = (CenterRow - Row) * SLM.PixelPitchYM;
		for (int32 Column = 0; Column < SLM.ResolutionX; ++Column)
		{
			if ((Column & 63) == 0 && CancelRequested.load(std::memory_order_relaxed))
			{
				return Failed(TEXT("PointFocus job cancelled."));
			}
			const double Yp = (Column - CenterColumn) * SLM.PixelPitchXM;
			const double IncidentPhase = Light.InitialPhaseRad + Aperture.K * (Light.DirectionSLM.Y * Yp + Light.DirectionSLM.Z * Zp);
			double Phase;
			if (Emitters.Num() == 1)
			{
				// Preserve the reference single-focus result and avoid unnecessary trigonometric work.
				const FEmitter& Emitter = Emitters[0];
				const double R = std::hypot(Emitter.Position.X, Emitter.Position.Y - Yp, Emitter.Position.Z - Zp);
				Phase = Emitter.Phase - Aperture.K * R - IncidentPhase;
			}
			else
			{
				FCompensatedSum Real;
				FCompensatedSum Imaginary;
				for (int32 Index = 0; Index < Emitters.Num(); ++Index)
				{
					if ((Index & 255) == 0 && CancelRequested.load(std::memory_order_relaxed))
					{
						return Failed(TEXT("PointFocus job cancelled."));
					}
					const FEmitter& Emitter = Emitters[Index];
					const double R = std::hypot(Emitter.Position.X, Emitter.Position.Y - Yp, Emitter.Position.Z - Zp);
					// Rotate the precomputed source field by exp(-i*k*r). Keeping the source phase
					// separate avoids losing an opposing pi offset when k*r is optically large.
					const double PropagationPhase = Aperture.K * R;
					const double C = std::cos(PropagationPhase);
					const double S = std::sin(PropagationPhase);
					Real.Add(Emitter.WeightedReal * C + Emitter.WeightedImaginary * S);
					Imaginary.Add(Emitter.WeightedImaginary * C - Emitter.WeightedReal * S);
				}
				const double FieldReal = Real.Value();
				const double FieldImaginary = Imaginary.Value();
				if (!std::isfinite(FieldReal) || !std::isfinite(FieldImaginary))
				{
					return Failed(TEXT("PointFocus encountered a nonfinite complex field."));
				}
				// No phase is defined at a field zero. Use deterministic zero SLM modulation there.
				Phase = std::hypot(FieldReal, FieldImaginary) <= ZeroFieldThreshold
					? 0.0 : std::atan2(FieldImaginary, FieldReal) - IncidentPhase;
			}
			if (!std::isfinite(Phase))
			{
				return Failed(TEXT("PointFocus encountered a nonfinite propagation phase."));
			}
			Result.Pattern.PhaseRad[Row * SLM.ResolutionX + Column] = WrapToTwoPi(Phase);
		}
	}
	if (CancelRequested.load(std::memory_order_relaxed))
	{
		return Failed(TEXT("PointFocus job cancelled."));
	}
	Result.bSucceeded = true;
	Result.ComputeSeconds = FPlatformTime::Seconds() - StartSeconds;
	return Result;
}
