#include "CGH/Actors/CGHSLMActor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Types/CGHPhasePatternAsset.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_EDITOR
#include "CGH/Components/CGHSLMPreviewComponent.h"
#include "Engine/Texture2D.h"
#endif

namespace
{
	struct FCGHStoredPhaseScene : FTestWorldWrapper
	{
		ACGHSLMActor* SLM = nullptr;
		TStrongObjectPtr<UCGHPhasePatternAsset> Asset;

		bool Initialize(FAutomationTestBase& Test)
		{
			if (!CreateTestWorld(EWorldType::Game))
			{
				ForwardErrorMessages(&Test);
				return false;
			}
			SLM = GetTestWorld()->SpawnActor<ACGHSLMActor>();
			if (!Test.TestNotNull(TEXT("Stored-pattern fixture creates an isolated SLM"), SLM))
			{
				return false;
			}
			SLM->Parameters.ResolutionX = 4;
			SLM->Parameters.ResolutionY = 2;
			SLM->Parameters.PixelPitchXUm = 7.5;
			SLM->Parameters.PixelPitchYUm = 11.5;
			SLM->ClearPhasePattern();

			Asset.Reset(NewObject<UCGHPhasePatternAsset>(GetTransientPackage(), NAME_None, RF_Transient));
			Asset->PatternLabel = FText::FromString(TEXT("Stored phase integration fixture"));
			Asset->bIsPreviewPattern = true;
			FCGHSLMPhasePattern Source;
			Source.ResolutionX = 2;
			Source.ResolutionY = 2;
			Source.PhaseRad = {0.0, UE_DOUBLE_PI / 2.0, UE_DOUBLE_PI, 3.0 * UE_DOUBLE_PI / 2.0};
			if (!Test.TestTrue(TEXT("Transient asset accepts its source pattern"), Asset->SetPattern(Source)))
			{
				return false;
			}
			SLM->StoredPhasePattern = Asset.Get();
			return true;
		}
	};

