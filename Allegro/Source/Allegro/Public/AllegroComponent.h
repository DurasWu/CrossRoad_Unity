// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once


#include "Components/MeshComponent.h"
#include "Allegro.h"
#include "AllegroBase.h"
#include "Chaos/Real.h"
#include "InstancedStruct.h"
#include "AlphaBlend.h"
#include "SpanAllocator.h"


#include "AllegroComponent.generated.h"

class UAllegroComponent;
class UAllegroAnimCollection;
struct FAllegroDynamicData;
class USkeletalMeshSocket;
class UAnimSequenceBase;

UENUM()
enum class EAllegroValidity : uint8
{
	Valid,
	NotValid,
};

UENUM()
enum class EAllegroInstanceFlags : uint16
{
	EIF_None = 0,

	EIF_Destroyed			= 1 << 0,
	EIF_New					= 1 << 1,	//instance is new, doesn't have prev frame data
	EIF_Hidden				= 1 << 2,	//instance is culled
	EIF_DynamicPose			= 1 << 3,	
	
	EIF_NeedLocalBoundUpdate = 1 << 4,

	EIF_AnimSkipTick		= 1 << 5, //ignore current tick
	EIF_AnimPaused			= 1 << 6, //animation is paused
	EIF_AnimLoop			= 1 << 7, //animation is looped
	EIF_AnimPlayingTransition	= 1 << 8, //animation is playing blend
	EIF_AnimNoSequence		= 1 << 9, //there is no sequence, CurrentSequence is 0xFFFF in this case
	EIF_AnimFinished		= 1 << 10, //animation is finished, CurrentSequence still has valid value

	
	EIF_BoundToSMC			= 1 << 11,

	//these flags can be used by developer
	EIF_BlendFrame			= 1 << 12,
	EIF_GPUTransition			= 1 << 13,
	EIF_UserFlag2			= 1 << 14,
	EIF_UserFlag3			= 1 << 15,

	EIF_AllAnimationFlags = EIF_AnimSkipTick | EIF_AnimPaused | EIF_AnimLoop | EIF_AnimPlayingTransition | EIF_AnimNoSequence | EIF_AnimFinished,

	EIF_AllUserFlags = EIF_BlendFrame | EIF_GPUTransition | EIF_UserFlag2 | EIF_UserFlag3,

	EIF_Default = EIF_New | EIF_AnimNoSequence | EIF_NeedLocalBoundUpdate,
};

ENUM_CLASS_FLAGS(EAllegroInstanceFlags);

static const int InstaceUserFlagStart = 12;

UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EInstanceUserFlags : uint8
{
	None = 0 UMETA(Hidden),

	BlendFrame = 1 << 0,
	GPUTranstion = 1 << 1,
	UserFlag2 = 1 << 2,
	UserFlag3 = 1 << 3,
	UserFlag4 = 1 << 4,
};

ENUM_CLASS_FLAGS(EInstanceUserFlags);


UENUM()
enum class EAnimAssetType : uint8
{
	AnimNull = 0,
	AnimSequeue,
	AnimMontage,
	AnimBlendSpace,
};
ENUM_CLASS_FLAGS(EAnimAssetType);


class FInstanceAnimStateExtend;


USTRUCT(BlueprintType)
struct ALLEGRO_API FAllegroInstanceAnimState
{
	GENERATED_USTRUCT_BODY()


	UPROPERTY()
	float Time = 0;
	
	UPROPERTY()
	float PlayScale = 1;
	
	//index for AnimCollection->Sequences, currently playing sequence index
	UPROPERTY()
	uint16 CurrentSequence = 0xFFff;	
	
	//index for AnimCollection->Transitions if any
	UPROPERTY()
	uint16 TransitionIndex = 0xFFff;

	bool IsValid() const { return CurrentSequence != 0xFFff; }

	bool IsTransitionValid() const { return TransitionIndex != 0xFFff; }

	void Tick(UAllegroComponent* Owner, int32 InstanceIndex, float Delta, uint32 FrameCounter, FMatrix* ViewProjection,FVector* ViewLocation,float LODScale);

	bool NeedTick(int32 InstanceIndex, uint32 FrameCounter, float& Delta, FBoxCenterExtentFloat& IB, FMatrix* ViewProjection, FVector* ViewLocation, float LODScale);

	bool IsTicked();
	void AddReferencedObjects(FReferenceCollector& Collector);
	//--------------------------------------------------------------------

	EAnimAssetType AssetType;

	UPROPERTY(Transient)
	TObjectPtr<UAnimationAsset> CurrentAnimAsset;

	FInstanceAnimStateExtend*  InstanceExtend = nullptr;

	void ResetAnimState();

	void SetCurrentAnimAsset(TObjectPtr<UAnimationAsset> Asset, EAnimAssetType Type);

	FInstanceAnimStateExtend* CreateNewInstanceExtend();

	struct TickDeferredEvent
	{
		UAnimSequenceBase* FinishedSequence;
		int FinishedTransitionIdx;

		uint8 GPUTransitionFinished : 1;  //新过渡是否结束事件位
	};

	struct FGPUTransitionInfo
	{
		int32 StartAnimSeqIndex; //暂时只做一个AS的过渡
		int32 BeginFrameIndex;   //帧所在的绝对位置
		int32 EndFrameIndex;

		EAlphaBlendOption BlendOption;
		float Time;
	};

	FGPUTransitionInfo GPUTransInfo;


	//---------------------------------------------
#if ALLEGRO_ANIMTION_TICK_LOD
	float DeltaTimeAccumulate = 0.0f;
	int   AnimtioneLOD = 0;
#endif

};


struct FInstanceBlendFrameInfo
{
	float Weight[ALLEGRO_BLEND_FRAME_NUM_MAX] = { 0.0f };
	float FrameIndex[ALLEGRO_BLEND_FRAME_NUM_MAX - 1] = { 0.0f };
};

/*
* SOA to keep data of instances
*/
USTRUCT()
struct ALLEGRO_API FAllegroInstancesData
{
	static const int LENGTH_ALIGN = 32;

	GENERATED_USTRUCT_BODY()

	TArray<EAllegroInstanceFlags> Flags;
	TArray<int32> FrameIndices;						//animation buffer frame index


	TArray<int32> BlendFrameInfoIndex;				//extend
	TArray<FInstanceBlendFrameInfo> BlendFrameInfo; //extend
	

