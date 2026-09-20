#include "CGH/Actors/CGHTargetActor.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CGH/Actors/CGHCameraActor.h"
#include "CGH/Actors/CGHReconstructionLightActor.h"
#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshResources.h"
#include "Tests/AutomationCommon.h"

#if WITH_EDITOR
#include "Misc/TransactionObjectEvent.h"
#include "StaticMeshCompiler.h"
#include "UObject/UnrealType.h"
#endif

namespace
{
	void FinishMeshFixtureCompilation(UStaticMesh* Mesh)
	{
#if WITH_EDITOR
		UStaticMesh* Meshes[] = {Mesh};
		FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
#endif
	}

	/** Isolated fixtures never load or save the user's workbench map. */
	struct FCGHTargetTestScene : FTestWorldWrapper
	{
		ACGHSLMActor* SLM = nullptr;
		ACGHTargetActor* Target = nullptr;
		UStaticMesh* Cube = nullptr;

		bool Initialize(FAutomationTestBase& Test, EWorldType::Type WorldType = EWorldType::Game)
		{
			if (!CreateTestWorld(WorldType))
			{
				ForwardErrorMessages(&Test);
				return false;
			}
			SLM = TestWorld->SpawnActor<ACGHSLMActor>();
			Target = TestWorld->SpawnActor<ACGHTargetActor>();
			Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (!SLM || !Target || !Cube)
			{
				Test.AddError(TEXT("Could not create the CGH target test fixture."));
				return false;
			}
			FinishMeshFixtureCompilation(Cube);
			Target->SetActorLocation(FVector(200.0, 0.0, 0.0));
			Target->SLM = SLM;
			Target->GeometryMesh->SetStaticMesh(Cube);
			Target->SliceCount = 4;
			Target->PointSpacingMm = 50.0;
			Target->Parameters.TargetType = ECGHTargetType::Mesh;
			return true;
		}
	};

	FVector MeshExtentMeters(const FCGHMeshGeometryResource& Mesh)
	{
		FBox Bounds(ForceInit);
		for (const FVector3d& Vertex : Mesh.VerticesM)
		{
			Bounds += Vertex;
		}
		return Bounds.GetSize();
	}