	TArray<double> ExpectedFourByTwoPattern()
	{
		return {0.0, 0.0, UE_DOUBLE_PI / 2.0, UE_DOUBLE_PI / 2.0,
			UE_DOUBLE_PI, UE_DOUBLE_PI, 3.0 * UE_DOUBLE_PI / 2.0, 3.0 * UE_DOUBLE_PI / 2.0};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHStoredPhaseActivationTest,
	"CGH.StoredPhasePattern.ActivationResamplesWithoutChangingSLMGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHStoredPhaseActivationTest::RunTest(const FString& Parameters)
{
	FCGHStoredPhaseScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	ACGHSLMActor& SLM = *Scene.SLM;
	const FCGHSLMParameters Configuration = SLM.Parameters;
	const double ActiveWidthMm = SLM.GetActiveWidthMm();
	const double ActiveHeightMm = SLM.GetActiveHeightMm();
	const FTransform ActorTransform = SLM.GetActorTransform();
	const FCGHSLMPhasePattern Source = Scene.Asset->GetPattern();
	const double* SourceStorage = Scene.Asset->GetPattern().PhaseRad.GetData();
	const uint64 EmptyRevision = SLM.GetPhasePatternRevision();

	SLM.LoadStoredPhasePattern();
	if (!TestTrue(TEXT("Loading the selected stored asset produces a valid pattern"), SLM.HasValidPhasePattern()))
	{
		AddError(SLM.PhasePatternError);
		return false;
	}
	const TArray<double> Expected = ExpectedFourByTwoPattern();
	TestEqual(TEXT("Stored pattern uses configured SLM width"), SLM.GetPhasePattern().ResolutionX, 4);
	TestEqual(TEXT("Stored pattern uses configured SLM height"), SLM.GetPhasePattern().ResolutionY, 2);
	TestTrue(TEXT("Nearest-center resampling duplicates source columns in row-major order"), SLM.GetPhasePattern().PhaseRad == Expected);
	for (double Phase : SLM.GetPhasePattern().PhaseRad)
	{
		TestTrue(TEXT("Activated samples are finite"), FMath::IsFinite(Phase));
	}
	TestTrue(TEXT("Activating a stored pattern advances the actor revision"), SLM.GetPhasePatternRevision() > EmptyRevision);
	TestTrue(TEXT("Asset label is displayed on the SLM"), SLM.PhasePatternLabel.EqualTo(Scene.Asset->PatternLabel));
	TestTrue(TEXT("Asset preview marker is displayed on the SLM"), SLM.bIsPreviewPhasePattern);
	TestTrue(TEXT("Successful activation clears errors"), SLM.PhasePatternError.IsEmpty());
	TestTrue(TEXT("Activated data is ready"), SLM.GenerationState == ECGHGenerationState::Ready);

	const uint64 LoadedRevision = SLM.GetPhasePatternRevision();
	const double* LoadedStorage = SLM.GetPhasePattern().PhaseRad.GetData();
#if WITH_EDITOR
	UCGHSLMPreviewComponent* Preview = SLM.FindComponentByClass<UCGHSLMPreviewComponent>();
	if (!TestNotNull(TEXT("Stored pattern supports the editor preview component"), Preview))
	{
		return false;
	}
	Preview->RefreshPreviewTexture();
	UTexture2D* Texture = Preview->GetPreviewTexture();
	if (!TestNotNull(TEXT("Activating stored data produces a preview texture"), Texture))
	{
		AddError(Preview->GetPreviewError());
		return false;
	}
	TestEqual(TEXT("Stored preview texture has configured width"), Texture->GetSizeX(), 4);
	TestEqual(TEXT("Stored preview texture has configured height"), Texture->GetSizeY(), 2);
	TestEqual(TEXT("Stored preview texture matches the activated revision"), Preview->GetPreviewRevision(), LoadedRevision);
#endif

	SLM.LoadStoredPhasePattern();
	TestEqual(TEXT("Reloading identical stored data preserves revision"), SLM.GetPhasePatternRevision(), LoadedRevision);
	TestTrue(TEXT("Reloading identical data preserves the actor's phase allocation"), SLM.GetPhasePattern().PhaseRad.GetData() == LoadedStorage);
#if WITH_EDITOR
	Preview->RefreshPreviewTexture();
	TestTrue(TEXT("Reloading identical data preserves the preview texture"), Preview->GetPreviewTexture() == Texture);
	TestEqual(TEXT("Reloading identical data keeps the cached preview revision"), Preview->GetPreviewRevision(), LoadedRevision);
#endif

	TestEqual(TEXT("Activation does not modify X resolution"), SLM.Parameters.ResolutionX, Configuration.ResolutionX);
	TestEqual(TEXT("Activation does not modify Y resolution"), SLM.Parameters.ResolutionY, Configuration.ResolutionY);
	TestEqual(TEXT("Activation does not modify X pixel pitch"), SLM.Parameters.PixelPitchXUm, Configuration.PixelPitchXUm);
	TestEqual(TEXT("Activation does not modify Y pixel pitch"), SLM.Parameters.PixelPitchYUm, Configuration.PixelPitchYUm);
	TestEqual(TEXT("Activation preserves active width"), SLM.GetActiveWidthMm(), ActiveWidthMm);
	TestEqual(TEXT("Activation preserves active height"), SLM.GetActiveHeightMm(), ActiveHeightMm);
	TestTrue(TEXT("Activation preserves the actor transform"), SLM.GetActorTransform().Equals(ActorTransform, 0.0));
	TestTrue(TEXT("Activation does not modify source asset samples"), Scene.Asset->GetPattern().PhaseRad == Source.PhaseRad);
	TestTrue(TEXT("Activation does not replace source asset storage"), Scene.Asset->GetPattern().PhaseRad.GetData() == SourceStorage);
	TestEqual(TEXT("Activation preserves source asset width"), Scene.Asset->GetPattern().ResolutionX, Source.ResolutionX);
	TestEqual(TEXT("Activation preserves source asset height"), Scene.Asset->GetPattern().ResolutionY, Source.ResolutionY);
	TestEqual(TEXT("Activation preserves source asset revision"), Scene.Asset->GetPattern().Revision, Source.Revision);

	SLM.ClearPhasePattern();
	const uint64 ClearRevision = SLM.GetPhasePatternRevision();
	TestFalse(TEXT("Clear removes active data"), SLM.HasValidPhasePattern());
	TestTrue(TEXT("Clear removes the active pattern label"), SLM.PhasePatternLabel.IsEmpty());
	TestFalse(TEXT("Clear removes the active preview marker"), SLM.bIsPreviewPhasePattern);
	TestTrue(TEXT("Clear advances the revision"), ClearRevision > LoadedRevision);
	TestTrue(TEXT("Clear keeps the preset selected for later loading"), SLM.StoredPhasePattern.Get() == Scene.Asset.Get());
#if WITH_EDITOR
	TestNull(TEXT("Clear immediately releases the cached preview texture"), Preview->GetPreviewTexture());
#endif
	SLM.LoadStoredPhasePattern();
	TestTrue(TEXT("The selected asset can be reloaded after clearing"), SLM.HasValidPhasePattern());
	TestTrue(TEXT("Reload after clearing restores resampled values"), SLM.GetPhasePattern().PhaseRad == Expected);
	TestTrue(TEXT("Reload after clearing advances revision"), SLM.GetPhasePatternRevision() > ClearRevision);
	TestTrue(TEXT("Reload after clearing restores the asset label"), SLM.PhasePatternLabel.EqualTo(Scene.Asset->PatternLabel));

	// Label-only preset edits must not invalidate an identical numerical image.
	const uint64 BeforeRelabelRevision = SLM.GetPhasePatternRevision();
	Scene.Asset->PatternLabel = FText::FromString(TEXT("Relabeled stored fixture"));
	Scene.Asset->bIsPreviewPattern = false;
	SLM.LoadStoredPhasePattern();
	TestTrue(TEXT("Reload reflects an updated asset label"), SLM.PhasePatternLabel.EqualTo(Scene.Asset->PatternLabel));
	TestFalse(TEXT("Reload reflects a solver-pattern marker"), SLM.bIsPreviewPhasePattern);
	TestEqual(TEXT("Label-only changes preserve the numerical revision"), SLM.GetPhasePatternRevision(), BeforeRelabelRevision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHStoredPhaseFailureTest,
	"CGH.StoredPhasePattern.FailedActivationPreservesPublishedPattern",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHStoredPhaseFailureTest::RunTest(const FString& Parameters)
{
	FCGHStoredPhaseScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	ACGHSLMActor& SLM = *Scene.SLM;
	SLM.LoadStoredPhasePattern();
	if (!TestTrue(TEXT("Failure fixture starts with valid stored data"), SLM.HasValidPhasePattern()))
	{
		AddError(SLM.PhasePatternError);
		return false;
	}
	const uint64 Revision = SLM.GetPhasePatternRevision();
	const TArray<double> Published = SLM.GetPhasePattern().PhaseRad;
	const double* PublishedStorage = SLM.GetPhasePattern().PhaseRad.GetData();
	const FText PublishedLabel = SLM.PhasePatternLabel;
#if WITH_EDITOR
	UCGHSLMPreviewComponent* Preview = SLM.FindComponentByClass<UCGHSLMPreviewComponent>();
	if (!TestNotNull(TEXT("Failure fixture has a preview component"), Preview))
	{
		return false;
	}
	Preview->RefreshPreviewTexture();
	UTexture2D* Texture = Preview->GetPreviewTexture();
#endif
	const auto CheckPreserved = [this, &SLM, Revision, &Published, PublishedStorage, &PublishedLabel]()
	{
		TestFalse(TEXT("Failed preset activation reports an error"), SLM.PhasePatternError.IsEmpty());
		TestTrue(TEXT("Failed activation retains valid phase data"), SLM.HasValidPhasePattern());
		TestTrue(TEXT("Failed activation retains data-ready state"), SLM.GenerationState == ECGHGenerationState::Ready);
		TestEqual(TEXT("Failed activation preserves revision"), SLM.GetPhasePatternRevision(), Revision);
		TestTrue(TEXT("Failed activation preserves sample values"), SLM.GetPhasePattern().PhaseRad == Published);
		TestTrue(TEXT("Failed activation preserves the phase allocation"), SLM.GetPhasePattern().PhaseRad.GetData() == PublishedStorage);
		TestTrue(TEXT("Failed activation preserves the active label"), SLM.PhasePatternLabel.EqualTo(PublishedLabel));
		TestTrue(TEXT("Failed activation preserves the active preview marker"), SLM.bIsPreviewPhasePattern);
		TestEqual(TEXT("Failed activation preserves configured width"), SLM.Parameters.ResolutionX, 4);
		TestEqual(TEXT("Failed activation preserves configured height"), SLM.Parameters.ResolutionY, 2);
		TestEqual(TEXT("Failed activation preserves X pitch"), SLM.Parameters.PixelPitchXUm, 7.5);
		TestEqual(TEXT("Failed activation preserves Y pitch"), SLM.Parameters.PixelPitchYUm, 11.5);
	};

	SLM.StoredPhasePattern.Reset();
	SLM.LoadStoredPhasePattern();
	CheckPreserved();

	// Empty stored data is invalid even though the referenced asset object exists.
	TStrongObjectPtr<UCGHPhasePatternAsset> EmptyAsset(NewObject<UCGHPhasePatternAsset>(GetTransientPackage(), NAME_None, RF_Transient));
	EmptyAsset->PatternLabel = FText::FromString(TEXT("Invalid empty preset"));
	SLM.StoredPhasePattern = EmptyAsset.Get();
	SLM.LoadStoredPhasePattern();
	CheckPreserved();
#if WITH_EDITOR
	Preview->RefreshPreviewTexture();
	TestTrue(TEXT("Rejected preset activation preserves the existing preview image"), Preview->GetPreviewTexture() == Texture);
	TestEqual(TEXT("Rejected preset activation preserves the texture revision"), Preview->GetPreviewRevision(), Revision);
#endif

	SLM.StoredPhasePattern = Scene.Asset.Get();
	SLM.LoadStoredPhasePattern();
	TestTrue(TEXT("Valid reload clears the previous error"), SLM.PhasePatternError.IsEmpty());
	TestTrue(TEXT("Valid reload after failures restores asset ownership label"), SLM.PhasePatternLabel.EqualTo(Scene.Asset->PatternLabel));
	TestEqual(TEXT("Recovering to identical samples keeps the original revision"), SLM.GetPhasePatternRevision(), Revision);
	TestTrue(TEXT("Failed activation and recovery never alter source samples"), Scene.Asset->GetPattern().PhaseRad == TArray<double>({0.0, UE_DOUBLE_PI / 2.0, UE_DOUBLE_PI, 3.0 * UE_DOUBLE_PI / 2.0}));
	return true;
}

#endif
