#include "CGH/Actors/CGHWorkbenchActor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Actors/CGHCameraActor.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHTargetActor.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

#if WITH_EDITOR
#include "Misc/TransactionObjectEvent.h"
#include "UObject/UnrealType.h"
#endif

namespace
{
	/** Owns an unsaved world; never loads or changes the user's workbench map. */
	struct FCGHTestScene : FTestWorldWrapper
	{
		ACGHSLMActor* SLM = nullptr;
		ACGHCameraActor* Camera = nullptr;
		ACGHReconstructionLightActor* Light = nullptr;
		ACGHTargetActor* FirstTarget = nullptr;
		ACGHTargetActor* SecondTarget = nullptr;
		ACGHWorkbenchActor* Workbench = nullptr;

		bool Initialize(FAutomationTestBase& Test, EWorldType::Type WorldType = EWorldType::Game)
		{
			if (!CreateTestWorld(WorldType))
			{
				ForwardErrorMessages(&Test);
				return false;
			}

			SLM = TestWorld->SpawnActor<ACGHSLMActor>();
			Camera = TestWorld->SpawnActor<ACGHCameraActor>();
			Light = TestWorld->SpawnActor<ACGHReconstructionLightActor>();
			FirstTarget = TestWorld->SpawnActor<ACGHTargetActor>();
			SecondTarget = TestWorld->SpawnActor<ACGHTargetActor>();
			Workbench = TestWorld->SpawnActor<ACGHWorkbenchActor>();
			if (!SLM || !Camera || !Light || !FirstTarget || !SecondTarget || !Workbench)
			{
				Test.AddError(TEXT("Could not spawn the CGH test actors."));
				return false;
			}

			Workbench->SLM = SLM;
			Workbench->Camera = Camera;
			Workbench->ReconstructionLight = Light;
			Workbench->Targets = {FirstTarget, SecondTarget};
			return true;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSceneDescriptionUnitsTest,
	"CGH.SceneDescription.SIUnitsAndOpticalCoordinates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSceneDescriptionUnitsTest::RunTest(const FString& Parameters)
{
	FCGHTestScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}

	Scene.SLM->Parameters.ResolutionX = 2048;
	Scene.SLM->Parameters.ResolutionY = 1080;
	Scene.SLM->Parameters.PixelPitchXUm = 6.4;
	Scene.SLM->Parameters.PixelPitchYUm = 7.2;
	Scene.SLM->SetActorLocationAndRotation(FVector(170.0, -240.0, 90.0), FRotator(0.0, 90.0, 0.0));

	Scene.Light->Parameters.WavelengthNm = 633.123456789;
	Scene.Light->Parameters.Amplitude = 0.375;
	Scene.Light->Parameters.InitialPhaseRad = 1.25;
	Scene.Light->Parameters.PolarizationAngleDeg = 90.0;
	Scene.Light->SetActorLocationAndRotation(FVector(170.0, -340.0, 90.0), FRotator(0.0, 180.0, 0.0));

	Scene.Camera->Parameters.FocalLengthMm = 85.123456789;
	Scene.Camera->Parameters.FNumber = 1.8;
	Scene.Camera->Parameters.FocusDistanceMm = 1250.125;
	Scene.Camera->Parameters.SensorWidthMm = 36.125;
	Scene.Camera->Parameters.SensorHeightMm = 24.25;
	Scene.Camera->Parameters.OutputResolutionX = 3072;
	Scene.Camera->Parameters.OutputResolutionY = 2048;
	Scene.Camera->SetActorLocationAndRotation(FVector(170.0, -260.0, 95.0), FRotator(0.0, 90.0, 0.0));
	Scene.Camera->GetOpticalReference()->SetRelativeLocationAndRotation(
		FVector(10.0, 2.0, -1.0), FRotator(0.0, 90.0, 0.0));

	Scene.FirstTarget->SetActorLocation(FVector(158.0, -190.0, 83.0));
	Scene.FirstTarget->Parameters.Amplitude = 0.125;
	Scene.FirstTarget->Parameters.InitialPhaseRad = -0.75;
	Scene.SecondTarget->SetActorLocation(FVector(200.0, -165.0, 110.0));
	Scene.SecondTarget->Parameters.Amplitude = 0.625;
	Scene.SecondTarget->Parameters.InitialPhaseRad = 2.25;
	Scene.Workbench->Targets = {Scene.SecondTarget, Scene.FirstTarget};
	Scene.Workbench->UpdateSceneDescription();

	const FCGHSceneDescription& Description = Scene.Workbench->SceneDescription;
	TestTrue(TEXT("Assigned actors produce a complete snapshot"), Scene.Workbench->bSceneDescriptionComplete);
	TestEqual(TEXT("Schema version"), Description.SchemaVersion, 2);
	TestEqual(TEXT("SLM resolution X"), Description.SLM.ResolutionX, 2048);
	TestEqual(TEXT("SLM resolution Y"), Description.SLM.ResolutionY, 1080);
	TestEqual(TEXT("Micrometers convert to meters for pitch X"), Description.SLM.PixelPitchXM, 6.4e-6, 1.0e-15);
	TestEqual(TEXT("Micrometers convert to meters for pitch Y"), Description.SLM.PixelPitchYM, 7.2e-6, 1.0e-15);
	TestEqual(TEXT("Active width derives from pitch and resolution"), Description.SLM.ActiveWidthM, 0.0131072, 1.0e-15);
	TestEqual(TEXT("Active height derives from pitch and resolution"), Description.SLM.ActiveHeightM, 0.007776, 1.0e-15);
	TestEqual(TEXT("Wavelength retains double precision in meters"), Description.ReconstructionLight.WavelengthM, 6.33123456789e-7, 1.0e-18);
	TestEqual(TEXT("Light amplitude"), Description.ReconstructionLight.Amplitude, 0.375, 1.0e-15);
	TestEqual(TEXT("Light phase remains radians"), Description.ReconstructionLight.InitialPhaseRad, 1.25, 1.0e-15);
	TestEqual(TEXT("Polarization converts degrees to radians"), Description.ReconstructionLight.PolarizationAngleRad, UE_DOUBLE_PI / 2.0, 1.0e-15);
	TestEqual(TEXT("Light direction is SLM-local"), Description.ReconstructionLight.DirectionSLM, FVector::YAxisVector, 1.0e-6f);
	TestEqual(TEXT("Light position is SLM-local meters"), Description.ReconstructionLight.PositionSLMM, FVector(-1.0, 0.0, 0.0), 1.0e-6f);
	TestEqual(TEXT("Camera pose uses the offset optical reference"), Description.Camera.OpticalPositionSLMM, FVector(-0.1, 0.02, 0.04), 1.0e-6f);
	TestEqual(TEXT("Camera forward includes optical reference rotation"), Description.Camera.ForwardDirectionSLM, FVector::YAxisVector, 1.0e-6f);
	TestEqual(TEXT("Focal length retains double precision in meters"), Description.Camera.FocalLengthM, 0.085123456789, 1.0e-15);
	TestEqual(TEXT("F-number is dimensionless"), Description.Camera.FNumber, 1.8, 1.0e-15);
	TestEqual(TEXT("Focus distance converts to meters"), Description.Camera.FocusDistanceM, 1.250125, 1.0e-15);
	TestEqual(TEXT("Sensor width converts to meters"), Description.Camera.SensorWidthM, 0.036125, 1.0e-15);
	TestEqual(TEXT("Sensor height converts to meters"), Description.Camera.SensorHeightM, 0.02425, 1.0e-15);
	TestEqual(TEXT("Camera output X"), Description.Camera.OutputResolutionX, 3072);
	TestEqual(TEXT("Camera output Y"), Description.Camera.OutputResolutionY, 2048);
	if (TestEqual(TEXT("Target count"), Description.Targets.Num(), 2))
	{
		TestEqual(TEXT("Export preserves first assigned target"), Description.Targets[0].PositionSLMM, FVector(0.75, -0.3, 0.2), 1.0e-6f);
		TestEqual(TEXT("First assigned target amplitude"), Description.Targets[0].Amplitude, 0.625, 1.0e-15);
		TestEqual(TEXT("First assigned target phase"), Description.Targets[0].PhaseRad, 2.25, 1.0e-15);
		TestEqual(TEXT("Export preserves second assigned target"), Description.Targets[1].PositionSLMM, FVector(0.5, 0.12, -0.07), 1.0e-6f);
		TestEqual(TEXT("Second assigned target amplitude"), Description.Targets[1].Amplitude, 0.125, 1.0e-15);
		TestEqual(TEXT("Second assigned target phase"), Description.Targets[1].PhaseRad, -0.75, 1.0e-15);
	}

	// Scale is a validation issue, never a change to the physical coordinate unit.
	Scene.SLM->SetActorScale3D(FVector(2.0, 3.0, 4.0));
	Scene.Workbench->UpdateSceneDescription();
	TestEqual(TEXT("SLM scale does not distort optical camera position"), Description.Camera.OpticalPositionSLMM, FVector(-0.1, 0.02, 0.04), 1.0e-6f);
	TestEqual(TEXT("SLM scale does not distort physical width"), Description.SLM.ActiveWidthM, 0.0131072, 1.0e-15);
	if (Description.Targets.Num() == 2)
	{
		TestEqual(TEXT("SLM scale does not distort target position"), Description.Targets[1].PositionSLMM, FVector(0.5, 0.12, -0.07), 1.0e-6f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSceneDescriptionNormalSignTest,
	"CGH.SceneDescription.SignedPositionsFollowSLMNormal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSceneDescriptionNormalSignTest::RunTest(const FString& Parameters)
{
	for (EWorldType::Type WorldType : {EWorldType::Game, EWorldType::Editor})
	{
		FCGHTestScene Scene;
		if (!Scene.Initialize(*this, WorldType))
		{
			return false;
		}

		const FVector OriginCm(170.0, -240.0, 90.0);
		Scene.SLM->SetActorLocationAndRotation(OriginCm, FRotator(0.0, 90.0, 0.0));
		Scene.FirstTarget->SetActorLocation(OriginCm + FVector(10.0, 50.0, 20.0));
		Scene.SecondTarget->SetActorLocation(OriginCm + FVector(10.0, -75.0, -20.0));
		Scene.Camera->SetActorLocationAndRotation(OriginCm + FVector(0.0, 25.0, 0.0), FRotator(0.0, 90.0, 0.0));
		Scene.Camera->GetOpticalReference()->SetRelativeLocation(FVector(5.0, 0.0, 0.0));
		Scene.Light->SetActorLocationAndRotation(OriginCm + FVector(0.0, -100.0, 0.0), FRotator(0.0, 90.0, 0.0));
		Scene.Workbench->UpdateSceneDescription();
		if (WorldType == EWorldType::Game && !Scene.BeginPlayInTestWorld())
		{
			Scene.ForwardErrorMessages(this);
			return false;
		}

		const FCGHSceneDescription& Description = Scene.Workbench->SceneDescription;
		if (!TestEqual(TEXT("Both sides of the SLM are exported"), Description.Targets.Num(), 2))
		{
			return false;
		}
		TestEqual(TEXT("Target on the normal side has positive local X"),
			Description.Targets[0].PositionSLMM, FVector(0.5, -0.1, 0.2), 1.0e-6f);
		TestEqual(TEXT("Target opposite the normal has negative local X"),
			Description.Targets[1].PositionSLMM, FVector(-0.75, -0.1, -0.2), 1.0e-6f);
		TestEqual(TEXT("Camera optical position follows the positive normal"),
			Description.Camera.OpticalPositionSLMM.X, 0.3, 1.0e-12);
		TestEqual(TEXT("Light behind the SLM has negative local X"),
			Description.ReconstructionLight.PositionSLMM.X, -1.0, 1.0e-12);
		TestEqual(TEXT("Light propagates along the initial normal"),
			Description.ReconstructionLight.DirectionSLM, FVector::XAxisVector, 1.0e-6f);

		// Keep all optical actors fixed in world space and reverse only the SLM normal.
		// No explicit refresh or tick: the SLM transform observer must update the signs.
		Scene.SLM->SetActorRotation(FRotator(0.0, -90.0, 0.0));
		TestEqual(TEXT("Reversing the normal moves the front target to negative X"),
			Description.Targets[0].PositionSLMM, FVector(-0.5, 0.1, 0.2), 1.0e-6f);
		TestEqual(TEXT("Reversing the normal moves the rear target to positive X"),
			Description.Targets[1].PositionSLMM, FVector(0.75, 0.1, -0.2), 1.0e-6f);
		TestEqual(TEXT("Target helper preserves the same signed frame"),
			Scene.FirstTarget->GetOpticalPositionMeters(Scene.SLM), FVector(-0.5, 0.1, 0.2), 1.0e-6f);
		TestEqual(TEXT("Reversing the normal updates camera position sign"),
			Description.Camera.OpticalPositionSLMM.X, -0.3, 1.0e-12);
		TestEqual(TEXT("Reversing the normal updates light position sign"),
			Description.ReconstructionLight.PositionSLMM.X, 1.0, 1.0e-12);
		TestEqual(TEXT("Light direction becomes opposite the reversed normal"),
			Description.ReconstructionLight.DirectionSLM, -FVector::XAxisVector, 1.0e-6f);
		TestEqual(TEXT("Camera forward becomes opposite the reversed normal"),
			Description.Camera.ForwardDirectionSLM, -FVector::XAxisVector, 1.0e-6f);

		Scene.FirstTarget->SetActorLocation(OriginCm + FVector(15.0, 0.0, -30.0));
		TestEqual(TEXT("Target in the SLM plane has zero normal coordinate"),
			Description.Targets[0].PositionSLMM, FVector(0.0, 0.15, -0.3), 1.0e-6f);

		// Signed depth is the projection onto the normal for arbitrary pitch/yaw/roll.
		Scene.SLM->SetActorRotation(FRotator(30.0, 40.0, 15.0));
		const double ExpectedDepthM = FVector::DotProduct(
			Scene.FirstTarget->GetActorLocation() - OriginCm, Scene.SLM->GetActorForwardVector()) * 0.01;
		TestEqual(TEXT("Tilted SLM exports signed normal projection in meters"),
			Description.Targets[0].PositionSLMM.X, ExpectedDepthM, 1.0e-12);
		Scene.ForwardErrorMessages(this);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSceneDescriptionRuntimeTest,
	"CGH.SceneDescription.RuntimeChangesAndReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSceneDescriptionRuntimeTest::RunTest(const FString& Parameters)
{
	FCGHTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.BeginPlayInTestWorld())
	{
		Scene.ForwardErrorMessages(this);
		return false;
	}
	TestTrue(TEXT("BeginPlay gathers assigned actors without an explicit refresh"), Scene.Workbench->bSceneDescriptionComplete);
	const FCGHSceneDescription& Description = Scene.Workbench->SceneDescription;

	// Direct writes deliberately do not call editor notification or refresh APIs.
	Scene.SLM->Parameters.PixelPitchXUm = 9.25;
	Scene.Light->Parameters.WavelengthNm = 450.25;
	Scene.Camera->Parameters.FocusDistanceMm = 750.5;
	Scene.FirstTarget->Parameters.Amplitude = 0.3125;
	Scene.FirstTarget->Parameters.InitialPhaseRad = -1.75;
	Scene.TickTestWorld();
	TestEqual(TEXT("Tick captures a direct SLM parameter write"), Description.SLM.PixelPitchXM, 9.25e-6, 1.0e-15);
	TestEqual(TEXT("Tick captures a direct light parameter write"), Description.ReconstructionLight.WavelengthM, 4.5025e-7, 1.0e-18);
	TestEqual(TEXT("Tick captures a direct camera parameter write"), Description.Camera.FocusDistanceM, 0.7505, 1.0e-15);
	if (TestEqual(TEXT("Runtime target count"), Description.Targets.Num(), 2))
	{
		TestEqual(TEXT("Tick captures a direct target amplitude write"), Description.Targets[0].Amplitude, 0.3125, 1.0e-15);
		TestEqual(TEXT("Tick captures a direct target phase write"), Description.Targets[0].PhaseRad, -1.75, 1.0e-15);
	}

	Scene.FirstTarget->SetActorLocation(FVector(75.0, -10.0, 20.0));
	Scene.Camera->GetOpticalReference()->SetRelativeLocation(FVector(5.0, 0.0, 0.0));
	TestEqual(TEXT("Optical reference movement automatically refreshes camera pose"), Description.Camera.OpticalPositionSLMM, FVector(0.05, 0.0, 0.0), 1.0e-6f);
	if (Description.Targets.Num() == 2)
	{
		TestEqual(TEXT("Actor movement automatically refreshes target pose"), Description.Targets[0].PositionSLMM, FVector(0.75, -0.1, 0.2), 1.0e-6f);
	}

	Scene.Workbench->Targets = {Scene.SecondTarget, Scene.FirstTarget};
	Scene.SecondTarget->Parameters.Amplitude = 0.8125;
	Scene.TickTestWorld();
	if (TestEqual(TEXT("Reordered target count"), Description.Targets.Num(), 2))
	{
		TestEqual(TEXT("Tick notices reordered target bindings"), Description.Targets[0].Amplitude, 0.8125, 1.0e-15);
	}

	Scene.Workbench->Camera = nullptr;
	Scene.TickTestWorld();
	TestFalse(TEXT("Missing camera marks snapshot incomplete"), Scene.Workbench->bSceneDescriptionComplete);
	TestEqual(TEXT("Missing camera clears stale focal length"), Description.Camera.FocalLengthM, 0.0);
	Scene.Workbench->Camera = Scene.Camera;
	Scene.TickTestWorld();
	TestTrue(TEXT("Restoring a runtime reference restores completeness"), Scene.Workbench->bSceneDescriptionComplete);
	Scene.Camera->SetActorLocation(FVector(10.0, 0.0, 0.0));
	TestEqual(TEXT("Restored camera binding observes transforms"), Description.Camera.OpticalPositionSLMM, FVector(0.15, 0.0, 0.0), 1.0e-6f);

	TestTrue(TEXT("Destroy referenced target"), Scene.FirstTarget->Destroy());
	Scene.TickTestWorld();
	TestFalse(TEXT("Destroyed target marks snapshot incomplete"), Scene.Workbench->bSceneDescriptionComplete);
	if (TestEqual(TEXT("Destroyed target preserves array indices"), Description.Targets.Num(), 2))
	{
		TestEqual(TEXT("Surviving target retains its amplitude"), Description.Targets[0].Amplitude, 0.8125, 1.0e-15);
		TestEqual(TEXT("Destroyed target clears stale amplitude"), Description.Targets[1].Amplitude, 0.0);
		TestEqual(TEXT("Destroyed target clears stale position"), Description.Targets[1].PositionSLMM, FVector::ZeroVector);
	}
	Scene.Workbench->Targets = {Scene.SecondTarget};
	Scene.TickTestWorld();
	TestTrue(TEXT("Removing destroyed reference restores completeness"), Scene.Workbench->bSceneDescriptionComplete);

	Scene.Workbench->SLM = nullptr;
	Scene.TickTestWorld();
	TestFalse(TEXT("Missing SLM marks snapshot incomplete"), Scene.Workbench->bSceneDescriptionComplete);
	TestEqual(TEXT("Missing SLM clears optical coordinate data"), Description.Targets.Num(), 0);
	TestEqual(TEXT("Missing SLM clears stale SLM data"), Description.SLM.ResolutionX, 0);
	TestEqual(TEXT("Missing SLM clears stale light data"), Description.ReconstructionLight.WavelengthM, 0.0);
	TestEqual(TEXT("Missing SLM clears stale camera data"), Description.Camera.FocalLengthM, 0.0);
	Scene.Workbench->SLM = Scene.SLM;
	Scene.TickTestWorld();
	TestTrue(TEXT("Restoring SLM rebuilds the full snapshot"), Scene.Workbench->bSceneDescriptionComplete);
	Scene.Workbench->Targets.Reset();
	Scene.TickTestWorld();
	TestFalse(TEXT("Empty target set is incomplete"), Scene.Workbench->bSceneDescriptionComplete);
	TestEqual(TEXT("Removing all targets clears stale target data"), Description.Targets.Num(), 0);
	Scene.ForwardErrorMessages(this);
	return true;
}

#if WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHSceneDescriptionEditorTest,
	"CGH.SceneDescription.EditorChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHSceneDescriptionEditorTest::RunTest(const FString& Parameters)
{
	FCGHTestScene Scene;
	if (!Scene.Initialize(*this, EWorldType::Editor))
	{
		return false;
	}
	Scene.Workbench->UpdateSceneDescription();
	const FCGHSceneDescription& Description = Scene.Workbench->SceneDescription;

	FProperty* LightParameters = FindFProperty<FProperty>(
		ACGHReconstructionLightActor::StaticClass(), GET_MEMBER_NAME_CHECKED(ACGHReconstructionLightActor, Parameters));
	Scene.Light->PreEditChange(LightParameters);
	Scene.Light->Parameters.WavelengthNm = 488.125;
	FPropertyChangedEvent PropertyEvent(LightParameters, EPropertyChangeType::ValueSet);
	Scene.Light->PostEditChangeProperty(PropertyEvent);
	TestEqual(TEXT("Editor property notification immediately updates the snapshot"), Description.ReconstructionLight.WavelengthM, 4.88125e-7, 1.0e-18);

	Scene.FirstTarget->SetActorLocation(FVector(50.0, 10.0, -5.0));
	Scene.FirstTarget->PostEditMove(true);
	Scene.Camera->GetOpticalReference()->SetRelativeLocation(FVector(2.0, 3.0, 4.0));
	if (TestEqual(TEXT("Editor target count"), Description.Targets.Num(), 2))
	{
		TestEqual(TEXT("Editor movement refreshes SLM-local target pose"), Description.Targets[0].PositionSLMM, FVector(0.5, 0.1, -0.05), 1.0e-6f);
	}
	TestEqual(TEXT("Editor component movement refreshes optical camera pose"), Description.Camera.OpticalPositionSLMM, FVector(0.02, 0.03, 0.04), 1.0e-6f);

	Scene.FirstTarget->Parameters.InitialPhaseRad = 0.875;
	Scene.FirstTarget->PostTransacted(FTransactionObjectEvent());
	if (Description.Targets.Num() == 2)
	{
		TestEqual(TEXT("Transaction notification refreshes edited target phase"), Description.Targets[0].PhaseRad, 0.875, 1.0e-15);
	}

	// Editor scripting may change public values without property notifications.
	Scene.SLM->Parameters.ResolutionX = 3072;
	Scene.GetTestWorld()->Tick(LEVELTICK_ViewportsOnly, 0.01f);
	TestEqual(TEXT("Viewport-only editor tick catches direct writes"), Description.SLM.ResolutionX, 3072);
	Scene.ForwardErrorMessages(this);
	return true;
}

#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
