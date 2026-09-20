#include "CGH/Types/CGHPhasePatternAsset.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "UObject/UnrealType.h"
#include <limits>

namespace
{
	FCGHSLMPhasePattern MakeStoredPattern()
	{
		FCGHSLMPhasePattern Pattern;
		Pattern.ResolutionX = 3;
		Pattern.ResolutionY = 2;
		Pattern.PhaseRad = {0.0, 0.5 * UE_DOUBLE_PI, UE_DOUBLE_PI, 1.5 * UE_DOUBLE_PI, -0.5 * UE_DOUBLE_PI, 2.0 * UE_DOUBLE_PI};
		Pattern.Revision = 123;
		return Pattern;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPhaseAssetStorageTest,
	"CGH.PhasePatternAsset.ValidationAndOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPhaseAssetStorageTest::RunTest(const FString& Parameters)
{
	UCGHPhasePatternAsset* Asset = NewObject<UCGHPhasePatternAsset>();
	if (!TestNotNull(TEXT("Phase assets can be constructed without an editor factory"), Asset))
	{
		return false;
	}
	TestFalse(TEXT("New assets have no valid pattern"), Asset->HasValidPattern());
	TestEqual(TEXT("New assets have no allocated samples"), Asset->GetSampleCount(), 0);
	TestFalse(TEXT("Saved solver data is not labeled as a preview by default"), Asset->bIsPreviewPattern);
	FCGHSLMPhasePattern Input = MakeStoredPattern();
	if (!TestTrue(TEXT("Finite rectangular patterns can be stored"), Asset->SetPattern(Input)))
	{
		AddError(Asset->GetValidationError());
		return false;
	}
	TestTrue(TEXT("Stored pattern passes validation"), Asset->HasValidPattern());
	TestEqual(TEXT("Stored width"), Asset->GetResolutionX(), 3);
	TestEqual(TEXT("Stored height"), Asset->GetResolutionY(), 2);
	TestEqual(TEXT("Stored count"), Asset->GetSampleCount(), 6);
	TestEqual(TEXT("Caller publication revisions are not saved as asset revisions"), Asset->GetPattern().Revision, uint64(0));
	Input.PhaseRad[0] = 100.0;
	TestEqual(TEXT("The asset owns its data independently of the input"), Asset->GetPattern().PhaseRad[0], 0.0);
	const FCGHSLMPhasePattern Original = Asset->GetPattern();
	const double* OriginalStorage = Asset->GetPattern().PhaseRad.GetData();
	const auto ExpectRejected = [&](const TCHAR* Label, const FCGHSLMPhasePattern& Invalid)
	{
		TestFalse(Label, Asset->SetPattern(Invalid));
		TestFalse(TEXT("Rejected writes provide a diagnostic"), Asset->GetValidationError().IsEmpty());
		TestTrue(TEXT("Rejected writes retain all saved values"), Asset->GetPattern().PhaseRad == Original.PhaseRad);
		TestTrue(TEXT("Rejected writes do not reallocate the saved buffer"), Asset->GetPattern().PhaseRad.GetData() == OriginalStorage);
		TestEqual(TEXT("Rejected writes retain dimensions"), Asset->GetResolutionX(), Original.ResolutionX);
		TestTrue(TEXT("A rejected update does not invalidate previously stored data"), Asset->HasValidPattern());
	};
	Input = MakeStoredPattern();
	Input.PhaseRad.Last() = std::numeric_limits<double>::quiet_NaN();
	ExpectRejected(TEXT("Nonfinite phase values are rejected"), Input);
	Input = MakeStoredPattern();
	Input.PhaseRad.Pop();
	ExpectRejected(TEXT("Incorrect sample counts are rejected"), Input);
	Input = MakeStoredPattern();
	Input.ResolutionX = 0;
	ExpectRejected(TEXT("Invalid dimensions are rejected"), Input);
	TestTrue(TEXT("A valid replacement clears the diagnostic"), Asset->SetPattern(MakeStoredPattern()));
	TestTrue(TEXT("Successful writes clear stale errors"), Asset->GetValidationError().IsEmpty());
	const FProperty* Payload = FindFProperty<FProperty>(UCGHPhasePatternAsset::StaticClass(), TEXT("StoredPattern"));
	if (TestNotNull(TEXT("Payload is reflected for asset serialization"), Payload))
	{
		TestFalse(TEXT("Payload persists in saved assets"), Payload->HasAnyPropertyFlags(CPF_Transient));
		TestFalse(TEXT("Bulk samples are hidden from Details and direct Blueprint writes"), Payload->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPhaseAssetResamplingTest,
	"CGH.PhasePatternAsset.NearestResamplingAndAtomicFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPhaseAssetResamplingTest::RunTest(const FString& Parameters)
{
	UCGHPhasePatternAsset* Asset = NewObject<UCGHPhasePatternAsset>();
	FCGHSLMPhasePattern Input = MakeStoredPattern();
	Asset->SetPattern(Input);
	FCGHSLMPhasePattern Output;
	FString Error;
	if (!TestTrue(TEXT("A stored grid can be copied at its native resolution"), Asset->BuildPatternForResolution(3, 2, Output, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Native resolution preserves exact unwrapped phases"), Output.PhaseRad == Input.PhaseRad);
	TestEqual(TEXT("Output revision is left for the receiving SLM"), Output.Revision, uint64(0));
	Output.PhaseRad[0] = -10.0;
	TestEqual(TEXT("The output does not alias the asset"), Asset->GetPattern().PhaseRad[0], Input.PhaseRad[0]);

	if (!TestTrue(TEXT("Nearest-neighbor upsampling succeeds"), Asset->BuildPatternForResolution(6, 4, Output, Error)))
	{
		return false;
	}
	TestEqual(TEXT("Resized output contains every requested pixel"), Output.PhaseRad.Num(), 24);
	for (int32 Y = 0; Y < 4; ++Y)
	{
		for (int32 X = 0; X < 6; ++X)
		{
			TestEqual(TEXT("Upsampling preserves top-row order and duplicates nearest source samples"),
				Output.PhaseRad[Y * 6 + X], Input.PhaseRad[(Y / 2) * 3 + X / 2]);
		}
	}
	Asset->BuildPatternForResolution(2, 3, Output, Error);
	const TArray<double> MixedExpected = {Input.PhaseRad[0], Input.PhaseRad[2], Input.PhaseRad[3], Input.PhaseRad[5], Input.PhaseRad[3], Input.PhaseRad[5]};
	TestTrue(TEXT("Mixed resizing chooses source texels nearest destination centers"), Output.PhaseRad == MixedExpected);
	TestTrue(TEXT("Resizing never changes saved source data"), Asset->GetPattern().PhaseRad == Input.PhaseRad);

	const FCGHSLMPhasePattern OriginalOutput = Output;
	const double* OriginalOutputStorage = Output.PhaseRad.GetData();
	const auto ExpectInvalidSize = [&](int32 X, int32 Y)
	{
		TestFalse(TEXT("Unsupported output dimensions are rejected"), Asset->BuildPatternForResolution(X, Y, Output, Error));
		TestFalse(TEXT("Failed conversion explains why"), Error.IsEmpty());
		TestTrue(TEXT("Failure preserves the previous output values"), Output.PhaseRad == OriginalOutput.PhaseRad);
		TestTrue(TEXT("Failure leaves the previous output allocation intact"), Output.PhaseRad.GetData() == OriginalOutputStorage);
		TestEqual(TEXT("Failure preserves output dimensions"), Output.ResolutionX, OriginalOutput.ResolutionX);
	};
	ExpectInvalidSize(0, 3);
	ExpectInvalidSize(-1, 3);
	ExpectInvalidSize(16385, 1);
	ExpectInvalidSize(8193, 8193);
	ExpectInvalidSize(TNumericLimits<int32>::Max(), TNumericLimits<int32>::Max());
	UCGHPhasePatternAsset* Empty = NewObject<UCGHPhasePatternAsset>();
	TestFalse(TEXT("An empty source cannot be resized"), Empty->BuildPatternForResolution(2, 2, Output, Error));
	TestTrue(TEXT("Invalid source leaves the old output intact"), Output.PhaseRad == OriginalOutput.PhaseRad);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHPhaseAssetSerializationTest,
	"CGH.PhasePatternAsset.SerializedRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHPhaseAssetSerializationTest::RunTest(const FString& Parameters)
{
	UCGHPhasePatternAsset* Source = NewObject<UCGHPhasePatternAsset>();
	Source->PatternLabel = FText::FromString(TEXT("Stored preview test"));
	Source->bIsPreviewPattern = true;
	Source->SetPattern(MakeStoredPattern());
	TArray<uint8> Bytes;
	{
		FMemoryWriter Writer(Bytes, true);
		FObjectAndNameAsStringProxyArchive Archive(Writer, false);
		Source->Serialize(Archive);
		if (!TestFalse(TEXT("Serializing phase data succeeds"), Archive.IsError()))
		{
			return false;
		}
	}
	UCGHPhasePatternAsset* Restored = NewObject<UCGHPhasePatternAsset>();
	{
		FMemoryReader Reader(Bytes, true);
		FObjectAndNameAsStringProxyArchive Archive(Reader, false);
		Restored->Serialize(Archive);
		if (!TestFalse(TEXT("Deserializing phase data succeeds"), Archive.IsError()))
		{
			return false;
		}
	}
	TestTrue(TEXT("Deserialized payload remains valid"), Restored->HasValidPattern());
	TestEqual(TEXT("Stored width survives serialization"), Restored->GetResolutionX(), 3);
	TestEqual(TEXT("Stored height survives serialization"), Restored->GetResolutionY(), 2);
	TestTrue(TEXT("Phase values retain exact precision and row order after serialization"), Restored->GetPattern().PhaseRad == Source->GetPattern().PhaseRad);
	TestEqual(TEXT("Asset display label survives serialization"), Restored->PatternLabel.ToString(), Source->PatternLabel.ToString());
	TestTrue(TEXT("Preview provenance survives serialization"), Restored->bIsPreviewPattern);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