	TArray<FAllegroInstanceAnimState> AnimationStates;

	//3 arrays instead of FTransform3f because most of the time Scale is untouched
	//location rotation and scale of instances in world space. the assumption is developer never set negative or non uniform scale.
	TArray<FVector3f> Locations;
	TArray<FQuat4f> Rotations;
	TArray<FVector3f> Scales;

	TArray<FMatrix44f> Matrices;
	TArray<float> RenderCustomData;

	TArray<int16> Stencil; //extend
	
	//indices of sub meshes attached to instances.
	//see @GetInstanceMeshSlots
	//each instance takes MaxMeshPerInstance + 1 elements. last one is termination sign (0xFF).
	//data is 0xFF terminated unique sequence with max length of MaxMeshPerInstance.
	TArray<uint8 /*index for UAllegroComponent.Submeshes[] */> MeshSlots;
	//
	TArray<FBoxCenterExtentFloat> LocalBounds;
	//#TODO Garbage collection support ?
	TArray<uint8, TAlignedHeapAllocator<16>> CustomPerInstanceStruct;

	void Reset();
	void Empty();

	friend FArchive& operator<<(FArchive& Ar, FAllegroInstancesData& R);

	void RemoveFlags(EAllegroInstanceFlags FlagsToRemove);

	void AddReferencedObjects(FReferenceCollector& Collector);
};

USTRUCT(BlueprintType)
struct FAllegroAnimFinishEvent
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Allegro|AnimCollection")
	int InstanceIndex = -1;
	UPROPERTY(BlueprintReadWrite, Category = "Allegro|AnimCollection")
	UAnimationAsset* AnimSequence = nullptr;
};

USTRUCT(BlueprintType)
struct FAllegroAnimNotifyEvent
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Allegro|AnimCollection")
	int InstanceIndex = -1;

	UPROPERTY(BlueprintReadWrite, Category = "Allegro|AnimCollection")
	UAnimationAsset* AnimAsset = nullptr;

	UPROPERTY(BlueprintReadWrite, Category = "Allegro|AnimCollection")
	FName NotifyName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro|AnimCollection")
	TObjectPtr<class UAnimNotify>  Notify;
};

struct FAllegroAnimPlayParams
{
	UAnimationAsset* Animation = nullptr;
	bool bLoop = true;
	float StartAt = 0;
	float PlayScale = 1;
	float TransitionDuration = 0.2f;
	EAlphaBlendOption BlendOption = EAlphaBlendOption::Linear;
	bool bIgnoreTransitionGeneration = false;
};


DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAllegroAnimationFinished, UAllegroComponent*, Component, const TArray<FAllegroAnimFinishEvent>&, Events);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAllegroAnimationNotify, UAllegroComponent*, Component, const TArray<FAllegroAnimNotifyEvent>&, Events);

//data of a dynamic pose instance bound to a USkeletalMeshComponent
USTRUCT(BlueprintType)
struct FAllegroDynInsTieData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Allegro")
	USkeletalMeshComponent* MeshComponent;
	UPROPERTY(BlueprintReadOnly, Category = "Allegro")
	int InstanceIndex = 0;
	UPROPERTY(BlueprintReadWrite, Category = "Allegro")
	int UserData = 0;
};

USTRUCT(BlueprintType)
struct FAllegroSubmeshSlot
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	USkeletalMesh* SkeletalMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	UStaticMesh* AdditionalStaticMesh = nullptr;

	UPROPERTY()
	UStaticMesh* StaticMesh = nullptr;

	//UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	bool PreSkinPostionOffset = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	FName Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin=0), Category = "Allegro")
	float MaxDrawDistance = 0;
	//if distance to view is higher than this then render OverrideSubmeshName instead. can be used for merging meshes that look similar in higher LODs.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta=(ClampMin=0), Category = "Allegro")
	float OverrideDistance = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "OverrideDistance > 0", EditConditionHides, GetOptions="GetSubmeshNames"), Category = "Allegro")
	FName OverrideSubmeshName;
	//
	int MeshDefIndex = -1; //index in AnimCollection.Meshes
	//SkeletalMesh.MinimumLOD won't effect us
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro", meta = (DisplayName = "Min LOD Index"))
	uint8 MinLODIndex = 0;

};


UINTERFACE()
class ALLEGRO_API UAllegroListenerInterface : public UInterface
{
	GENERATED_BODY()
};

class ALLEGRO_API IAllegroListenerInterface
{
	GENERATED_BODY()
public:

	virtual void CustomInstanceData_Initialize(int UserData, int InstanceIndex) { }
	virtual void CustomInstanceData_Destroy(int UserData, int InstanceIndex) {  }
	virtual void CustomInstanceData_Move(int UserData, int DstIndex, int SrcIndex) {  }
	virtual void CustomInstanceData_SetNum(int UserData, int NewNum) {  }

	virtual void OnAnimationFinished(int UserData, const TArray<FAllegroAnimFinishEvent>& Events) {}
	virtual void OnAnimationNotify(int UserData, const TArray<FAllegroAnimNotifyEvent>& Events) {}

	virtual void OnAllInstanceTicked(int UserData) {}
};


