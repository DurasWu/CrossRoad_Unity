// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroActor.h"
#include "AllegroAnimCollection.h"
#include "Async/ParallelFor.h"

FTransform3f ConvertFTransformType(const FTransform& InTransform)
{
	const auto& trans = InTransform.GetTranslation();
	const auto& rot = InTransform.GetRotation();
	const auto& scale = InTransform.GetScale3D();

	FTransform3f t;
	t.SetTranslation(FVector3f(trans.X, trans.Y, trans.Z));
	t.SetScale3D(FVector3f(scale.X, scale.Y, scale.Z));
	t.SetRotation(FQuat4f(rot.X, rot.Y, rot.Z, rot.W));
	return t;
}

FTransform ConvertFTransformType(const FTransform3f& InTransform)
{
	const auto& trans = InTransform.GetTranslation();
	const auto& rot = InTransform.GetRotation();
	const auto& scale = InTransform.GetScale3D();

	FTransform t;
	t.SetTranslation(FVector(trans.X, trans.Y, trans.Z));
	t.SetScale3D(FVector(scale.X, scale.Y, scale.Z));
	t.SetRotation(FQuat(rot.X, rot.Y, rot.Z, rot.W));
	return t;
}


FAttachmentInstance* FAttachmentInfo::GetAttachmentInstanceOrCreate(int32 MainInstanceIndex, bool Create, AAllegroActor* Actor)
{
	if (!AttachmentInstances.IsEmpty())
	{
		FAttachmentInstance** AttachmentInstance = AttachmentInstances.Find(MainInstanceIndex);
		if (AttachmentInstance)
		{
			return *AttachmentInstance;
		}
	}

	if (!Create)
		return nullptr;

	FAttachmentInstance* NewAttachmentInstance = new FAttachmentInstance();
	AttachmentInstances.Add(MainInstanceIndex, NewAttachmentInstance);

	auto& Ref = Actor->ParallelInfo.AddDefaulted_GetRef();
	Ref.BoneIndex = this->BoneIndex;
	Ref.RelativeTrans = &this->RelativeTrans;
	Ref.MainInstanceIndex = MainInstanceIndex;
	Ref.Attachments = NewAttachmentInstance;

	return NewAttachmentInstance;
}

void FAttachmentInfo::DeleteAttachmentInstance(int32 MainInstanceIndex, AAllegroActor* Actor)
{
	FAttachmentInstance** AttachmentInstance = AttachmentInstances.Find(MainInstanceIndex);
	if (AttachmentInstance)
	{
		for (int i = 0; i < Actor->ParallelInfo.Num(); ++i)
		{
			auto& Info = Actor->ParallelInfo[i];
			if (Info.Attachments == *(AttachmentInstance))
			{
				Actor->ParallelInfo.RemoveAtSwap(i);
				break;
			}
		}
		(*AttachmentInstance)->AttachAllegroInstances.Empty();
		delete* AttachmentInstance;
	}
	AttachmentInstances.Remove(MainInstanceIndex);
}

//--------------------------------------------------------------------

AAllegroActor::AAllegroActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bAllowTickOnDedicatedServer = false;
	PrimaryActorTick.TickGroup = ETickingGroup::TG_PrePhysics;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));;
	RootComponent->CreationMethod = EComponentCreationMethod::Native;

	MainAllegro = (CreateDefaultSubobject<UAllegroComponent>(TEXT("AllegroComponent_Main"), true));
	MainAllegro->CreationMethod = EComponentCreationMethod::Instance;
	MainAllegro->SetupAttachment(RootComponent);
}


void AAllegroActor::SetAnimCollectionAndSkeletalMesh(UAllegroAnimCollection* asset, USkeletalMesh* InMesh , int32 NumCustomDataFloats)
{
	MainAllegro->SetAnimCollectionAndSkeletalMesh(asset, InMesh);
	MainAllegro->NumCustomDataFloats = NumCustomDataFloats;
}

int AAllegroActor::AddSkeletalMesh(USkeletalMesh* InMesh)
{
	FAllegroSubmeshSlot Slot;
	Slot.SkeletalMesh = InMesh;
	int OldNum = MainAllegro->GetSubmeshCount();
	MainAllegro->AddSubmesh(Slot);
	int NewNum = MainAllegro->GetSubmeshCount();
	if (NewNum > OldNum && NewNum > 0)
	{
		return NewNum - 1;
	}
	return -1;
}