	void CheckResourceVersions(FAutomationTestBase& Test, const ACGHTargetActor& Target)
	{
		Test.TestTrue(TEXT("Target identity is nonzero"), Target.TargetDescription.ResourceId != 0);
		Test.TestTrue(TEXT("Initialized revision is nonzero"), Target.TargetDescription.Revision != 0);
		Test.TestEqual(TEXT("Mesh refers to the target identity"),
			Target.MeshGeometryResource.ResourceId, Target.TargetDescription.ResourceId);
		Test.TestEqual(TEXT("Cloud refers to the target identity"),
			Target.PointCloudResource.ResourceId, Target.TargetDescription.ResourceId);
		Test.TestEqual(TEXT("Mesh revision agrees with description"),
			Target.MeshGeometryResource.Revision, Target.TargetDescription.Revision);
		Test.TestEqual(TEXT("Cloud revision agrees with description"),
			Target.PointCloudResource.Revision, Target.TargetDescription.Revision);
		Test.TestEqual(TEXT("Legacy target identity remains synchronized"),
			Target.TargetDescription.TargetId, Target.TargetDescription.ResourceId);
		Test.TestEqual(TEXT("Geometry reference remains synchronized"),
			Target.TargetDescription.GeometryResourceId, Target.TargetDescription.ResourceId);
		Test.TestEqual(TEXT("Geometry reference revision remains synchronized"),
			Target.TargetDescription.GeometryRevision, Target.TargetDescription.Revision);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHTargetResourcesRuntimeTest,
	"CGH.TargetResources.RuntimeUpdatesAndIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHTargetResourcesRuntimeTest::RunTest(const FString& Parameters)
{
	FCGHTargetTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.BeginPlayInTestWorld())
	{
		Scene.ForwardErrorMessages(this);
		return false;
	}
	ACGHTargetActor& Target = *Scene.Target;
	TestTrue(TEXT("BeginPlay generates mesh resources"), Target.bResourcesValid);
	TestTrue(TEXT("BeginPlay creates vertices"), !Target.MeshGeometryResource.VerticesM.IsEmpty());
	TestTrue(TEXT("BeginPlay creates contour points"), !Target.PointCloudResource.Points.IsEmpty());
	CheckResourceVersions(*this, Target);
	const uint64 ResourceId = Target.TargetDescription.ResourceId;
	uint64 Revision = Target.TargetDescription.Revision;
	const int32 InitialPointCount = Target.PointCloudResource.Points.Num();
	TestEqual(TEXT("Cube dimensions convert centimeters to meters"),
		MeshExtentMeters(Target.MeshGeometryResource), FVector::OneVector, 1.0e-6f);
	TestEqual(TEXT("Target description uses SLM-relative meters"),
		Target.TargetDescription.PositionSLMM, FVector(2.0, 0.0, 0.0), 1.0e-6f);

	Target.UpdateTargetResources();
	Scene.TickTestWorld();
	TestEqual(TEXT("Unchanged refresh and tick retain the revision"), Target.TargetDescription.Revision, Revision);

	Target.SetActorScale3D(FVector(2.0, 3.0, 4.0));
	Scene.TickTestWorld();
	TestTrue(TEXT("Actor scale advances revision"), Target.TargetDescription.Revision > Revision);
	TestEqual(TEXT("Actor scale changes the physical mesh dimensions"),
		MeshExtentMeters(Target.MeshGeometryResource), FVector(2.0, 3.0, 4.0), 1.0e-6f);
	CheckResourceVersions(*this, Target);
	Revision = Target.TargetDescription.Revision;

	Target.GeometryMesh->SetRelativeScale3D(FVector(0.5, 1.0, 1.0));
	Scene.TickTestWorld();
	TestTrue(TEXT("Mesh component transform advances revision"), Target.TargetDescription.Revision > Revision);
	TestEqual(TEXT("Component scale is included in physical mesh dimensions"),
		MeshExtentMeters(Target.MeshGeometryResource), FVector(1.0, 3.0, 4.0), 1.0e-6f);
	Revision = Target.TargetDescription.Revision;

	Target.SliceCount = 2;
	Target.PointSpacingMm = 100.0;
	Scene.TickTestWorld();
	TestTrue(TEXT("Direct sampling-parameter writes advance revision"), Target.TargetDescription.Revision > Revision);
	TestTrue(TEXT("New sample parameters produce a nonempty cloud"), !Target.PointCloudResource.Points.IsEmpty());
	Revision = Target.TargetDescription.Revision;

	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!TestNotNull(TEXT("Sphere mesh fixture is available"), Sphere))
	{
		return false;
	}
	FinishMeshFixtureCompilation(Sphere);
	const int32 CubeVertexCount = Target.MeshGeometryResource.VerticesM.Num();
	Target.GeometryMesh->SetStaticMesh(Sphere);
	Scene.TickTestWorld();
	TestTrue(TEXT("Runtime mesh swap advances revision"), Target.TargetDescription.Revision > Revision);
	TestTrue(TEXT("Runtime mesh swap replaces geometry"), Target.MeshGeometryResource.VerticesM.Num() != CubeVertexCount);
	TestTrue(TEXT("Runtime mesh swap resamples points"), !Target.PointCloudResource.Points.IsEmpty());
	Revision = Target.TargetDescription.Revision;

	Scene.SLM->SetActorLocation(FVector(0.0, 100.0, 0.0));
	Scene.TickTestWorld();
	TestTrue(TEXT("Moving the reference SLM advances revision"), Target.TargetDescription.Revision > Revision);
	TestEqual(TEXT("SLM movement updates optical coordinates"),
		Target.TargetDescription.PositionSLMM, FVector(2.0, -1.0, 0.0), 1.0e-6f);
	Revision = Target.TargetDescription.Revision;

	Target.Parameters.Amplitude = 0.375;
	Target.Parameters.InitialPhaseRad = 0.625;
	Scene.TickTestWorld();
	TestTrue(TEXT("Optical parameter changes advance revision"), Target.TargetDescription.Revision > Revision);
	TestEqual(TEXT("Description exports changed amplitude"), Target.TargetDescription.Amplitude, 0.375);
	TestEqual(TEXT("Description exports changed phase"), Target.TargetDescription.PhaseRad, 0.625);
	TestEqual(TEXT("One actor keeps its identity throughout changes"), Target.TargetDescription.ResourceId, ResourceId);
	CheckResourceVersions(*this, Target);

