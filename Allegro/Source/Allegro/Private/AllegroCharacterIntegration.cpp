// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroCharacterIntegration.h"
#include "AllegroActor.h"
#include "Async/ParallelFor.h"

ACharacter_AllegroBound::ACharacter_AllegroBound(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer.DoNotCreateDefaultSubobject(ACharacter::MeshComponentName))
{

}

void ACharacter_AllegroBound::BindToAllegro(UAllegroCharacterSyncComponent* InAllegroComponent)
{
	UnbindFromAllegro();

	if (IsValid(InAllegroComponent))
	{
		this->AllegroComponent = InAllegroComponent;
		this->InstanceIndex = InAllegroComponent->AddInstance(InAllegroComponent->MeshTransform * FTransform3f(this->GetActorTransform()));
		InAllegroComponent->CharacterActors[this->InstanceIndex] = this;
		
	}
}

void ACharacter_AllegroBound::UnbindFromAllegro()
{
	if (AllegroComponent.IsValid() && InstanceIndex != -1)
	{
		AllegroComponent->DestroyInstance(InstanceIndex);
		AllegroComponent = nullptr;
		InstanceIndex = -1;
		return;
	}
}

void ACharacter_AllegroBound::Destroyed()
{
	UnbindFromAllegro();
	Super::Destroyed();
}

UAllegroCharacterSyncComponent::UAllegroCharacterSyncComponent()
{
	MeshTransform = FTransform3f(FRotator3f(0, -90, 0), FVector3f(0, 0, -87));
}

void UAllegroCharacterSyncComponent::CustomInstanceData_Initialize(int InstanceIndex)
{
	this->CharacterActors[InstanceIndex] = nullptr;
}

void UAllegroCharacterSyncComponent::CustomInstanceData_Destroy(int InstanceIndex)
{
	this->CharacterActors[InstanceIndex] = nullptr;
}

void UAllegroCharacterSyncComponent::CustomInstanceData_Move(int DstIndex, int SrcIndex)
{
	this->CharacterActors[DstIndex] = MoveTemp(this->CharacterActors[SrcIndex]);
}

void UAllegroCharacterSyncComponent::CustomInstanceData_SetNum(int NewNum)
{
	this->CharacterActors.SetNum(NewNum);
}

void UAllegroCharacterSyncComponent::OnAnimationFinished(const TArray<FAllegroAnimFinishEvent>& Events)
{

}

void UAllegroCharacterSyncComponent::OnAnimationNotify(const TArray<FAllegroAnimNotifyEvent>& Events)
{

}

void UAllegroCharacterSyncComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	CopyTransforms();
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

void UAllegroCharacterSyncComponent::CopyTransforms()
{
	for (int InstanceIndex = 0; InstanceIndex < this->GetInstanceCount(); InstanceIndex++)
	{
		if (this->IsInstanceAlive(InstanceIndex))
		{
			ACharacter* Chr = CharacterActors[InstanceIndex].Get();
			if (Chr)
			{
				this->SetInstanceTransform(InstanceIndex, MeshTransform * FTransform3f(Chr->GetActorTransform()));
			}
// 			else
// 			{
// 				this->DestroyInstance(InstanceIndex);
// 			}
		}
	}
}


//-------------------------------------------------------------------------------

AAllegroTickManagerActor* gs_AllegroTickManagerActor = nullptr;
AAllegroTickManagerActor* AAllegroTickManagerActor::GetAllegroTickManagerActor()
{
	return gs_AllegroTickManagerActor;
}

AAllegroTickManagerActor::AAllegroTickManagerActor(const FObjectInitializer& ObjectInitializer)
{
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bAllowTickOnDedicatedServer = false;
	PrimaryActorTick.TickGroup = ETickingGroup::TG_PrePhysics;

	gs_AllegroTickManagerActor = this;
}

void AAllegroTickManagerActor::BeginPlay()
{
	Super::BeginPlay();
}

void AAllegroTickManagerActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	for (int i = 0; i < SortedActors.Num(); ++i)
	{
		delete SortedActors[i];
	}
	SortedActors.Reset();

	Super::EndPlay(EndPlayReason);
}