void AAllegroActor::SetNoAnimStaticMesh(UStaticMesh* InMesh, int32 NumCustomDataFloats)
{
	MainAllegro->SetNoAnimStaticMesh(InMesh);
	MainAllegro->NumCustomDataFloats = NumCustomDataFloats;
}

int AAllegroActor::AddStaticMesh(UStaticMesh* InMesh)
{
	FAllegroSubmeshSlot Slot;
	Slot.StaticMesh = InMesh;
	int OldNum = MainAllegro->GetSubmeshCount();
	MainAllegro->AddSubmesh(Slot);
	int NewNum = MainAllegro->GetSubmeshCount();
	if (NewNum > OldNum && NewNum > 0)
	{
		return NewNum - 1;
	}
	return -1;
}

int32 AAllegroActor::AddInstance(bool bIsSkeletalMesh, const FTransform& InstanceTransform)
{

	return MainAllegro->AddInstance(ConvertFTransformType(InstanceTransform));
}

bool AAllegroActor::InstanceAttachSubmeshByIndex(int InstanceIndex, uint8 MeshIndex, bool bAttach)
{
	return MainAllegro->InstanceAttachSubmeshByIndex(InstanceIndex, MeshIndex, bAttach);
}

bool AAllegroActor::RemoveInstance(int32 InstanceIndex)
{
	OnMainInstanceRemoved(InstanceIndex);
	return MainAllegro->DestroyInstance(InstanceIndex);
}

void AAllegroActor::RemoveInstances(const TArray<int32>& InstanceIndices)
{
	for (auto index : InstanceIndices)
	{
		OnMainInstanceRemoved(index);
	}
	MainAllegro->DestroyInstances(InstanceIndices);
}

void AAllegroActor::ClearInstances()
{
	RemoveAllAttachment();

	MainAllegro->ClearInstances(true);
}

void AAllegroActor::InstancePlayAnimation(int32 InstanceIndex, UAnimationAsset* Anim, bool bLooping, float StartAt, float TransitionDuration, float PlayScale)
{
	MainAllegro->InstancePlayAnimation(InstanceIndex, Anim, bLooping, StartAt, PlayScale, TransitionDuration);
}

void AAllegroActor::SetInstanceStencil(int32 InstanceIndex, int32 Stencil)
{
	MainAllegro->SetInstanceStencil(InstanceIndex, Stencil);
}

void AAllegroActor::BatchUpdateTransforms(int32 StartInstanceIndex, const TArray<FTransform>& NewTransforms)
{
	TArray<FTransform3f> Transfroms;
	Transfroms.Reset(NewTransforms.Num());
	for (auto Trans : NewTransforms)
	{
		auto& Ref = Transfroms.AddDefaulted_GetRef();
		Ref = ConvertFTransformType(Trans);
	}
	MainAllegro->BatchUpdateTransforms(StartInstanceIndex, Transfroms);
}

void AAllegroActor::SetInstanceTransform(int32 InstanceIndex, const FTransform& NewTransform)
{
	MainAllegro->SetInstanceTransform(InstanceIndex, ConvertFTransformType(NewTransform));
}

void AAllegroActor::SetInstanceBlendSpacePosition(int32 InstanceIndex, float InX, float InY)
{
	MainAllegro->SetInstanceBlendSpacePosition(InstanceIndex, InX, InY);
}

bool AAllegroActor::InstanceMontageJumpToSectionName(int32 InstanceIndex, const FString& SectionName, bool bEndOfSection)
{
	return MainAllegro->InstanceMontageJumpToSectionName(InstanceIndex, SectionName, bEndOfSection);
}

void AAllegroActor::SetInstanceCustomData(int32 InstanceIndex, int FloatIndex, float InValue)
{
	MainAllegro->SetInstanceCustomData(InstanceIndex, FloatIndex, InValue);
}

float AAllegroActor::GetInstanceCustomData(int32 InstanceIndex, int FloatIndex) const
{
	return MainAllegro->GetInstanceCustomData(InstanceIndex, FloatIndex);
}


void AAllegroActor::BeginPlay()
{
	Super::BeginPlay();

	MainAllegro->AddListener(this, 0);
}