	ACGHTargetActor* Other = Scene.GetTestWorld()->SpawnActor<ACGHTargetActor>();
	Other->UpdateTargetResources();
	TestTrue(TEXT("Different actors have different resource identities"), Other->TargetDescription.ResourceId != ResourceId);
	TestTrue(TEXT("Initial fixture sampled more than one point"), InitialPointCount > 1);
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHTargetResourcesMirroredTransformTest,
	"CGH.TargetResources.MirroredMeshTransformAndNormals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHTargetResourcesMirroredTransformTest::RunTest(const FString& Parameters)
{
	FCGHTargetTestScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	// Smooth sphere normals have multiple nonzero components, so nonuniform scaling
	// distinguishes a correct normal transform from transforming normals like positions.
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!TestNotNull(TEXT("Sphere normal fixture is available"), Sphere))
	{
		return false;
	}
	FinishMeshFixtureCompilation(Sphere);
	ACGHTargetActor& Target = *Scene.Target;
	Target.SetActorTransform(FTransform(FRotator(23.0, -41.0, 12.0),
		FVector(230.0, -65.0, 84.0), FVector(-1.75, 0.6, 2.4)));
	Target.GeometryMesh->SetRelativeTransform(FTransform(FRotator(-17.0, 32.0, 9.0),
		FVector(15.0, -8.0, 11.0), FVector(0.7, 1.8, 0.9)));
	Target.GeometryMesh->SetStaticMesh(Sphere);
	Scene.SLM->SetActorLocationAndRotation(FVector(-40.0, 75.0, -18.0), FRotator(11.0, 73.0, -8.0));
	Target.UpdateTargetResources();
	if (!TestTrue(TEXT("Mirrored, rotated and offset mesh produces valid resources"), Target.bResourcesValid))
	{
		AddError(Target.ResourceError);
		return false;
	}
	const FTransform MeshTransform = Target.GeometryMesh->GetComponentTransform();
	const FTransform SLMTransform = Scene.SLM->GetActorTransform();
	TestTrue(TEXT("Fixture includes an odd number of mirrored axes"), MeshTransform.ToMatrixWithScale().Determinant() < 0.0);
	const FStaticMeshLODResources& LOD = Sphere->GetRenderData()->LODResources[0];
	const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& Attributes = LOD.VertexBuffers.StaticMeshVertexBuffer;
	const FIndexArrayView SourceIndices = LOD.IndexBuffer.GetArrayView();
	const FCGHMeshGeometryResource& Geometry = Target.MeshGeometryResource;
	const FCGHTargetDescription& Description = Target.TargetDescription;
	if (!TestEqual(TEXT("Mirrored resource retains all source vertices"), Geometry.VerticesM.Num(), int32(Positions.GetNumVertices()))
		|| !TestEqual(TEXT("Mirrored resource retains all vertex normals"), Geometry.Normals.Num(), Geometry.VerticesM.Num())
		|| !TestEqual(TEXT("Mirrored resource retains all triangles"), Geometry.Indices.Num(), SourceIndices.Num()))
	{
		return false;
	}

