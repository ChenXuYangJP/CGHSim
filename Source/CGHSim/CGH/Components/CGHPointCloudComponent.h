#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "CGHPointCloudComponent.generated.h"

struct FCGHObjectPoint;

/** Persistent point visualization. Input positions already include the target's scale. */
UCLASS(ClassGroup = CGH)
class CGHSIM_API UCGHPointCloudComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UCGHPointCloudComponent(const FObjectInitializer& ObjectInitializer);

	/** Positions are in rigid target-local meters; this component must use unit world scale. */
	void SetPoints(const TArray<FCGHObjectPoint>& Points, float PointSize, FLinearColor Color);
	void ClearPoints();

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	TArray<FVector> PositionsCm;
	FBox LocalBounds = FBox(ForceInit);
	float DrawPointSize = 3.0f;
	FLinearColor DrawColor = FLinearColor::Green;
};
