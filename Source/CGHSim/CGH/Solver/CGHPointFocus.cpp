#include "CGH/Solver/CGHPointFocus.h"

#include "CGH/Utils/CGHPhasePreview.h"
#include "HAL/PlatformTime.h"
#include <cmath>

namespace
{
	constexpr double TwoPi = 2.0 * UE_DOUBLE_PI;

	bool IsPositiveFinite(double Value)
	{
		return std::isfinite(Value) && Value > 0.0;
	}

	double WrapToTwoPi(double Phase)
	{
		double Wrapped = std::fmod(Phase, TwoPi);
		if (Wrapped < 0.0)
		{
			Wrapped += TwoPi;
		}
		// Adding a tiny negative remainder can round to exactly 2*pi.
		return Wrapped >= TwoPi || Wrapped == 0.0 ? 0.0 : Wrapped;
	}
}

bool CGHPointFocus::ValidateInput(const FCGHSolverInput& Input, FString& OutError)
{
	OutError.Reset();
	const auto Fail = [&OutError](const TCHAR* Error)
	{
		OutError = Error;
		return false;
	};
	if (Input.Scene.SchemaVersion != 2)
	{
		return Fail(TEXT("PointFocus requires scene schema version 2."));
	}
	if (Input.Algorithm != ECGHSolverAlgorithm::PointFocus)
	{
		return Fail(TEXT("The CPU backend supports only the PointFocus algorithm."));
	}
	if (Input.PropagationConvention != ECGHPropagationConvention::ExpPositiveIKR)
	{
		return Fail(TEXT("PointFocus requires the exp(+i*k*r) propagation convention."));
	}
	const FCGHSLMDescription& SLM = Input.Scene.SLM;
	if (SLM.ModulationType != ECGHSLMModulationType::PhaseOnly)
	{
		return Fail(TEXT("PointFocus requires a phase-only SLM."));
	}
	if (SLM.ResolutionX <= 0 || SLM.ResolutionY <= 0
		|| SLM.ResolutionX > CGHPhasePreview::MaximumAxisResolution
		|| SLM.ResolutionY > CGHPhasePreview::MaximumAxisResolution
		|| static_cast<int64>(SLM.ResolutionX) * SLM.ResolutionY > CGHPhasePreview::MaximumPixelCount)
	{
		return Fail(TEXT("SLM dimensions exceed the supported phase-pattern bounds."));
	}
	if (!IsPositiveFinite(SLM.PixelPitchXM) || !IsPositiveFinite(SLM.PixelPitchYM))
	{
		return Fail(TEXT("SLM pixel pitches must be finite and positive, in meters."));
	}
	const FCGHReconstructionLightDescription& Light = Input.Scene.ReconstructionLight;
	if (Light.SourceType != ECGHSourceType::PlaneWave)
	{
		return Fail(TEXT("PointFocus supports only PlaneWave reconstruction illumination."));
	}
	const double Wavelength = Light.WavelengthM;
	if (!IsPositiveFinite(Wavelength) || !std::isfinite(TwoPi / Wavelength))
	{
		return Fail(TEXT("The reconstruction wavelength must be finite and positive with a finite wave number."));
	}
	if (!std::isfinite(Light.InitialPhaseRad))
	{
		return Fail(TEXT("The plane-wave phase at the SLM origin must be finite."));
	}
	const FVector& Direction = Light.DirectionSLM;
	const double DirectionNormSquared = Direction.X * Direction.X + Direction.Y * Direction.Y + Direction.Z * Direction.Z;
	if (!std::isfinite(Direction.X) || !std::isfinite(Direction.Y) || !std::isfinite(Direction.Z)
		|| !std::isfinite(DirectionNormSquared) || std::abs(DirectionNormSquared - 1.0) > 1.0e-6)
	{
		return Fail(TEXT("Plane-wave DirectionSLM must be finite and unit length (squared-norm tolerance 1e-6)."));
	}
	if (!std::isfinite(Light.Amplitude) || Light.Amplitude < 0.0)
	{
		return Fail(TEXT("The reconstruction amplitude must be finite and nonnegative."));
	}
	if (!std::isfinite(Light.PolarizationAngleRad))
	{
		return Fail(TEXT("The reconstruction polarization angle must be finite."));
	}
	if (Input.Scene.Targets.Num() != 1 || Input.Scene.Targets[0].TargetType != ECGHTargetType::Point)
	{
		return Fail(TEXT("PointFocus requires exactly one Point target; meshes and multiple targets are unsupported."));
	}
	const FCGHTargetDescription& Target = Input.Scene.Targets[0];
	const FVector& Position = Target.PositionSLMM;
	if (!std::isfinite(Position.X) || !std::isfinite(Position.Y) || !std::isfinite(Position.Z)
		|| !std::isfinite(Target.PhaseRad))
	{
		return Fail(TEXT("The target position and phase must be finite."));
	}
	if (Position.X == 0.0)
	{
		return Fail(TEXT("The point target must lie off the SLM plane (SLM-local X must be nonzero)."));
	}

	// Check the farthest corner before allocation. hypot avoids unnecessary squared-distance overflow.
	const double HalfY = (SLM.ResolutionX - 1) / 2.0 * SLM.PixelPitchXM;
	const double HalfZ = (SLM.ResolutionY - 1) / 2.0 * SLM.PixelPitchYM;
	const double FarthestY = std::abs(Position.Y) + HalfY;
	const double FarthestZ = std::abs(Position.Z) + HalfZ;
	const double MaximumDistance = std::hypot(Position.X, FarthestY, FarthestZ);
	const double K = TwoPi / Wavelength;
	const double MaximumPropagationPhase = K * MaximumDistance;
	// The plane wave's spatial phase reaches its extrema at aperture corners.
	const double MaximumIncidentSpatialPhase = K * (std::abs(Direction.Y) * HalfY + std::abs(Direction.Z) * HalfZ);
	const double MinimumIncidentPhase = Light.InitialPhaseRad - MaximumIncidentSpatialPhase;
	const double MaximumIncidentPhase = Light.InitialPhaseRad + MaximumIncidentSpatialPhase;
	const double MinimumPhaseBeforeIllumination = Target.PhaseRad - MaximumPropagationPhase;
	if (!std::isfinite(HalfY) || !std::isfinite(HalfZ) || !std::isfinite(MaximumDistance)
		|| !std::isfinite(MaximumPropagationPhase) || !std::isfinite(MaximumIncidentSpatialPhase)
		|| !std::isfinite(MinimumIncidentPhase) || !std::isfinite(MaximumIncidentPhase)
		|| !std::isfinite(MinimumPhaseBeforeIllumination)
		|| !std::isfinite(MinimumPhaseBeforeIllumination - MaximumIncidentPhase)
		|| !std::isfinite(Target.PhaseRad - MinimumIncidentPhase))
	{
		return Fail(TEXT("The scene geometry, propagation phase or incident plane-wave phase exceeds finite double-precision bounds."));
	}
	return true;
}

