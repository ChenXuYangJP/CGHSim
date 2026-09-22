#include "CGH/Reconstruction/CGHReconstruction.h"

#include "CGH/Utils/CGHPhasePreview.h"
#include "HAL/PlatformTime.h"
#include <cmath>

namespace CGHReconstruction
{
namespace
{
	constexpr double ReconstructionTwoPi = 2.0 * UE_DOUBLE_PI;
	constexpr double UnitNormTolerance = 1.0e-6;

	bool IsFiniteVector(const FVector& V)
	{
		return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
	}

	bool Fail(FString& OutError, const TCHAR* Message)
	{
		OutError = Message;
		return false;
	}

	bool IsCancelled(const std::atomic<bool>* CancelRequested)
	{
		return CancelRequested && CancelRequested->load(std::memory_order_relaxed);
	}

	bool ValidateGrid(int32 X, int32 Y, double PitchX, double PitchY, FString& OutError)
	{
		if (X <= 0 || Y <= 0 || X > CGHPhasePreview::MaximumAxisResolution
			|| Y > CGHPhasePreview::MaximumAxisResolution
			|| static_cast<int64>(X) * Y > CGHPhasePreview::MaximumPixelCount)
		{
			return Fail(OutError, TEXT("SLM and observer grids require positive dimensions, at most 16384 per axis and 64M pixels."));
		}
		if (!FMath::IsFinite(PitchX) || !FMath::IsFinite(PitchY) || PitchX <= 0.0 || PitchY <= 0.0
			|| !FMath::IsFinite(X * PitchX) || !FMath::IsFinite(Y * PitchY))
		{
			return Fail(OutError, TEXT("SLM and observer pixel pitches and extents must be finite and positive."));
		}
		return true;
	}

	FVector ObserverPixel(const FCGHObserverPlaneDescription& Plane, int32 Column, int32 Row)
	{
		return Plane.PositionSLMM + Plane.RotationSLM.RotateVector(FVector(0.0,
			(Column - (Plane.ResolutionX - 1) / 2.0) * Plane.PixelPitchXM,
			((Plane.ResolutionY - 1) / 2.0 - Row) * Plane.PixelPitchYM));
	}

	bool ValidatePattern(const FCGHReconstructionInput& Input, FString& OutError,
		const std::atomic<bool>* CancelRequested)
	{
		if (!CGHReconstruction::ValidateScene(Input, OutError))
		{
			return false;
		}
		const int64 PixelCount = static_cast<int64>(Input.SLM.ResolutionX) * Input.SLM.ResolutionY;
		if (Input.Pattern.ResolutionX != Input.SLM.ResolutionX
			|| Input.Pattern.ResolutionY != Input.SLM.ResolutionY || Input.Pattern.PhaseRad.Num() != PixelCount)
		{
			return Fail(OutError, TEXT("Reconstruction requires a complete phase pattern matching the SLM resolution."));
		}
		for (int32 Index = 0; Index < Input.Pattern.PhaseRad.Num(); ++Index)
		{
			if ((Index & 255) == 0 && IsCancelled(CancelRequested))
			{
				return Fail(OutError, TEXT("Reconstruction was cancelled."));
			}
			if (!FMath::IsFinite(Input.Pattern.PhaseRad[Index]))
			{
				return Fail(OutError, TEXT("SLM phase samples must all be finite."));
			}
		}
		return true;
	}

	struct FIncidentSample
	{
		double Y;
		double Z;
		double Real;
		double Imaginary;
	};

