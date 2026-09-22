#include "CGH/Actors/CGHCameraActor.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "CineCameraComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "ScopedTransaction.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UnrealType.h"

namespace
{
	void EditLens(ACGHCameraActor* Camera, FName Name, float Value, EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet)
	{
		UCineCameraComponent* Cine = Camera->FindComponentByClass<UCineCameraComponent>();
		const bool bFocus = Name == GET_MEMBER_NAME_CHECKED(FCameraFocusSettings, ManualFocusDistance);
		FProperty* Member = FindFProperty<FProperty>(UCineCameraComponent::StaticClass(), bFocus
			? GET_MEMBER_NAME_CHECKED(UCineCameraComponent, FocusSettings) : Name);
		FProperty* Leaf = bFocus ? FindFProperty<FProperty>(FCameraFocusSettings::StaticStruct(), Name) : Member;
		FEditPropertyChain Chain;
		Chain.AddTail(Member);
		if (bFocus) { Chain.AddTail(Leaf); }
		Chain.SetActiveMemberPropertyNode(Member);
		Chain.SetActivePropertyNode(Leaf);
		static_cast<UObject*>(Cine)->PreEditChange(Chain);
		if (bFocus) { Cine->FocusSettings.ManualFocusDistance = Value; }
		else if (Name == GET_MEMBER_NAME_CHECKED(UCineCameraComponent, CurrentFocalLength)) { Cine->CurrentFocalLength = Value; }
		else { Cine->CurrentAperture = Value; }
		FPropertyChangedEvent Event(Leaf, ChangeType);
		Event.SetActiveMemberProperty(Member);
		FPropertyChangedChainEvent ChainEvent(Chain, Event);
		Cine->PostEditChangeChainProperty(ChainEvent);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraLensEditorTest,
	"CGH.CameraField.EditorLensControlsPersistAndReachOptics", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHCameraLensEditorTest::RunTest(const FString& InParameters)
{
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Editor)) { Scene.ForwardErrorMessages(this); return false; }
	UClass* BlueprintClass = LoadClass<ACGHCameraActor>(nullptr, TEXT("/Game/CGHSim/Blueprints/BP_CGHCamera.BP_CGHCamera_C"));
	if (!TestNotNull(TEXT("Existing camera Blueprint loads without migration"), BlueprintClass)) return false;
	ACGHWorkbenchActor* Workbench = Scene.GetTestWorld()->SpawnActor<ACGHWorkbenchActor>();
	Workbench->SLM = Scene.GetTestWorld()->SpawnActor<ACGHSLMActor>();
	for (UClass* CameraClass : {ACGHCameraActor::StaticClass(), BlueprintClass})
	{
		ACGHCameraActor* Camera = Scene.GetTestWorld()->SpawnActor<ACGHCameraActor>(CameraClass);
		if (!TestNotNull(TEXT("Native or existing Blueprint camera spawns"), Camera)) return false;
		Workbench->Camera = Camera;
		Camera->Parameters.FocalLengthMm = 50.123456789;
		Camera->Parameters.FNumber = 4.123456789;
		Camera->Parameters.FocusDistanceMm = 500.123456789;
		Camera->RefreshVisualization();
		UCineCameraComponent* Cine = Camera->FindComponentByClass<UCineCameraComponent>();
		TestTrue(TEXT("Preview focal range permits changes in either direction"), Cine->LensSettings.MinFocalLength < 10.0f && Cine->LensSettings.MaxFocalLength > 200.0f);
		TestTrue(TEXT("Preview aperture range permits changes in either direction"), Cine->LensSettings.MinFStop < 1.0f && Cine->LensSettings.MaxFStop > 32.0f);
		EditLens(Camera, GET_MEMBER_NAME_CHECKED(UCineCameraComponent, CurrentFocalLength), 80.125f, EPropertyChangeType::Interactive);
		TestEqual(TEXT("Interactive focal edit updates optical focal length"), Camera->Parameters.FocalLengthMm, 80.125);
		TestEqual(TEXT("Focal edit preserves unedited aperture precision"), Camera->Parameters.FNumber, 4.123456789);
		TestEqual(TEXT("Focal edit preserves unedited focus precision"), Camera->Parameters.FocusDistanceMm, 500.123456789);
		EditLens(Camera, GET_MEMBER_NAME_CHECKED(UCineCameraComponent, CurrentFocalLength), 80.125f);
		EditLens(Camera, GET_MEMBER_NAME_CHECKED(UCineCameraComponent, CurrentAperture), 11.25f);
		EditLens(Camera, GET_MEMBER_NAME_CHECKED(FCameraFocusSettings, ManualFocusDistance), 145.75f);
		Camera->RefreshVisualization();
		Camera->RerunConstructionScripts();
		Cine = Camera->FindComponentByClass<UCineCameraComponent>();
		TestEqual(TEXT("Focal length survives construction and preview refresh"), Camera->Parameters.FocalLengthMm, 80.125);
		TestEqual(TEXT("Aperture survives construction and preview refresh"), Camera->Parameters.FNumber, 11.25);
		TestEqual(TEXT("Focus converts editor centimeters to optical millimeters"), Camera->Parameters.FocusDistanceMm, 1457.5);
		TestEqual(TEXT("Preview retains edited focal length"), Cine->CurrentFocalLength, 80.125f);
		TestEqual(TEXT("Preview retains edited aperture"), Cine->CurrentAperture, 11.25f);
		TestEqual(TEXT("Preview retains edited focus distance"), Cine->FocusSettings.ManualFocusDistance, 145.75f);
		Workbench->UpdateSceneDescription();
		TestEqual(TEXT("Compute snapshot receives edited focal length in meters"), Workbench->SceneDescription.Camera.FocalLengthM, 0.080125, 1.e-15);
		TestEqual(TEXT("Compute snapshot receives edited aperture"), Workbench->SceneDescription.Camera.FNumber, 11.25);
		TestEqual(TEXT("Compute snapshot receives edited focus in meters"), Workbench->SceneDescription.Camera.FocusDistanceM, 1.4575, 1.e-15);

		FProperty* ParametersProperty = FindFProperty<FProperty>(ACGHCameraActor::StaticClass(), GET_MEMBER_NAME_CHECKED(ACGHCameraActor, Parameters));
		Camera->PreEditChange(ParametersProperty);
		Camera->Parameters.FocalLengthMm = 35.123456789;
		Camera->Parameters.FNumber = 2.123456789;
		Camera->Parameters.FocusDistanceMm = 750.123456789;
		FPropertyChangedEvent ParametersEvent(ParametersProperty, EPropertyChangeType::ValueSet);
		Camera->PostEditChangeProperty(ParametersEvent);
		Cine = Camera->FindComponentByClass<UCineCameraComponent>();
		TestEqual(TEXT("Actor optical edits still drive the component"), Cine->CurrentFocalLength, float(35.123456789));
		FPropertyChangedEvent UnrelatedEvent(FindFProperty<FProperty>(UCineCameraComponent::StaticClass(), TEXT("bConstrainAspectRatio")), EPropertyChangeType::ValueSet);
		static_cast<UObject*>(Cine)->PostEditChangeProperty(UnrelatedEvent);
		FPropertyChangedEvent UndoEvent(nullptr);
		static_cast<UObject*>(Cine)->PostEditChangeProperty(UndoEvent);
		TestEqual(TEXT("Unrelated and undo callbacks preserve double focal precision"), Camera->Parameters.FocalLengthMm, 35.123456789);
		TestEqual(TEXT("Unrelated and undo callbacks preserve double aperture precision"), Camera->Parameters.FNumber, 2.123456789);
		TestEqual(TEXT("Unrelated and undo callbacks preserve double focus precision"), Camera->Parameters.FocusDistanceMm, 750.123456789);
	}
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHCameraLensUndoTest,
	"CGH.CameraField.EditorLensControlsUndoRedo", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHCameraLensUndoTest::RunTest(const FString& InParameters)
{
	// Exercise the real transaction system without creating a desktop toast in headless runs.
	struct FSuppressTransactionToast
	{
		bool bPrevious = GEditor->bSquelchTransactionNotification;
		FSuppressTransactionToast() { GEditor->bSquelchTransactionNotification = true; }
		~FSuppressTransactionToast() { GEditor->bSquelchTransactionNotification = bPrevious; }
	} SuppressToast;
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Editor)) { Scene.ForwardErrorMessages(this); return false; }
	ACGHCameraActor* Camera = Scene.GetTestWorld()->SpawnActor<ACGHCameraActor>();
	Camera->SetFlags(RF_Transactional);
	Camera->FindComponentByClass<UCineCameraComponent>()->SetFlags(RF_Transactional);
	Camera->Parameters.FocalLengthMm = 50.123456789;
	Camera->RefreshVisualization();
	{
		FScopedTransaction Transaction(NSLOCTEXT("CGHTests", "CameraLensEdit", "CGH test camera lens edit"));
		EditLens(Camera, GET_MEMBER_NAME_CHECKED(UCineCameraComponent, CurrentFocalLength), 100.25f);
	}
	TestEqual(TEXT("Lens transaction updates authoritative optics"), Camera->Parameters.FocalLengthMm, 100.25);
	TestTrue(TEXT("Component lens edit can be undone"), GEditor->UndoTransaction());
	TestEqual(TEXT("Undo restores exact double optical value"), Camera->Parameters.FocalLengthMm, 50.123456789);
	TestEqual(TEXT("Undo restores matching geometric preview"), Camera->FindComponentByClass<UCineCameraComponent>()->CurrentFocalLength, float(50.123456789));
	TestTrue(TEXT("Component lens edit can be redone"), GEditor->RedoTransaction());
	TestEqual(TEXT("Redo restores edited optical value"), Camera->Parameters.FocalLengthMm, 100.25);
	TestEqual(TEXT("Redo restores matching geometric preview"), Camera->FindComponentByClass<UCineCameraComponent>()->CurrentFocalLength, 100.25f);
	Scene.ForwardErrorMessages(this);
	return true;
}
#endif
