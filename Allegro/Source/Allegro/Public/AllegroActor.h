
#pragma once


#include "AllegroComponent.h"
#include "AllegroActor.generated.h"



//utils function
ALLEGRO_API FTransform3f ConvertFTransformType(const FTransform& InTransform);
ALLEGRO_API FTransform ConvertFTransformType(const FTransform3f& InTransform);


struct FAttachmentInstance
{	
	TMap<UAllegroComponent*,int>     AttachAllegroInstances;    //原有逻辑，一种mesh在同一个骨骼下只挂一个
};

class AAllegroActor;
struct FAttachmentInfo
{
	int			 BoneIndex;
	FTransform3f RelativeTrans;

	TMap<int32,FAttachmentInstance*> AttachmentInstances;		//Key: MainSkelotIndex 

	FAttachmentInstance* GetAttachmentInstanceOrCreate(int32 AttachAllegroInstances, bool Create, AAllegroActor* Actor);

	void DeleteAttachmentInstance(int32 AttachAllegroInstances, AAllegroActor* Actor);
};


UCLASS(Blueprintable, BlueprintType)
class ALLEGRO_API AAllegroActor : public AActor , public IAllegroListenerInterface
{
	GENERATED_BODY()

public:

	AAllegroActor(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay()  override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetAnimCollectionAndSkeletalMesh(UAllegroAnimCollection* asset, USkeletalMesh* InMesh = nullptr, int32 NumCustomDataFloats = 0);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	int AddSkeletalMesh(USkeletalMesh* InMesh);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetNoAnimStaticMesh(UStaticMesh* InMesh, int32 NumCustomDataFloats = 0);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	int AddStaticMesh(UStaticMesh* InMesh);

	/** Add a mesh instance to this actor and returns an InstanceIndex. */
	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	int32 AddInstance(bool bIsSkeletalMesh, const FTransform& InstanceTransform);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	bool InstanceAttachSubmeshByIndex(int InstanceIndex, uint8 MeshIndex, bool bAttach);

	/** Remove a mesh instance by the InstanceIndex, returns if succeed. */
	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	bool RemoveInstance(int32 InstanceIndex);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void RemoveInstances(const TArray<int32>& InstanceIndices);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void ClearInstances();

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void InstancePlayAnimation(int32 InstanceIndex, UAnimationAsset* Anim, bool bLooping = true, float StartAt = 0.0f, float TransitionDuration = 0.0f,float PlayScale = 1);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetInstanceStencil(int32 InstanceIndex, int32 Stencil);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void BatchUpdateTransforms(int32 StartInstanceIndex, const TArray<FTransform>& NewTransforms);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetInstanceTransform(int32 InstanceIndex, const FTransform& NewTransform);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetInstanceBlendSpacePosition(int32 InstanceIndex, float InX, float InY = 0);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	bool InstanceMontageJumpToSectionName(int32 InstanceIndex, const FString& SectionName, bool bEndOfSection = false);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetInstanceCustomData(int32 InstanceIndex, int FloatIndex, float InValue);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	float GetInstanceCustomData(int32 InstanceIndex, int FloatIndex) const;

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void RegistAttachment(const FString& AttachName,const FString& BoneName, FTransform3f RelativeTrans, UAllegroAnimCollection* asset, USkeletalMesh* InMesh = nullptr, int32 NumCustomDataFloats = 0);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void RegistStaticMeshAttachment(const FString& AttachName, const FString& BoneName, FTransform3f RelativeTrans, UStaticMesh* InMesh , int32 NumCustomDataFloats = 0);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	int32 AddAttachInstance( const FString& AttachName,int32 MainInstanceIndex );

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void RemoveAttachedInstance( const FString& AttachName, int32 MainInstanceIndex);
	
	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void AttachedInstancePlayAnimation(const FString& AttachName,int32 InstanceIndex, UAnimationAsset* Anim, bool bLooping = true, float StartAt = 0.0f, float TransitionDuration = 0.0f, float PlayScale = 1);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetAttachedInstanceStencil(const FString& AttachName,int32 InstanceIndex, int32 Stencil);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetAttachedInstanceCustomData(const FString& AttachName,int32 InstanceIndex, int FloatIndex, float InValue);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetPreSkinPostionOffsetStaticMesh(int SlotIndex, UStaticMesh* InMesh);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetShadowLOD(uint8 StartShadowLODBias, uint8 ShadowLODBias);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetAttachmentShadowLOD(const FString& AttachName,uint8 StartShadowLODBias, uint8 ShadowLODBias);

	void Tick(float DeltaTime) override;

	void SetNextTickActor(AAllegroActor* Actor);

	FTransform3f GetInstanceBoneTransform(int InstanceIndex, int BoneIndex, FTransform3f* RelativeTrans=nullptr,bool IsGameThread=false);

	UFUNCTION(BlueprintCallable, Category = "AllegroActor")
	void SetSpecialCustomDepthStencilValue(int Value);

protected:
	
	void TickImple(float DeltaTime);


	virtual void CustomInstanceData_Initialize(int UserData, int InstanceIndex) override { }
	virtual void CustomInstanceData_Destroy(int UserData, int InstanceIndex) override {  }
	virtual void CustomInstanceData_Move(int UserData, int DstIndex, int SrcIndex)override {  }
	virtual void CustomInstanceData_SetNum(int UserData, int NewNum)override {  }

	virtual void OnAnimationFinished(int UserData, const TArray<FAllegroAnimFinishEvent>& Events) override;
	virtual void OnAnimationNotify(int UserData, const TArray<FAllegroAnimNotifyEvent>& Events) override;

	virtual void OnAllInstanceTicked(int UserData) override;

	FAttachmentInfo* GetAttachmenInfoOrCreate(const FString& AttachName, const FString& BoneName, FTransform3f RelativeTrans,bool Create);

	void UpdateAttachmentTransfrom();

	void OnMainInstanceRemoved(int32 MainInstanceIndex);

	void RemoveAllAttachment();

private:
	
	friend struct FAttachmentInfo;

	UAllegroComponent* MainAllegro;

	//FName: AttachmentName
	TMap<FString, FAttachmentInfo*>  AttachmentInfos;

	struct AttachmentAllegroInfo
	{
		UAllegroComponent* Allegro;
		FString			  BoneName;
		FTransform3f      RelativeTrans;
	};

	//FName: AttachmentName
	TMap<FString, AttachmentAllegroInfo>  AttachAllegros;

	struct FAttachmentParallelInfo
	{
		int			  BoneIndex;
		FTransform3f* RelativeTrans;
		int32		  MainInstanceIndex;
		FAttachmentInstance* Attachments;
	};

	TArray<FAttachmentParallelInfo> ParallelInfo;

	//---------------------------------------------------------
	AAllegroActor* NextTickActor = nullptr;
	bool		   BeTickedByOther = false;

	//---------------------------------------------------------
	friend class AAllegroTickManagerActor;


};