	void AddCompensated(double Value, double& Sum, double& Correction)
	{
		const double Adjusted = Value - Correction;
		const double Next = Sum + Adjusted;
		Correction = (Next - Sum) - Adjusted;
		Sum = Next;
	}
	/** Build the pupil in SLM coordinates and the sensor in an abstract propagation frame.
	 * Camera +X looks into the scene; physical light travels toward camera -X.
	 * In stage two, abstract +X follows that propagation while Y/Z retain sensor axes. */
	bool BuildCameraGeometry(const FCGHReconstructionInput& Input,
		FCGHObserverPlaneDescription& Pupil, FCGHObserverPlaneDescription& Sensor, FString& OutError)
	{
		const FCGHCameraDescription& Camera = Input.Camera;
		if (!FMath::IsFinite(Camera.FocalLengthM) || Camera.FocalLengthM <= 0.0
			|| !FMath::IsFinite(Camera.FNumber) || Camera.FNumber <= 0.0
			|| !FMath::IsFinite(Camera.FocusDistanceM) || Camera.FocusDistanceM <= Camera.FocalLengthM)
		{
			return Fail(OutError, TEXT("Camera requires a finite positive focal length and f-number, and focus distance greater than focal length."));
		}
		if (Camera.PupilResolutionX < 1 || Camera.PupilResolutionY < 1
			|| Camera.PupilResolutionX > 2048 || Camera.PupilResolutionY > 2048)
		{
			return Fail(OutError, TEXT("Camera pupil sampling requires 1 to 2048 samples per axis."));
		}
		const double Diameter = Camera.FocalLengthM / Camera.FNumber;
		const double SensorDistance = Camera.FocalLengthM / (1.0 - Camera.FocalLengthM / Camera.FocusDistanceM);
		if (!FMath::IsFinite(Diameter) || Diameter <= 0.0 || !FMath::IsFinite(SensorDistance) || SensorDistance <= 0.0)
		{
			return Fail(OutError, TEXT("Camera aperture diameter and lens-to-sensor distance must be finite and positive."));
		}
		Pupil.ResolutionX = Camera.PupilResolutionX;
		Pupil.ResolutionY = Camera.PupilResolutionY;
		Pupil.PixelPitchXM = Diameter / Camera.PupilResolutionX;
		Pupil.PixelPitchYM = Diameter / Camera.PupilResolutionY;
		Pupil.PositionSLMM = Camera.OpticalPositionSLMM;
		Pupil.RotationSLM = Camera.OpticalRotationSLM;
		Sensor.ResolutionX = Camera.OutputResolutionX;
		Sensor.ResolutionY = Camera.OutputResolutionY;
		Sensor.PixelPitchXM = Camera.PixelPitchXM;
		Sensor.PixelPitchYM = Camera.PixelPitchYM;
		Sensor.PositionSLMM = FVector(SensorDistance, 0.0, 0.0);
		Sensor.RotationSLM = FQuat::Identity;

		// Reuse observer geometry/light validation without copying the phase buffer.
		FCGHReconstructionInput Stage;
		Stage.SLM = Input.SLM;
		Stage.Light = Input.Light;
		Stage.PropagationConvention = Input.PropagationConvention;
		Stage.ObserverPlane = Pupil;
		if (!CGHReconstruction::ValidateScene(Stage, OutError))
		{
			OutError = TEXT("Camera pupil: ") + OutError;
			return false;
		}
		const FVector Forward = Camera.OpticalRotationSLM.RotateVector(FVector::XAxisVector);
		for (int32 Row : {0, Input.SLM.ResolutionY - 1})
		{
			for (int32 Column : {0, Input.SLM.ResolutionX - 1})
			{
				const FVector P(0.0, (Column - (Input.SLM.ResolutionX - 1) / 2.0) * Input.SLM.PixelPitchXM,
					((Input.SLM.ResolutionY - 1) / 2.0 - Row) * Input.SLM.PixelPitchYM);
				const double FrontDistance = FVector::DotProduct(P - Camera.OpticalPositionSLMM, Forward);
				if (!FMath::IsFinite(FrontDistance) || FrontDistance <= 0.0)
				{
					return Fail(OutError, TEXT("Camera optical +X must face the SLM; every SLM pixel must lie in front of the lens."));
				}
			}
		}
		const double K = ReconstructionTwoPi / Input.Light.WavelengthM;
		const double HalfY = (Pupil.ResolutionX - 1) / 2.0 * Pupil.PixelPitchXM;
		const double HalfZ = (Pupil.ResolutionY - 1) / 2.0 * Pupil.PixelPitchYM;
		const double MaximumLensPhase = -0.5 * K * (HalfY * (HalfY / Camera.FocalLengthM) + HalfZ * (HalfZ / Camera.FocalLengthM));
		if (!FMath::IsFinite(MaximumLensPhase))
		{
			return Fail(OutError, TEXT("Camera thin-lens phase exceeds the finite numerical range."));
		}
		Stage.SLM.ResolutionX = Pupil.ResolutionX;
		Stage.SLM.ResolutionY = Pupil.ResolutionY;
		Stage.SLM.PixelPitchXM = Pupil.PixelPitchXM;
		Stage.SLM.PixelPitchYM = Pupil.PixelPitchYM;
		Stage.ObserverPlane = Sensor;
		if (!CGHReconstruction::ValidateScene(Stage, OutError))
		{
			OutError = TEXT("Camera sensor: ") + OutError;
			return false;
		}
		return true;
	}