	double MaxWorldPositionErrorCm = 0.0;
	double MaxNormalTangentDot = 0.0;
	double MaxNormalLengthError = 0.0;
	bool bNormalsKeepOutwardDirection = true;
	for (int32 Index = 0; Index < Geometry.VerticesM.Num(); ++Index)
	{
		const FVector PositionSLMM = Description.PositionSLMM + Description.RotationSLM.RotateVector(Geometry.VerticesM[Index]);
		const FVector ReconstructedWorldCm = SLMTransform.TransformPositionNoScale(PositionSLMM * 100.0);
		const FVector ExpectedWorldCm = MeshTransform.TransformPosition(FVector(Positions.VertexPosition(Index)));
		MaxWorldPositionErrorCm = FMath::Max(MaxWorldPositionErrorCm, FVector::Distance(ReconstructedWorldCm, ExpectedWorldCm));

		const FVector NormalSLM = Description.RotationSLM.RotateVector(FVector(Geometry.Normals[Index]));
		const FVector WorldNormal = SLMTransform.TransformVectorNoScale(NormalSLM);
		const FVector SourceNormal = FVector(Attributes.VertexTangentZ(Index)).GetSafeNormal();
		FVector TangentA, TangentB;
		SourceNormal.FindBestAxisVectors(TangentA, TangentB);
		// A normal must remain perpendicular to both transformed surface tangents.
		// This tests inverse-transpose behavior without reusing the exporter formula.
		MaxNormalTangentDot = FMath::Max(MaxNormalTangentDot,
			FMath::Abs(FVector::DotProduct(WorldNormal, MeshTransform.TransformVector(TangentA).GetSafeNormal())));
		MaxNormalTangentDot = FMath::Max(MaxNormalTangentDot,
			FMath::Abs(FVector::DotProduct(WorldNormal, MeshTransform.TransformVector(TangentB).GetSafeNormal())));
		MaxNormalLengthError = FMath::Max(MaxNormalLengthError, FMath::Abs(WorldNormal.Size() - 1.0));
		bNormalsKeepOutwardDirection &= FVector::DotProduct(WorldNormal, MeshTransform.TransformVector(SourceNormal)) > 0.0;
	}
	TestTrue(TEXT("SLM description reconstructs rendered mesh vertex positions within a micrometer"), MaxWorldPositionErrorCm < 1.0e-4);
	TestTrue(TEXT("Nonuniform mirrored scale preserves perpendicular surface normals"), MaxNormalTangentDot < 1.0e-5);
	TestTrue(TEXT("Exported normals retain unit length"), MaxNormalLengthError < 1.0e-6);
	TestTrue(TEXT("Mirrored normals retain their outward direction"), bNormalsKeepOutwardDirection);

