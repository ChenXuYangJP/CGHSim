#include "CGH/Actors/CGHTargetActor.h"

#include "CGH/Actors/CGHSLMActor.h"
#include "CGH/Actors/CGHWorkbenchActor.h"
#include "CGH/Components/CGHPointCloudComponent.h"
#include "CGH/Utils/CGHMeshSampling.h"
#include "CGH/Utils/CGHUnitConversion.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "HAL/PlatformProperties.h"
#include "StaticMeshResources.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// All target resource updates run on the game thread. IDs are unique for this process/session.
	uint64 NextTargetResourceId = 0;

	bool SameScalar(double A, double B)
	{
		return A == B || (FMath::IsNaN(A) && FMath::IsNaN(B));
	}
}

ACGHTargetActor::ACGHTargetActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	GeometryMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("GeometryMesh"));
	GeometryMesh->SetupAttachment(Root);
	GeometryMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GeometryMesh->SetCanEverAffectNavigation(false);

	PointCloudView = CreateDefaultSubobject<UCGHPointCloudComponent>(TEXT("PointCloudView"));
	PointCloudView->SetupAttachment(Root);
	PointCloudView->SetUsingAbsoluteScale(true);
	PointCloudView->SetRelativeScale3D(FVector::OneVector);
	PointCloudView->SetVisibility(false);

	MarkerMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MarkerMesh"));
	MarkerMesh->SetupAttachment(Root);
	MarkerMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MarkerMesh->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded())
	{
		MarkerMesh->SetStaticMesh(Sphere.Object);
	}
	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
	Label->SetupAttachment(Root);
	Label->SetWorldSize(2.0f);
	Label->SetHorizontalAlignment(EHTA_Center);
}

void ACGHTargetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	UpdateTargetResources();
}

void ACGHTargetActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (IsTemplate() || !GetWorld())
	{
		return;
	}
#if WITH_EDITOR
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
	FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &ACGHTargetActor::OnObjectPropertyChanged);
	FCoreUObjectDelegates::OnObjectTransacted.RemoveAll(this);
	FCoreUObjectDelegates::OnObjectTransacted.AddUObject(this, &ACGHTargetActor::OnObjectTransacted);
#endif
	UpdateTargetResources();
}

void ACGHTargetActor::PostUnregisterAllComponents()
{
	RemoveObservers();
#if WITH_EDITOR
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
	FCoreUObjectDelegates::OnObjectTransacted.RemoveAll(this);
#endif
	Super::PostUnregisterAllComponents();
}

void ACGHTargetActor::BeginPlay()
{
	Super::BeginPlay();
	UpdateTargetResources();
}

void ACGHTargetActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// C++ and Blueprint can write public parameters or swap meshes without editor notifications.
	UpdateTargetResources();
}

FVector ACGHTargetActor::GetOpticalPositionMeters(const AActor* ReferenceActor) const
{
	const FVector PositionCm = IsValid(ReferenceActor)
		? ReferenceActor->GetActorTransform().InverseTransformPositionNoScale(GetActorLocation())
		: GetActorLocation();
	return PositionCm * CGHUnits::CmToM(1.0);
}

ACGHSLMActor* ACGHTargetActor::GetReferenceSLM() const
{
	ACGHSLMActor* Reference = SLM.Get();
	if (!Reference)
	{
		Reference = WorkbenchSLM.Get();
		if (const ACGHWorkbenchActor* Workbench = ReferenceWorkbench.Get())
		{
			if (Workbench->IsActorBeingDestroyed() || Workbench->SLM != Reference || !Workbench->Targets.Contains(this))
			{
				Reference = nullptr;
			}
		}
		else if (ReferenceWorkbench.IsStale())
		{
			Reference = nullptr;
		}
	}
	return IsValid(Reference) && !Reference->IsActorBeingDestroyed() && Reference->GetWorld() == GetWorld()
		? Reference : nullptr;
}