void AAllegroActor::RemoveAllAttachment()
{
	for (auto& BindKV : AttachmentInfos)
	{
		FAttachmentInfo* Info = BindKV.Value;
		if (Info->AttachmentInstances.Num() > 0)
		{
			for (auto& InstanceKV : Info->AttachmentInstances)
			{
				FAttachmentInstance* AttachmentInstance = InstanceKV.Value;

				for (auto& AttachAllegroInstanceKV : AttachmentInstance->AttachAllegroInstances)
				{
					AttachAllegroInstanceKV.Key->DestroyInstance(AttachAllegroInstanceKV.Value);
				}
				AttachmentInstance->AttachAllegroInstances.Empty();

				delete AttachmentInstance;
			}
			Info->AttachmentInstances.Empty();
		}
		delete Info;
	}
	ParallelInfo.Empty();
	AttachmentInfos.Empty();
}

void AAllegroActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	MainAllegro->RemoveListener(this);

	RemoveAllAttachment();

	Super::EndPlay(EndPlayReason);
}


FAttachmentInfo* AAllegroActor::GetAttachmenInfoOrCreate(const FString& AttachName,const FString& BoneName, FTransform3f RelativeTrans, bool Create)
{

	if (!AttachmentInfos.IsEmpty())
	{
		FAttachmentInfo** AttachmentInfo = AttachmentInfos.Find(AttachName);
		if (AttachmentInfo)
		{
			return *AttachmentInfo;
		}
	}

	if (!Create)
		return nullptr;

	UAllegroComponent::FSocketMinimalInfo info = MainAllegro->GetSocketMinimalInfo(FName(*BoneName));
	if (info.BoneIndex > -1)
	{
		FAttachmentInfo* NewAttachmentInfo = new FAttachmentInfo();
		NewAttachmentInfo->BoneIndex = info.BoneIndex;
		NewAttachmentInfo->RelativeTrans = RelativeTrans;

		AttachmentInfos.Add(AttachName, NewAttachmentInfo);
		return NewAttachmentInfo;
	}
	
	return nullptr;
}

void AAllegroActor::SetNextTickActor(AAllegroActor* Actor)
{
	if (NextTickActor)
	{
		NextTickActor->BeTickedByOther = false;
	}

	NextTickActor = Actor;

	if (NextTickActor)
	{
		NextTickActor->BeTickedByOther = true;
	}
}

void AAllegroActor::OnAllInstanceTicked(int UserData)
{
	UpdateAttachmentTransfrom();
}

void AAllegroActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	
	if (BeTickedByOther)
	{
		return;
	}

	TickImple(DeltaTime);
}

void AAllegroActor::TickImple(float DeltaTime)
{

	MainAllegro->TickImpl(DeltaTime);

	for (auto& kv : AttachAllegros)
	{
		AttachmentAllegroInfo& Info = kv.Value;
		Info.Allegro->TickImpl(DeltaTime);
	}

	if (NextTickActor)
	{
		NextTickActor->TickImple(DeltaTime);
	}
}

FTransform3f AAllegroActor::GetInstanceBoneTransform(int InstanceIndex, int BoneIndex, FTransform3f* RelativeTrans, bool IsGameThread)
{
	const FTransform3f& BoneTransform = MainAllegro->GetInstanceBoneTransformCS(InstanceIndex, BoneIndex, IsGameThread);
	FTransform3f ResultTrans;
	if (!RelativeTrans)
	{
		ResultTrans = BoneTransform * MainAllegro->GetInstanceTransform(InstanceIndex);
	}
	else
	{
		ResultTrans = (*(RelativeTrans)) * BoneTransform * MainAllegro->GetInstanceTransform(InstanceIndex);
	}
	return ResultTrans;
}

void AAllegroActor::UpdateAttachmentTransfrom()
{
	UAllegroComponent* MainComponent = this->MainAllegro;

#if ALLEGRO_GPU_TRANSITION
	//nothing
#else
	//主线程串行
	if(MainComponent->AnimCollection)
	{
		for (auto& Ref : ParallelInfo)
		{
			if (MainComponent->IsInstanceValid(Ref.MainInstanceIndex)  )
			{
				//#TODO should we block here ?
				MainComponent->AnimCollection->ConditionalFlushDeferredTransitions(MainComponent->InstancesData.FrameIndices[Ref.MainInstanceIndex]);
			}
		}
	}
#endif


	TArray<FAttachmentParallelInfo>& ParalleInfos = ParallelInfo;
	if(ParalleInfos.Num() > 0)
	{
		//多线程并行
		ParallelFor(TEXT("ParallelForAttachment"), ParalleInfos.Num(), 300 , [MainComponent, &ParalleInfos](int Index) {
				auto& Ref = ParalleInfos[Index];
				if (!MainComponent->IsInstanceValid(Ref.MainInstanceIndex))
				{
					return;
				}

//#if ALLEGRO_ANIMTION_TICK_LOD  
//				FAllegroInstanceAnimState& State = MainComponent->InstancesData.AnimationStates[Ref.MainInstanceIndex];
//				if (!State.IsTicked())
//				{
//					return;
//				}
//#endif

				const FTransform3f& BoneTransform = MainComponent->GetInstanceBoneTransformCS(Ref.MainInstanceIndex, Ref.BoneIndex,false);
				FTransform3f ResultTrans = (*(Ref.RelativeTrans)) * BoneTransform * MainComponent->GetInstanceTransform(Ref.MainInstanceIndex);

				for (auto& Attachment : (Ref.Attachments)->AttachAllegroInstances)
				{
					Attachment.Key->SetInstanceTransform(Attachment.Value, ResultTrans);
				}
			});

	}
	
}

