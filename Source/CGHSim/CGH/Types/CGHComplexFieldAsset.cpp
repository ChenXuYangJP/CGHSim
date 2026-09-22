#include "CGH/Types/CGHComplexFieldAsset.h"

namespace
{
	bool ValidateField(const FCGHComplexField& Field, double PixelPitchXM, double PixelPitchYM, FString& OutError)
	{
		if (!Field.IsValid(&OutError)) { return false; }
		if (!FMath::IsFinite(PixelPitchXM) || !FMath::IsFinite(PixelPitchYM)
			|| PixelPitchXM <= 0.0 || PixelPitchYM <= 0.0)
		{
			OutError = TEXT("Complex-field pixel pitches must be finite positive values in meters.");
			return false;
		}
		return true;
	}
}

bool UCGHComplexFieldAsset::SetField(const FCGHComplexField& Field, double PixelPitchXM, double PixelPitchYM)
{
	if (!ValidateField(Field, PixelPitchXM, PixelPitchYM, ValidationError)) { return false; }
	if (StoredField.ResolutionX != Field.ResolutionX || StoredField.ResolutionY != Field.ResolutionY
		|| StoredField.Samples != Field.Samples || StoredField.Revision != 0
		|| StoredPixelPitchXM != PixelPitchXM || StoredPixelPitchYM != PixelPitchYM)
	{
		Modify();
		StoredField = Field;
		StoredField.Revision = 0;
		StoredPixelPitchXM = PixelPitchXM;
		StoredPixelPitchYM = PixelPitchYM;
		UpdateSummary();
	}
	return true;
}

bool UCGHComplexFieldAsset::HasValidField() const
{
	FString Error;
	return ValidateField(StoredField, StoredPixelPitchXM, StoredPixelPitchYM, Error);
}

void UCGHComplexFieldAsset::PostLoad()
{
	Super::PostLoad();
	StoredField.Revision = 0;
	UpdateSummary();
	ValidateField(StoredField, StoredPixelPitchXM, StoredPixelPitchYM, ValidationError);
}

void UCGHComplexFieldAsset::UpdateSummary()
{
	StoredResolutionX = StoredField.ResolutionX;
	StoredResolutionY = StoredField.ResolutionY;
	StoredSampleCount = StoredField.Samples.Num();
}
