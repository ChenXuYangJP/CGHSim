#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CGH/Types/CGHTypes.h"
#include "CGHTargetActor.generated.h"

class ACGHSLMActor;
class ACGHWorkbenchActor;
class UCGHPointCloudComponent;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UTextRenderComponent;

UCLASS(Blueprintable)
class CGHSIM_API ACGHTargetActor : public AActor
{
	GENERATED_BODY()

public:
	ACGHTargetActor();
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void PostUnregisterAllComponents() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Target")
	FCGHTargetParameters Parameters;

	/** Explicit optical reference. When unset, an associated workbench supplies its SLM. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "CGH|Sampling")
	TObjectPtr<ACGHSLMActor> SLM;

	/** LOD 0 supplies triangles, normals and UV channel 0. Enable Allow CPU Access for cooked builds. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UStaticMeshComponent> GeometryMesh;

	/** Equally spaced cross-sections perpendicular to the target-to-SLM direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Sampling", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 SliceCount = 32;

	/** Maximum spacing on each intersection segment, measured after mesh/actor scaling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Sampling", meta = (ClampMin = "0.001", Units = "mm"))
	double PointSpacingMm = 5.0;

	/** Reject excessive sampling instead of returning an incomplete cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Sampling", meta = (ClampMin = "1", ClampMax = "1000000"))
	int32 MaxPointCount = 100000;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Resources")
	FCGHTargetDescription TargetDescription;

	/** Bulk caches remain accessible to solvers and Blueprints. Exposing their arrays in
	 * Details eagerly creates a property row for every vertex/point on actor selection. */
	UPROPERTY(BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Resources")
	FCGHMeshGeometryResource MeshGeometryResource;

	UPROPERTY(BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Resources")
	FCGHPointCloudResource PointCloudResource;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Resources")
	int32 MeshVertexCount = 0;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Resources")
	int32 MeshTriangleCount = 0;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Resources")
	int32 PointCount = 0;

	/** Point targets are valid without mesh resources. Mesh failures clear both resource arrays. */
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Resources")
	bool bResourcesValid = false;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Transient, NonTransactional, Category = "CGH|Resources")
	FString ResourceError;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Visualization")
	bool bShowPointCloud = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Visualization", meta = (ClampMin = "1.0", ClampMax = "64.0"))
	float DebugPointSize = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Visualization")
	FLinearColor DebugPointColor = FLinearColor(0.1f, 1.0f, 0.3f);

	/** Selection aid only; never represents the extent of the mathematical point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CGH|Visualization", meta = (ClampMin = "0.01", Units = "cm"))
	double MarkerRadiusCm = 5.0;

	/** Position in reference-local meters, ignoring reference scale. Null uses world axes/origin. */
	UFUNCTION(BlueprintPure, Category = "CGH|Target")
	FVector GetOpticalPositionMeters(const AActor* ReferenceActor) const;

	UFUNCTION(BlueprintPure, Category = "CGH|Target")
	ACGHSLMActor* GetReferenceSLM() const;

	/** Check for changes before consuming resources in the same frame as a direct write. */
	UFUNCTION(BlueprintCallable, Category = "CGH|Target")
	void UpdateTargetResources();

	/** Force rereading mesh data, e.g. after a custom runtime mesh edit. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Target")
	void RebuildTargetResources();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CGH|Visualization")
	void RefreshVisualization();

	void SetWorkbenchSLM(ACGHSLMActor* Reference, const ACGHWorkbenchActor* Workbench = nullptr);

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UStaticMeshComponent> MarkerMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UCGHPointCloudComponent> PointCloudView;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CGH|Components")
	TObjectPtr<UTextRenderComponent> Label;

private:
	bool ReadMeshGeometry(FCGHMeshGeometryResource& OutGeometry, FString& OutError) const;
	void UpdateVisualization();
	void RefreshObservers();
	void RemoveObservers();
	void OnTransformUpdated(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport);

	TWeakObjectPtr<ACGHSLMActor> WorkbenchSLM;
	TWeakObjectPtr<const ACGHWorkbenchActor> ReferenceWorkbench;
	TArray<TWeakObjectPtr<USceneComponent>> ObservedComponents;
	TWeakObjectPtr<UStaticMesh> ObservedMesh;

	// Instance identity survives registration/construction refreshes but is never copied to another actor.
	uint64 InstanceResourceId = 0;
	uint64 ResourceRevision = 0;
	bool bUpdatingResources = false;
	bool bForceRebuild = true;
	bool bHasCachedInputs = false;
	FCGHTargetParameters CachedParameters;
	FTransform CachedActorTransform;
	FTransform CachedMeshTransform;
	FTransform CachedSLMTransform;
	TWeakObjectPtr<ACGHSLMActor> CachedSLM;
	TWeakObjectPtr<UStaticMesh> CachedMesh;
	const void* CachedRenderData = nullptr;
	int32 CachedFirstLOD = INDEX_NONE;
	bool bCachedMeshCompiling = false;
	int32 CachedSliceCount = 0;
	double CachedPointSpacingMm = 0.0;
	int32 CachedMaxPointCount = 0;
	uint64 DisplayedRevision = MAX_uint64;
	float DisplayedPointSize = 0.0f;
	FLinearColor DisplayedPointColor;

#if WITH_EDITOR
	void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event);
	void OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event);
	void OnMeshChanged();
#endif
};