/*
* component for rendering instanced skeletal mesh.
* this component is designed to be runtime only, you can't select or edit instances in editor world, and also transform of the component doesn't matter since all instances are in world space.

this component handles frustum cull and LODing of its instances, so you can use it as a singleton.
but its recommended to spawn one component for a group/flock of instances close to each other in order to utilize engine's per component frustum and occlusion culling. 

every frame all instance data (Transform, PerInstanceCustomData,...) is sent to RenderThread and VRAM. 
if all of the instances are static (not moving and not playing anything) disabling tick may save some CPU performance.

in order to keep the code clean and optimal, there is no runtime check for those functions that take InstanceIndex, so caller (both C++ and Blueprint) must be careful. @see IsInsanceAlive/IsInstanceValid.
#TODO make all BP function safe ?

#Note even though UE5 switched to double precision this component still insist on float :D

#Note setting material through Details Panel is painful when you have lots of SubMeshes . use SetSubmeshMaterial() in Beginplay()
*/
UCLASS(Blueprintable, BlueprintType, editinlinenew, meta = (BlueprintSpawnableComponent),hideCategories=("Collision", "Physics", "Navigation", "HLOD"))
class ALLEGRO_API UAllegroComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UAllegroComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Allegro")
	UAllegroAnimCollection* AnimCollection;
	//All the skeletal meshes that may be rendered by this component must be added here. must be registered in AnimCollection.Meshes[] already.
	//Use InstanceAttachSubmesh*** to Attach/Detach meshes to an instance.
	//Its possible to have several sub meshes with identical USkeletalMesh* but different materials. that's why there is Name and InstanceAttachSubmeshByName().
	//Length must be < 256
	//
	UPROPERTY(EditAnywhere, meta=(TitleProperty = "{Name}"), Category = "Allegro")
	TArray<FAllegroSubmeshSlot> Submeshes;



	UPROPERTY()
	USkeletalMesh* SkeletalMesh_DEPRECATED;
	//max draw distance used for culling instances
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	float InstanceMaxDrawDistance;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	float InstanceMinDrawDistance;
	//how much to expand bounding box of this component.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	FVector3f ComponentBoundExtent;
	//distance based LOD. must be sorted.
	UPROPERTY(EditAnywhere, Category = "Allegro")
	float LODDistances[ALLEGRO_MAX_LOD - 1];
	//Level Of Detail calculation is distance based and is not effected by Field Of View. 
	//You can change it per component by SetLODDistanceScale or globally through "GAllegro_DistanceScale" or console variable "Allegro.DistanceScale"
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	float LODDistanceScale;
	// Defines the number of floats that will be available per instance for custom data. see also /Content/GetPerInstanceCustomData
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	int32 NumCustomDataFloats;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro", meta=(DisplayName="Start Shadow LOD Bias"))
	uint8 StartShadowLODBias;
	//can be used as an small optimization to render meshes with higher LODIndex for shadow pass 
	//only applies if calculated LODIndex of instance is >= @StartShadowLODBias
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro", meta=(DisplayName="Shadow LOD Bias"))
	uint8 ShadowLODBias = 2;
	
	//true if per instance custom data should be generated for instances being sent to shadow pass. 
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(DisplayAfter="NumCustomDataFloats"), Category = "Allegro")
	uint8 bNeedCustomDataForShadowPass : 1;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Allegro")
	uint8 bUseFixedInstanceBound : 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	uint8 bIgnoreAnimationsTick : 1;
	//
	uint8 bAnyValidSubmesh : 1;
	//maximum number of unique meshes per instance. must not be change at runtime.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin=1), Category = "Allegro")
	uint8 MaxMeshPerInstance;
	//index of the default mesh (Index for this->Meshes[]) to attach to instances by default. set 255 to attach nothing.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Allegro")
	uint8 InstanceDefaultAttachIndex;
	//
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro", meta=(ClampMin=0))
	float AnimationPlayRate;
	
	UPROPERTY(EditAnywhere, Category = "Allegro")
	const UScriptStruct* PerInstanceScriptStruct;

	//Struct of Arrays
	FAllegroInstancesData InstancesData;

	FSpanAllocator IndexAllocator;
	FSpanAllocator BlendFrameIndexAllocator;

	int NumAliveInstance;
	int NumAliveBlendFrame;

	TArray<FAllegroAnimFinishEvent> AnimationFinishEvents;
	TArray<FAllegroAnimNotifyEvent> AnimationNotifyEvents;

	float TimeSinceLastLocalBoundUpdate;
	int PrevDynamicDataInstanceCount;

	void PostApplyToComponent() override;
	void OnComponentCreated() override;
	void FixInstanceData();

	////BP arrays that start with 'InstanceData_'. valid on CDO only
	//TArray<FArrayProperty*> InstanceDataArrays;
	////
	//const auto& GetBPInstanceDataArrays() const { return GetClass()->GetDefaultObject<UAllegroComponent>()->InstanceDataArrays; }

	
#if WITH_EDITOR
	void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
	void PreEditChange(FProperty* PropertyThatWillChange) override;
	bool CanEditChange(const FProperty* InProperty) const override;
