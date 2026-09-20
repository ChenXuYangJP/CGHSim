#include "CGH/Actors/CGHTargetActor.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CGH/Actors/CGHSLMActor.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "StaticMeshCompiler.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UnrealType.h"

namespace
{
	void CollectTargetDetailProperties(const TArray<TSharedRef<IDetailTreeNode>>& Nodes,
		TMap<FName, TSharedPtr<IPropertyHandle>>& OutProperties)
	{
		for (const TSharedRef<IDetailTreeNode>& Node : Nodes)
		{
			const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
			const FProperty* Property = Handle.IsValid() ? Handle->GetProperty() : nullptr;
			if (Property && Property->GetOwnerStruct() == ACGHTargetActor::StaticClass())
			{
				OutProperties.Add(Property->GetFName(), Handle);
				// Detect bulk-resource exposure without walking thousands of element rows
				// if a future change accidentally makes these properties visible again.
				if (Property->GetFName() == TEXT("MeshGeometryResource") || Property->GetFName() == TEXT("PointCloudResource"))
				{
					continue;
				}
			}
			TArray<TSharedRef<IDetailTreeNode>> Children;
			Node->GetChildren(Children, true);
			CollectTargetDetailProperties(Children, OutProperties);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHTargetSelectionDetailsTest,
	"CGH.TargetResources.EditorSelectionKeepsResourcesAndCompactDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHTargetSelectionDetailsTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("Editor selection is available"), GEditor))
	{
		return false;
	}
	FTestWorldWrapper Scene;
	if (!Scene.CreateTestWorld(EWorldType::Editor))
	{
		Scene.ForwardErrorMessages(this);
		return false;
	}
	ACGHTargetActor* Target = Scene.GetTestWorld()->SpawnActor<ACGHTargetActor>();
	ACGHSLMActor* SLM = Scene.GetTestWorld()->SpawnActor<ACGHSLMActor>();
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Target || !SLM || !Cube)
	{
		AddError(TEXT("Could not create the isolated editor selection fixture."));
		return false;
	}
	UStaticMesh* Meshes[] = {Cube};
	FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
	Target->SetActorLocation(FVector(200.0, 0.0, 0.0));
	Target->SLM = SLM;
	Target->GeometryMesh->SetStaticMesh(Cube);
	Target->SliceCount = 8;
	Target->PointSpacingMm = 10.0;
	Target->Parameters.TargetType = ECGHTargetType::Mesh;
	Target->UpdateTargetResources();
	if (!TestTrue(TEXT("Selection fixture has valid mesh resources"), Target->bResourcesValid)
		|| !TestTrue(TEXT("Selection fixture contains thousands of points"), Target->PointCloudResource.Points.Num() > 1000))
	{
		AddError(Target->ResourceError);
		return false;
	}
	const uint64 Revision = Target->TargetDescription.Revision;
	const FCGHObjectPoint* PointStorage = Target->PointCloudResource.Points.GetData();
	const FVector* VertexStorage = Target->MeshGeometryResource.VerticesM.GetData();

	// Only this temporary actor is selected/deselected; existing editor selections
	// and the user's map remain untouched. Release it before destroying the world.
	ON_SCOPE_EXIT
	{
		GEditor->SelectActor(Target, false, true, true);
	};
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	TSharedRef<IPropertyRowGenerator> Rows = PropertyEditor.CreatePropertyRowGenerator(FPropertyRowGeneratorArgs());
	Rows->OnRowsRefreshed().AddLambda([]() {});

	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		const double SelectionStart = FPlatformTime::Seconds();
		GEditor->SelectActor(Target, true, true, true);
		const double SelectionMs = (FPlatformTime::Seconds() - SelectionStart) * 1000.0;
		TestTrue(TEXT("Real editor selection includes the target"), GEditor->GetSelectedActors()->IsSelected(Target));

		// This is the same property-tree generation used by the Details UI. Merely
		// selecting an actor in a commandlet would not exercise array-row creation.
		const double DetailsStart = FPlatformTime::Seconds();
		Rows->SetObjects({Target});
		const double DetailsMs = (FPlatformTime::Seconds() - DetailsStart) * 1000.0;
		TMap<FName, TSharedPtr<IPropertyHandle>> VisibleProperties;
		CollectTargetDetailProperties(Rows->GetRootTreeNodes(), VisibleProperties);
		TestFalse(TEXT("Details does not create the full mesh-resource tree"), VisibleProperties.Contains(TEXT("MeshGeometryResource")));
		TestFalse(TEXT("Details does not create a row for every sampled point"), VisibleProperties.Contains(TEXT("PointCloudResource")));
		TestTrue(TEXT("Details retains the target description and resource revision"), VisibleProperties.Contains(TEXT("TargetDescription")));

		const auto CheckCount = [this, &VisibleProperties](FName Name, int32 Expected)
		{
			const TSharedPtr<IPropertyHandle>* Handle = VisibleProperties.Find(Name);
			if (TestTrue(*FString::Printf(TEXT("Details displays %s"), *Name.ToString()), Handle && Handle->IsValid()))
			{
				int32 Actual = INDEX_NONE;
				TestTrue(TEXT("Details summary can be read"), (*Handle)->GetValue(Actual) == FPropertyAccess::Success);
				TestEqual(*FString::Printf(TEXT("%s matches the cached resource"), *Name.ToString()), Actual, Expected);
			}
		};
		CheckCount(TEXT("MeshVertexCount"), Target->MeshGeometryResource.VerticesM.Num());
		CheckCount(TEXT("MeshTriangleCount"), Target->MeshGeometryResource.Indices.Num() / 3);
		CheckCount(TEXT("PointCount"), Target->PointCloudResource.Points.Num());

		TestEqual(TEXT("Selection and Details generation do not rebuild the target"), Target->TargetDescription.Revision, Revision);
		TestTrue(TEXT("Selection keeps the existing point buffer"), Target->PointCloudResource.Points.GetData() == PointStorage);
		TestTrue(TEXT("Selection keeps the existing mesh buffer"), Target->MeshGeometryResource.VerticesM.GetData() == VertexStorage);
		AddInfo(FString::Printf(TEXT("Selection pass %d: selection %.3f ms, Details %.3f ms, %d points, revision %llu"),
			Pass + 1, SelectionMs, DetailsMs, Target->PointCloudResource.Points.Num(), Revision));

		Rows->SetObjects({});
		GEditor->SelectActor(Target, false, true, true);
		TestFalse(TEXT("Real editor deselection removes the target"), GEditor->GetSelectedActors()->IsSelected(Target));
		TestEqual(TEXT("Deselecting does not rebuild the target"), Target->TargetDescription.Revision, Revision);
	}
	Scene.ForwardErrorMessages(this);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