	bool bWindingPreservesOrientation = true;
	int32 CheckedTriangleCount = 0;
	for (int32 Index = 0; Index < SourceIndices.Num(); Index += 3)
	{
		const uint32 SourceA = SourceIndices[Index];
		const uint32 SourceB = SourceIndices[Index + 1];
		const uint32 SourceC = SourceIndices[Index + 2];
		const FVector SourceCross = FVector::CrossProduct(
			FVector(Positions.VertexPosition(SourceB) - Positions.VertexPosition(SourceA)),
			FVector(Positions.VertexPosition(SourceC) - Positions.VertexPosition(SourceA)));
		const FVector SourceNormal = FVector(Attributes.VertexTangentZ(SourceA)
			+ Attributes.VertexTangentZ(SourceB) + Attributes.VertexTangentZ(SourceC));
		const uint32 A = Geometry.Indices[Index], B = Geometry.Indices[Index + 1], C = Geometry.Indices[Index + 2];
		const FVector ExportedCross = FVector::CrossProduct(Geometry.VerticesM[B] - Geometry.VerticesM[A],
			Geometry.VerticesM[C] - Geometry.VerticesM[A]);
		const FVector ExportedNormal = FVector(Geometry.Normals[A] + Geometry.Normals[B] + Geometry.Normals[C]);
		const double SourceOrientation = FVector::DotProduct(SourceCross, SourceNormal);
		if (FMath::Abs(SourceOrientation) > 1.0e-8)
		{
			bWindingPreservesOrientation &= SourceOrientation * FVector::DotProduct(ExportedCross, ExportedNormal) > 0.0;
			++CheckedTriangleCount;
		}
	}
	TestTrue(TEXT("Fixture checks nondegenerate triangles"), CheckedTriangleCount > 0);
	TestTrue(TEXT("Mirrored triangle winding preserves source orientation relative to normals"), bWindingPreservesOrientation);
	CheckResourceVersions(*this, Target);
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHTargetResourcesInvalidAndPointTest,
	"CGH.TargetResources.InvalidResourcesAndPointTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHTargetResourcesInvalidAndPointTest::RunTest(const FString& Parameters)
{
	FCGHTargetTestScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	ACGHTargetActor& Target = *Scene.Target;
	Target.UpdateTargetResources();
	const uint64 ResourceId = Target.TargetDescription.ResourceId;
	uint64 Revision = Target.TargetDescription.Revision;
	Target.GeometryMesh->SetStaticMesh(nullptr);
	Target.UpdateTargetResources();
	TestFalse(TEXT("Removing the mesh invalidates resources"), Target.bResourcesValid);
	TestTrue(TEXT("Invalid resources explain the missing mesh"), Target.ResourceError.Contains(TEXT("Assign a static mesh")));
	TestTrue(TEXT("Removing the mesh advances revision"), Target.TargetDescription.Revision > Revision);
	TestTrue(TEXT("Invalid mesh clears stale geometry"), Target.MeshGeometryResource.VerticesM.IsEmpty());
	TestTrue(TEXT("Invalid mesh clears stale sampled points"), Target.PointCloudResource.Points.IsEmpty());

	Target.GeometryMesh->SetStaticMesh(Scene.Cube);
	Target.MaxPointCount = 1;
	Target.UpdateTargetResources();
	TestFalse(TEXT("Exceeding the configured point budget invalidates resources"), Target.bResourcesValid);
	TestTrue(TEXT("Point budget failure clears partial point cloud"), Target.PointCloudResource.Points.IsEmpty());

	Revision = Target.TargetDescription.Revision;
	Target.Parameters.TargetType = ECGHTargetType::Point;
	Target.GeometryMesh->SetStaticMesh(nullptr);
	Target.SliceCount = 0;
	Target.PointSpacingMm = -1.0;
	Target.UpdateTargetResources();
	TestTrue(TEXT("Point target ignores missing geometry and invalid sampling settings"), Target.bResourcesValid);
	TestTrue(TEXT("Switching to Point advances revision"), Target.TargetDescription.Revision > Revision);
	TestEqual(TEXT("Point target retains the actor identity"), Target.TargetDescription.ResourceId, ResourceId);
	TestTrue(TEXT("Point target does not retain mesh vertices"), Target.MeshGeometryResource.VerticesM.IsEmpty());
	TestTrue(TEXT("Point target does not retain sampled points"), Target.PointCloudResource.Points.IsEmpty());
	TestEqual(TEXT("Point target clears the unused geometry reference"), Target.TargetDescription.GeometryResourceId, uint64(0));

	ACGHTargetActor* DefaultPoint = Scene.GetTestWorld()->SpawnActor<ACGHTargetActor>();
	if (!TestNotNull(TEXT("Default point target fixture is available"), DefaultPoint))
	{
		return false;
	}
	DefaultPoint->SLM = Scene.SLM;
	DefaultPoint->UpdateTargetResources();
	const uint64 PointRevision = DefaultPoint->TargetDescription.Revision;
	DefaultPoint->GeometryMesh->SetStaticMesh(Scene.Cube);
	DefaultPoint->SliceCount = -12;
	DefaultPoint->PointSpacingMm = -1.0;
	DefaultPoint->MaxPointCount = -3;
	DefaultPoint->UpdateTargetResources();
	TestTrue(TEXT("Default point ignores invalid mesh sampling settings"), DefaultPoint->bResourcesValid);
	TestEqual(TEXT("Changing only ignored mesh settings leaves the point revision unchanged"),
		DefaultPoint->TargetDescription.Revision, PointRevision);
	TestTrue(TEXT("Default point never generates mesh geometry"), DefaultPoint->MeshGeometryResource.VerticesM.IsEmpty());
	TestTrue(TEXT("Default point never generates sampled points"), DefaultPoint->PointCloudResource.Points.IsEmpty());
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHTargetResourcesDestroyedSLMTest,
	"CGH.TargetResources.DestroyedIdentitySLM",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHTargetResourcesDestroyedSLMTest::RunTest(const FString& Parameters)
{
	FCGHTargetTestScene Scene;
	if (!Scene.Initialize(*this) || !Scene.BeginPlayInTestWorld())
	{
		Scene.ForwardErrorMessages(this);
		return false;
	}
	ACGHTargetActor& Target = *Scene.Target;
	TestTrue(TEXT("Reference starts at identity, matching the absent-reference transform"),
		Scene.SLM->GetActorTransform().Equals(FTransform::Identity));
	TestTrue(TEXT("Identity SLM produces valid resources before destruction"), Target.bResourcesValid);
	TestTrue(TEXT("Identity SLM produces a cloud before destruction"), !Target.PointCloudResource.Points.IsEmpty());
	const uint64 ResourceId = Target.TargetDescription.ResourceId;
	const uint64 Revision = Target.TargetDescription.Revision;
	TestTrue(TEXT("Reference SLM can be destroyed"), Scene.SLM->Destroy());
	Scene.TickTestWorld();
	TestNull(TEXT("Destroyed SLM is no longer a valid reference"), Target.GetReferenceSLM());
	TestFalse(TEXT("Destroying an identity SLM invalidates mesh resources"), Target.bResourcesValid);
	TestTrue(TEXT("Destroyed identity SLM advances revision despite unchanged reference transform"),
		Target.TargetDescription.Revision > Revision);
	TestEqual(TEXT("Destroyed SLM does not change target identity"), Target.TargetDescription.ResourceId, ResourceId);
	TestTrue(TEXT("Destroyed SLM clears stale geometry"), Target.MeshGeometryResource.VerticesM.IsEmpty());
	TestTrue(TEXT("Destroyed SLM clears stale point cloud"), Target.PointCloudResource.Points.IsEmpty());
	TestTrue(TEXT("Destroyed SLM explains why resources cannot be sampled"), Target.ResourceError.Contains(TEXT("SLM")));
	CheckResourceVersions(*this, Target);
	Scene.ForwardErrorMessages(this);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHTargetResourcesWorkbenchTest,
	"CGH.TargetResources.WorkbenchReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHTargetResourcesWorkbenchTest::RunTest(const FString& Parameters)
{
	FCGHTargetTestScene Scene;
	if (!Scene.Initialize(*this))
	{
		return false;
	}
	ACGHTargetActor& Target = *Scene.Target;
	Target.SLM = nullptr; // The workbench supplies a reference when the actor has no explicit one.
	ACGHWorkbenchActor* Workbench = Scene.GetTestWorld()->SpawnActor<ACGHWorkbenchActor>();
	Workbench->SLM = Scene.SLM;
	Workbench->Camera = Scene.GetTestWorld()->SpawnActor<ACGHCameraActor>();
	Workbench->ReconstructionLight = Scene.GetTestWorld()->SpawnActor<ACGHReconstructionLightActor>();
	Workbench->Targets = {&Target};
	Target.SetActorScale3D(FVector(2.0, 3.0, 4.0));
	Workbench->UpdateSceneDescription();
	TestTrue(TEXT("Workbench supplies the default SLM reference"), Target.GetReferenceSLM() == Scene.SLM);
	TestTrue(TEXT("Scaled mesh target produces a complete snapshot"), Workbench->bSceneDescriptionComplete);
	TestTrue(TEXT("Scaled mesh targets are valid scene members"), Workbench->ValidateScene());
	if (TestEqual(TEXT("Workbench exports one target"), Workbench->SceneDescription.Targets.Num(), 1))
	{
		TestEqual(TEXT("Workbench retains resource identity"),
			Workbench->SceneDescription.Targets[0].ResourceId, Target.TargetDescription.ResourceId);
		TestEqual(TEXT("Workbench retains resource revision"),
			Workbench->SceneDescription.Targets[0].Revision, Target.TargetDescription.Revision);
	}

	Target.GeometryMesh->SetStaticMesh(nullptr);
	Workbench->UpdateSceneDescription();
	TestFalse(TEXT("Invalid mesh resources mark the snapshot incomplete"), Workbench->bSceneDescriptionComplete);
	Target.GeometryMesh->SetStaticMesh(Scene.Cube);

	ACGHSLMActor* OtherSLM = Scene.GetTestWorld()->SpawnActor<ACGHSLMActor>();
	OtherSLM->SetActorLocation(FVector(0.0, 50.0, 0.0));
	Target.SLM = OtherSLM;
	Workbench->UpdateSceneDescription();
	TestTrue(TEXT("Explicit target SLM overrides workbench fallback"), Target.GetReferenceSLM() == OtherSLM);
	TestFalse(TEXT("Incompatible SLM frames mark the snapshot incomplete"), Workbench->bSceneDescriptionComplete);
	if (Workbench->SceneDescription.Targets.Num() == 1)
	{
		TestEqual(TEXT("Incompatible frames never publish misleading target coordinates"),
			Workbench->SceneDescription.Targets[0].ResourceId, uint64(0));
	}
	Target.SLM = nullptr;
	Workbench->UpdateSceneDescription();
	TestTrue(TEXT("Clearing the explicit override restores workbench completeness"), Workbench->bSceneDescriptionComplete);

	ACGHWorkbenchActor* OtherWorkbench = Scene.GetTestWorld()->SpawnActor<ACGHWorkbenchActor>();
	OtherWorkbench->SLM = OtherSLM;
	OtherWorkbench->Camera = Workbench->Camera;
	OtherWorkbench->ReconstructionLight = Workbench->ReconstructionLight;
	OtherWorkbench->Targets = {&Target};
	OtherWorkbench->UpdateSceneDescription();
	TestFalse(TEXT("Another workbench cannot steal a target into a different SLM frame"),
		OtherWorkbench->bSceneDescriptionComplete);
	TestTrue(TEXT("Original workbench retains its target reference frame"), Target.GetReferenceSLM() == Scene.SLM);

	Workbench->SLM = OtherSLM;
	Workbench->UpdateSceneDescription();
	TestTrue(TEXT("Owning workbench can change its SLM reference"), Target.GetReferenceSLM() == OtherSLM);
	TestTrue(TEXT("Changing the owning SLM restores a consistent snapshot"), Workbench->bSceneDescriptionComplete);

	Workbench->Targets.Reset();
	Workbench->UpdateSceneDescription();
	OtherWorkbench->SLM = Scene.SLM;
	OtherWorkbench->UpdateSceneDescription();
	TestTrue(TEXT("Released target can join a different workbench"), Target.GetReferenceSLM() == Scene.SLM);
	TestTrue(TEXT("New owner publishes a complete snapshot"), OtherWorkbench->bSceneDescriptionComplete);
	Scene.ForwardErrorMessages(this);
	return true;
}

#if WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCGHTargetResourcesEditorTest,
	"CGH.TargetResources.EditorChangesAndDebugView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCGHTargetResourcesEditorTest::RunTest(const FString& Parameters)
{
	FCGHTargetTestScene Scene;
	if (!Scene.Initialize(*this, EWorldType::Editor))
	{
		return false;
	}
	ACGHTargetActor& Target = *Scene.Target;
	Target.UpdateTargetResources();
	uint64 Revision = Target.TargetDescription.Revision;
	FProperty* SliceProperty = FindFProperty<FProperty>(
		ACGHTargetActor::StaticClass(), GET_MEMBER_NAME_CHECKED(ACGHTargetActor, SliceCount));
	Target.PreEditChange(SliceProperty);
	Target.SliceCount = 3;
	FPropertyChangedEvent PropertyEvent(SliceProperty, EPropertyChangeType::ValueSet);
	Target.PostEditChangeProperty(PropertyEvent);
	TestTrue(TEXT("Editor property edits advance resource revision"), Target.TargetDescription.Revision > Revision);
	Revision = Target.TargetDescription.Revision;

	Target.SetActorScale3D(FVector(1.0, 2.0, 1.0));
	Target.PostEditMove(true);
	TestTrue(TEXT("Editor scaling refreshes resources"), Target.TargetDescription.Revision > Revision);
	Revision = Target.TargetDescription.Revision;

	Target.Parameters.InitialPhaseRad = 0.75;
	Target.PostTransacted(FTransactionObjectEvent());
	TestTrue(TEXT("Editor transaction refreshes resources"), Target.TargetDescription.Revision > Revision);
	TestEqual(TEXT("Transaction exports changed phase"), Target.TargetDescription.PhaseRad, 0.75);
	Revision = Target.TargetDescription.Revision;

	Target.PointSpacingMm = 100.0;
	Scene.GetTestWorld()->Tick(LEVELTICK_ViewportsOnly, 0.01f);
	TestTrue(TEXT("Viewport ticks detect direct sampling-parameter writes"), Target.TargetDescription.Revision > Revision);

	Target.bShowPointCloud = true;
	Target.RefreshVisualization();
	TestFalse(TEXT("Point-cloud debug view hides the source static mesh"), Target.GeometryMesh->IsVisible());
	Target.bShowPointCloud = false;
	Target.RefreshVisualization();
	TestTrue(TEXT("Disabling point-cloud debug view shows the source static mesh"), Target.GeometryMesh->IsVisible());
	Scene.ForwardErrorMessages(this);
	return true;
}

#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