#endif

	void ApplyWorldOffset(const FVector& InOffset, bool bWorldShift) override;
	bool ShouldRecreateProxyOnUpdateTransform() const override { return false; }

	void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;
	void TickImpl(float DeltaTime);

	void SendRenderTransform_Concurrent() override;
	void SendRenderDynamicData_Concurrent() override;
	void CreateRenderState_Concurrent(FRegisterComponentContext* Context) override;
	bool IsPostLoadThreadSafe() const override { return false; }
	void PostLoad() override;
	void BeginDestroy() override;

	void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport = ETeleportType::None) override;
	void DetachFromComponent(const FDetachmentTransformRules& DetachmentRules) override;
	void InitializeComponent() override;
	void UninitializeComponent() override;
	void PostDuplicate(bool bDuplicateForPIE) override;
	void PreDuplicate(FObjectDuplicationParameters& DupParams) override;
	void OnRegister() override;
	void OnUnregister() override;
	FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	FBoxSphereBounds CalcLocalBounds() const override;

	TStructOnScope<FActorComponentInstanceData> GetComponentInstanceData() const override;
	void ApplyInstanceData(FAllegroComponentInstanceData& ID);
	void PostInitProperties() override;
	void PostCDOContruct() override;
	
	void OnVisibilityChanged() override;
	void OnHiddenInGameChanged() override;

	void BeginPlay() override;
	void EndPlay(EEndPlayReason::Type Reason) override;

	int32 GetNumMaterials() const override;
	UMaterialInterface* GetMaterial(int32 MaterialIndex) const override;
	int32 GetMaterialIndex(FName MaterialSlotName) const override;
	TArray<FName> GetMaterialSlotNames() const override;
	bool IsMaterialSlotNameValid(FName MaterialSlotName) const override;
	FPrimitiveSceneProxy* CreateSceneProxy() override;
	bool ShouldCreateRenderState() const;

	void Serialize(FArchive& Ar) override;

	void TickAnimations(float DeltaTime); 

	void CalcAnimationFrameIndices();

	//fast enough. won't recreate render state.
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	void SetLODDistanceScale(float NewLODDistanceScale);

	UFUNCTION(BlueprintCallable, meta=(DisplayName="MarkRenderStateDirty"), Category = "Allegro|Rendering")
	void K2_MarkRenderStateDirty() { this->MarkRenderStateDirty(); }

	//
	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void SetAnimCollection(UAllegroAnimCollection* asset);
	//

	void CheckAssets_Internal();
	
	//reset to reference pose (releases transition/dynamic pose if any)
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	void ResetInstanceAnimationState(int InstanceIndex);

	//return total number of instances (including destroyed instances)
	UFUNCTION(BlueprintPure, meta=(Keywords="Num Instance,"), Category="Allegro")
	int32 GetInstanceCount() const { return IndexAllocator.GetMaxSize(); }
	//return number of alive instances
	UFUNCTION(BlueprintPure, Category = "Allegro")
	int32 GetAliveInstanceCount() const { return NumAliveInstance; }
	//return number of destroyed instances
	UFUNCTION(BlueprintPure, Category = "Allegro")
	int32 GetDestroyedInstanceCount() const { return IndexAllocator.GetMaxSize() - NumAliveInstance; }
	
	//return True if instance is not destroyed
	bool IsInstanceAlive(int32 InstanceIndex) const { return !EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Destroyed); }

	//return true if index and flags of the specified instance are valid
	UFUNCTION(BlueprintPure, Category="Allegro")
	bool IsInstanceValid(int32 InstanceIndex) const;

	//add an instance to this component (adding/removing is fast enough, won't recreate render state)
	//@return	instance index
	UFUNCTION(BlueprintCallable, Category="Allegro")
	int AddInstance(const FTransform3f& WorldTransform);
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro")
	int AddInstance_CopyFrom(const UAllegroComponent* Src, int SrcInstanceIndex);
	//Destroy the instance at specified index, Note that this will not remove anything from the arrays but mark the index as destroyed.
	//Returns True on success
	UFUNCTION(BlueprintCallable, Category="Allegro", meta = (Keywords = "Remove, Delete"))
	bool DestroyInstance(int InstanceIndex);
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro", meta = (Keywords = "Remove, Delete"))
	void DestroyInstances(const TArray<int>& InstanceIndices);
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro", meta = (Keywords = "Remove, Delete"))
	void DestroyInstancesByRange(int StartIndex, int Count);
	//
	bool DestroyAt_Internal(int InstanceIndex);
	//
	void DestroyCustomStruct_Internal(int InstanceIndex);

	/*
	remove all destroyed instances from the arrays and shrink the memory, it will invalidate indices.
	typically you need to call it if you have lots of destroyed instances that are not going to be reused. 
	for example you add 100k instances then remove 90k, so 90% of your instances are unused, they are taking memory and iteration takes more time.
	
	@param RemapArray	can be used to map old indices to new indices. NewInstanceIndex == RemapArray[OldInstanceIndex];
	@return				number of freed instances
	*/
	UFUNCTION(BlueprintCallable, meta=(DisplayName="FlushInstances"), Category = "Allegro")
	int K2_FlushInstances(TArray<int>& RemapArray);
	virtual int FlushInstances(TArray<int>* OutRemapArray = nullptr);

	void RemoveTailDataIfAny();
	void InstanceDataSetNum_Internal(int NewArrayLen);

	/*
	clear all the instances being rendered by this component.
	@param bEmpty	true if memory should be freed
	*/
	UFUNCTION(BlueprintCallable, Category = "Allegro", meta = (Keywords = "Remove, Delete"))
	void ClearInstances(bool bEmpty = true);

	/*
	try to copy instance data (transform, animation state, ...) from another component to the specified instance in this component.
	*/
	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void InstanceCopyFrom(int InstanceIndex, const UAllegroComponent* SrcComponent, int SrcInstanceIndex);
	
	
	//subclass can override the following functions to register its own cache friendly per instance data
	//#Note Blueprint can use PerInstanceScriptStruct
	
	//subclass should set the initial value of an element.
	virtual void CustomInstanceData_Initialize(int InstanceIndex) { /*e.g: AgentBodies[InstanceIndex] = new FMyAgentBody();*/ }
	//subclass should destroy the value of an element.
	virtual void CustomInstanceData_Destroy(int InstanceIndex) { /*e.g:  delete AgentBodies[InstanceIndex]; */ }
	//subclass should move the value of an element to another index.
	virtual void CustomInstanceData_Move(int DstIndex, int SrcIndex) { /* e.g: AgendBodies[DstIndex] = AgendBodies[SrcIndex]; */ }
	//subclass should change the length of array using SetNum
	virtual void CustomInstanceData_SetNum(int NewNum) { /* e.g: AgendBodies.SetNum(NewNum); */ }


	void CallCustomInstanceData_Initialize(int InstanceIndex);
	void CallCustomInstanceData_Destroy(int InstanceIndex);
	void CallCustomInstanceData_Move(int DstIndex, int SrcIndex);
	void CallCustomInstanceData_SetNum(int NewNum);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Transform")
	void BatchUpdateTransforms(int StartInstanceIndex, const TArray<FTransform3f>& NewTransforms);

	//all the transforms are in world space
	UFUNCTION(BlueprintCallable, Category = "Allegro|Transform")
	void SetInstanceTransform(int InstanceIndex, const FTransform3f& NewTransform);
	UFUNCTION(BlueprintCallable, Category = "Allegro|Transform")
	void SetInstanceLocation(int InstanceIndex, const FVector3f& NewLocation);
	UFUNCTION(BlueprintCallable, Category = "Allegro|Transform", meta = (DisplayName = "Set Instance Rotation (Rotator)"))
	void SetInstanceRotator(int InstanceIndex, const FRotator3f& NewRotator);
	UFUNCTION(BlueprintCallable, Category = "Allegro|Transform", meta=(DisplayName="Set Instance Rotation (Quat)"))
	void SetInstanceRotation(int InstanceIndex, const FQuat4f& NewRotation);
	UFUNCTION(BlueprintCallable, Category = "Allegro|Transform")
	void SetInstanceScale(int InstanceIndex, const FVector3f& NewScale);
	UFUNCTION(BlueprintCallable, Category = "Allegro|Transform")
	void AddInstanceLocation(int InstanceIndex, const FVector3f& Offset);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Transform")
	void SetInstanceLocationAndRotation(int InstanceIndex, const FVector3f& NewLocation, const FQuat4f& NewRotation);
	
	UFUNCTION(BlueprintPure, Category = "Allegro|Transform")
	FTransform3f GetInstanceTransform(int InstanceIndex) const;
	UFUNCTION(BlueprintPure, Category = "Allegro|Transform")
	const FVector3f& GetInstanceLocation(int InstanceIndex) const;
	UFUNCTION(BlueprintPure, Category = "Allegro|Transform", meta=(Keywords="Get Rotation Quat"))
	const FQuat4f& GetInstanceRotation(int InstanceIndex) const;
	UFUNCTION(BlueprintPure, Category = "Allegro|Transform", meta=(Keywords="Get Rotation Rotator"))
	FRotator3f GetInstanceRotator(int InstanceIndex) const;

	//return current local bounding box of an instance
	const FBoxCenterExtentFloat& GetInstanceLocalBound(int InstanceIndex);
	//calculate and return bounding box of an instance in world space
	FBoxCenterExtentFloat CalculateInstanceBound(int InstanceIndex);
	//returns true if this component doesn't need local bound. InstanceData.LocalBounds will be empty.
	bool ShouldUseFixedInstanceBound() const;
	//
	void OnInstanceTransformChange(int InstanceIndex);
	
	//add offset to the location of all valid instances, mostly used by Blueprint since looping through thousands of instances takes too much time there
	UFUNCTION(BlueprintCallable, Category="Allegro|Transform")
	void MoveAllInstances(const FVector3f& Offset);
	

	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void InstanceAddUserFlags(int InstanceIndex, UPARAM(meta = (Bitmask, BitmaskEnum=EInstanceUserFlags)) int32 Flags)
	{
		check(IsInstanceValid(InstanceIndex));
		InstanceAddFlags(InstanceIndex, static_cast<EAllegroInstanceFlags>(Flags << InstaceUserFlagStart));
	}
	//add the flags to the specified instances
	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void BatchAddUserFlags(const TArray<int>& InstanceIndices, UPARAM(meta = (Bitmask, BitmaskEnum = EInstanceUserFlags)) int32 Flags);

	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void InstanceRemoveUserFlags(int InstanceIndex, UPARAM(meta = (Bitmask, BitmaskEnum = EInstanceUserFlags)) int32 Flags)
	{
		check(IsInstanceValid(InstanceIndex));
		InstanceRemoveFlags(InstanceIndex, static_cast<EAllegroInstanceFlags>(Flags << InstaceUserFlagStart));
	}
	//remove flags from the specified instances
	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void BatchRemoveUserFlags(const TArray<int>& InstanceIndices, UPARAM(meta = (Bitmask, BitmaskEnum = EInstanceUserFlags)) int32 Flags);

	UFUNCTION(BlueprintCallable, Category = "Allegro")
	bool InstanceHasAnyUserFlags(int InstanceIndex, UPARAM(meta = (Bitmask, BitmaskEnum = EInstanceUserFlags)) int32 Flags) const
	{
		check(IsInstanceValid(InstanceIndex));
		return InstanceHasAnyFlag(InstanceIndex, static_cast<EAllegroInstanceFlags>(Flags << InstaceUserFlagStart));
	}

	void InstanceSetFlags(int InstanceIndex, EAllegroInstanceFlags Flags, bool bSet)
	{
		if(bSet)
			EnumAddFlags(InstancesData.Flags[InstanceIndex], Flags);
		else
			EnumRemoveFlags(InstancesData.Flags[InstanceIndex], Flags);
	}
	void InstanceAddFlags(int InstanceIndex, EAllegroInstanceFlags FlagsToAdd)			{ EnumAddFlags(InstancesData.Flags[InstanceIndex], FlagsToAdd); }
	void InstanceRemoveFlags(int InstanceIndex, EAllegroInstanceFlags FlagsToRemove)		{ EnumRemoveFlags(InstancesData.Flags[InstanceIndex], FlagsToRemove); }
	bool InstanceHasAnyFlag(int InstanceIndex, EAllegroInstanceFlags FlagsToTest) const	{ return EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], FlagsToTest); }
	bool InstanceHasAllFlag(int InstanceIndex, EAllegroInstanceFlags FlagsToTest) const	{ return EnumHasAllFlags(InstancesData.Flags[InstanceIndex], FlagsToTest); }
	

	//fast enough to be used at real time, won't recreate render state
	UFUNCTION(BlueprintCallable,Category = "Allegro|Rendering")
	void SetInstanceHidden(int InstanceIndex, bool bHidden);
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	void ToggleInstanceVisibility(int InstanceIndex);
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	bool IsInstanceHidden(int InstanceIndex) const;
	


	/*
	play animation on an instance.
	@param InstanceIndex		instance to play animation on
	@param Animation			animation sequence to be played, should exist in AnimSet->Sequences
	@param bLoop				true if animation should loop, false if needs to be played once (when animation is finished the last frame index is kept and OnAnimationFinished is called so you can play animation inside it again).
	@param StartAt				time to start playback at
	@param PlayScale			speed of playback, negative is not supported
	@param TransitionDuration	if <= 0 plays without blending (jumps to the first target frame )
	@param BlendOption			
	@return						sequence length if playing started, otherwise -1
	*/
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	UPARAM(DisplayName = "SequenceLength") 
	float InstancePlayAnimation(int InstanceIndex, UAnimationAsset* Animation, bool bLoop = true, float StartAt = 0, float PlayScale = 1, float TransitionDuration = 0.2f, EAlphaBlendOption BlendOption = EAlphaBlendOption::Linear, bool bIgnoreTransitionGeneration = false);

	float InstancePlayAnimation(int InstanceIndex, FAllegroAnimPlayParams Params);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	void PauseInstanceAnimation(int InstanceIndex, bool bPause);
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	bool IsInstanceAnimationPaused(int InstanceIndex) const;
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	void ToggleInstanceAnimationPause(int InstanceIndex);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	void SetInstanceAnimationLooped(int InstanceIndex, bool bLoop);
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	bool IsInstanceAnimationLooped(int InstanceIndex) const;
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	void ToggleInstanceAnimationLoop(int InstanceIndex);

	//#TODO confusing name! Playing or active ? 
	//returns true if the specified animation is being played by the the instance
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	bool IsInstancePlayingAnimation(int InstanceIndex, const UAnimSequenceBase* Animation) const;
	//returns true if the specified instance is playing any animation
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	bool IsInstancePlayingAnyAnimation(int InstanceIndex) const;
	
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	UAnimSequenceBase* GetInstanceCurrentAnimSequence(int InstanceIndex) const;

	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	void SetInstancePlayScale(int InstanceIndex, float NewPlayScale);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	void SetInstanceBlendSpacePosition(int InstanceIndex,float InX, float InY = 0);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	bool InstanceMontageJumpToSectionName(int InstanceIndex,const FString& SectionName, bool bEndOfSection = false);
	
	float GetInstancePlayLength(int InstanceIndex) const;
	float GetInstancePlayTime(int InstanceIndex) const;
	float GetInstancePlayTimeRemaining(int InstanceIndex) const;
	float GetInstancePlayTimeFraction(int InstanceIndex) const;
	float GetInstancePlayTimeRemainingFraction(int InstanceIndex) const;

	
	void CallOnAnimationFinished();
	void CallOnAnimationNotify();

	//
	virtual void OnAnimationFinished(const TArray<FAllegroAnimFinishEvent>& Events) {}
	virtual void OnAnimationNotify(const TArray<FAllegroAnimNotifyEvent>& Events) {}

	//is called when animation is finished (only non-looped animations)
	UPROPERTY(BlueprintAssignable, meta=(DisplayName = "OnAnimationFinished"))
	FAllegroAnimationFinished OnAnimationFinishedDelegate;
	//is called for name only notifications
	UPROPERTY(BlueprintAssignable, meta = (DisplayName = "OnAnimationNotify"))
	FAllegroAnimationNotify OnAnimationNotifyDelegate;

	
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	void PlayAnimationOnAll(UAnimationAsset* Animation, bool bLoop = true, float StartAt = 0, float PlayScale = 1, float TransitionDuration = 0.2f, EAlphaBlendOption BlendOption = EAlphaBlendOption::Linear, bool bIgnoreTransitionGeneration = false);

	void ResetAnimationStates();
	void LeaveTransitionIfAny_Internal(int InstanceIndex);


	FTransform3f GetInstanceBoneTransform(int InstanceIndex, int SkeletonBoneIndex, bool bWorldSpace) const
	{
		if(bWorldSpace)
			return GetInstanceBoneTransformWS(InstanceIndex, SkeletonBoneIndex);

		return GetInstanceBoneTransformCS(InstanceIndex, SkeletonBoneIndex);
	}
	//returns the cached bone transform (component space)
	FTransform3f GetInstanceBoneTransformCS(int InstanceIndex, int SkeletonBoneIndex,bool InGameThread = true) const;
	//returns the cached bone transform in world space
	FTransform3f GetInstanceBoneTransformWS(int InstanceIndex, int SkeletonBoneIndex) const;

	//returns skeleton bone index
	UFUNCTION(BlueprintCallable, Category = "Allegro")
	int32 GetBoneIndex(FName BoneName) const;
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro")
	FName GetBoneName(int32 SkeletonBoneIndex) const;

	struct FSocketMinimalInfo
	{
		FTransform3f LocalTransform = FTransform3f::Identity;
		int BoneIndex = -1;
	};

	USkeletalMeshSocket* FindSocket(FName SocketName, const USkeletalMesh* InMesh = nullptr) const;
	
	//return the transform of a socket/bone on success, identity otherwise.
	//@SocketName Bone/Socket Name on Skeleton Or SkeletalMesh
	//@InMesh	  SkeletalMesh to get the socket data from. set null to take from default mesh or skeleton.
	UFUNCTION(BlueprintCallable, Category = "Allegro|Transform")
	FTransform3f GetInstanceSocketTransform(int32 InstanceIndex, FName SocketName, USkeletalMesh* InMesh = nullptr, bool bWorldSpace = true) const;

	/*
	* get socket/bone transform of all valid instances (fills destroyed instances with identity)
	*/
	UFUNCTION(BlueprintCallable, Category = "Allegro|Transform")
	void GetInstancesSocketTransform(TArray<FTransform3f>& OutTransforms, FName SocketName, USkeletalMesh* InMesh = nullptr, bool bWorldSpace = true) const;

	//C++ version of above that takes callable instead of copying to an array
	void GetInstancesSocketTransform(const FName SocketName, USkeletalMesh* InMesh, const bool bWorldSpace, TFunctionRef<void(int InstanceIndex, const FTransform3f& T)> Proc) const;
	
	//@BoneName Bone/Socket Name on Skeleton Or SkeletalMesh
	//generally we call this once before looping over instances and calling GetInstanceSocketTransform_Fast
	FSocketMinimalInfo GetSocketMinimalInfo(FName InSocketName, USkeletalMesh* InMesh = nullptr) const;
	//
	FTransform3f GetInstanceSocketTransform_Fast(int InstanceIndex, const FSocketMinimalInfo& SocketInfo, bool bWorldSpace) const;

	//return indices of instances whose location are inside the specified sphere
	void QueryLocationOverlappingSphere(const FVector3f& Center, float Radius, TArray<int>& OutIndices) const;
	//return indices of instances whose location are inside the specified box
	void QueryLocationOverlappingBox(const FBox3f& Box, TArray<int>& OutIndices) const;

	#pragma region CustomDataFloat

	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	void SetInstanceCustomData(int InstanceIndex, int FloatIndex, float InValue)
	{
		check(IsInstanceValid(InstanceIndex) && FloatIndex < NumCustomDataFloats);
		InstancesData.RenderCustomData[InstanceIndex * NumCustomDataFloats + FloatIndex] = InValue;
	}
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	float GetInstanceCustomData(int InstanceIndex, int FloatIndex) const
	{
		check(IsInstanceValid(InstanceIndex) && FloatIndex < NumCustomDataFloats);
		return InstancesData.RenderCustomData[InstanceIndex * NumCustomDataFloats + FloatIndex];
	}

	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	void SetInstanceCustomDataFloat1(int InstanceIndex, float Value)			{ SetInstanceCustomData<float>(InstanceIndex, Value);		}
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	void SetInstanceCustomDataFloat2(int InstanceIndex, const FVector2f& Value) { SetInstanceCustomData<FVector2f>(InstanceIndex, Value);	}
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	void SetInstanceCustomDataFloat3(int InstanceIndex, const FVector3f& Value) { SetInstanceCustomData<FVector3f>(InstanceIndex, Value);	}
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	void SetInstanceCustomDataFloat4(int InstanceIndex, const FVector4f& Value) { SetInstanceCustomData<FVector4f>(InstanceIndex, Value);	}


	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	float GetInstanceCustomDataFloat1(int InstanceIndex) const		{ return GetInstanceCustomData<float>(InstanceIndex);		}
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	FVector2f GetInstanceCustomDataFloat2(int InstanceIndex) const	{ return GetInstanceCustomData<FVector2f>(InstanceIndex);	}
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	FVector3f GetInstanceCustomDataFloat3(int InstanceIndex) const	{ return GetInstanceCustomData<FVector3f>(InstanceIndex);	}
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	FVector4f GetInstanceCustomDataFloat4(int InstanceIndex) const	{ return GetInstanceCustomData<FVector4f>(InstanceIndex);	}

	void ZeroInstanceCustomData(int InstanceIndex)
	{
		check(IsInstanceValid(InstanceIndex));
		for (int i = 0; i < NumCustomDataFloats; i++)
			InstancesData.RenderCustomData[InstanceIndex * NumCustomDataFloats + i] = 0;
	}
	//
	template<typename TData /*float, FVector2f, ... */> void SetInstanceCustomData(int InstanceIndex, const TData& InValue)
	{
		check(IsInstanceValid(InstanceIndex) && NumCustomDataFloats > 0 && sizeof(TData) == (sizeof(float) * NumCustomDataFloats));
		new (GetInstanceCustomDataFloats(InstanceIndex)) TData(InValue);
	}
	//
	template<typename TData /*float, FVector2f, ... */> const TData& GetInstanceCustomData(int InstanceIndex) const
	{
		check(IsInstanceValid(InstanceIndex) && NumCustomDataFloats > 0 && sizeof(TData) == (sizeof(float) * NumCustomDataFloats));
		return *reinterpret_cast<const TData*>(GetInstanceCustomDataFloats(InstanceIndex));
	}


	float* GetInstanceCustomDataFloats(int InstanceIndex) { return &InstancesData.RenderCustomData[InstanceIndex * NumCustomDataFloats]; }
	const float* GetInstanceCustomDataFloats(int InstanceIndex) const { return &InstancesData.RenderCustomData[InstanceIndex * NumCustomDataFloats]; }
	
	#pragma endregion CustomDataFloat

	int GetInstanceBoundIndex(int InstanceIndex) const;

	void DebugDrawInstanceBound(int InstanceIndex, float BoundExtentOffset, FColor const& Color, bool bPersistentLines = false, float LifeTime = -1.f, uint8 DepthPriority = 0, float Thickness = 0.f);
	
	FAllegroDynamicData* GenerateDynamicData_Internal();

	void CalcInstancesBound(FBoxMinMaxFloat& OutCompBound, FBoxCenterExtentFloat* OutInstancesBounds);
	void UpdateInstanceLocalBound(int InstanceIndex);
	void UpdateLocalBounds();