void ACGHTargetActor::SetWorkbenchSLM(ACGHSLMActor* Reference, const ACGHWorkbenchActor* Workbench)
{
	const ACGHWorkbenchActor* Existing = ReferenceWorkbench.Get();
	if (Existing && Existing != Workbench && !Existing->IsActorBeingDestroyed() && Existing->Targets.Contains(this))
	{
		return; // One cached cloud has one optical frame; a second workbench must report the conflict.
	}
	WorkbenchSLM = Reference;
	ReferenceWorkbench = Workbench;
}

void ACGHTargetActor::UpdateTargetResources()
{
	if (bUpdatingResources || IsTemplate() || !GetWorld() || IsActorBeingDestroyed())
	{
		return;
	}
	TGuardValue<bool> UpdatingGuard(bUpdatingResources, true);
	// A root transform notification can arrive before the engine propagates it to children.
	GeometryMesh->UpdateComponentToWorld();
	RefreshObservers();

	ACGHSLMActor* Reference = GetReferenceSLM();
	const bool bMeshTarget = Parameters.TargetType == ECGHTargetType::Mesh;
	UStaticMesh* Mesh = bMeshTarget ? GeometryMesh->GetStaticMesh() : nullptr;
	const bool bCompiling = IsValid(Mesh) && Mesh->IsCompiling();
	const FStaticMeshRenderData* RenderData = IsValid(Mesh) && !bCompiling ? Mesh->GetRenderData() : nullptr;
	const int32 FirstLOD = RenderData ? RenderData->CurrentFirstLODIdx : INDEX_NONE;
	const FTransform ActorTransform = GetActorTransform();
	const FTransform MeshTransform = bMeshTarget ? GeometryMesh->GetComponentTransform() : FTransform::Identity;
	const FTransform ReferenceTransform = Reference
		? FTransform(Reference->GetActorQuat(), Reference->GetActorLocation()) : FTransform::Identity;
	const bool bChanged = bForceRebuild || !bHasCachedInputs
		|| CachedParameters.TargetType != Parameters.TargetType
		|| !SameScalar(CachedParameters.Amplitude, Parameters.Amplitude)
		|| !SameScalar(CachedParameters.InitialPhaseRad, Parameters.InitialPhaseRad)
		|| !CachedActorTransform.Equals(ActorTransform, 0.0)
		|| !CachedSLMTransform.Equals(ReferenceTransform, 0.0) || !CachedSLM.HasSameIndexAndSerialNumber(TWeakObjectPtr<ACGHSLMActor>(Reference))
		|| (bMeshTarget && (!CachedMesh.HasSameIndexAndSerialNumber(TWeakObjectPtr<UStaticMesh>(Mesh)) || !CachedMeshTransform.Equals(MeshTransform, 0.0)
			|| CachedRenderData != RenderData || CachedFirstLOD != FirstLOD || bCachedMeshCompiling != bCompiling
			|| CachedSliceCount != SliceCount || !SameScalar(CachedPointSpacingMm, PointSpacingMm)
			|| CachedMaxPointCount != MaxPointCount));
	if (!bChanged)
	{
		UpdateVisualization();
		return;
	}

	bForceRebuild = false;
	bHasCachedInputs = true;
	CachedParameters = Parameters;
	CachedActorTransform = ActorTransform;
	CachedMeshTransform = MeshTransform;
	CachedSLMTransform = ReferenceTransform;
	CachedSLM = Reference;
	CachedMesh = Mesh;
	CachedRenderData = RenderData;
	CachedFirstLOD = FirstLOD;
	bCachedMeshCompiling = bCompiling;
	CachedSliceCount = SliceCount;
	CachedPointSpacingMm = PointSpacingMm;
	CachedMaxPointCount = MaxPointCount;

	if (InstanceResourceId == 0)
	{
		InstanceResourceId = ++NextTargetResourceId;
	}
	++ResourceRevision;
	FCGHTargetDescription Description;
	Description.TargetId = InstanceResourceId;
	Description.ResourceId = InstanceResourceId;
	Description.Revision = ResourceRevision;
	Description.PositionSLMM = GetOpticalPositionMeters(Reference);
	Description.PositionSLM = Description.PositionSLMM;
	Description.RotationSLM = ReferenceTransform.GetRotation().Inverse() * GetActorQuat();
	Description.Amplitude = Parameters.Amplitude;
	Description.PhaseRad = Parameters.InitialPhaseRad;
	Description.TargetType = Parameters.TargetType;

	FCGHMeshGeometryResource Geometry;
	FCGHPointCloudResource Cloud;
	ResourceError.Reset();
	bResourcesValid = true;
	if (bMeshTarget)
	{
		Description.GeometryResourceId = Geometry.ResourceId = Cloud.ResourceId = InstanceResourceId;
		Description.GeometryRevision = Geometry.Revision = Cloud.Revision = ResourceRevision;
		if (!Reference)
		{
			ResourceError = TEXT("Assign a valid SLM on the target or its workbench before sampling a mesh.");
			bResourcesValid = false;
		}
		else
		{
			bResourcesValid = ReadMeshGeometry(Geometry, ResourceError);
			if (bResourcesValid)
			{
				FVector AxisWorld = Reference->GetActorLocation() - GetActorLocation();
				if (!AxisWorld.Normalize())
				{
					AxisWorld = -Reference->GetActorForwardVector();
				}
				const FVector AxisLocal = GetActorQuat().UnrotateVector(AxisWorld);
				bResourcesValid = CGHMeshSampling::Sample(Geometry, AxisLocal, SliceCount,
					CGHUnits::MmToM(PointSpacingMm), MaxPointCount, Parameters.Amplitude,
					Parameters.InitialPhaseRad, Cloud.Points, ResourceError);
			}
		}
		if (!bResourcesValid)
		{
			Geometry.VerticesM.Reset();
			Geometry.Indices.Reset();
			Geometry.Normals.Reset();
			Geometry.UVs.Reset();
			Cloud.Points.Reset();
		}
	}
	else if (Parameters.TargetType != ECGHTargetType::Point)
	{
		bResourcesValid = false;
		ResourceError = TEXT("Unsupported CGH target type.");
	}

	// Publish one complete generation. A failed generation never leaves old geometry or points behind.
	TargetDescription = MoveTemp(Description);
	MeshGeometryResource = MoveTemp(Geometry);
	PointCloudResource = MoveTemp(Cloud);
	MeshVertexCount = MeshGeometryResource.VerticesM.Num();
	MeshTriangleCount = MeshGeometryResource.Indices.Num() / 3;
	PointCount = PointCloudResource.Points.Num();
	UpdateVisualization();
}

