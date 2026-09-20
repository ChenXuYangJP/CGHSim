#include "CGH/Utils/CGHPhasePreview.h"

bool CGHPhasePreview::ValidatePattern(const FCGHSLMPhasePattern& Pattern, int32 ExpectedX, int32 ExpectedY,
	FString& OutError)
{
	OutError.Reset();
	if (ExpectedX <= 0 || ExpectedY <= 0 || Pattern.ResolutionX <= 0 || Pattern.ResolutionY <= 0)
	{
		OutError = TEXT("Phase-pattern and SLM dimensions must be positive.");
		return false;
	}
	if (Pattern.ResolutionX > MaximumAxisResolution || Pattern.ResolutionY > MaximumAxisResolution)
	{
		OutError = FString::Printf(TEXT("Phase-pattern dimensions must not exceed %d pixels per axis."), MaximumAxisResolution);
		return false;
	}
	// Widen before multiplication: even invalid int32 dimensions must not overflow the count.
	const int64 PixelCount = static_cast<int64>(Pattern.ResolutionX) * Pattern.ResolutionY;
	if (PixelCount > MaximumPixelCount)
	{
		OutError = FString::Printf(TEXT("Phase patterns must contain at most %lld pixels."), MaximumPixelCount);
		return false;
	}
	if (Pattern.ResolutionX != ExpectedX || Pattern.ResolutionY != ExpectedY)
	{
		OutError = FString::Printf(TEXT("Phase-pattern dimensions %d x %d do not match the SLM dimensions %d x %d."),
			Pattern.ResolutionX, Pattern.ResolutionY, ExpectedX, ExpectedY);
		return false;
	}
	if (Pattern.PhaseRad.Num() != PixelCount)
	{
		OutError = FString::Printf(TEXT("Phase-pattern dimensions require %lld samples, but %d were provided."),
			PixelCount, Pattern.PhaseRad.Num());
		return false;
	}
	for (int32 Index = 0; Index < Pattern.PhaseRad.Num(); ++Index)
	{
		if (!FMath::IsFinite(Pattern.PhaseRad[Index]))
		{
			OutError = FString::Printf(TEXT("Phase sample %d is not finite."), Index);
			return false;
		}
	}
	return true;
}

uint8 CGHPhasePreview::PhaseToGray(double PhaseRad)
{
	if (!FMath::IsFinite(PhaseRad))
	{
		return 0;
	}
	constexpr double FullCycle = 2.0 * UE_DOUBLE_PI;
	double Wrapped = FMath::Fmod(PhaseRad, FullCycle);
	if (Wrapped < 0.0)
	{
		Wrapped += FullCycle;
	}
	return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32((Wrapped / FullCycle) * 255.0), 0, 255));
}

bool CGHPhasePreview::BuildGrayscale(const FCGHSLMPhasePattern& Pattern, TArray<FColor>& OutPixels,
	FString& OutError)
{
	OutPixels.Reset();
	if (!ValidatePattern(Pattern, Pattern.ResolutionX, Pattern.ResolutionY, OutError))
	{
		return false;
	}
	OutPixels.SetNumUninitialized(Pattern.PhaseRad.Num());
	for (int32 Index = 0; Index < Pattern.PhaseRad.Num(); ++Index)
	{
		const uint8 Gray = PhaseToGray(Pattern.PhaseRad[Index]);
		OutPixels[Index] = FColor(Gray, Gray, Gray, 255);
	}
	return true;
}