void AAllegroActor::OnMainInstanceRemoved(int32 MainInstanceIndex)
{
	for (auto& BindKV : AttachmentInfos)
	{
		FAttachmentInfo* Info = BindKV.Value;
		if (Info->AttachmentInstances.Num() > 0)
		{
			FAttachmentInstance** Attachment = Info->AttachmentInstances.Find(MainInstanceIndex);
			if (Attachment)
			{
				for (auto& AttachAllegroInstanceKV : (*Attachment)->AttachAllegroInstances)
				{
					AttachAllegroInstanceKV.Key->DestroyInstance(AttachAllegroInstanceKV.Value);
				}
				Info->DeleteAttachmentInstance(MainInstanceIndex, this);
			}
		}
	}
}

void AAllegroActor::RegistAttachment(const FString& AttachName, const FString& BoneName, FTransform3f RelativeTrans, UAllegroAnimCollection* asset, USkeletalMesh* InMesh, int32 NumCustomDataFloats)
{
	AttachmentAllegroInfo* Info = AttachAllegros.Find(AttachName);
	if (!Info)
	{
		FString AllegroName = "AllegroComponent_Attach_";
		AllegroName += AttachName;
	
		auto* AttachAllegro = NewObject<UAllegroComponent>(this);
		AttachAllegro->RegisterComponent();
		AttachAllegro->AttachToComponent(this->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);

		AttachAllegro->SetAnimCollectionAndSkeletalMesh(asset, InMesh);
		AttachAllegro->NumCustomDataFloats = NumCustomDataFloats;

		AttachAllegros.Add(AttachName, { AttachAllegro,BoneName, RelativeTrans });
	}
}

void AAllegroActor::RegistStaticMeshAttachment(const FString& AttachName, const FString& BoneName, FTransform3f RelativeTrans, UStaticMesh* InMesh , int32 NumCustomDataFloats)
{
	AttachmentAllegroInfo* Info = AttachAllegros.Find(AttachName);
	if (!Info)
	{
		FString AllegroName = "AllegroComponent_SM_Attach_";
		AllegroName += AttachName;

		auto* AttachAllegro = NewObject<UAllegroComponent>(this);
		AttachAllegro->RegisterComponent();
		AttachAllegro->AttachToComponent(this->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);

		AttachAllegro->SetNoAnimStaticMesh(InMesh);
		AttachAllegro->NumCustomDataFloats = NumCustomDataFloats;
		AttachAllegro->IsAttachment = true;
		AttachAllegros.Add(AttachName, { AttachAllegro,BoneName, RelativeTrans });
	}
}


int32 AAllegroActor::AddAttachInstance(const FString& AttachName, int32 MainInstanceIndex)
{
	int32 retIdx = -1;
	AttachmentAllegroInfo* Info = AttachAllegros.Find(AttachName);
	if (Info)
	{
		FAttachmentInfo* AttachmentInfo = GetAttachmenInfoOrCreate(AttachName,Info->BoneName,Info->RelativeTrans, true);
		if (AttachmentInfo)
		{
			FAttachmentInstance* AttachmentInstance = AttachmentInfo->GetAttachmentInstanceOrCreate(MainInstanceIndex, true,this);
			retIdx = Info->Allegro->AddInstance(FTransform3f());
			AttachmentInstance->AttachAllegroInstances.Add(Info->Allegro, retIdx);
		}
	}
	return retIdx;
}


