#include "CGH/Utils/CGHComplexFieldPreview.h"

#include "CGH/Utils/CGHPhasePreview.h"

bool CGHComplexFieldPreview::BuildGrayscale(const FCGHComplexField& Field, ECGHObserverPreviewMode Mode,
	TArray<FColor>& OutPixels, FString& OutError)
{
	OutPixels.Reset();
	if (!Field.IsValid(&OutError))
	{
		return false;
	}
	if (Mode != ECGHObserverPreviewMode::Phase && Mode != ECGHObserverPreviewMode::Amplitude
		&& Mode != ECGHObserverPreviewMode::Intensity)
	{
		OutError = TEXT("Unknown observer preview mode.");
		return false;
	}
	// Scale components before finding magnitudes so finite fields near DBL_MAX or near zero
	// produce a correct normalized image without overflow or underflow from squaring.
	double ComponentScale = 0.0;
	double MaximumScaledMagnitude = 0.0;
	if (Mode != ECGHObserverPreviewMode::Phase)
	{
		for (const FCGHComplexSample& Sample : Field.Samples)
		{
			ComponentScale = FMath::Max(ComponentScale, FMath::Max(FMath::Abs(Sample.Real), FMath::Abs(Sample.Imaginary)));
		}
		if (ComponentScale > 0.0)
		{
			for (const FCGHComplexSample& Sample : Field.Samples)
			{
				MaximumScaledMagnitude = FMath::Max(MaximumScaledMagnitude,
					FMath::Sqrt(FMath::Square(Sample.Real / ComponentScale) + FMath::Square(Sample.Imaginary / ComponentScale)));
			}
		}
	}
	OutPixels.SetNumUninitialized(Field.Samples.Num());
	for (int32 Index = 0; Index < Field.Samples.Num(); ++Index)
	{
		const FCGHComplexSample& Sample = Field.Samples[Index];
		uint8 Gray = 0;
		if (Mode == ECGHObserverPreviewMode::Phase)
		{
			// atan2 of signed zero is platform-sensitive; an absent field has no phase.
			Gray = Sample.Real == 0.0 && Sample.Imaginary == 0.0 ? 0
				: CGHPhasePreview::PhaseToGray(FMath::Atan2(Sample.Imaginary, Sample.Real));
		}
		else if (MaximumScaledMagnitude > 0.0)
		{
			const double Magnitude = FMath::Sqrt(FMath::Square(Sample.Real / ComponentScale)
				+ FMath::Square(Sample.Imaginary / ComponentScale));
			const double NormalizedAmplitude = Magnitude / MaximumScaledMagnitude;
			// |U|^2 / max(|U|^2) = (|U| / max(|U|))^2. Square before byte
			// quantization, without overflowing the original field components.
			const double NormalizedValue = Mode == ECGHObserverPreviewMode::Intensity
				? FMath::Square(NormalizedAmplitude) : NormalizedAmplitude;
			Gray = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(255.0 * NormalizedValue), 0, 255));
		}
		OutPixels[Index] = FColor(Gray, Gray, Gray, 255);
	}
	return true;
}