bool ACGHTargetActor::ReadMeshGeometry(FCGHMeshGeometryResource& OutGeometry, FString& OutError) const
{
	UStaticMesh* Mesh = GeometryMesh->GetStaticMesh();
	const auto Fail = [&OutError](const TCHAR* Message) { OutError = Message; return false; };
	if (!IsValid(Mesh))
	{
		return Fail(TEXT("Assign a static mesh to GeometryMesh."));
	}
	if (Mesh->IsCompiling())
	{
		return Fail(TEXT("Static mesh is compiling; sampling will retry when compilation finishes."));
	}
	if (FPlatformProperties::RequiresCookedData() && !Mesh->bAllowCPUAccess)
	{
		return Fail(TEXT("Enable Allow CPU Access on the static mesh before cooking to sample it at runtime."));
	}
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.IsEmpty() || RenderData->CurrentFirstLODIdx > 0)
	{
		return Fail(TEXT("Static mesh LOD 0 is unavailable. Keep LOD 0 resident (disable LOD streaming) for CGH sampling."));
	}
	// Hold a reader reference while copying so LOD streaming cannot discard these buffers.
	TRefCountPtr<const FStaticMeshLODResources> LOD(&RenderData->LODResources[0]);
	const FPositionVertexBuffer& Positions = LOD->VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& Attributes = LOD->VertexBuffers.StaticMeshVertexBuffer;
	if (!Positions.GetVertexData() || Positions.GetNumVertices() == 0
		|| !Attributes.GetTangentData() || Attributes.GetNumVertices() != Positions.GetNumVertices()
		|| (Attributes.GetNumTexCoords() > 0 && !Attributes.GetTexCoordData())
		|| LOD->IndexBuffer.GetIndexDataSize() == 0)
	{
		return Fail(TEXT("Static mesh LOD 0 has no CPU-readable triangle data. Enable Allow CPU Access before cooking."));
	}
	const FIndexArrayView Indices = LOD->IndexBuffer.GetArrayView();
	if (Indices.Num() == 0 || Indices.Num() % 3 != 0)
	{
		return Fail(TEXT("Static mesh LOD 0 has no valid triangle index buffer."));
	}
	const FTransform MeshTransform = GeometryMesh->GetComponentTransform();
	const FVector Scale = MeshTransform.GetScale3D();
	if (MeshTransform.ContainsNaN() || GetActorTransform().ContainsNaN()
		|| FMath::Abs(Scale.X) < UE_DOUBLE_SMALL_NUMBER || FMath::Abs(Scale.Y) < UE_DOUBLE_SMALL_NUMBER
		|| FMath::Abs(Scale.Z) < UE_DOUBLE_SMALL_NUMBER)
	{
		return Fail(TEXT("Mesh transform must be finite with nonzero scale on every axis."));
	}
	const FMatrix NormalMatrix = MeshTransform.ToMatrixWithScale().Inverse().GetTransposed();
	const FQuat ActorRotation = GetActorQuat();
	const int32 VertexCount = static_cast<int32>(Positions.GetNumVertices());
	OutGeometry.VerticesM.Reserve(VertexCount);
	OutGeometry.Normals.Reserve(VertexCount);
	OutGeometry.UVs.Reserve(VertexCount);
	for (int32 Index = 0; Index < VertexCount; ++Index)
	{
		const FVector WorldPosition = MeshTransform.TransformPosition(FVector(Positions.VertexPosition(Index)));
		OutGeometry.VerticesM.Add(ActorRotation.UnrotateVector(WorldPosition - GetActorLocation()) * CGHUnits::CmToM(1.0));
		const FVector WorldNormal = FVector(NormalMatrix.TransformVector(FVector(Attributes.VertexTangentZ(Index)))).GetSafeNormal();
		OutGeometry.Normals.Add(FVector3f(ActorRotation.UnrotateVector(WorldNormal)));
		OutGeometry.UVs.Add(Attributes.GetNumTexCoords() > 0 ? Attributes.GetVertexUV(Index, 0) : FVector2f::ZeroVector);
	}
	const bool bMirrored = MeshTransform.ToMatrixWithScale().Determinant() < 0.0;
	OutGeometry.Indices.Reserve(Indices.Num());
	for (int32 Index = 0; Index < Indices.Num(); Index += 3)
	{
		const uint32 A = Indices[Index], B = Indices[Index + 1], C = Indices[Index + 2];
		if (A >= Positions.GetNumVertices() || B >= Positions.GetNumVertices() || C >= Positions.GetNumVertices())
		{
			return Fail(TEXT("Static mesh contains an out-of-range triangle index."));
		}
		OutGeometry.Indices.Add(A);
		OutGeometry.Indices.Add(bMirrored ? C : B);
		OutGeometry.Indices.Add(bMirrored ? B : C);
	}
	return true;
}