void AAllegroActor::RemoveAttachedInstance(const FString& AttachName, int32 MainInstanceIndex)
{
	AttachmentAllegroInfo* Info = AttachAllegros.Find(AttachName);
	if (Info)
	{
		FAttachmentInfo* AttachmentInfo = GetAttachmenInfoOrCreate(AttachName,Info->BoneName, Info->RelativeTrans, false);
		if (AttachmentInfo)
		{
			FAttachmentInstance* AttachmentInstance = AttachmentInfo->GetAttachmentInstanceOrCreate(MainInstanceIndex, false,this);
			if (AttachmentInstance)
			{
				int* InstanceIndex = AttachmentInstance->AttachAllegroInstances.Find(Info->Allegro);
				if (InstanceIndex)
				{
					Info->Allegro->DestroyInstance(*InstanceIndex);
					AttachmentInstance->AttachAllegroInstances.Remove(Info->Allegro);
				}
				if (AttachmentInstance->AttachAllegroInstances.Num() == 0)
				{
					AttachmentInfo->DeleteAttachmentInstance(MainInstanceIndex,this);
				}
			}
		}
	}
}

void AAllegroActor::AttachedInstancePlayAnimation(const FString& AttachName,int32 InstanceIndex, UAnimationAsset* Anim, bool bLooping , float StartAt , float TransitionDuration , float PlayScale)
{
	AttachmentAllegroInfo* Info = AttachAllegros.Find(AttachName);
	if (Info)
	{
		(Info->Allegro)->InstancePlayAnimation(InstanceIndex, Anim, bLooping, StartAt, PlayScale, TransitionDuration);
	}
}

void  AAllegroActor::SetAttachedInstanceStencil(const FString& AttachName, int32 InstanceIndex, int32 Stencil)
{
	AttachmentAllegroInfo* Info = AttachAllegros.Find(AttachName);
	if (Info)
	{
		(Info->Allegro)->SetInstanceStencil(InstanceIndex, Stencil);
	}
}

void AAllegroActor::SetAttachedInstanceCustomData(const FString& AttachName, int32 InstanceIndex, int FloatIndex, float InValue)
{
	AttachmentAllegroInfo* Info = AttachAllegros.Find(AttachName);
	if (Info)
	{
		(Info->Allegro)->SetInstanceCustomData(InstanceIndex, FloatIndex, InValue);
	}
}

void AAllegroActor::SetPreSkinPostionOffsetStaticMesh(int SlotIndex, UStaticMesh* InMesh)
{
	MainAllegro->SetPreSkinPostionOffsetStaticMesh(SlotIndex, InMesh);
}


void AAllegroActor::SetShadowLOD(uint8 StartShadowLODBias, uint8 ShadowLODBias)
{
	MainAllegro->StartShadowLODBias = StartShadowLODBias;
	MainAllegro->ShadowLODBias = ShadowLODBias;
}


void AAllegroActor::SetAttachmentShadowLOD(const FString& AttachName, uint8 StartShadowLODBias, uint8 ShadowLODBias)
{
	AttachmentAllegroInfo* Info = AttachAllegros.Find(AttachName);
	if (Info)
	{
		(Info->Allegro)->StartShadowLODBias = StartShadowLODBias;
		(Info->Allegro)->ShadowLODBias = ShadowLODBias;
	}
}

void AAllegroActor::OnAnimationFinished(int UserData, const TArray<FAllegroAnimFinishEvent>& Events)
{
#if ALLEGRO_DEBUG
	for (auto Event : Events)
	{
		UE_LOG(LogAllegro, Log, TEXT("AAllegroActor::OnAnimationFinished(),index:%d"), Event.InstanceIndex);
	}
#endif
}

void AAllegroActor::OnAnimationNotify(int UserData, const TArray<FAllegroAnimNotifyEvent>& Events) 
{
#if ALLEGRO_DEBUG
	for (auto Event : Events)
	{
		UE_LOG(LogAllegro, Log, TEXT("AAllegroActor::OnAnimationNotify(),index:%d,EventName:%s"), Event.InstanceIndex, *(Event.NotifyName.ToString()));
	}
#endif
}

void AAllegroActor::SetSpecialCustomDepthStencilValue(int Value)
{
	MainAllegro->SetSpecialCustomDepthStencilValue(Value);

	for (auto& kv : AttachAllegros)
	{
		AttachmentAllegroInfo& Info = kv.Value;
		Info.Allegro->SetSpecialCustomDepthStencilValue(Value);
	}
}