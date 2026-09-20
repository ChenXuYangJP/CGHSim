#include "CGH/Actors/CGHSLMActor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Types/CGHSLMPhasePattern.h"
#include "CoreGlobals.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UnrealType.h"
#include <limits>

#if WITH_EDITOR
#include "CGH/Components/CGHSLMPreviewComponent.h"
#include "Camera/CameraTypes.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/Texture2D.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "LevelEditorViewport.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#endif

namespace
{
	struct FCGHSLMPhaseTestScene : FTestWorldWrapper
	{
		ACGHSLMActor* SLM = nullptr;

		bool Initialize(FAutomationTestBase& Test, EWorldType::Type WorldType = EWorldType::Game)
		{
			if (!CreateTestWorld(WorldType))
			{
				ForwardErrorMessages(&Test);
				return false;
			}
			SLM = TestWorld->SpawnActor<ACGHSLMActor>();
			if (!SLM)
			{
				Test.AddError(TEXT("Could not create the isolated SLM phase-pattern fixture."));
				return false;
			}
			SLM->Parameters.ResolutionX = 2;
			SLM->Parameters.ResolutionY = 2;
			SLM->SynchronizePhasePattern();
			return true;
		}
	};

	FCGHSLMPhasePattern MakeSmallPhasePattern()
	{
		FCGHSLMPhasePattern Pattern;
		Pattern.ResolutionX = 2;
		Pattern.ResolutionY = 2;
		Pattern.PhaseRad = {0.0, UE_DOUBLE_PI / 2.0, UE_DOUBLE_PI, 3.0 * UE_DOUBLE_PI / 2.0};
		return Pattern;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSLMPhaseStorageTest,
	"CGH.SLMPhasePattern.AtomicStorageAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSLMPhaseStorageTest::RunTest(const FString& Parameters)
{
	FCGHSLMPhaseTestScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	ACGHSLMActor& SLM = *Scene.SLM;
	TestFalse(TEXT("New SLM has no fabricated phase data"), SLM.bHasPhaseData);
	TestFalse(TEXT("New SLM has no valid phase pattern"), SLM.HasValidPhasePattern());
	TestTrue(TEXT("New SLM does not allocate a full zero phase buffer"), SLM.GetPhasePattern().PhaseRad.IsEmpty());
	TestTrue(TEXT("New SLM reports unimplemented generation"), SLM.GenerationState == ECGHGenerationState::NotImplemented);

	FCGHSLMPhasePattern Input = MakeSmallPhasePattern();
	Input.Revision = 12345; // Publication revisions belong to the receiving actor.
	const uint64 EmptyRevision = SLM.GetPhasePatternRevision();
	if (!TestTrue(TEXT("Matching finite row-major phase data is accepted"), SLM.SetPhasePattern(Input)))
	{
		AddError(SLM.PhasePatternError);
		return false;
	}
	const uint64 Revision = SLM.GetPhasePatternRevision();
	TestTrue(TEXT("Publishing data advances the actor revision"), Revision > EmptyRevision);
	TestTrue(TEXT("Input cannot override the actor publication revision"), Revision != Input.Revision);
	TestEqual(TEXT("Stored pattern carries the actor publication revision"), SLM.GetPhasePattern().Revision, Revision);
	TestTrue(TEXT("Stored phase values retain row-major order and precision"), SLM.GetPhasePattern().PhaseRad == Input.PhaseRad);
	TestTrue(TEXT("Valid stored data sets the data-ready state"), SLM.GenerationState == ECGHGenerationState::Ready);
	TestTrue(TEXT("Valid stored data sets HasPhaseData"), SLM.bHasPhaseData && SLM.HasValidPhasePattern());
	TestFalse(TEXT("External phase data is not marked as a demo"), SLM.bIsPreviewPhasePattern);

	Input.PhaseRad[0] = 0.75;
	TestEqual(TEXT("Actor owns its phase buffer independently of the caller"), SLM.GetPhasePattern().PhaseRad[0], 0.0);
	FCGHSLMPhasePattern BlueprintCopy = SLM.GetPhasePatternCopy();
	BlueprintCopy.PhaseRad[0] = 1.25;
	BlueprintCopy.ResolutionX = 99;
	TestEqual(TEXT("Editing a Blueprint phase copy does not mutate stored samples"), SLM.GetPhasePattern().PhaseRad[0], 0.0);
	TestEqual(TEXT("Editing a Blueprint phase copy does not mutate stored dimensions"), SLM.GetPhasePattern().ResolutionX, 2);
	TestEqual(TEXT("Editing a Blueprint phase copy leaves publication revision unchanged"), SLM.GetPhasePatternRevision(), Revision);
	const FProperty* StoredPatternProperty = FindFProperty<FProperty>(ACGHSLMActor::StaticClass(), TEXT("PhasePattern"));
	if (TestNotNull(TEXT("Private phase storage remains reflected for object ownership"), StoredPatternProperty))
	{
		TestFalse(TEXT("Blueprint Set Members cannot obtain a mutable reference to private phase storage"),
			StoredPatternProperty->HasAnyPropertyFlags(CPF_BlueprintVisible));
	}
	const TArray<double> OriginalValues = SLM.GetPhasePattern().PhaseRad;
	const double* OriginalStorage = SLM.GetPhasePattern().PhaseRad.GetData();
	const auto CheckRejected = [this, &SLM, Revision, &OriginalValues, OriginalStorage](const FCGHSLMPhasePattern& Invalid)
	{
		TestFalse(TEXT("Invalid phase data is rejected"), SLM.SetPhasePattern(Invalid));
		TestFalse(TEXT("Invalid phase data provides a diagnostic"), SLM.PhasePatternError.IsEmpty());
		TestEqual(TEXT("Rejected input preserves the published revision"), SLM.GetPhasePatternRevision(), Revision);
		TestTrue(TEXT("Rejected input preserves all existing phase values"), SLM.GetPhasePattern().PhaseRad == OriginalValues);
		TestTrue(TEXT("Rejected input does not replace the existing buffer"), SLM.GetPhasePattern().PhaseRad.GetData() == OriginalStorage);
		TestTrue(TEXT("Rejected input leaves the existing valid data ready"), SLM.HasValidPhasePattern() && SLM.GenerationState == ECGHGenerationState::Ready);
	};
	FCGHSLMPhasePattern Invalid = MakeSmallPhasePattern();
	Invalid.ResolutionX = 3;
	CheckRejected(Invalid);
	Invalid = MakeSmallPhasePattern();
	Invalid.PhaseRad.Pop();
	CheckRejected(Invalid);
	Invalid = MakeSmallPhasePattern();
	Invalid.PhaseRad[2] = std::numeric_limits<double>::quiet_NaN();
	CheckRejected(Invalid);
	Invalid.PhaseRad[3] = std::numeric_limits<double>::infinity();
	Invalid.PhaseRad[2] = 0.0;
	CheckRejected(Invalid);

	TestTrue(TEXT("Republishing identical values succeeds"), SLM.SetPhasePattern(MakeSmallPhasePattern()));
	TestEqual(TEXT("Republishing identical data preserves revision"), SLM.GetPhasePatternRevision(), Revision);
	TestTrue(TEXT("Successful publication clears prior validation errors"), SLM.PhasePatternError.IsEmpty());
	SLM.RefreshVisualization();
	TestTrue(TEXT("Refreshing physical geometry preserves phase values"), SLM.GetPhasePattern().PhaseRad == OriginalValues);
	TestEqual(TEXT("Refreshing physical geometry preserves phase revision"), SLM.GetPhasePatternRevision(), Revision);
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSLMPhaseDimensionsTest,
	"CGH.SLMPhasePattern.ResolutionChangesAndClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSLMPhaseDimensionsTest::RunTest(const FString& Parameters)
{
	// FTestWorldWrapper restores this for begun game worlds, but not editor worlds.
	TGuardValue<uint64> RestoreFrameCounter(GFrameCounter, GFrameCounter);
	for (const EWorldType::Type WorldType : {EWorldType::Game, EWorldType::Editor})
	{
		FCGHSLMPhaseTestScene Scene;
		if (!Scene.Initialize(*this, WorldType) || (WorldType == EWorldType::Game && !Scene.BeginPlayInTestWorld()))
		{
			Scene.ForwardErrorMessages(this);
			return false;
		}
		ACGHSLMActor& SLM = *Scene.SLM;
		SLM.SetPhasePattern(MakeSmallPhasePattern());
		uint64 Revision = SLM.GetPhasePatternRevision();
		const TArray<double> OriginalValues = SLM.GetPhasePattern().PhaseRad;
		const auto Tick = [&Scene, WorldType]()
		{
			if (WorldType == EWorldType::Editor)
			{
				Scene.GetTestWorld()->Tick(LEVELTICK_ViewportsOnly, 0.01f);
				// Tick functions run at most once per engine frame. Emulate the frame
				// advance performed by FTestWorldWrapper for begun game worlds.
				++GFrameCounter;
			}
			else
			{
				Scene.TickTestWorld();
			}
		};
		SLM.Parameters.PixelPitchXUm = 4.0;
		SLM.Parameters.PixelPitchYUm = 12.0;
		SLM.RefreshVisualization();
		Tick();
		TestTrue(TEXT("Pixel-pitch changes preserve phase samples"), SLM.GetPhasePattern().PhaseRad == OriginalValues);
		TestEqual(TEXT("Pixel-pitch changes preserve phase revision"), SLM.GetPhasePatternRevision(), Revision);

		SLM.Parameters.ResolutionX = 3; // Direct writes must be detected without property events.
		Tick();
		TestFalse(TEXT("Resolution edits invalidate mismatched phase data"), SLM.HasValidPhasePattern() || SLM.bHasPhaseData);
		TestTrue(TEXT("Resolution edits release stale samples"), SLM.GetPhasePattern().PhaseRad.IsEmpty());
		TestTrue(TEXT("Resolution edits advance the publication revision"), SLM.GetPhasePatternRevision() > Revision);
		TestEqual(TEXT("Empty pattern tracks current X resolution"), SLM.GetPhasePattern().ResolutionX, 3);
		TestEqual(TEXT("Empty pattern tracks current Y resolution"), SLM.GetPhasePattern().ResolutionY, 2);
		Revision = SLM.GetPhasePatternRevision();
		Tick();
		TestEqual(TEXT("Unchanged ticks do not repeatedly invalidate the pattern"), SLM.GetPhasePatternRevision(), Revision);

		FCGHSLMPhasePattern Replacement;
		Replacement.ResolutionX = 3;
		Replacement.ResolutionY = 2;
		Replacement.PhaseRad = {0.0, 0.5, 1.0, 1.5, 2.0, 2.5};
		TestTrue(TEXT("New data matching the resized SLM is accepted"), SLM.SetPhasePattern(Replacement));
		Revision = SLM.GetPhasePatternRevision();
		SLM.ClearPhasePattern();
		TestFalse(TEXT("Clearing removes valid phase data"), SLM.HasValidPhasePattern() || SLM.bHasPhaseData);
		TestTrue(TEXT("Clearing releases the phase buffer"), SLM.GetPhasePattern().PhaseRad.IsEmpty());
		TestTrue(TEXT("Clearing published data advances revision"), SLM.GetPhasePatternRevision() > Revision);
		TestTrue(TEXT("Clearing restores the unimplemented generator state"), SLM.GenerationState == ECGHGenerationState::NotImplemented);
		Revision = SLM.GetPhasePatternRevision();
		SLM.ClearPhasePattern();
		TestEqual(TEXT("Clearing an already empty unchanged SLM is idempotent"), SLM.GetPhasePatternRevision(), Revision);

#if WITH_EDITOR
		if (WorldType == EWorldType::Editor)
		{
			SLM.Parameters.ResolutionX = 2;
			SLM.Parameters.ResolutionY = 2;
			SLM.SetPhasePattern(MakeSmallPhasePattern());
			Revision = SLM.GetPhasePatternRevision();
			FProperty* ParametersProperty = FindFProperty<FProperty>(ACGHSLMActor::StaticClass(),
				GET_MEMBER_NAME_CHECKED(ACGHSLMActor, Parameters));
			if (!TestNotNull(TEXT("SLM parameters are exposed for Details editing"), ParametersProperty))
			{
				return false;
			}

			SLM.PreEditChange(ParametersProperty);
			SLM.Parameters.PixelPitchXUm = 6.0;
			FPropertyChangedEvent PitchEvent(ParametersProperty, EPropertyChangeType::ValueSet);
			SLM.PostEditChangeProperty(PitchEvent);
			TestTrue(TEXT("Details pitch edit preserves valid phase data through construction"), SLM.HasValidPhasePattern());
			TestTrue(TEXT("Details pitch edit preserves phase values"), SLM.GetPhasePattern().PhaseRad == OriginalValues);
			TestEqual(TEXT("Details pitch edit preserves phase revision"), SLM.GetPhasePatternRevision(), Revision);

			SLM.PreEditChange(ParametersProperty);
			SLM.Parameters.ResolutionY = 3;
			FPropertyChangedEvent ResolutionEvent(ParametersProperty, EPropertyChangeType::ValueSet);
			SLM.PostEditChangeProperty(ResolutionEvent);
			TestFalse(TEXT("Details resolution edit immediately invalidates phase data"), SLM.HasValidPhasePattern() || SLM.bHasPhaseData);
			TestTrue(TEXT("Details resolution edit immediately clears stale samples"), SLM.GetPhasePattern().PhaseRad.IsEmpty());
			TestTrue(TEXT("Details resolution edit advances phase revision"), SLM.GetPhasePatternRevision() > Revision);
			TestEqual(TEXT("Details resolution edit updates stored dimensions"), SLM.GetPhasePattern().ResolutionY, 3);
		}
#endif
		Scene.ForwardErrorMessages(this);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSLMPhaseDemoTest,
	"CGH.SLMPhasePattern.ExplicitDemoRamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSLMPhaseDemoTest::RunTest(const FString& Parameters)
{
	FCGHSLMPhaseTestScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	ACGHSLMActor& SLM = *Scene.SLM;
	SLM.Parameters.ResolutionX = 4;
	SLM.Parameters.ResolutionY = 2;
	SLM.GeneratePreviewPhaseRamp();
	TestTrue(TEXT("Explicit demo command provides phase data"), SLM.HasValidPhasePattern());
	TestTrue(TEXT("Demo data is labeled as a preview pattern"), SLM.bIsPreviewPhasePattern);
	const FCGHSLMPhasePattern& Pattern = SLM.GetPhasePattern();
	if (!TestEqual(TEXT("Demo creates one sample per SLM pixel"), Pattern.PhaseRad.Num(), 8))
	{
		return false;
	}
	for (int32 Y = 0; Y < 2; ++Y)
	{
		for (int32 X = 0; X < 4; ++X)
		{
			TestEqual(TEXT("Demo is a horizontal row-major ramp over [0, 2pi)"),
				Pattern.PhaseRad[Y * 4 + X], 2.0 * UE_DOUBLE_PI * X / 4.0, 1.0e-12);
		}
	}
	const uint64 DemoRevision = SLM.GetPhasePatternRevision();
	FCGHSLMPhasePattern External = Pattern;
	TestTrue(TEXT("External publication can replace identical demo samples"), SLM.SetPhasePattern(External));
	TestFalse(TEXT("External publication clears the demo label"), SLM.bIsPreviewPhasePattern);
	TestEqual(TEXT("Changing only the demo label does not reupload identical samples"), SLM.GetPhasePatternRevision(), DemoRevision);
	Scene.ForwardErrorMessages(this);
	return true;
}

#if WITH_EDITOR

namespace
{
	bool ContainsPhaseDataRows(const TArray<TSharedRef<IDetailTreeNode>>& Nodes)
	{
		for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
		{
			const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
			const FProperty* Property = Handle.IsValid() ? Handle->GetProperty() : nullptr;
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if ((StructProperty && StructProperty->Struct == FCGHSLMPhasePattern::StaticStruct())
				|| (Property && Property->GetOwnerStruct() == FCGHSLMPhasePattern::StaticStruct()))
			{
				return true;
			}
			TArray<TSharedRef<IDetailTreeNode>> Children;
			Node->GetChildren(Children, true);
			if (ContainsPhaseDataRows(Children))
			{
				return true;
			}
		}
		return false;
	}

	TArray<FColor> ReadPreviewPixels(UTexture2D* Texture)
	{
		TArray<FColor> Pixels;
		if (Texture && Texture->GetPlatformData() && !Texture->GetPlatformData()->Mips.IsEmpty())
		{
			FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
			if (Mip.BulkData.GetBulkDataSize() > 0)
			{
				Pixels.SetNumUninitialized(Mip.BulkData.GetBulkDataSize() / sizeof(FColor));
				const void* Data = Mip.BulkData.LockReadOnly();
				FMemory::Memcpy(Pixels.GetData(), Data, Pixels.Num() * sizeof(FColor));
				Mip.BulkData.Unlock();
			}
		}
		return Pixels;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSLMPhaseEditorPreviewTest,
	"CGH.SLMPhasePattern.SelectedEditorPreviewAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSLMPhaseEditorPreviewTest::RunTest(const FString& Parameters)
{
	FCGHSLMPhaseTestScene Scene;
	if (!Scene.Initialize(*this, EWorldType::Editor) || !TestNotNull(TEXT("Editor selection is available"), GEditor))
	{
		return false;
	}
	ACGHSLMActor* SLM = Scene.SLM;
	UCGHSLMPreviewComponent* Preview = SLM->FindComponentByClass<UCGHSLMPreviewComponent>();
	if (!TestNotNull(TEXT("SLM owns a native editor preview component"), Preview))
	{
		return false;
	}
	TestTrue(TEXT("Preview component participates in native selected-actor discovery"), Preview->IsActive());
	TestTrue(TEXT("Level editor discovers the SLM preview without a camera component"),
		FLevelEditorViewportClient::FindViewComponentForActor(SLM) == Preview);
	FMinimalViewInfo View;
	TestTrue(TEXT("SLM supplies native editor preview information"), Preview->GetEditorPreviewInfo(0.0f, View));
	TestTrue(TEXT("SLM supplies a custom phase-display widget"), Preview->GetCustomEditorPreviewWidget().IsValid());
	Preview->RefreshPreviewTexture();
	TestNull(TEXT("Empty SLM has no fabricated zero-valued phase texture"), Preview->GetPreviewTexture());

	SLM->SetPhasePattern(MakeSmallPhasePattern());
	Preview->RefreshPreviewTexture();
	UTexture2D* Texture = Preview->GetPreviewTexture();
	if (!TestNotNull(TEXT("Published phase data creates a preview texture"), Texture))
	{
		AddError(Preview->GetPreviewError());
		return false;
	}
	TestEqual(TEXT("Preview preserves SLM horizontal pixel count"), Texture->GetSizeX(), 2);
	TestEqual(TEXT("Preview preserves SLM vertical pixel count"), Texture->GetSizeY(), 2);
	TestTrue(TEXT("Preview stores grayscale in BGRA8"), Texture->GetPixelFormat() == PF_B8G8R8A8);
	TestTrue(TEXT("Preview keeps individual SLM pixels sharp"), Texture->Filter == TF_Nearest);
	TestFalse(TEXT("Phase grayscale is displayed without sRGB conversion"), Texture->SRGB);
	const TArray<FColor> Pixels = ReadPreviewPixels(Texture);
	if (TestEqual(TEXT("Preview texture contains one pixel per phase value"), Pixels.Num(), 4))
	{
		const uint8 Expected[] = {0, 64, 128, 191};
		for (int32 Index = 0; Index < 4; ++Index)
		{
			TestEqual(TEXT("Preview maps phase values to row-major grayscale"), Pixels[Index],
				FColor(Expected[Index], Expected[Index], Expected[Index], 255));
		}
	}
	const uint64 Revision = SLM->GetPhasePatternRevision();
	TestEqual(TEXT("Texture cache records the published phase revision"), Preview->GetPreviewRevision(), Revision);
	const double* PhaseStorage = SLM->GetPhasePattern().PhaseRad.GetData();
	ON_SCOPE_EXIT
	{
		GEditor->SelectActor(SLM, false, true, true);
	};
	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		GEditor->SelectActor(SLM, true, true, true);
		TestTrue(TEXT("Real editor selection includes the SLM"), GEditor->GetSelectedActors()->IsSelected(SLM));
		Preview->GetCustomEditorPreviewWidget();
		Preview->RefreshPreviewTexture();
		TestTrue(TEXT("Reselection reuses the phase-preview texture"), Preview->GetPreviewTexture() == Texture);
		TestEqual(TEXT("Reselection does not change phase revision"), SLM->GetPhasePatternRevision(), Revision);
		TestEqual(TEXT("Reselection retains the texture revision"), Preview->GetPreviewRevision(), Revision);
		TestTrue(TEXT("Reselection preserves the CPU phase buffer"), SLM->GetPhasePattern().PhaseRad.GetData() == PhaseStorage);
		GEditor->SelectActor(SLM, false, true, true);
	}

	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	TSharedRef<IPropertyRowGenerator> Rows = PropertyEditor.CreatePropertyRowGenerator(FPropertyRowGeneratorArgs());
	Rows->OnRowsRefreshed().AddLambda([]() {});
	Rows->SetObjects({SLM});
	TestFalse(TEXT("Details does not generate per-pixel phase array rows"), ContainsPhaseDataRows(Rows->GetRootTreeNodes()));
	Rows->SetObjects({});

	FCGHSLMPhasePattern Updated = MakeSmallPhasePattern();
	Updated.PhaseRad[0] = UE_DOUBLE_PI;
	SLM->SetPhasePattern(Updated);
	Preview->RefreshPreviewTexture();
	TestTrue(TEXT("Same-size phase updates reuse the preview texture"), Preview->GetPreviewTexture() == Texture);
	TestTrue(TEXT("New phase data advances the preview cache revision"), Preview->GetPreviewRevision() > Revision);
	const TArray<FColor> UpdatedPixels = ReadPreviewPixels(Texture);
	if (TestEqual(TEXT("Updated preview preserves pixel count"), UpdatedPixels.Num(), 4))
	{
		TestEqual(TEXT("Updated phase data refreshes preview grayscale"), UpdatedPixels[0], FColor(128, 128, 128, 255));
	}
	SLM->ClearPhasePattern();
	TestNull(TEXT("Clearing phase data immediately releases the cached preview texture"), Preview->GetPreviewTexture());
	Preview->RefreshPreviewTexture();
	TestNull(TEXT("Clearing phase data returns preview to its empty state"), Preview->GetPreviewTexture());
	Scene.ForwardErrorMessages(this);
	return true;
}

#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