#if WITH_EDITOR  
	UFUNCTION(CallInEditor, Category="Allegro")
	void AddAllSkeletalMeshes();
#endif
	//returns pointer to the sub mesh indices of instance
	uint8* GetInstanceMeshSlots(int InstanceIndex) { return &this->InstancesData.MeshSlots[InstanceIndex * (MaxMeshPerInstance + 1)]; }
	const uint8* GetInstanceMeshSlots(int InstanceIndex) const { return &this->InstancesData.MeshSlots[InstanceIndex * (MaxMeshPerInstance + 1)]; }

	//detach sub meshes from all instances
	void ResetMeshSlots();
	
	UFUNCTION(BlueprintCallable, Category="Allegro|Rendering")
	void AddSubmesh(const FAllegroSubmeshSlot& InData);
	
	//default register all skeletal meshes of the AnimCollection as Submesh of this component
	UFUNCTION(BlueprintCallable, Category="Allegro|Rendering")
	void InitSubmeshesFromAnimCollection();

	UFUNCTION(BlueprintCallable, Category="Allegro|Rendering")
	int GetSubmeshCount() const { return this->Submeshes.Num(); }

	//
	UFUNCTION(BlueprintCallable, Category="Allegro|Rendering")
	void SetSubmeshAsset(int SubmeshIndex, USkeletalMesh* InMesh);

	int FindSubmeshIndex(const USkeletalMesh* SubmeshAsset) const;
	int FindSubmeshIndex(FName SubmeshName) const;
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	int FindSubmeshIndex(FName SubmeshName, const USkeletalMesh* OrSubmeshAsset) const;
	//#Note feel free to attach/detach at real time . it won't recreate render state.
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	bool InstanceAttachSubmeshByName(int InstanceIndex, FName SubMeshName, bool bAttach);
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	bool InstanceAttachSubmeshByAsset(int InstanceIndex, USkeletalMesh* InMesh, bool bAttach);
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	bool InstanceAttachSubmeshByIndex(int InstanceIndex, uint8 SubMeshIndex, bool bAttach);
	//
	bool InstanceAttachSubmeshByIndex_Internal(int InstanceIndex, uint8 SubMeshIndex, bool bAttach);
	//attach all the specified skeletal meshes to an instance
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	int InstanceAttachSubmeshes(int InstanceIndex, const TArray<USkeletalMesh*>& InMeshes, bool bAttach);
	//detach all meshes from the specified instance
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	void InstanceDetachAllSubmeshes(int InstanceIndex);
	//get all the attached meshes of an instance
	UFUNCTION(BlueprintCallable, BlueprintPure=false, Category = "Allegro|Rendering")
	void GetInstanceAttachedSkeletalMeshes(int InstanceIndex, TArray<USkeletalMesh*>& OutMeshes) const;

	
	
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	int GetSubmeshBaseMaterialIndex(int SubmeshIndex) const;
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	int GetSubmeshBaseMaterialIndexByAsset(USkeletalMesh* SubmeshAsset) const;
	//
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	int GetSubmeshBaseMaterialIndexByName(FName SunmeshName) const;


	void SetSubmeshMaterial(int SubmeshIndex, int MaterialIndex, UMaterialInterface* Material);
	void SetSubmeshMaterial(int SubmeshIndex, FName MaterialSlotName, UMaterialInterface* Material);

	/*
	if SubmeshName is none uses OrSubmeshAsset
	if MaterialSlotName is none uses OrMaterialIndex
	*/
	UFUNCTION(BlueprintCallable, Category = "Allegro|Rendering")
	void SetSubmeshMaterial(FName SubmeshName, USkeletalMesh* OrSubmeshAsset, FName MaterialSlotName, int OrMaterialIndex, UMaterialInterface* Material);
	
	UFUNCTION(BlueprintCallable, Category="Allegro|Rendering")
	TArray<FName> GetSubmeshNames() const;


	template<typename TProc> void ForEachSubmeshOfInstance(int InstanceIndex, TProc Proc /*[](uint8 SubMeshIndex){}*/) const
	{
		const uint8* MeshSlotIter = GetInstanceMeshSlots(InstanceIndex);
		while (*MeshSlotIter != 0xFF)
		{
			Proc(*MeshSlotIter);
			MeshSlotIter++;
		}
	}

	//trace a ray against the specified instance (uses CompactPhysicsAsset). 
	//return -1 if no hit found, otherwise reference skeleton bone index of the hit
	UFUNCTION(BlueprintCallable, Category="Allegro|Utility")
	int LineTraceInstanceAny(int InstanceIndex, const FVector& Start, const FVector& End) const;
	
	UFUNCTION(BlueprintCallable, Category = "Allegro|Utility")
	int OverlapTestInstance(int InstanceIndex, const FVector& Point, float Thickness) const;

	//line trace the specified instance and return bone index of the closest shape hit
	UFUNCTION(BlueprintCallable, Category = "Allegro|Utility")
	int LineTraceInstanceSingle(int InstanceIndex, const FVector& Start, const FVector& End, float Thickness, double& OutTime, FVector& OutPosition, FVector& OutNormal) const;

	//line trace over the specified instances and return the instance index of the closest hit
	int LineTraceInstancesSingle(const TArrayView<int> InstanceIndices, const FVector& Start, const FVector& End, double Thickness, double& OutTime, FVector& OutPosition, FVector& OutNormal, int& OutBoneIndex) const;

	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Allegro|Utility", meta = (CustomStructureParam = "OutStruct", BlueprintInternalUseOnly=false, ExpandEnumAsExecs = "ExecResult", DisplayName="GetInstanceCustomStruct"))
	void K2_GetInstanceCustomStruct(EAllegroValidity& ExecResult, int InstanceIndex, int32& OutStruct);

	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Allegro|Utility", meta = (CustomStructureParam = "InStruct", BlueprintInternalUseOnly=false, DisplayName="SetInstanceCustomStruct"))
	void K2_SetInstanceCustomStruct(int InstanceIndex, const int32& InStruct);


	#pragma region dynamic pose

	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	bool EnableInstanceDynamicPose(int InstanceIndex, bool bEnable);
	
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	bool IsInstanceDynamicPose(int InstanceIndex) const { return InstanceHasAnyFlag(InstanceIndex, EAllegroInstanceFlags::EIF_DynamicPose); }

	void ReleaseDynamicPose_Internal(int InstanceIndex);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	void TieDynamicPoseToComponent(int InstanceIndex, USkeletalMeshComponent* SrcComponent, int UserData);
	UFUNCTION(BlueprintCallable, Category = "Allegro|Animation")
	void UntieDynamicPoseFromComponent(int InstanceIndex);

	FMatrix3x4* InstanceRequestDynamicPoseUpload(int InstanceIndex);

	void FillDynamicPoseFromComponents();
	void FillDynamicPoseFromComponents_Concurrent();


	UPROPERTY(BlueprintReadOnly, Transient, Category = "Allegro")
	TMap<int /*Dynamic Pose Index*/, FAllegroDynInsTieData> DynamicPoseInstancesTiedToSMC;

	USkeletalMeshComponent* GetInstanceTiedSkeletalMeshComponent(int InstanceIndex) const;

	#pragma endregion dynamic pose
	
	#pragma region Listener
	static const int MAX_LISTENER = 8;

	IAllegroListenerInterface* ListenersPtr[MAX_LISTENER + 1];
	int ListenersUserData[MAX_LISTENER];
	int NumListener;

	bool CanAddListener() const { return NumListener < MAX_LISTENER; }
	int FindListener(IAllegroListenerInterface* InterfacePtr) const;
	int FindListenerUserData(IAllegroListenerInterface* InterfacePtr) const;
	void RemoveListener(IAllegroListenerInterface* InterfacePtr);
	int AddListener(IAllegroListenerInterface* InterfacePtr, int UserData);
	#pragma endregion

	UPROPERTY(EditAnywhere, BlueprintReadWrite,Category = "Allegro")
	bool UseTaskMode = true;  

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	int NumPreTask = 100;

	void AddEvent(int InstanceIndex, const FAllegroAnimNotifyEvent& Notify);

	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void SetInstanceStencil(int InstanceIndex, int32 Stencil);

	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void SetAnimCollectionAndSkeletalMesh(UAllegroAnimCollection* asset, USkeletalMesh* InMesh = nullptr);

	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void SetNoAnimStaticMesh(UStaticMesh* InMesh);

	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void SetPreSkinPostionOffsetStaticMesh(int SlotIndex, UStaticMesh* InMesh);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro")
	bool UseGPUTransition = true;

	TArray<FAllegroInstanceAnimState::TickDeferredEvent>  ThreadSafeTickTempEvent;

	bool IsAttachment = false;

	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);

	UFUNCTION(BlueprintCallable, Category = "Allegro")
	void SetSpecialCustomDepthStencilValue(int Value) { SpecialCustomDepthStencilValue = Value; }

	int  SpecialCustomDepthStencilValue = 0;
private:
	
	TArray< TArray<FAllegroAnimNotifyEvent> > AnimationNotifyEventsTemp;

	
	DECLARE_FUNCTION(execK2_GetInstanceCustomStruct);
	DECLARE_FUNCTION(execK2_SetInstanceCustomStruct);
	

	int AllocateBlendFrameIndex();
	void FreeBlendFrameIndex(int index);

	uint32 FrameCounter = 0;
};



USTRUCT()
struct FAllegroComponentInstanceData : public FPrimitiveComponentInstanceData
{
	GENERATED_USTRUCT_BODY()

	FAllegroComponentInstanceData() = default;
	FAllegroComponentInstanceData(const UAllegroComponent* InComponent);
	
	bool ContainsData() const override { return true; }
	void ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase) override;
	void AddReferencedObjects(FReferenceCollector& Collector) override;

	FAllegroInstancesData InstanceData;
	FSpanAllocator IndexAllocator;
};