FCGHSolverResult CGHPointFocus::Solve(const FCGHSolverInput& Input,
	const std::atomic<bool>& CancelRequested)
{
	const double StartSeconds = FPlatformTime::Seconds();
	FCGHSolverResult Result;
	const auto Fail = [&Result, StartSeconds](FString Error) -> FCGHSolverResult
	{
		Result.Pattern = FCGHSLMPhasePattern();
		Result.Error = MoveTemp(Error);
		Result.ComputeSeconds = FPlatformTime::Seconds() - StartSeconds;
		return MoveTemp(Result);
	};
	if (!ValidateInput(Input, Result.Error))
	{
		return Fail(MoveTemp(Result.Error));
	}
	if (CancelRequested.load(std::memory_order_relaxed))
	{
		return Fail(TEXT("PointFocus job cancelled."));
	}

	const FCGHSLMDescription& SLM = Input.Scene.SLM;
	const FCGHTargetDescription& Target = Input.Scene.Targets[0];
	const FVector& Position = Target.PositionSLMM;
	const FCGHReconstructionLightDescription& Light = Input.Scene.ReconstructionLight;
	const double K = TwoPi / Light.WavelengthM;
	const double CenterColumn = (SLM.ResolutionX - 1) / 2.0;
	const double CenterRow = (SLM.ResolutionY - 1) / 2.0;
	Result.Pattern.ResolutionX = SLM.ResolutionX;
	Result.Pattern.ResolutionY = SLM.ResolutionY;
	Result.Pattern.PhaseRad.SetNumUninitialized(SLM.ResolutionX * SLM.ResolutionY);

	for (int32 Row = 0; Row < SLM.ResolutionY; ++Row)
	{
		if (CancelRequested.load(std::memory_order_relaxed))
		{
			return Fail(TEXT("PointFocus job cancelled."));
		}
		const double Zp = (CenterRow - Row) * SLM.PixelPitchYM;
		for (int32 Column = 0; Column < SLM.ResolutionX; ++Column)
		{
			const double Yp = (Column - CenterColumn) * SLM.PixelPitchXM;
			const double R = std::hypot(Position.X, Position.Y - Yp, Position.Z - Zp);
			const double IncidentPhase = Light.InitialPhaseRad + K * (Light.DirectionSLM.Y * Yp + Light.DirectionSLM.Z * Zp);
			const double Phase = Target.PhaseRad - K * R - IncidentPhase;
			if (!std::isfinite(Phase))
			{
				return Fail(TEXT("PointFocus encountered a nonfinite propagation phase."));
			}
			Result.Pattern.PhaseRad[Row * SLM.ResolutionX + Column] = WrapToTwoPi(Phase);
		}
	}
	if (CancelRequested.load(std::memory_order_relaxed))
	{
		return Fail(TEXT("PointFocus job cancelled."));
	}
	Result.bSucceeded = true;
	Result.ComputeSeconds = FPlatformTime::Seconds() - StartSeconds;
	return Result;
}
