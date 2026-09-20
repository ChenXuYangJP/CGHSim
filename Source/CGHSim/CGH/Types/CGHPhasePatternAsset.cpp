#include "CGH/Types/CGHPhasePatternAsset.h"

#include "CGH/Utils/CGHPhasePreview.h"

bool UCGHPhasePatternAsset::SetPattern(const FCGHSLMPhasePattern& Pattern)
{
	if (!CGHPhasePreview::ValidatePattern(Pattern, Pattern.ResolutionX, Pattern.ResolutionY, ValidationError))
	{
		return false;
	}
	if (StoredPattern.ResolutionX != Pattern.ResolutionX || StoredPattern.ResolutionY != Pattern.ResolutionY
		|| StoredPattern.PhaseRad != Pattern.PhaseRad)
	{
		Modify();
		StoredPattern = Pattern;
		StoredPattern.Revision = 0;
		UpdateSummary();
	}
	return true;
}

bool UCGHPhasePatternAsset::HasValidPattern() const
{
	FString Error;
	return CGHPhasePreview::ValidatePattern(StoredPattern, StoredPattern.ResolutionX, StoredPattern.ResolutionY, Error);
}

bool UCGHPhasePatternAsset::BuildPatternForResolution(int32 ResolutionX, int32 ResolutionY,
	FCGHSLMPhasePattern& OutPattern, FString& OutError) const
{
	if (!CGHPhasePreview::ValidatePattern(StoredPattern, StoredPattern.ResolutionX, StoredPattern.ResolutionY, OutError))
	{
		return false;
	}
	if (ResolutionX <= 0 || ResolutionY <= 0
		|| ResolutionX > CGHPhasePreview::MaximumAxisResolution || ResolutionY > CGHPhasePreview::MaximumAxisResolution)
	{
		OutError = TEXT("Destination dimensions must be positive and at most 16384 pixels per axis.");
		return false;
	}
	const int64 PixelCount = static_cast<int64>(ResolutionX) * ResolutionY;
	if (PixelCount > CGHPhasePreview::MaximumPixelCount)
	{
		OutError = TEXT("Destination dimensions exceed the 64 million pixel phase-pattern limit.");
		return false;
	}

	FCGHSLMPhasePattern Result;
	Result.ResolutionX = ResolutionX;
	Result.ResolutionY = ResolutionY;
	if (ResolutionX == StoredPattern.ResolutionX && ResolutionY == StoredPattern.ResolutionY)
	{
		Result.PhaseRad = StoredPattern.PhaseRad;
	}
	else
	{
		Result.PhaseRad.SetNumUninitialized(static_cast<int32>(PixelCount));
		for (int32 Y = 0; Y < ResolutionY; ++Y)
		{
			const int64 SourceY = ((2ll * Y + 1) * StoredPattern.ResolutionY) / (2ll * ResolutionY);
			const int64 SourceRow = SourceY * StoredPattern.ResolutionX;
			const int64 DestinationRow = static_cast<int64>(Y) * ResolutionX;
			for (int32 X = 0; X < ResolutionX; ++X)
			{
				const int64 SourceX = ((2ll * X + 1) * StoredPattern.ResolutionX) / (2ll * ResolutionX);
				Result.PhaseRad[static_cast<int32>(DestinationRow + X)] =
					StoredPattern.PhaseRad[static_cast<int32>(SourceRow + SourceX)];
			}
		}
	}
	OutPattern = MoveTemp(Result);
	return true;
}

void UCGHPhasePatternAsset::PostLoad()
{
	Super::PostLoad();
	UpdateSummary();
	CGHPhasePreview::ValidatePattern(StoredPattern, StoredPattern.ResolutionX, StoredPattern.ResolutionY, ValidationError);
}

void UCGHPhasePatternAsset::UpdateSummary()
{
	StoredResolutionX = StoredPattern.ResolutionX;
	StoredResolutionY = StoredPattern.ResolutionY;
	StoredSampleCount = StoredPattern.PhaseRad.Num();
}