void AAllegroTickManagerActor::RegistActor(AAllegroActor* Actor, int Priority)
{
	FActorInfo* Info = nullptr;
	for (int i = 0; i < SortedActors.Num(); ++i)
	{
		if (SortedActors[i]->Actor == Actor)
		{
			Info = SortedActors[i];
			break;
		}
	}
	
	if (!Info)
	{
		Info = new FActorInfo();
		Info->Actor = Actor;

		SortedActors.Add(Info);
	}

	Info->Priority = Priority;

	Actor->BeTickedByOther = true;
	
	//sort
	SortedActors.Sort([](const FActorInfo& LHS, const FActorInfo& RHS)
		{
			return LHS.Priority < RHS.Priority;
		}
	);
}

void AAllegroTickManagerActor::UnRegistActor(AAllegroActor* Actor)
{
	for (int i = 0; i < SortedActors.Num(); ++i)
	{
		if (SortedActors[i]->Actor == Actor)
		{
			delete SortedActors[i];
			SortedActors.RemoveAt(i);
			break;
		}
	}
}


void AAllegroTickManagerActor::AttachTo(ALLEGRO_OBJECT* Src, int SrcInsIdx, ALLEGRO_OBJECT* Des, int DesInsIdx, const FString& BoneName, FTransform3f RelativeTrans)
{
	FActorInfo* Info = nullptr;
	for (int i = 0; i < SortedActors.Num(); ++i)
	{
#if USE_ACTOR_MAIN_ALLEGRO_COMP
		if (SortedActors[i]->Actor->MainAllegro == Des)
#else
		if (SortedActors[i]->Actor == Des)
#endif
		{
			Info = SortedActors[i];
			break;
		}
	}
	if (!Info)
	{
		return;
	}

	{

#if USE_ACTOR_MAIN_ALLEGRO_COMP
		UAllegroComponent::FSocketMinimalInfo BoneInfo = Des->GetSocketMinimalInfo(FName(*BoneName));
#else
		UAllegroComponent::FSocketMinimalInfo BoneInfo = Des->MainAllegro->GetSocketMinimalInfo(FName(*BoneName));
#endif
		if (BoneInfo.BoneIndex > -1)
		{
			FAttachmentInfo* AttachInfo = nullptr;
			for (auto& Attach : Info->AttachmentInfo)
			{
				if (Attach.DesInsIdx == DesInsIdx && Attach.BoneIndex == BoneInfo.BoneIndex)
				{
					AttachInfo = &Attach;
					break;
				}
			}

			if (AttachInfo)
			{
				FAttachmentInstance InstInfo;
				InstInfo.SrcInstActor = Src;
				InstInfo.SrcInstIdx = SrcInsIdx;
				AttachInfo->AttachmentInstance.Add(InstInfo);
			}
			else
			{
				FAttachmentInfo NewAttachInfo;
				NewAttachInfo.BoneIndex = BoneInfo.BoneIndex;
				NewAttachInfo.RelativeTrans = RelativeTrans;
				NewAttachInfo.DesInsIdx = DesInsIdx;

				FAttachmentInstance InstInfo;
				InstInfo.SrcInstActor = Src;
				InstInfo.SrcInstIdx = SrcInsIdx;

				NewAttachInfo.AttachmentInstance.Add(InstInfo);

				Info->AttachmentInfo.Add(NewAttachInfo);
			}
		}
	}
}