	bool PropagateSamples(const TArray<FIncidentSample>& Sources, double K, double AreaOverTwoPi,
		const FCGHObserverPlaneDescription& Plane, FCGHComplexField& OutField,
		const std::atomic<bool>& CancelRequested, FString& OutError)
	{
		OutField.ResolutionX = Plane.ResolutionX;
		OutField.ResolutionY = Plane.ResolutionY;
		OutField.Samples.SetNumUninitialized(Plane.ResolutionX * Plane.ResolutionY);
		for (int32 Index = 0; Index < OutField.Samples.Num(); ++Index)
		{
			if (CancelRequested.load(std::memory_order_relaxed))
			{
				return Fail(OutError, TEXT("Reconstruction was cancelled."));
			}
			const FVector Q = ObserverPixel(Plane, Index % Plane.ResolutionX, Index / Plane.ResolutionX);
			double Real = 0.0, Imaginary = 0.0, RealCorrection = 0.0, ImaginaryCorrection = 0.0;
			for (int32 SourceIndex = 0; SourceIndex < Sources.Num(); ++SourceIndex)
			{
				if ((SourceIndex & 255) == 0 && CancelRequested.load(std::memory_order_relaxed))
				{
					return Fail(OutError, TEXT("Reconstruction was cancelled."));
				}
				const FIncidentSample& Source = Sources[SourceIndex];
				const double R = std::hypot(Q.X, Q.Y - Source.Y, Q.Z - Source.Z);
				const double Phase = K * R;
				const double Base = AreaOverTwoPi * (Q.X / R) / R;
				const double Near = Base / R;
				const double Far = Base * K;
				if (!FMath::IsFinite(Phase) || !FMath::IsFinite(Near) || !FMath::IsFinite(Far))
				{
					return Fail(OutError, TEXT("Diffraction kernel exceeds the finite numerical range; increase observer distance or adjust optical settings."));
				}
				const double Cos = std::cos(Phase);
				const double Sin = std::sin(Phase);
				const double KernelReal = Near * Cos + Far * Sin;
				const double KernelImaginary = Near * Sin - Far * Cos;
				AddCompensated(Source.Real * KernelReal - Source.Imaginary * KernelImaginary, Real, RealCorrection);
				AddCompensated(Source.Real * KernelImaginary + Source.Imaginary * KernelReal, Imaginary, ImaginaryCorrection);
			}
			if (!FMath::IsFinite(Real) || !FMath::IsFinite(Imaginary))
			{
				return Fail(OutError, TEXT("Reconstructed complex field exceeds the finite numerical range."));
			}
			OutField.Samples[Index].Real = Real;
			OutField.Samples[Index].Imaginary = Imaginary;
		}
		return true;
	}

}

} // namespace CGHReconstruction

