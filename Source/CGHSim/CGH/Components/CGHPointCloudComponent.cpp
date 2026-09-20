#include "CGH/Components/CGHPointCloudComponent.h"

#include "CGH/Types/CGHTypes.h"
#include "MeshElementCollector.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "SceneManagement.h"
#include "SceneView.h"

namespace
{
class FCGHPointCloudSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FCGHPointCloudSceneProxy(const UPrimitiveComponent* Component, const TArray<FVector>& InPositionsCm,
		float InPointSize, const FLinearColor& InColor)
		: FPrimitiveSceneProxy(Component)
		, PositionsCm(InPositionsCm)
		, PointSize(InPointSize)
		, Color(InColor)
	{
		bWillEverBeLit = false;
	}

	virtual SIZE_T GetTypeHash() const override
	{
		static const uint8 UniqueType = 0;
		return reinterpret_cast<SIZE_T>(&UniqueType);
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			if ((VisibilityMap & (1u << ViewIndex)) == 0)
			{
				continue;
			}

			FPrimitiveDrawInterface* PDI = Collector.GetPDI(ViewIndex);
			const uint8 DepthPriority = GetDepthPriorityGroup(Views[ViewIndex]);
			for (const FVector& PositionCm : PositionsCm)
			{
				PDI->DrawPoint(GetLocalToWorld().TransformPosition(PositionCm), Color, PointSize, DepthPriority);
			}
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Relevance;
		Relevance.bDrawRelevance = IsShown(View);
		Relevance.bDynamicRelevance = true;
		Relevance.bNormalTranslucency = true;
		Relevance.bSeparateTranslucency = true;
		Relevance.bEditorPrimitiveRelevance = UseEditorCompositing(View);
		return Relevance;
	}

	virtual uint32 GetMemoryFootprint() const override
	{
		return sizeof(*this) + FPrimitiveSceneProxy::GetAllocatedSize() + PositionsCm.GetAllocatedSize();
	}

private:
	TArray<FVector> PositionsCm;
	float PointSize;
	FLinearColor Color;
};
}

UCGHPointCloudComponent::UCGHPointCloudComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
	SetCanEverAffectNavigation(false);
	CastShadow = false;
	bUseEditorCompositing = true;
	SetIgnoreStreamingManagerUpdate(true);
}

void UCGHPointCloudComponent::SetPoints(const TArray<FCGHObjectPoint>& Points, float PointSize, FLinearColor Color)
{
	PositionsCm.Reset(Points.Num());
	LocalBounds.Init();
	for (const FCGHObjectPoint& Point : Points)
	{
		const FVector PositionCm = Point.PositionLocalM * 100.0;
		if (!PositionCm.ContainsNaN())
		{
			PositionsCm.Add(PositionCm);
			LocalBounds += PositionCm;
		}
	}

	DrawPointSize = FMath::IsFinite(PointSize) ? FMath::Max(1.0f, PointSize) : 3.0f;
	DrawColor = Color;
	UpdateBounds();
	MarkRenderStateDirty();
}

void UCGHPointCloudComponent::ClearPoints()
{
	if (PositionsCm.IsEmpty())
	{
		return;
	}

	PositionsCm.Reset();
	LocalBounds.Init();
	UpdateBounds();
	MarkRenderStateDirty();
}

FPrimitiveSceneProxy* UCGHPointCloudComponent::CreateSceneProxy()
{
	return PositionsCm.IsEmpty() ? nullptr : new FCGHPointCloudSceneProxy(this, PositionsCm, DrawPointSize, DrawColor);
}

FBoxSphereBounds UCGHPointCloudComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// A small extent also keeps planar and single-point clouds from having degenerate bounds.
	const FBox Bounds = LocalBounds.IsValid ? LocalBounds.ExpandBy(1.0) : FBox(FVector(-1.0), FVector(1.0));
	return FBoxSphereBounds(Bounds).TransformBy(LocalToWorld);
}
