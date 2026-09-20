#include "CGH/Utils/CGHPhasePreview.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPhasePatternValidationTest,
	"CGH.PhasePreview.PatternValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPhasePatternValidationTest::RunTest(const FString& Parameters)
{
	FCGHSLMPhasePattern Pattern;
	TestEqual(TEXT("New patterns have zero width"), Pattern.ResolutionX, 0);
	TestEqual(TEXT("New patterns have zero height"), Pattern.ResolutionY, 0);
	TestEqual(TEXT("New patterns have zero revision"), Pattern.Revision, uint64(0));
	TestTrue(TEXT("New patterns contain no phase samples"), Pattern.PhaseRad.IsEmpty());

	FString Error;
	const auto ExpectInvalid = [&](const TCHAR* Label, int32 ExpectedX, int32 ExpectedY)
	{
		Error = TEXT("old diagnostic");
		TestFalse(Label, CGHPhasePreview::ValidatePattern(Pattern, ExpectedX, ExpectedY, Error));
		TestTrue(TEXT("Invalid input has a fresh explanation"), !Error.IsEmpty() && Error != TEXT("old diagnostic"));
	};
	ExpectInvalid(TEXT("An empty pattern is invalid"), 2, 2);
	Pattern.ResolutionX = 2;
	Pattern.ResolutionY = 2;
	Pattern.PhaseRad = {0.0, UE_DOUBLE_PI, -UE_DOUBLE_PI, 4.0 * UE_DOUBLE_PI};
	Pattern.Revision = 7;
	TestTrue(TEXT("Finite unwrapped phases are accepted"), CGHPhasePreview::ValidatePattern(Pattern, 2, 2, Error));
	TestTrue(TEXT("Success clears a stale error"), Error.IsEmpty());
	TestEqual(TEXT("Validation preserves the caller's revision"), Pattern.Revision, uint64(7));
	ExpectInvalid(TEXT("SLM resolution must match the pattern"), 4, 1);
	ExpectInvalid(TEXT("SLM dimensions must be positive"), 0, 2);
	Pattern.ResolutionY = -2;
	ExpectInvalid(TEXT("Negative pattern dimensions are rejected"), 2, 2);
	Pattern.ResolutionY = 2;
	Pattern.PhaseRad.Pop();
	ExpectInvalid(TEXT("Missing phase samples are rejected"), 2, 2);
	Pattern.PhaseRad.Add(0.0);
	Pattern.PhaseRad.Add(0.0);
	ExpectInvalid(TEXT("Extra phase samples are rejected"), 2, 2);
	Pattern.PhaseRad.Pop();
	Pattern.PhaseRad.Last() = std::numeric_limits<double>::quiet_NaN();
	ExpectInvalid(TEXT("NaN phase values are rejected"), 2, 2);
	Pattern.PhaseRad.Last() = std::numeric_limits<double>::infinity();
	ExpectInvalid(TEXT("Infinite phase values are rejected"), 2, 2);
	Pattern.PhaseRad.Last() = -std::numeric_limits<double>::infinity();
	ExpectInvalid(TEXT("Negative infinite phase values are rejected"), 2, 2);
	Pattern.ResolutionX = 16385;
	Pattern.ResolutionY = 1;
	ExpectInvalid(TEXT("Oversized texture axes are rejected"), 16385, 1);
	Pattern.ResolutionX = 8193;
	Pattern.ResolutionY = 8193;
	ExpectInvalid(TEXT("Images above the pixel budget are rejected"), 8193, 8193);
	Pattern.ResolutionX = TNumericLimits<int32>::Max();
	Pattern.ResolutionY = TNumericLimits<int32>::Max();
	ExpectInvalid(TEXT("Huge dimensions fail without multiplying in int32"), Pattern.ResolutionX, Pattern.ResolutionY);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPhaseGrayMappingTest,
	"CGH.PhasePreview.WrappedGrayscaleMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPhaseGrayMappingTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Zero phase is black"), CGHPhasePreview::PhaseToGray(0.0), uint8(0));
	TestEqual(TEXT("A quarter cycle is quarter gray"), CGHPhasePreview::PhaseToGray(UE_DOUBLE_PI / 2.0), uint8(64));
	TestEqual(TEXT("Half a cycle is middle gray"), CGHPhasePreview::PhaseToGray(UE_DOUBLE_PI), uint8(128));
	TestEqual(TEXT("Three quarters of a cycle is three-quarter gray"), CGHPhasePreview::PhaseToGray(1.5 * UE_DOUBLE_PI), uint8(191));
	TestEqual(TEXT("Phase just below a cycle is white"), CGHPhasePreview::PhaseToGray(2.0 * UE_DOUBLE_PI - 1.0e-6), uint8(255));
	TestEqual(TEXT("One full cycle wraps to black"), CGHPhasePreview::PhaseToGray(2.0 * UE_DOUBLE_PI), uint8(0));
	TestEqual(TEXT("Multiple cycles wrap to black"), CGHPhasePreview::PhaseToGray(8.0 * UE_DOUBLE_PI), uint8(0));
	TestEqual(TEXT("Phases greater than a cycle retain their remainder"), CGHPhasePreview::PhaseToGray(5.0 * UE_DOUBLE_PI), uint8(128));
	TestEqual(TEXT("Negative quarter cycles wrap to three-quarter gray"), CGHPhasePreview::PhaseToGray(-UE_DOUBLE_PI / 2.0), uint8(191));
	TestEqual(TEXT("Negative half cycles wrap to middle gray"), CGHPhasePreview::PhaseToGray(-UE_DOUBLE_PI), uint8(128));
	TestEqual(TEXT("Negative full cycles wrap to black"), CGHPhasePreview::PhaseToGray(-2.0 * UE_DOUBLE_PI), uint8(0));
	TestEqual(TEXT("Small negative phases wrap to white"), CGHPhasePreview::PhaseToGray(-1.0e-6), uint8(255));
	TestEqual(TEXT("Standalone NaN mapping is safe"), CGHPhasePreview::PhaseToGray(std::numeric_limits<double>::quiet_NaN()), uint8(0));
	TestEqual(TEXT("Standalone infinity mapping is safe"), CGHPhasePreview::PhaseToGray(std::numeric_limits<double>::infinity()), uint8(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPhasePixelsTest,
	"CGH.PhasePreview.RowOrderAndAtomicFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPhasePixelsTest::RunTest(const FString& Parameters)
{
	FCGHSLMPhasePattern Pattern;
	Pattern.ResolutionX = 3;
	Pattern.ResolutionY = 2;
	Pattern.PhaseRad = {0.0, 0.5 * UE_DOUBLE_PI, UE_DOUBLE_PI, 1.5 * UE_DOUBLE_PI, 2.0 * UE_DOUBLE_PI, -0.5 * UE_DOUBLE_PI};
	TArray<FColor> Pixels;
	FString Error;
	if (!TestTrue(TEXT("A rectangular pattern produces preview pixels"), CGHPhasePreview::BuildGrayscale(Pattern, Pixels, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("Every input phase has one output pixel"), Pixels.Num(), 6))
	{
		return false;
	}
	const uint8 Expected[2][3] = {{0, 64, 128}, {191, 0, 191}};
	for (int32 Y = 0; Y < Pattern.ResolutionY; ++Y)
	{
		for (int32 X = 0; X < Pattern.ResolutionX; ++X)
		{
			const uint8 Gray = Expected[Y][X];
			TestEqual(TEXT("Pixels retain top-to-bottom row-major order with opaque grayscale channels"),
				Pixels[Y * Pattern.ResolutionX + X], FColor(Gray, Gray, Gray, 255));
		}
	}
	Pattern.PhaseRad.Last() = std::numeric_limits<double>::quiet_NaN();
	TestFalse(TEXT("A nonfinite last sample rejects the whole conversion"), CGHPhasePreview::BuildGrayscale(Pattern, Pixels, Error));
	TestTrue(TEXT("Failure clears previously built pixels without a partial image"), Pixels.IsEmpty());
	TestFalse(TEXT("Conversion failure reports its cause"), Error.IsEmpty());
	Pattern.PhaseRad.Last() = 0.0;
	TestTrue(TEXT("Valid data recovers after a failed conversion"), CGHPhasePreview::BuildGrayscale(Pattern, Pixels, Error));
	TestTrue(TEXT("Recovery clears the error"), Error.IsEmpty());
	Pattern.ResolutionX = 0;
	TestFalse(TEXT("Invalid dimensions reject conversion"), CGHPhasePreview::BuildGrayscale(Pattern, Pixels, Error));
	TestTrue(TEXT("Dimension failure also clears stale pixels"), Pixels.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