bool CGHReconstruction::ValidateScene(const FCGHReconstructionInput& Input, FString& OutError)
{
	OutError.Reset();
	if (Input.Mode == ECGHReconstructionMode::Camera)
	{
		FCGHObserverPlaneDescription Pupil, Sensor;
		return BuildCameraGeometry(Input, Pupil, Sensor, OutError);
	}
	if (Input.Mode != ECGHReconstructionMode::ObserverPlane)
	{
		return Fail(OutError, TEXT("Unknown reconstruction mode."));
	}
	if (Input.PropagationConvention != ECGHPropagationConvention::ExpPositiveIKR)
	{
		return Fail(OutError, TEXT("Reconstruction requires the exp(+i*k*r) propagation convention."));
	}
	if (Input.SLM.ModulationType != ECGHSLMModulationType::PhaseOnly)
	{
		return Fail(OutError, TEXT("Reconstruction currently supports phase-only SLM modulation."));
	}
	const FCGHSLMDescription& SLM = Input.SLM;
	const FCGHObserverPlaneDescription& Plane = Input.ObserverPlane;
	if (!ValidateGrid(SLM.ResolutionX, SLM.ResolutionY, SLM.PixelPitchXM, SLM.PixelPitchYM, OutError)
		|| !ValidateGrid(Plane.ResolutionX, Plane.ResolutionY, Plane.PixelPitchXM, Plane.PixelPitchYM, OutError))
	{
		return false;
	}
	const double PixelArea = SLM.PixelPitchXM * SLM.PixelPitchYM;
	if (!FMath::IsFinite(PixelArea) || PixelArea <= 0.0)
	{
		return Fail(OutError, TEXT("SLM pixel area must be finite and positive."));
	}
	const FCGHReconstructionLightDescription& Light = Input.Light;
	const double K = ReconstructionTwoPi / Light.WavelengthM;
	if (!FMath::IsFinite(Light.WavelengthM) || Light.WavelengthM <= 0.0 || !FMath::IsFinite(K) || K <= 0.0)
	{
		return Fail(OutError, TEXT("Reconstruction wavelength and wave number must be finite and positive."));
	}
	if (!FMath::IsFinite(Light.Amplitude) || Light.Amplitude < 0.0
		|| !FMath::IsFinite(Light.InitialPhaseRad) || !FMath::IsFinite(Light.PolarizationAngleRad))
	{
		return Fail(OutError, TEXT("Light amplitude must be finite and nonnegative; phase and polarization must be finite."));
	}
	if (Light.SourceType == ECGHSourceType::PlaneWave)
	{
		if (!IsFiniteVector(Light.DirectionSLM) || !FMath::IsFinite(Light.DirectionSLM.SizeSquared())
			|| FMath::Abs(Light.DirectionSLM.SizeSquared() - 1.0) > UnitNormTolerance)
		{
			return Fail(OutError, TEXT("Plane-wave propagation direction must be finite and unit length."));
		}
	}
	else if (Light.SourceType == ECGHSourceType::PointSource)
	{
		if (!IsFiniteVector(Light.PositionSLMM))
		{
			return Fail(OutError, TEXT("Point-source position must be finite."));
		}
	}
	else
	{
		return Fail(OutError, TEXT("Unknown reconstruction illumination source type."));
	}
	const FQuat& Rotation = Plane.RotationSLM;
	if (!IsFiniteVector(Plane.PositionSLMM) || !FMath::IsFinite(Rotation.X)
		|| !FMath::IsFinite(Rotation.Y) || !FMath::IsFinite(Rotation.Z) || !FMath::IsFinite(Rotation.W)
		|| !FMath::IsFinite(Rotation.SizeSquared()) || FMath::Abs(Rotation.SizeSquared() - 1.0) > UnitNormTolerance)
	{
		return Fail(OutError, TEXT("Observer pose requires a finite position and a unit rotation quaternion."));
	}
	// An affine rectangular grid attains each coordinate's extrema at its four corner centers.
	for (int32 Row : {0, Plane.ResolutionY - 1})
	{
		for (int32 Column : {0, Plane.ResolutionX - 1})
		{
			const FVector Q = ObserverPixel(Plane, Column, Row);
			if (!IsFiniteVector(Q) || Q.X <= 0.0)
			{
				return Fail(OutError, TEXT("Every observer pixel center must lie in front of the SLM (SLM-local X > 0)."));
			}
			for (int32 SourceRow : {0, SLM.ResolutionY - 1})
			{
				for (int32 SourceColumn : {0, SLM.ResolutionX - 1})
				{
					const double Y = (SourceColumn - (SLM.ResolutionX - 1) / 2.0) * SLM.PixelPitchXM;
					const double Z = ((SLM.ResolutionY - 1) / 2.0 - SourceRow) * SLM.PixelPitchYM;
					const double R = std::hypot(Q.X, Q.Y - Y, Q.Z - Z);
					if (!FMath::IsFinite(R) || !FMath::IsFinite(K * R))
					{
						return Fail(OutError, TEXT("SLM-to-observer distance or propagation phase exceeds the finite numerical range."));
					}
				}
			}
		}
	}
	return true;
}

