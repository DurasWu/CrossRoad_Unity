// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

/*

*/

#pragma once

#include "AllegroComponent.h"

#include "GameFramework/Character.h"

#include "AllegroCharacterIntegration.generated.h"

class UAllegroCharacterSyncComponent;
class AAllegroActor;
class UAllegroComponent;

/*
ACharacter without USkeletalMeshComponent, uses UAllegroCharacterSyncComponent for rendering instead
*/
UCLASS()
class ACharacter_AllegroBound : public ACharacter
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadOnly, Category="Allegro|Character Integration")
	int InstanceIndex = -1;
	UPROPERTY(BlueprintReadOnly, Category="Allegro|Character Integration")
	TWeakObjectPtr<UAllegroCharacterSyncComponent> AllegroComponent;

	ACharacter_AllegroBound(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	UFUNCTION(BlueprintCallable, Category = "Allegro|Character Integration")
	void BindToAllegro(UAllegroCharacterSyncComponent* InAllegroComponent);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Character Integration")
	void UnbindFromAllegro();

	void Destroyed() override;
};

/*
copies transform from bound actors to the the instances
*/
UCLASS(meta = (BlueprintSpawnableComponent))
class UAllegroCharacterSyncComponent : public UAllegroComponent
{
	GENERATED_BODY()
public:
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Allegro|Character Integration")
	FTransform3f MeshTransform;

	TArray<TWeakObjectPtr<ACharacter_AllegroBound>> CharacterActors;

	UAllegroCharacterSyncComponent();

	void CustomInstanceData_Initialize(int InstanceIndex) override;
	void CustomInstanceData_Destroy(int InstanceIndex) override;
	void CustomInstanceData_Move(int DstIndex, int SrcIndex) override;
	void CustomInstanceData_SetNum(int NewNum) override;

	void OnAnimationFinished(const TArray<FAllegroAnimFinishEvent>& Events) override;
	void OnAnimationNotify(const TArray<FAllegroAnimNotifyEvent>& Events) override;

	int FlushInstances(TArray<int>* OutRemapArray) override
	{
		int Ret = Super::FlushInstances(OutRemapArray);
		
		for (int i = 0; i < CharacterActors.Num(); i++)
		{
			ACharacter_AllegroBound* Chr = CharacterActors[i].Get();
			if (Chr)
			{
				Chr->InstanceIndex = i;
			}
		}

		return Ret;
	}
	void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintCallable, Category = "Allegro|Character Integration")
	ACharacter_AllegroBound* GetCharacterActor(int InstanceIndex) const { return CharacterActors[InstanceIndex].Get(); }

	void CopyTransforms();
};


//上层使用接口是否直接用主体组件
#define USE_ACTOR_MAIN_ALLEGRO_COMP 1

#if USE_ACTOR_MAIN_ALLEGRO_COMP
#define ALLEGRO_OBJECT UAllegroComponent
#else
#define ALLEGRO_OBJECT AAllegroActor
#endif


UCLASS(Blueprintable, BlueprintType)
class ALLEGRO_API AAllegroTickManagerActor : public AActor
{
	GENERATED_BODY()

public:

	struct FBindInfo
	{
		FString			  BoneName;
		FTransform3f      RelativeTrans;
	};

	//全局只能创建一个
	static AAllegroTickManagerActor* GetAllegroTickManagerActor();

	AAllegroTickManagerActor(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay()  override;

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	void Tick(float DeltaTime) override;

	//注册AAllegroActor的tick优先级，优先级小的先tick
	void RegistActor(AAllegroActor* Actor, int Priority);

	void UnRegistActor(AAllegroActor* Actor);

	//AAllegroActor 之间的挂载，会在Tick中自动设置Transfrom，按使用需要，ALLEGRO_OBJECT可为acotr也可为主组件
	void AttachTo(ALLEGRO_OBJECT* Src, int SrcInsIdx, ALLEGRO_OBJECT* Des, int DesInsIdx, const FString& BoneName, FTransform3f RelativeTrans);

	//移除挂载
	void DetachFrom(ALLEGRO_OBJECT* Src, int SrcInsIdx, ALLEGRO_OBJECT* Des, int DesInsIdx, const FString& BoneName);

private:

	struct FAttachmentInstance
	{
		int				   SrcInstIdx=0;
		ALLEGRO_OBJECT*	   SrcInstActor=nullptr;
	};

	struct FAttachmentInfo
	{
		FTransform3f      RelativeTrans;
		int				  BoneIndex=0;
		int				  DesInsIdx = 0;

		TArray<FAttachmentInstance> AttachmentInstance;
	};

	struct FActorInfo
	{
		int   Priority = 0;
		AAllegroActor* Actor;  //DesActor
		TArray<FAttachmentInfo>  AttachmentInfo;
	};

	TArray<FActorInfo*>    SortedActors;

};