void ACGHTargetActor::RebuildTargetResources()
{
	bForceRebuild = true;
	UpdateTargetResources();
}

void ACGHTargetActor::RefreshVisualization()
{
	UpdateTargetResources();
}

void ACGHTargetActor::UpdateVisualization()
{
	const bool bPoint = Parameters.TargetType == ECGHTargetType::Point;
	const double Radius = FMath::IsFinite(MarkerRadiusCm) ? FMath::Max(0.01, MarkerRadiusCm) : 5.0;
	MarkerMesh->SetRelativeScale3D(FVector(Radius / 50.0));
	MarkerMesh->SetVisibility(bPoint);
	GeometryMesh->SetVisibility(!bPoint && !bShowPointCloud);
	PointCloudView->SetVisibility(!bPoint && bShowPointCloud && bResourcesValid);
	if (DisplayedRevision != ResourceRevision || DisplayedPointSize != DebugPointSize || DisplayedPointColor != DebugPointColor)
	{
		if (!bPoint && bResourcesValid)
		{
			PointCloudView->SetPoints(PointCloudResource.Points, DebugPointSize, DebugPointColor);
		}
		else
		{
			PointCloudView->ClearPoints();
		}
		DisplayedRevision = ResourceRevision;
		DisplayedPointSize = DebugPointSize;
		DisplayedPointColor = DebugPointColor;
	}
	Label->SetRelativeLocation(FVector(0.0, 0.0, Radius + 3.0));
	const FString Title = bPoint ? TEXT("Target Point (marker only)")
		: bResourcesValid ? FString::Printf(TEXT("Mesh Target | %d points"), PointCloudResource.Points.Num())
		: FString::Printf(TEXT("Mesh Target | %s"), *ResourceError);
	const FText Text = FText::FromString(FString::Printf(TEXT("%s\nAmplitude: %.3f | Phase: %.3f rad"),
		*Title, Parameters.Amplitude, Parameters.InitialPhaseRad));
	if (!Label->Text.EqualTo(Text))
	{
		Label->SetText(Text);
	}
}