bool CGHReconstruction::ValidateInput(const FCGHReconstructionInput& Input, FString& OutError)
{
	return ValidatePattern(Input, OutError, nullptr);
}

FCGHReconstructionResult CGHReconstruction::Reconstruct(const FCGHReconstructionInput& Input,
	const std::atomic<bool>& CancelRequested)
{
	const double StartSeconds = FPlatformTime::Seconds();
	FCGHReconstructionResult Result;
	Result.PropagationConvention = Input.PropagationConvention;
	const auto Failed = [&Result, StartSeconds](const TCHAR* Message)
	{
		Result.Error = Message;
		Result.Field = FCGHComplexField();
		Result.ComputeSeconds = FPlatformTime::Seconds() - StartSeconds;
		return MoveTemp(Result);
	};
	if (CancelRequested.load(std::memory_order_relaxed))
	{
		return Failed(TEXT("Reconstruction was cancelled."));
	}
	FString ValidationError;
	if (!ValidatePattern(Input, ValidationError, &CancelRequested))
	{
		return Failed(*ValidationError);
	}
	const FCGHSLMDescription& SLM = Input.SLM;
	const FCGHObserverPlaneDescription& Plane = Input.ObserverPlane;
	const FCGHReconstructionLightDescription& Light = Input.Light;
	const double K = ReconstructionTwoPi / Light.WavelengthM;
	const double AreaOverTwoPi = (SLM.PixelPitchXM * SLM.PixelPitchYM) / ReconstructionTwoPi;
	const double OriginPhase = std::remainder(Light.InitialPhaseRad, ReconstructionTwoPi);
	TArray<FIncidentSample> Sources;
	Sources.SetNumUninitialized(Input.Pattern.PhaseRad.Num());
	for (int32 Index = 0; Index < Sources.Num(); ++Index)
	{
		if ((Index & 255) == 0 && CancelRequested.load(std::memory_order_relaxed))
		{
			return Failed(TEXT("Reconstruction was cancelled."));
		}
		FIncidentSample& Source = Sources[Index];
		Source.Y = (Index % SLM.ResolutionX - (SLM.ResolutionX - 1) / 2.0) * SLM.PixelPitchXM;
		Source.Z = ((SLM.ResolutionY - 1) / 2.0 - Index / SLM.ResolutionX) * SLM.PixelPitchYM;
		double Amplitude = Light.Amplitude;
		double IncidentPropagation;
		if (Light.SourceType == ECGHSourceType::PointSource)
		{
			const double Distance = std::hypot(Light.PositionSLMM.X,
				Source.Y - Light.PositionSLMM.Y, Source.Z - Light.PositionSLMM.Z);
			if (!FMath::IsFinite(Distance) || Distance <= 0.0)
			{
				return Failed(TEXT("Point source must have a finite nonzero distance from every SLM pixel center."));
			}
			Amplitude /= Distance; // Numerically 1 meter / Distance; A is specified at one meter.
			IncidentPropagation = K * Distance;
		}
		else
		{
			IncidentPropagation = K * (Light.DirectionSLM.Y * Source.Y + Light.DirectionSLM.Z * Source.Z);
		}
		if (!FMath::IsFinite(Amplitude) || !FMath::IsFinite(IncidentPropagation))
		{
			return Failed(TEXT("Incident illumination amplitude or phase exceeds the finite numerical range."));
		}
		const double Phase = OriginPhase + std::remainder(IncidentPropagation, ReconstructionTwoPi)
			+ std::remainder(Input.Pattern.PhaseRad[Index], ReconstructionTwoPi);
		Source.Real = Amplitude * std::cos(Phase);
		Source.Imaginary = Amplitude * std::sin(Phase);
	}
	FCGHObserverPlaneDescription Pupil;
	FCGHObserverPlaneDescription Sensor;
	if (Input.Mode == ECGHReconstructionMode::Camera)
	{
		if (!BuildCameraGeometry(Input, Pupil, Sensor, ValidationError)) return Failed(*ValidationError);
		FCGHComplexField PupilField;
		if (!PropagateSamples(Sources, K, AreaOverTwoPi, Pupil, PupilField, CancelRequested, ValidationError))
		{
			return Failed(*ValidationError);
		}
		Sources.Empty(); // Release the SLM source storage before constructing pupil sources.
		const double Diameter = Input.Camera.FocalLengthM / Input.Camera.FNumber;
		for (int32 Index = 0; Index < PupilField.Samples.Num(); ++Index)
		{
			if ((Index & 255) == 0 && CancelRequested.load(std::memory_order_relaxed))
			{
				return Failed(TEXT("Reconstruction was cancelled."));
			}
			const double Y = (Index % Pupil.ResolutionX - (Pupil.ResolutionX - 1) / 2.0) * Pupil.PixelPitchXM;
			const double Z = ((Pupil.ResolutionY - 1) / 2.0 - Index / Pupil.ResolutionX) * Pupil.PixelPitchYM;
			const double NormalizedY = 2.0 * (Y / Diameter);
			const double NormalizedZ = 2.0 * (Z / Diameter);
			if (NormalizedY * NormalizedY + NormalizedZ * NormalizedZ > 1.0) continue;
			const double LensPhase = -0.5 * K * (Y * (Y / Input.Camera.FocalLengthM) + Z * (Z / Input.Camera.FocalLengthM));
			if (!FMath::IsFinite(LensPhase)) return Failed(TEXT("Camera thin-lens phase exceeds the finite numerical range."));
			const double C = std::cos(LensPhase);
			const double S = std::sin(LensPhase);
			const FCGHComplexSample& Sample = PupilField.Samples[Index];
			const double Real = Sample.Real * C - Sample.Imaginary * S;
			const double Imaginary = Sample.Real * S + Sample.Imaginary * C;
			if (!FMath::IsFinite(Real) || !FMath::IsFinite(Imaginary))
			{
				return Failed(TEXT("Camera pupil field exceeds the finite numerical range."));
			}
			Sources.Add({Y, Z, Real, Imaginary});
		}
		PupilField.Samples.Empty();
		if (!PropagateSamples(Sources, K, (Pupil.PixelPitchXM * Pupil.PixelPitchYM) / ReconstructionTwoPi,
			Sensor, Result.Field, CancelRequested, ValidationError)) return Failed(*ValidationError);
	}
	else if (!PropagateSamples(Sources, K, AreaOverTwoPi, Plane, Result.Field, CancelRequested, ValidationError))
	{
		return Failed(*ValidationError);
	}
	if (CancelRequested.load(std::memory_order_relaxed))
	{
		return Failed(TEXT("Reconstruction was cancelled."));
	}
	Result.bSucceeded = true;
	Result.ComputeSeconds = FPlatformTime::Seconds() - StartSeconds;
	return Result;
}
