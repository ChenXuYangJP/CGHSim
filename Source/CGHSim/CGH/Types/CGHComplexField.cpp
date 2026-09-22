#include "CGH/Types/CGHComplexField.h"

bool FCGHComplexField::IsValid(FString* OutError) const
{
	const auto Fail = [OutError](const FString& Message)
	{
		if (OutError) { *OutError = Message; }
		return false;
	};
	if (OutError) { OutError->Reset(); }
	if (ResolutionX <= 0 || ResolutionY <= 0 || ResolutionX > 16384 || ResolutionY > 16384)
	{
		return Fail(TEXT("Complex-field dimensions must be positive and at most 16384 pixels per axis."));
	}
	const int64 Count = int64(ResolutionX) * ResolutionY;
	if (Count > 64ll * 1024 * 1024 || Samples.Num() != Count)
	{
		return Fail(TEXT("Complex fields must contain exactly one sample per pixel and at most 64 million pixels."));
	}
	for (int32 Index = 0; Index < Samples.Num(); ++Index)
	{
		if (!FMath::IsFinite(Samples[Index].Real) || !FMath::IsFinite(Samples[Index].Imaginary))
		{
			return Fail(FString::Printf(TEXT("Complex sample %d is not finite."), Index));
		}
	}
	return true;
}