void ACGHTargetActor::RefreshObservers()
{
	TArray<TWeakObjectPtr<USceneComponent>> Components;
	Components.Add(Root);
	Components.Add(GeometryMesh);
	if (ACGHSLMActor* Reference = GetReferenceSLM())
	{
		Components.AddUnique(Reference->GetRootComponent());
	}
	UStaticMesh* Mesh = Parameters.TargetType == ECGHTargetType::Mesh ? GeometryMesh->GetStaticMesh() : nullptr;
	if (Components == ObservedComponents && ObservedMesh.Get() == Mesh)
	{
		return;
	}
	RemoveObservers();
	ObservedComponents = MoveTemp(Components);
	ObservedMesh = Mesh;
	for (const TWeakObjectPtr<USceneComponent>& Component : ObservedComponents)
	{
		if (Component.IsValid())
		{
			Component->TransformUpdated.AddUObject(this, &ACGHTargetActor::OnTransformUpdated);
		}
	}
#if WITH_EDITOR
	if (IsValid(Mesh))
	{
		Mesh->GetOnMeshChanged().AddUObject(this, &ACGHTargetActor::OnMeshChanged);
	}
#endif
}

void ACGHTargetActor::RemoveObservers()
{
	for (const TWeakObjectPtr<USceneComponent>& Component : ObservedComponents)
	{
		if (Component.IsValid())
		{
			Component->TransformUpdated.RemoveAll(this);
		}
	}
	ObservedComponents.Reset();
#if WITH_EDITOR
	if (ObservedMesh.IsValid())
	{
		ObservedMesh->GetOnMeshChanged().RemoveAll(this);
	}
#endif
	ObservedMesh.Reset();
}

void ACGHTargetActor::OnTransformUpdated(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport)
{
	UpdateTargetResources();
}

#if WITH_EDITOR
void ACGHTargetActor::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	if (Object == this || Object == GeometryMesh || Object == GetReferenceSLM())
	{
		UpdateTargetResources();
	}
	else if (Object && Object == ObservedMesh.Get())
	{
		RebuildTargetResources();
	}
}

void ACGHTargetActor::OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event)
{
	if (Object == this || Object == GeometryMesh || Object == GetReferenceSLM() || Object == ObservedMesh.Get())
	{
		if (Object == ObservedMesh.Get())
		{
			bForceRebuild = true;
		}
		UpdateTargetResources();
	}
}

void ACGHTargetActor::OnMeshChanged()
{
	// Asset builds can broadcast while holding render-data locks. Retry on the next update.
	bForceRebuild = true;
}
#endif