void AAllegroTickManagerActor::DetachFrom(ALLEGRO_OBJECT* Src, int SrcInsIdx, ALLEGRO_OBJECT* Des, int DesInsIdx, const FString& BoneName)
{
	FActorInfo* Info = nullptr;
	for (int i = 0; i < SortedActors.Num(); ++i)
	{
#if USE_ACTOR_MAIN_ALLEGRO_COMP
		if (SortedActors[i]->Actor->MainAllegro == Des)
#else
		if (SortedActors[i]->Actor == Des)
#endif
		{
			Info = SortedActors[i];
			break;
		}
	}
	if (!Info)
	{
		return;
	}

#if USE_ACTOR_MAIN_ALLEGRO_COMP
	UAllegroComponent::FSocketMinimalInfo BoneInfo = Des->GetSocketMinimalInfo(FName(*BoneName));
#else
	UAllegroComponent::FSocketMinimalInfo BoneInfo = Des->MainAllegro->GetSocketMinimalInfo(FName(*BoneName));
#endif
	if (BoneInfo.BoneIndex < 0)
	{
		return;
	}

	FAttachmentInfo* AttachInfo = nullptr;
	for (auto& Attach : Info->AttachmentInfo)
	{
		if (Attach.DesInsIdx == DesInsIdx && Attach.BoneIndex == BoneInfo.BoneIndex)
		{
			AttachInfo = &Attach;
			break;
		}
	}

	if (AttachInfo)
	{
		FAttachmentInstance InstInfo;
		InstInfo.SrcInstActor = Src;
		InstInfo.SrcInstIdx = SrcInsIdx;

		for (int i = 0; i < AttachInfo->AttachmentInstance.Num(); ++i)
		{
			auto& Inst = AttachInfo->AttachmentInstance[i];
			if (Inst.SrcInstActor == Src && Inst.SrcInstIdx == SrcInsIdx)
			{
				AttachInfo->AttachmentInstance.RemoveAtSwap(i);
				break;
			}
		}
	}
}

void AAllegroTickManagerActor::Tick(float DeltaTime)
{
	//tick
	for (auto& Ref : SortedActors)
	{
		//tick transfrom
		(Ref->Actor)->TickImple(DeltaTime);

		//update attachtment
		if (Ref->AttachmentInfo.Num() > 0)
		{
			ParallelFor(TEXT("ParallelTickForAttachment"), Ref->AttachmentInfo.Num(), 200, [&Ref](int Index) {
				
				auto& Info = Ref->AttachmentInfo[Index];
				if (Info.AttachmentInstance.Num() > 0)
				{
					if (Ref->Actor->MainAllegro->IsInstanceValid(Info.DesInsIdx))
					{
						FTransform3f Trans = Ref->Actor->GetInstanceBoneTransform(Info.DesInsIdx, Info.BoneIndex, &Info.RelativeTrans);

						for (auto& AttachInstance : Info.AttachmentInstance)
						{
#if USE_ACTOR_MAIN_ALLEGRO_COMP
							AttachInstance.SrcInstActor->SetInstanceTransform(AttachInstance.SrcInstIdx, Trans);
#else
							AttachInstance.SrcInstActor->MainAllegro->SetInstanceTransform(AttachInstance.SrcInstIdx, Trans);
#endif
						}
					}
				}
			});
		}
	}

	//这之后在处理Allegro的对象的添加和移除，上面的tick回调的事件里不要做这个actor和instance的增删的事情
	//notify game logic 
}

#if 0

void Test()
{
	//游戏中创建全局唯一的actor
	GetWorld()->SpawnActor<AAllegroTickManagerActor>();
	//...

	//注册Tick 时序
	//...
	GetAllegroTickManagerActor()->RegistActor(HorseActor_1, 100);
	GetAllegroTickManagerActor()->RegistActor(HorseActor_2, 100);
	//...
	GetAllegroTickManagerActor()->RegistActor(PawnActor_1, 200);
	GetAllegroTickManagerActor()->RegistActor(PawnActor_2, 200);
	//...

	//PawnActor_2 的 instace 10,挂到HorseActor_1 的 5 上的 Horseback骨骼,一下展示了2中接口使用方式
	//tick 中会保证顺序，自动设置instance的transform
#if USE_ACTOR_MAIN_ALLEGRO_COMP
	GetAllegroTickManagerActor()->AttachTo(PawComp_2,10,HorseComp_1,5,"Horseback");
#else
	GetAllegroTickManagerActor()->AttachTo(PawnActor_2,10,HorseActor_1,5,"Horseback");
#endif


};
#endif