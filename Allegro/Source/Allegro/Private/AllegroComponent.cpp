// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroComponent.h"
#include "AllegroPrivate.h"
#include "AllegroRender.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "AllegroComponent.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Engine/SkeletalMeshSocket.h"
#include "DrawDebugHelpers.h"
#include "Misc/MemStack.h"
#include "AllegroAnimCollection.h"
#include "AnimationRuntime.h"
#include "Math/GenericOctree.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

#include "Chaos/Sphere.h"
#include "Chaos/Box.h"
#include "Chaos/Capsule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "AllegroPrivateUtils.h"
#include "ContentStreaming.h"
#include "AllegroObjectVersion.h"
#include "UObject/UObjectIterator.h"
#include "UnrealEngine.h"
#include "SceneInterface.h"
#include "Engine/World.h"
#include "RHICommandList.h"
#include "Animation/Skeleton.h"
#include "InstanceAnimStateExtend.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Engine/StaticMesh.h"

float GAllegro_LocalBoundUpdateInterval = 1 / 25.0f;
FAutoConsoleVariableRef CVar_LocalBoundUpdateInterval(TEXT("Allegro.LocalBoundUpdateInterval"), GAllegro_LocalBoundUpdateInterval, TEXT(""), ECVF_Default);


ALLEGRO_AUTO_CVAR_DEBUG(bool, DebugAnimations, false, "", ECVF_Default);
ALLEGRO_AUTO_CVAR_DEBUG(bool, DebugTransitions, false, "", ECVF_Default);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
FAutoConsoleCommand ConsoleCmd_PrintAllTransitions(TEXT("Allegro_DebugPrintTransitions"), TEXT(""), FConsoleCommandDelegate::CreateLambda([]() {
	
	for(UAllegroAnimCollection* AC : TObjectRange<UAllegroAnimCollection>())
	{
		FString A, B;
		AC->TransitionPoseAllocator.DebugPrint(A, B);
		UE_LOG(LogAllegro, Log, TEXT("%s"), *A);
		UE_LOG(LogAllegro, Log, TEXT("%s"), *B);
	}

}), ECVF_Default);
#endif


//same as FAnimationRuntime::AdvanceTime but we dont support negative play scale
inline ETypeAdvanceAnim AnimAdvanceTime(bool bAllowLooping, float MoveDelta, float& InOutTime, float EndTime)
{
	InOutTime += MoveDelta;

	if (InOutTime > EndTime)
	{
		if (bAllowLooping)
		{
			InOutTime = FMath::Fmod(InOutTime, EndTime);
			return ETAA_Looped;
		}
		else
		{
			// If not, snap time to end of sequence and stop playing.
			InOutTime = EndTime;
			return ETAA_Finished;
		}
	}
	return ETAA_Default;
}

bool FAllegroInstanceAnimState::NeedTick(int32 InstanceIndex, uint32 FrameCounter, float& Delta, FBoxCenterExtentFloat& IB, FMatrix* ViewProjection, FVector* ViewLocation, float LODScale)
{
#if	ALLEGRO_ANIMTION_TICK_LOD
	FVector4 Origin(IB.Center.X, IB.Center.Y, IB.Center.Z, 0);
	float SphereRadius = FMath::Max(FMath::Max(IB.Extent.Y, IB.Extent.Z), IB.Extent.X);
	FVector4 ViewOrigin(ViewLocation->X, ViewLocation->Y, ViewLocation->Z, 0);

	float ScreenRadiusSquared = ComputeBoundsScreenRadiusSquared(Origin, SphereRadius, ViewOrigin,*ViewProjection) * LODScale * LODScale;

	uint8 FrameTickInter = 1;
	AnimtioneLOD = 0;
	if (ScreenRadiusSquared < 0.001f)
	{
		FrameTickInter = 5;
		AnimtioneLOD = 3;
	}
	else if (ScreenRadiusSquared < 0.005f)
	{
		FrameTickInter = 3;
		AnimtioneLOD = 2;
	}
	else if (ScreenRadiusSquared < 0.02f)
	{
		FrameTickInter = 2;
		AnimtioneLOD = 1;
	}

	if (FrameTickInter == 1 || (InstanceIndex + FrameCounter) % FrameTickInter == 0)
	{
		Delta += DeltaTimeAccumulate;
		DeltaTimeAccumulate = 0.0f;
		return true;
	}
	DeltaTimeAccumulate += Delta;
	return false;
#else
	return true;
#endif
}

bool FAllegroInstanceAnimState::IsTicked()
{
#if ALLEGRO_ANIMTION_TICK_LOD
	return DeltaTimeAccumulate < 0.001f;
#else
	return true;
#endif
}

void FAllegroInstanceAnimState::Tick(UAllegroComponent* Owner, int32 InstanceIndex, float Delta,uint32 FrameCounter, FMatrix* ViewProjection, FVector* ViewLocation, float LODScale){
	UAllegroAnimCollection* AnimCollection = Owner->AnimCollection;
	EAllegroInstanceFlags& Flags = Owner->InstancesData.Flags[InstanceIndex];
	TickDeferredEvent& DeferredEvent = Owner->ThreadSafeTickTempEvent[InstanceIndex];
	DeferredEvent.FinishedSequence = nullptr;
	DeferredEvent.FinishedTransitionIdx = -1;
	DeferredEvent.GPUTransitionFinished = 0;

	if (EnumHasAnyFlags(Flags, EAllegroInstanceFlags::EIF_Destroyed | EAllegroInstanceFlags::EIF_AnimPaused | EAllegroInstanceFlags::EIF_AnimNoSequence | EAllegroInstanceFlags::EIF_DynamicPose))
		return;

	if (EnumHasAnyFlags(Flags, EAllegroInstanceFlags::EIF_AnimSkipTick))
	{
		EnumRemoveFlags(Flags, EAllegroInstanceFlags::EIF_AnimSkipTick);
		return;
	}

#if ALLEGRO_ANIMTION_TICK_LOD
	if (ViewProjection)
	{
		FBoxCenterExtentFloat IB = Owner->InstancesData.LocalBounds[InstanceIndex].TransformBy(Owner->InstancesData.Matrices[InstanceIndex]);
		if (!NeedTick(InstanceIndex, FrameCounter, Delta, IB,ViewProjection, ViewLocation, LODScale))
		{
			return;
		}
	}
#endif

	float OldTime = Time;
	float NewDelta = PlayScale * Delta;
	FAllegroSequenceDef* ActiveSequenceStruct = nullptr;
	ETypeAdvanceAnim result = ETAA_Default;

	if (AssetType == EAnimAssetType::AnimSequeue)
	{
		ActiveSequenceStruct = &(AnimCollection->Sequences[CurrentSequence]);
		if (EnumHasAnyFlags(Flags, EAllegroInstanceFlags::EIF_AnimFinished))
		{
			EnumRemoveFlags(Flags, EAllegroInstanceFlags::EIF_AnimFinished);
			EnumAddFlags(Flags, EAllegroInstanceFlags::EIF_AnimNoSequence);

			//线程不安全
			//Owner->AnimationFinishEvents.Add(FAllegroAnimFinishEvent{ InstanceIndex, ActiveSequenceStruct->Sequence });
			DeferredEvent.FinishedSequence = ActiveSequenceStruct->Sequence;

			return;
		}

		const bool bShouldLoop = EnumHasAnyFlags(Flags, EAllegroInstanceFlags::EIF_AnimLoop);
		result = AnimAdvanceTime(bShouldLoop, NewDelta, Time, ActiveSequenceStruct->GetSequenceLength());

		//get notifications
		if (ActiveSequenceStruct->Notifies.Num() != 0)
		{
			if (result == ETAA_Looped)
			{
				for (int i = ActiveSequenceStruct->Notifies.Num() - 1; i >= 0; i--)
				{
					const FAllegroSimpleAnimNotifyEvent& Notify = ActiveSequenceStruct->Notifies[i];
					if ((OldTime < Notify.Time) || (Time >= Notify.Time))
					{
						//线程不安全
						//Owner->AnimationNotifyEvents.Add(FAllegroAnimNotifyEvent{ InstanceIndex, ActiveSequenceStruct.Sequence, Notify.Name });
						Owner->AddEvent(InstanceIndex, FAllegroAnimNotifyEvent{ InstanceIndex, ActiveSequenceStruct->Sequence, Notify.Name,Notify.Notify });
					}
				}
			}
			else
			{
				for (int i = 0; i < ActiveSequenceStruct->Notifies.Num(); i++)
				{
					const FAllegroSimpleAnimNotifyEvent& Notify = ActiveSequenceStruct->Notifies[i];
					if (Time >= Notify.Time && OldTime < Notify.Time)
					{
						//线程不安全
						//Owner->AnimationNotifyEvents.Add(FAllegroAnimNotifyEvent{ InstanceIndex, ActiveSequenceStruct.Sequence, Notify.Name });
						Owner->AddEvent(InstanceIndex, FAllegroAnimNotifyEvent{ InstanceIndex,ActiveSequenceStruct->Sequence, Notify.Name,Notify.Notify });
					}
				}
			}
		}
	}
	else if(AssetType == EAnimAssetType::AnimMontage || AssetType == EAnimAssetType::AnimBlendSpace)
	{
		ActiveSequenceStruct = &(AnimCollection->Sequences[CurrentSequence]);

		if (EnumHasAnyFlags(Flags, EAllegroInstanceFlags::EIF_AnimFinished))
		{
			EnumRemoveFlags(Flags, EAllegroInstanceFlags::EIF_AnimFinished);
			EnumAddFlags(Flags, EAllegroInstanceFlags::EIF_AnimNoSequence);
			return;
		}

		this->Time += NewDelta;

		InstanceExtend->Update(NewDelta);
		InstanceExtend->PostUpdate(InstanceIndex, Owner);

		if (!InstanceExtend->IsPlaying())
		{
			EnumAddFlags(Flags, EAllegroInstanceFlags::EIF_AnimFinished);
		}
	}

	//trans handle
	int LocalFrameIndex = static_cast<int>(Time * ActiveSequenceStruct->SampleFrequencyFloat);
	//check(LocalFrameIndex < ActiveSequenceStruct->AnimationFrameCount);
	if (LocalFrameIndex >= ActiveSequenceStruct->AnimationFrameCount)
	{
		LocalFrameIndex = ActiveSequenceStruct->AnimationFrameCount - 1;
	}

	//transition
	if (Owner->UseGPUTransition)
	{
		//new transition
		if (EnumHasAnyFlags(Flags, EAllegroInstanceFlags::EIF_GPUTransition))
		{
			FAllegroSequenceDef* BeginSequenceStruct = &(AnimCollection->Sequences[GPUTransInfo.StartAnimSeqIndex]);
			GPUTransInfo.Time += NewDelta;
			int LocalFrame = GPUTransInfo.Time * BeginSequenceStruct->SampleFrequencyFloat;
			
			if (LocalFrame + GPUTransInfo.BeginFrameIndex >= GPUTransInfo.EndFrameIndex)
			{
				DeferredEvent.GPUTransitionFinished = 1;
			}
			else
			{
				//先简单实现一版
				const float TransitionAlpha = (LocalFrame + 1) / static_cast<float>(GPUTransInfo.EndFrameIndex - GPUTransInfo.BeginFrameIndex + 1);
				check(TransitionAlpha != 0 && TransitionAlpha != 1); //not having any blend is waste
				const float FinalAlpha = FAlphaBlend::AlphaToBlendOption(TransitionAlpha, GPUTransInfo.BlendOption);

				int32 DataIndex = Owner->InstancesData.BlendFrameInfoIndex[InstanceIndex];
				if (DataIndex > 0)
				{
					FInstanceBlendFrameInfo& BlendInfo = Owner->InstancesData.BlendFrameInfo[DataIndex];
					BlendInfo.Weight[0] = FinalAlpha;
					BlendInfo.Weight[1] = 1 - FinalAlpha;
					BlendInfo.Weight[2] = 0.0f;
					BlendInfo.Weight[3] = 0.0f;

					BlendInfo.FrameIndex[0] = LocalFrame + GPUTransInfo.BeginFrameIndex;
				}
			}
		}
	}
	else if (EnumHasAnyFlags(Flags, EAllegroInstanceFlags::EIF_AnimPlayingTransition))
	{
		//old transition
		const UAllegroAnimCollection::FTransition& Transition = AnimCollection->Transitions[this->TransitionIndex];
		check((Transition.ToFI + Transition.FrameCount) <= ActiveSequenceStruct->AnimationFrameCount);
		if((LocalFrameIndex >= (Transition.ToFI + Transition.FrameCount)) || result != ETAA_Default)
		{
			EnumRemoveFlags(Flags, EAllegroInstanceFlags::EIF_AnimPlayingTransition);
			
			//线程不安全
			//AnimCollection->DecTransitionRef(this->TransitionIndex);
			DeferredEvent.FinishedTransitionIdx = this->TransitionIndex;
		}
		else
		{
			int TransitionLFI = LocalFrameIndex - Transition.ToFI;
			check(TransitionLFI < Transition.FrameCount);
			Owner->InstancesData.FrameIndices[InstanceIndex] = Transition.FrameIndex + TransitionLFI;
			return;
		}
	}
	
	//not trans handle
	if (AssetType == EAnimAssetType::AnimSequeue)
	{
		int GlobalFrameIndex = ActiveSequenceStruct->AnimationFrameIndex + LocalFrameIndex;
		check(GlobalFrameIndex < AnimCollection->FrameCountSequences);
		Owner->InstancesData.FrameIndices[InstanceIndex] = GlobalFrameIndex;
		if (result == ETAA_Finished)
		{
			check(LocalFrameIndex == ActiveSequenceStruct->AnimationFrameCount - 1);
			//we don't finish right away, because current frame must be rendered
			EnumAddFlags(Flags, EAllegroInstanceFlags::EIF_AnimFinished);
		}
	}
	else if (AssetType == EAnimAssetType::AnimMontage || AssetType == EAnimAssetType::AnimBlendSpace)
	{
		const FAllegroSequenceDef& ActiveSequence = AnimCollection->Sequences[InstanceExtend->CurrentSequence];
		int LocalIndex = static_cast<int>(InstanceExtend->Time * ActiveSequence.SampleFrequencyFloat);
	
		//check(LocalIndex < ActiveSequence.AnimationFrameCount);
		if (LocalIndex >= ActiveSequence.AnimationFrameCount)
		{
			LocalIndex = ActiveSequence.AnimationFrameCount - 1;
		}

		int GlobalIndex = ActiveSequence.AnimationFrameIndex + LocalIndex;
		Owner->InstancesData.FrameIndices[InstanceIndex] = GlobalIndex;
		this->CurrentSequence = InstanceExtend->CurrentSequence;
	}

	if (Owner->UseGPUTransition)
	{
		if (EnumHasAllFlags(Flags, EAllegroInstanceFlags::EIF_GPUTransition | EAllegroInstanceFlags::EIF_AnimFinished))
		{
			DeferredEvent.GPUTransitionFinished = 1;
		}
	}
}


void FAllegroInstanceAnimState::SetCurrentAnimAsset(TObjectPtr<UAnimationAsset> Asset, EAnimAssetType Type)
{
	/*if (!Asset.IsValid())
	{
		AssetType = EAnimAssetType::AnimNull;
		return;
	}*/
	CurrentAnimAsset = Asset;
	AssetType = Type;
}

void FAllegroInstanceAnimState::ResetAnimState()
{
	CurrentAnimAsset = nullptr;
	AssetType = EAnimAssetType::AnimNull;

	if (InstanceExtend)
	{
		delete InstanceExtend;
		InstanceExtend = nullptr;
	}
}

FInstanceAnimStateExtend* FAllegroInstanceAnimState::CreateNewInstanceExtend()
{
	if (InstanceExtend)
	{
		delete InstanceExtend;
		InstanceExtend = nullptr;
	}
	InstanceExtend = new FInstanceAnimStateExtend();
	return InstanceExtend;
}

void FAllegroInstanceAnimState::AddReferencedObjects(FReferenceCollector& Collector)
{
	if (CurrentAnimAsset)
	{
		Collector.AddReferencedObject(CurrentAnimAsset);
	}
}

//---------------------------------------------------------------------------------------------------------------

namespace Utils
{
	template<bool bGlobal> int TransitionFrameRangeToSeuqnceFrameRange(const FAllegroInstanceAnimState& AS, int FrameIndex /*frame index in transition range*/, const UAllegroAnimCollection* AnimCollection)
	{
		checkSlow(AnimCollection->IsTransitionFrameIndex(FrameIndex));
		const UAllegroAnimCollection::FTransition& Transition = AnimCollection->Transitions[AS.TransitionIndex];
		const int TransitionFrameIndex = FrameIndex - Transition.FrameIndex;
		check(TransitionFrameIndex < Transition.FrameCount);
		const FAllegroSequenceDef& SeqDef = AnimCollection->Sequences[AS.CurrentSequence];
		const int SeqFrameIndex = Transition.ToFI + TransitionFrameIndex;
		check(SeqFrameIndex < SeqDef.AnimationFrameCount);
		return bGlobal ? SeqDef.AnimationFrameIndex + SeqFrameIndex : SeqFrameIndex;
	}
};





UAllegroComponent::UAllegroComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false; // true;

	//PrimaryComponentTick.bStartWithTickEnabled = true;
	//PrimaryComponentTick.bAllowTickOnDedicatedServer = false;
	//PrimaryComponentTick.TickGroup = ETickingGroup::TG_PrePhysics;
	
	InstanceMaxDrawDistance = 0;
	InstanceMinDrawDistance = 0;

	bCanEverAffectNavigation = false;
	
	Mobility = EComponentMobility::Movable;
	bCastStaticShadow = false;
	bCastDynamicShadow = true;
	bWantsInitializeComponent = true;
	bComputeFastLocalBounds = true;

	//SortMode = EAllegroInstanceSortMode::SortTranslucentOnly;

	LODDistances[0] = 1000;
	LODDistances[1] = 3000;
	LODDistances[2] = 8000;
	LODDistances[3] = 14000;
	LODDistances[4] = 20000;
	LODDistances[5] = 40000;
	LODDistances[6] = 80000;
	
	LODDistanceScale = 1;

	//MinLODIndex = 0;
	//MaxLODIndex = 0xFF;

	AnimationPlayRate = 1;
	MaxMeshPerInstance = 4;

}

void UAllegroComponent::PostApplyToComponent()
{
	Super::PostApplyToComponent();
}

void UAllegroComponent::OnComponentCreated()
{
	Super::OnComponentCreated();
}

void UAllegroComponent::FixInstanceData()
{
	InstancesData.Matrices.SetNum(InstancesData.Locations.Num());
	//InstancesData.RenderMatrices.SetNum(InstancesData.Locations.Num());
	InstancesData.FrameIndices.SetNum(InstancesData.Locations.Num());
	InstancesData.RenderCustomData.SetNumZeroed(InstancesData.Locations.Num() * NumCustomDataFloats);
	InstancesData.MeshSlots.SetNumZeroed(InstancesData.Locations.Num() * (MaxMeshPerInstance + 1));

	for (int InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)
	{
		if (IsInstanceAlive(InstanceIndex))
			OnInstanceTransformChange(InstanceIndex);
	}

	for (int InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)
	{
		if (IsInstanceAlive(InstanceIndex))
		{
			FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
			bool bValidSeq = (AS.IsValid() && AnimCollection->Sequences.IsValidIndex(AS.CurrentSequence) && AnimCollection->Sequences[AS.CurrentSequence].Sequence);
			if (!bValidSeq)
			{
				AS = FAllegroInstanceAnimState();
			}
		}
	}

	CalcAnimationFrameIndices();

	if (ShouldUseFixedInstanceBound())
	{
		InstancesData.LocalBounds.Empty();
	}
	else
	{
		InstancesData.LocalBounds.SetNum(InstancesData.Locations.Num());
		UpdateLocalBounds();
	}

	//for (FArrayProperty* Arr : GetBPInstanceDataArrays())
	//{
	//	FScriptArrayHelper Helper(Arr, Arr->GetPropertyValuePtr_InContainer(this));
	//	Helper.Resize(InstancesData.Locations.Num());
	//}

}

#if WITH_EDITOR

void UAllegroComponent::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	FName PrpName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : FName();

	if (PrpName == GET_MEMBER_NAME_CHECKED(UAllegroComponent, AnimCollection))
	{
		UAllegroAnimCollection* CurAC = AnimCollection;
		AnimCollection = nullptr;
		SetAnimCollection(CurAC);
	}

	if (PrpName == GET_MEMBER_NAME_CHECKED(UAllegroComponent, Submeshes))
	{
		
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);

	MarkRenderStateDirty();
}

void UAllegroComponent::PreEditChange(FProperty* PropertyThatWillChange)
{
	Super::PreEditChange(PropertyThatWillChange);

	if (PropertyThatWillChange && PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(UAllegroComponent, AnimCollection))
	{
		SetAnimCollection(nullptr);
	}

	if (PropertyThatWillChange && PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(UAllegroComponent, Submeshes))
	{
		ResetMeshSlots();
	}
}

bool UAllegroComponent::CanEditChange(const FProperty* InProperty) const
{
	if (!Super::CanEditChange(InProperty))
		return false;
	
	// Specific logic associated with "MyProperty"
	const FName PropertyName = InProperty->GetFName();

	return true;
}

#endif


void UAllegroComponent::ApplyWorldOffset(const FVector& InOffset, bool bWorldShift)
{
	Super::ApplyWorldOffset(InOffset, bWorldShift);

	for (int InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)
	{
		if(IsInstanceAlive(InstanceIndex))
			AddInstanceLocation(InstanceIndex, FVector3f(InOffset));
	}
}


void UAllegroComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	TickImpl(DeltaTime);
}

void UAllegroComponent::TickImpl(float DeltaTime)
{

	if(GetAliveInstanceCount() > 0 )
	{
		if (!bIgnoreAnimationsTick)
		{
			if(AnimCollection)
				this->TickAnimations(DeltaTime);
		}

		if(IsVisible() && IsRenderStateCreated() && this->SceneProxy)
		{
			MarkRenderTransformDirty();
		}
	}

	{
		if (AnimationNotifyEvents.Num())
		{
			CallOnAnimationNotify();
		}

		if (AnimationFinishEvents.Num())
		{
			CallOnAnimationFinished();
		}

		AnimationFinishEvents.Reset();
		AnimationNotifyEvents.Reset();

		FillDynamicPoseFromComponents_Concurrent();
	}


#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (GAllegro_DebugAnimations || GAllegro_DebugTransitions)
	{
		for (int InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)
		{
			if (!IsInstanceAlive(InstanceIndex))
				continue;

			if(GAllegro_DebugAnimations)
			{
				FAllegroInstanceAnimState& state = InstancesData.AnimationStates[InstanceIndex];
				FString Str = FString::Printf(TEXT("Time:%f, FrameIndex:%d"), state.Time, (int)InstancesData.FrameIndices[InstanceIndex]);
				DrawDebugString(GetWorld(), FVector(GetInstanceLocation(InstanceIndex)), Str, nullptr, FColor::Green, 0, false, 2);
			}
					
		}
	}
#endif

}

void UAllegroComponent::SendRenderTransform_Concurrent()
{
	//DoDeferredRenderUpdates_Concurrent is not override-able so we are doing all of our end of frame computes here. its not just transform. 

	ALLEGRO_SCOPE_CYCLE_COUNTER(SendRenderTransform_Concurrent);

	FBoxSphereBounds OriginalBounds = this->Bounds;

	FAllegroProxy* SKProxy = static_cast<FAllegroProxy*>(this->SceneProxy);

	// If the primitive isn't hidden update its transform.
	const bool bDetailModeAllowsRendering = DetailMode <= GetCachedScalabilityCVars().DetailMode;
	if (SKProxy && bDetailModeAllowsRendering && (ShouldRender() || bCastHiddenShadow || bAffectIndirectLightingWhileHidden || bRayTracingFarField))
	{
		
		FAllegroDynamicData* DynamicData = GenerateDynamicData_Internal();
		//no need to call UpdateBounds because we have a calculated bound now
		this->Bounds = FBoxSphereBounds(DynamicData->CompBound.ToBoxSphereBounds());
		check(!this->Bounds.ContainsNaN());

		// Update the scene info's transform for this primitive.
		GetWorld()->Scene->UpdatePrimitiveTransform(this);
		
		//send dynamic data to render thread proxy
		ENQUEUE_RENDER_COMMAND(Allegro_SendRenderDynamicData)([=](FRHICommandListImmediate& RHICmdList) {
			SKProxy->SetDynamicDataRT(DynamicData);
		});
	}
	else
	{
		UpdateBounds();
	}

	UActorComponent::SendRenderTransform_Concurrent();

#if WITH_EDITOR //copied from UPrimitiveComponent::UpdateBounds()
	if (IsRegistered() && (GetWorld() != nullptr) && !GetWorld()->IsGameWorld() && (!OriginalBounds.Origin.Equals(Bounds.Origin) || !OriginalBounds.BoxExtent.Equals(Bounds.BoxExtent)))
	{
		if (!bIgnoreStreamingManagerUpdate && !bHandledByStreamingManagerAsDynamic && bAttachedToStreamingManagerAsStatic)
		{
			FStreamingManagerCollection* Collection = IStreamingManager::Get_Concurrent();
			if (Collection)
			{
				Collection->NotifyPrimitiveUpdated_Concurrent(this);
			}
		}
	}
#endif

}

void UAllegroComponent::CreateRenderState_Concurrent(FRegisterComponentContext* Context)
{
	Super::CreateRenderState_Concurrent(Context);
	SendRenderTransform_Concurrent();
	
}

void UAllegroComponent::PostLoad()
{
	Super::PostLoad();

	if(SkeletalMesh_DEPRECATED && GetClass() /*!= UAllegroComponent::StaticClass() && !this->HasAnyFlags(RF_ClassDefaultObject)*/)
	{
		if(Submeshes.Num() == 0)
			Submeshes.AddDefaulted();

		if(Submeshes[0].SkeletalMesh == nullptr)
			Submeshes[0].SkeletalMesh = SkeletalMesh_DEPRECATED;

		SkeletalMesh_DEPRECATED = nullptr;
	}
}

void UAllegroComponent::BeginDestroy()
{
	ClearInstances(true);
	Super::BeginDestroy();
}

void UAllegroComponent::PostInitProperties()
{
	Super::PostInitProperties();


}

void UAllegroComponent::PostCDOContruct()
{
	Super::PostLoad();

	//for (FProperty* PropertyIter : TFieldRange<FProperty>(this->GetClass(), EFieldIteratorFlags::IncludeSuper))
	//{
	//	if (FArrayProperty* ArrayPrp = CastField<FArrayProperty>(PropertyIter))
	//	{
	//		if (!ArrayPrp->IsNative())
	//		{
	//			if (ArrayPrp->GetName().StartsWith(TEXT("InstanceData_")))
	//			{
	//				InstanceDataArrays.AddUnique(ArrayPrp);
	//			}
	//		}
	//	}
	//}
}

void UAllegroComponent::OnVisibilityChanged()
{
	Super::OnVisibilityChanged();
	MarkRenderTransformDirty();
}

void UAllegroComponent::OnHiddenInGameChanged()
{
	Super::OnHiddenInGameChanged();
	MarkRenderTransformDirty();
}

void UAllegroComponent::BeginPlay()
{
	Super::BeginPlay();

	NumAliveBlendFrame = 0;
	int idx = BlendFrameIndexAllocator.Allocate();
	FInstanceBlendFrameInfo& Info = InstancesData.BlendFrameInfo.AddDefaulted_GetRef();
	Info.Weight[0] = 1;
	Info.Weight[1] = 0;
	Info.Weight[2] = 0;
	Info.Weight[3] = 0;
	Info.FrameIndex[0] = 0;
}


void UAllegroComponent::EndPlay(EEndPlayReason::Type Reason)
{
	ClearInstances(true);
	Super::EndPlay(Reason);
}

int32 UAllegroComponent::GetNumMaterials() const
{
	int Counter = 0;
	for (const FAllegroSubmeshSlot& MeshSlot : Submeshes)
	{
		if (MeshSlot.SkeletalMesh)
		{
			Counter += MeshSlot.SkeletalMesh->GetMaterials().Num();
		}
		else if (MeshSlot.StaticMesh)
		{
			Counter += MeshSlot.StaticMesh->GetStaticMaterials().Num();
		}
	}
	return Counter;
}

UMaterialInterface* UAllegroComponent::GetMaterial(int32 MaterialIndex) const
{
	if (OverrideMaterials.IsValidIndex(MaterialIndex) && OverrideMaterials[MaterialIndex])
		return OverrideMaterials[MaterialIndex];

	for (const FAllegroSubmeshSlot& MeshSlot : Submeshes)
	{
		if (MeshSlot.SkeletalMesh)
		{
			const TArray<FSkeletalMaterial>& Mats = MeshSlot.SkeletalMesh->GetMaterials();
			if (MaterialIndex >= Mats.Num())
			{
				MaterialIndex -= Mats.Num();
			}
			else
			{
				return Mats[MaterialIndex].MaterialInterface;
			}
		}
		else if (MeshSlot.StaticMesh)
		{
			const TArray<FStaticMaterial>& Mats = MeshSlot.StaticMesh->GetStaticMaterials();
			if (MaterialIndex >= Mats.Num())
			{
				MaterialIndex -= Mats.Num();
			}
			else
			{
				return Mats[MaterialIndex].MaterialInterface;
			}
		}
	}

	return nullptr;
}

int32 UAllegroComponent::GetMaterialIndex(FName MaterialSlotName) const
{
	int Counter = 0;
	for (const FAllegroSubmeshSlot& MeshSlot : Submeshes)
	{
		if (MeshSlot.SkeletalMesh)
		{
			const TArray<FSkeletalMaterial>& SkeletalMeshMaterials = MeshSlot.SkeletalMesh->GetMaterials();
			for (int32 MaterialIndex = 0; MaterialIndex < SkeletalMeshMaterials.Num(); ++MaterialIndex)
			{
				const FSkeletalMaterial& SkeletalMaterial = SkeletalMeshMaterials[MaterialIndex];
				if (SkeletalMaterial.MaterialSlotName == MaterialSlotName)
					return Counter + MaterialIndex;
			}
			Counter += SkeletalMeshMaterials.Num();
		}
	}
	return INDEX_NONE;
}

TArray<FName> UAllegroComponent::GetMaterialSlotNames() const
{
	TArray<FName> MaterialNames;
	for (const FAllegroSubmeshSlot& MeshSlot : Submeshes)
	{
		if (MeshSlot.SkeletalMesh)
		{
			const TArray<FSkeletalMaterial>& SkeletalMeshMaterials = MeshSlot.SkeletalMesh->GetMaterials();
			for (int32 MaterialIndex = 0; MaterialIndex < SkeletalMeshMaterials.Num(); ++MaterialIndex)
			{
				const FSkeletalMaterial& SkeletalMaterial = SkeletalMeshMaterials[MaterialIndex];
				MaterialNames.Add(SkeletalMaterial.MaterialSlotName);
			}
		}
	}
	return MaterialNames;
}

bool UAllegroComponent::IsMaterialSlotNameValid(FName MaterialSlotName) const
{
	return GetMaterialIndex(MaterialSlotName) >= 0;
}

void UAllegroComponent::TickAnimations(float DeltaTime)
{
	ALLEGRO_SCOPE_CYCLE_COUNTER(AnimationsTick);

	//GAnimFinishEvents.Reset();
	//GAnimNotifyEvents.Reset();

#if ALLEGRO_ANIMTION_TICK_LOD
	++FrameCounter;
#endif

	int InstanceNum = GetInstanceCount();
	if (InstanceNum <= 0)
	{
		return;
	}

	DeltaTime *= AnimationPlayRate;
	if (DeltaTime > 0)
	{
		FMatrix ViewProjectMatrix;
		FVector ViewLocation;
		float LODScale = 1.0f;
		bool UseViewInfo = false;

#if ALLEGRO_ANIMTION_TICK_LOD
		if (GetWorld()->GetFirstPlayerController())
		{
			const FMinimalViewInfo& ViewInfo = GetWorld()->GetFirstPlayerController()->PlayerCameraManager->GetCameraCacheView();
			ViewProjectMatrix = ViewInfo.CalculateProjectionMatrix();
			ViewLocation = ViewInfo.Location;
			UseViewInfo = true;

			static const auto* SkeletalMeshLODRadiusScale = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.SkeletalMeshLODRadiusScale"));
			LODScale = FMath::Clamp(SkeletalMeshLODRadiusScale->GetValueOnGameThread(), 0.25f, 1.0f);
		}
#endif
		FMatrix* ViewProjectMatrixPtr = UseViewInfo ? &ViewProjectMatrix : nullptr;
		FVector* ViewLocationPtr = UseViewInfo ? &ViewLocation : nullptr;

		TimeSinceLastLocalBoundUpdate += DeltaTime;		
		ThreadSafeTickTempEvent.Reset(InstanceNum);
		ThreadSafeTickTempEvent.AddUninitialized(InstanceNum);

		if (UseTaskMode && NumPreTask > 1
			//&& InstanceNum > NumPreTask
			)
		{
			
			AnimationNotifyEventsTemp.Reset(InstanceNum);
			AnimationNotifyEventsTemp.AddDefaulted(InstanceNum);

			TArray<FAllegroInstanceAnimState>& InstanceState = InstancesData.AnimationStates;
			UAllegroComponent* Owner = this;
			uint32 FrameCnt = FrameCounter;

			ParallelFor(TEXT("ParallelForAnimState"), InstanceNum, NumPreTask, 
				[&InstanceState, Owner, DeltaTime, FrameCnt, ViewProjectMatrixPtr, ViewLocationPtr, LODScale](int Index) {
					auto& Ref = InstanceState[Index];
					Ref.Tick(Owner, Index, DeltaTime, FrameCnt, ViewProjectMatrixPtr, ViewLocationPtr, LODScale);
				});

			for (int i = 0; i < AnimationNotifyEventsTemp.Num(); ++i)
			{
				TArray<FAllegroAnimNotifyEvent>& Notifys = AnimationNotifyEventsTemp[i];
				if (Notifys.Num() > 0)
				{
					for (int n = 0; n < Notifys.Num(); ++n)
					{
						AnimationNotifyEvents.Add(Notifys[n]);
					}
				}
			}
			
		}
		else
		{
			for (int InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)
			{
				FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
				AS.Tick(this, InstanceIndex, DeltaTime, FrameCounter,ViewProjectMatrixPtr, ViewLocationPtr, LODScale);
			}
		}
	}

	{
		for (int ListenerIndex = 0; ListenerIndex < this->NumListener; ListenerIndex++)
			this->ListenersPtr[ListenerIndex]->OnAllInstanceTicked(this->ListenersUserData[ListenerIndex]);
	}

	//最后发事件
	for (int idx = 0; idx < ThreadSafeTickTempEvent.Num();++idx)
	{
		auto& Ref = ThreadSafeTickTempEvent[idx];
		if (Ref.FinishedSequence)
		{
			AnimationFinishEvents.Add(FAllegroAnimFinishEvent{ idx, Ref.FinishedSequence });

			if (UseGPUTransition)
			{
				if (InstanceHasAnyFlag(idx, EAllegroInstanceFlags::EIF_GPUTransition))
				{
					Ref.GPUTransitionFinished = 1;   //主动画结束了，也要跟着停止过渡
				}
			}
		}

		if (Ref.FinishedTransitionIdx >= 0)
		{
			AllegroTransitionIndex TransitionIndex = Ref.FinishedTransitionIdx;
			AnimCollection->DecTransitionRef(TransitionIndex);
		}

		if (Ref.GPUTransitionFinished > 0)
		{
			if (InstancesData.AnimationStates[idx].AssetType != EAnimAssetType::AnimBlendSpace)
			{
				InstanceRemoveFlags(idx, EAllegroInstanceFlags::EIF_BlendFrame | EAllegroInstanceFlags::EIF_GPUTransition);

				if (InstancesData.BlendFrameInfoIndex[idx] > 0)
				{
					FreeBlendFrameIndex(InstancesData.BlendFrameInfoIndex[idx]);
					InstancesData.BlendFrameInfoIndex[idx] = 0;
				}
			}
			else
			{
				InstanceRemoveFlags(idx, EAllegroInstanceFlags::EIF_GPUTransition);
			}
		}
	}

	ThreadSafeTickTempEvent.Reset(0);
}


void UAllegroComponent::AddEvent(int InstanceIndex, const FAllegroAnimNotifyEvent& Notify)
{
	if (UseTaskMode)
	{
		AnimationNotifyEventsTemp[InstanceIndex].Add(Notify);
	}
	else
	{
		AnimationNotifyEvents.Add(Notify);
	}
}


void UAllegroComponent::CalcAnimationFrameIndices()
{
	FMemory::Memzero(InstancesData.FrameIndices.GetData(), InstancesData.FrameIndices.Num() * InstancesData.FrameIndices.GetTypeSize());

	if(!AnimCollection)
		return;

	for (int InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)
	{
		if (IsInstanceAlive(InstanceIndex))
		{
			const bool bShouldLoop = InstanceHasAnyFlag(InstanceIndex, EAllegroInstanceFlags::EIF_AnimLoop);
			const FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];

			if(AS.IsValid() && !InstanceHasAnyFlag(InstanceIndex, EAllegroInstanceFlags::EIF_DynamicPose | EAllegroInstanceFlags::EIF_AnimPlayingTransition))
			{
				const FAllegroSequenceDef& ActiveSequenceStruct = AnimCollection->Sequences[AS.CurrentSequence];
				float AnimTime = AS.Time;
				AnimAdvanceTime(bShouldLoop, 0, AnimTime, ActiveSequenceStruct.GetSequenceLength());
				InstancesData.FrameIndices[InstanceIndex] = AnimCollection->CalcFrameIndex(ActiveSequenceStruct, AnimTime);
			}
		}
	}
}



void UAllegroComponent::SetLODDistanceScale(float NewLODDistanceScale)
{
	LODDistanceScale = FMath::Max(0.000001f, NewLODDistanceScale);
	FAllegroProxy* SKProxy = static_cast<FAllegroProxy*>(this->SceneProxy);
	if(SKProxy)
	{
		ENQUEUE_RENDER_COMMAND(Allegro_SetLODDistanceScale)([this](FRHICommandListImmediate& RHICmdList) {
			FAllegroProxy* LocalSKProxy = static_cast<FAllegroProxy*>(this->SceneProxy);
			LocalSKProxy->DistanceScale = this->LODDistanceScale;
		});
	}
}

void UAllegroComponent::SetAnimCollection(UAllegroAnimCollection* asset)
{
	if (AnimCollection != asset)
	{
		ResetAnimationStates();
		ResetMeshSlots();
		
		AnimCollection = asset;
		CheckAssets_Internal();
	}
}


void UAllegroComponent::ResetInstanceAnimationState(int InstanceIndex)
{
	check(IsInstanceValid(InstanceIndex));

	if(this->InstanceHasAnyFlag(InstanceIndex, EAllegroInstanceFlags::EIF_DynamicPose))
	{
		if (this->InstanceHasAnyFlag(InstanceIndex, EAllegroInstanceFlags::EIF_BoundToSMC))
		{
			int DPI = AnimCollection->FrameIndexToDynamicPoseIndex(this->InstancesData.FrameIndices[InstanceIndex]);
			DynamicPoseInstancesTiedToSMC.FindAndRemoveChecked(DPI);
		}

		ReleaseDynamicPose_Internal(InstanceIndex);
	}
	else if(this->InstanceHasAnyFlag(InstanceIndex, EAllegroInstanceFlags::EIF_AnimPlayingTransition))
	{
		AnimCollection->DecTransitionRef(InstancesData.AnimationStates[InstanceIndex].TransitionIndex);
	}

	if (InstancesData.BlendFrameInfoIndex[InstanceIndex] > 0)
	{
		FreeBlendFrameIndex(InstancesData.BlendFrameInfoIndex[InstanceIndex]);
		InstancesData.BlendFrameInfoIndex[InstanceIndex] = 0;
	}

	//释放扩展的类实例
	InstancesData.AnimationStates[InstanceIndex].ResetAnimState();

	InstanceRemoveFlags(InstanceIndex, EAllegroInstanceFlags::EIF_AllUserFlags | EAllegroInstanceFlags::EIF_AllAnimationFlags | EAllegroInstanceFlags::EIF_DynamicPose | EAllegroInstanceFlags::EIF_BoundToSMC);
	InstanceAddFlags(InstanceIndex, EAllegroInstanceFlags::EIF_AnimNoSequence | EAllegroInstanceFlags::EIF_New | EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate);

	InstancesData.AnimationStates[InstanceIndex] = FAllegroInstanceAnimState();
	InstancesData.FrameIndices[InstanceIndex] = 0;

}

bool UAllegroComponent::IsInstanceValid(int32 InstanceIndex) const
{
	return InstancesData.Flags.IsValidIndex(InstanceIndex) && IsInstanceAlive(InstanceIndex);
}


void UAllegroComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();
}

int UAllegroComponent::AllocateBlendFrameIndex()
{
	int Index = BlendFrameIndexAllocator.Allocate();
	if (BlendFrameIndexAllocator.GetMaxSize() > InstancesData.BlendFrameInfo.Num())
	{
		const int Count = FAllegroInstancesData::LENGTH_ALIGN;
		InstancesData.BlendFrameInfo.AddUninitialized(Count);
	}
	++NumAliveBlendFrame;
	return Index;
}

void UAllegroComponent::FreeBlendFrameIndex(int index)
{
	BlendFrameIndexAllocator.Free(index);
	if (BlendFrameIndexAllocator.GetNumPendingFreeSpans() >= 10)
		BlendFrameIndexAllocator.Consolidate();

	--NumAliveBlendFrame;
}

int UAllegroComponent::AddInstance(const FTransform3f& worldTransform)
{
	//if(GetAliveInstanceCount() == 0)
	//{
	//	if (IsRenderStateCreated())
	//		MarkRenderStateDirty();
	//	else if (ShouldCreateRenderState())
	//		RecreateRenderState_Concurrent();
	//}

	check(worldTransform.IsValid());

	NumAliveInstance++;
	int InstanceIndex = IndexAllocator.Allocate();

	if (IndexAllocator.GetMaxSize() > InstancesData.Flags.Num())
	{
		const int Begin = InstancesData.Flags.Num();
		const int Count = FAllegroInstancesData::LENGTH_ALIGN;

		InstancesData.Flags.AddUninitialized(Count);
		for (int i = 0; i < Count; i++)
			InstancesData.Flags[Begin + i] = EAllegroInstanceFlags::EIF_Destroyed;

		InstancesData.FrameIndices.AddUninitialized(Count);
		InstancesData.BlendFrameInfoIndex.AddUninitialized(Count);
		InstancesData.AnimationStates.AddUninitialized(Count);

		InstancesData.Locations.AddUninitialized(Count);
		InstancesData.Rotations.AddUninitialized(Count);
		InstancesData.Scales.AddUninitialized(Count);

		InstancesData.Matrices.AddUninitialized(Count);
		InstancesData.Stencil.AddUninitialized(Count);

		if (NumCustomDataFloats > 0)
			InstancesData.RenderCustomData.AddUninitialized(Count * NumCustomDataFloats);

		InstancesData.MeshSlots.AddUninitialized(Count * (this->MaxMeshPerInstance + 1));

		if (!ShouldUseFixedInstanceBound())
		{
			InstancesData.LocalBounds.AddUninitialized(Count);
		}

		CallCustomInstanceData_SetNum(Begin + Count);

		//for (FArrayProperty* Arr : GetBPInstanceDataArrays())
		//{
		//	FScriptArrayHelper Helper(Arr, Arr->GetPropertyValuePtr_InContainer(this));
		//	Helper.AddValues(Count);
		//	check(Helper.Num() == InstancesData.Flags.Num());
		//}

		if (PerInstanceScriptStruct)
		{
			const int StructSize = PerInstanceScriptStruct->GetStructureSize();
			check(StructSize > 0);
			InstancesData.CustomPerInstanceStruct.AddUninitialized(Count * StructSize);
		}
	}


	InstancesData.FrameIndices[InstanceIndex] = 0;
	InstancesData.AnimationStates[InstanceIndex] = FAllegroInstanceAnimState();
	InstancesData.Flags[InstanceIndex] = EAllegroInstanceFlags::EIF_Default;
	InstancesData.BlendFrameInfoIndex[InstanceIndex] = 0;
	
	check(MaxMeshPerInstance > 0);
	{
		uint8* MeshSlots = this->GetInstanceMeshSlots(InstanceIndex);
		MeshSlots[0] = this->InstanceDefaultAttachIndex;
		MeshSlots[1] = 0xFF;
	}

	InstancesData.Stencil[InstanceIndex] = -1;

	if(NumCustomDataFloats > 0)
		ZeroInstanceCustomData(InstanceIndex);

	SetInstanceTransform(InstanceIndex, worldTransform);

	if (PerInstanceScriptStruct)
	{
		const int StructSize = PerInstanceScriptStruct->GetStructureSize();
		check(StructSize > 0);
		uint8* CSD = &InstancesData.CustomPerInstanceStruct[StructSize * InstanceIndex];
		check(IsAligned(CSD, PerInstanceScriptStruct->GetMinAlignment()));
		PerInstanceScriptStruct->InitializeStruct(CSD);
	}

	CallCustomInstanceData_Initialize(InstanceIndex);

	if(!IsRenderTransformDirty())
		MarkRenderTransformDirty();

	return InstanceIndex;
}

int UAllegroComponent::AddInstance_CopyFrom(const UAllegroComponent* Src, int SrcInstanceIndex)
{
	int InstanceIndex = AddInstance(FTransform3f::Identity);
	this->InstanceCopyFrom(InstanceIndex, Src, SrcInstanceIndex);
	return InstanceIndex;
}

bool UAllegroComponent::DestroyInstance(int InstanceIndex)
{
	if (IsInstanceValid(InstanceIndex))
	{
		return DestroyAt_Internal(InstanceIndex);
	}

	return false;
}

void UAllegroComponent::DestroyInstances(const TArray<int>& InstanceIndices)
{
	for (int Index : InstanceIndices)
	{
		DestroyInstance(Index);
	}
}

void UAllegroComponent::DestroyInstancesByRange(int Index, int Count)
{
	for(int i = Index; i < (Index+Count); i++)
	{
		DestroyInstance(i);
	}
}

bool UAllegroComponent::DestroyAt_Internal(int InstanceIndex)
{
	CallCustomInstanceData_Destroy(InstanceIndex);
	DestroyCustomStruct_Internal(InstanceIndex);
	ResetInstanceAnimationState(InstanceIndex);

	IndexAllocator.Free(InstanceIndex);

	if (IndexAllocator.GetNumPendingFreeSpans() >= 10)
		IndexAllocator.Consolidate();

	NumAliveInstance--;
	InstancesData.Flags[InstanceIndex] = EAllegroInstanceFlags::EIF_Destroyed;
	

	//if(GetAliveInstanceCount() == 0) 
	//{
	//	ClearInstances();
	//	MarkRenderStateDirty(); //won't create proxy if there is no instance
	//}

	if (!IsRenderTransformDirty())
		MarkRenderTransformDirty();

	return true;
}

void UAllegroComponent::DestroyCustomStruct_Internal(int InstanceIndex)
{
	if (PerInstanceScriptStruct)
	{
		const int StructSize = PerInstanceScriptStruct->GetStructureSize();
		uint8* CSD = &InstancesData.CustomPerInstanceStruct[StructSize * InstanceIndex];
		PerInstanceScriptStruct->DestroyStruct(CSD); //ClearScriptStruct ?
	}
}

int UAllegroComponent::K2_FlushInstances(TArray<int>& InOutRemapArray)
{
	int OldNumFree = GetDestroyedInstanceCount();
	if (OldNumFree <= FAllegroInstancesData::LENGTH_ALIGN)
		return 0;

	check(IsAligned(InstancesData.Flags.Num(), FAllegroInstancesData::LENGTH_ALIGN));

	const int OldInstanceCount = GetInstanceCount();
	InOutRemapArray.SetNumUninitialized(OldInstanceCount);
	IndexAllocator.Reset();
	IndexAllocator.Allocate(NumAliveInstance);

	int WriteIndex = 0;
	for (int InstanceIndex = 0; InstanceIndex < OldInstanceCount; InstanceIndex++)
	{
		if (EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Destroyed))
		{
			InOutRemapArray[InstanceIndex] = -1;
			continue;
		}

		InOutRemapArray[InstanceIndex] = WriteIndex;

		if (WriteIndex != InstanceIndex)
		{
			InstancesData.Flags[WriteIndex] = InstancesData.Flags[InstanceIndex] | EAllegroInstanceFlags::EIF_New;
			InstancesData.FrameIndices[WriteIndex] = InstancesData.FrameIndices[InstanceIndex];
			InstancesData.AnimationStates[WriteIndex] = InstancesData.AnimationStates[InstanceIndex];

			InstancesData.Locations[WriteIndex] = InstancesData.Locations[InstanceIndex];
			InstancesData.Rotations[WriteIndex] = InstancesData.Rotations[InstanceIndex];
			InstancesData.Scales[WriteIndex] = InstancesData.Scales[InstanceIndex];

			InstancesData.Matrices[WriteIndex] = InstancesData.Matrices[InstanceIndex];
			//InstancesData.RenderMatrices[WriteIndex] = InstancesData.RenderMatrices[InstanceIndex];

			if (InstancesData.LocalBounds.Num())
			{
				InstancesData.LocalBounds[WriteIndex] = InstancesData.LocalBounds[InstanceIndex];
			}

			if (NumCustomDataFloats)
				AllegroElementCopy(GetInstanceCustomDataFloats(WriteIndex), GetInstanceCustomDataFloats(InstanceIndex), NumCustomDataFloats);


			AllegroElementCopy(GetInstanceMeshSlots(WriteIndex), GetInstanceMeshSlots(InstanceIndex), MaxMeshPerInstance + 1);

			CallCustomInstanceData_Move(WriteIndex, InstanceIndex);

			//for (FArrayProperty* Arr : GetBPInstanceDataArrays())
			//{
			//	FScriptArray* ScriptArray = Arr->GetPropertyValuePtr_InContainer(this);
			//	uint8* ArrayData = (uint8*)ScriptArray->GetData();
			//	Arr->Inner->DestroyValue(ArrayData + (WriteIndex * Arr->Inner->ElementSize)); //#TODO do we need to call destroy or ... ?
			//	Arr->Inner->CopySingleValue(ArrayData + (WriteIndex * Arr->Inner->ElementSize), ArrayData + (InstanceIndex * Arr->Inner->ElementSize));
			//}
			if (PerInstanceScriptStruct)
			{
				const int StructSize = PerInstanceScriptStruct->GetStructureSize();
				uint8* CSD_Write = &InstancesData.CustomPerInstanceStruct[StructSize * WriteIndex];
				uint8* CSD_Read = &InstancesData.CustomPerInstanceStruct[StructSize * InstanceIndex];
				PerInstanceScriptStruct->CopyScriptStruct(CSD_Write, CSD_Read);
				PerInstanceScriptStruct->ClearScriptStruct(CSD_Read);
			}

		}

		WriteIndex++;
	}

	
	check(WriteIndex == NumAliveInstance);
	//arrays length need to be multiple of FAllegroInstancesData::LENGTH_ALIGN (SIMD Friendly)
	const int NewArrayLen = Align(WriteIndex, FAllegroInstancesData::LENGTH_ALIGN);
	for (int i = WriteIndex; i < NewArrayLen; i++)
		InstancesData.Flags[i] |= EAllegroInstanceFlags::EIF_Destroyed;

	InstanceDataSetNum_Internal(NewArrayLen);

	if (!IsRenderTransformDirty())
		MarkRenderTransformDirty();

	for (auto& PairData : DynamicPoseInstancesTiedToSMC)
	{
		PairData.Value.InstanceIndex = InOutRemapArray[PairData.Value.InstanceIndex];
	}

	return OldNumFree;
}

int UAllegroComponent::FlushInstances(TArray<int>* InOutRemapArray)
{
	TArray<int> LocalRemap;
	return K2_FlushInstances(InOutRemapArray ? *InOutRemapArray : LocalRemap);
}


void UAllegroComponent::RemoveTailDataIfAny()
{
	//#Note IndexAllocator can decrease MaxSize
	int RightSlack = InstancesData.Flags.Num() - IndexAllocator.GetMaxSize(); 
	if (RightSlack > (FAllegroInstancesData::LENGTH_ALIGN * 16))
	{
		const int NewArrayLen = Align(NumAliveInstance, FAllegroInstancesData::LENGTH_ALIGN);
		InstanceDataSetNum_Internal(NewArrayLen);
	}
}

void UAllegroComponent::InstanceDataSetNum_Internal(int NewArrayLen)
{
	InstancesData.Flags.SetNumUninitialized(NewArrayLen, true);
	InstancesData.FrameIndices.SetNumUninitialized(NewArrayLen, true);
	InstancesData.AnimationStates.SetNumUninitialized(NewArrayLen, true);

	InstancesData.Locations.SetNumUninitialized(NewArrayLen, true);
	InstancesData.Rotations.SetNumUninitialized(NewArrayLen, true);
	InstancesData.Scales.SetNumUninitialized(NewArrayLen, true);

	InstancesData.Matrices.SetNumUninitialized(NewArrayLen, true);

	InstancesData.Stencil.SetNumUninitialized(NewArrayLen, true);

	if (NumCustomDataFloats > 0)
		InstancesData.RenderCustomData.SetNumUninitialized(NewArrayLen * NumCustomDataFloats, true);

	if (InstancesData.LocalBounds.Num())
		InstancesData.LocalBounds.SetNumUninitialized(NewArrayLen, true);

	InstancesData.MeshSlots.SetNumUninitialized(NewArrayLen * (MaxMeshPerInstance + 1), true);

	//for (FArrayProperty* Arr : GetBPInstanceDataArrays())
	//{
	//	FScriptArrayHelper Helper(Arr, Arr->GetPropertyValuePtr_InContainer(this));
	//	Helper.Resize(NewArrayLen);
	//}

	if (PerInstanceScriptStruct)
	{
		const int StructSize = PerInstanceScriptStruct->GetStructureSize();
		InstancesData.CustomPerInstanceStruct.SetNumUninitialized(NewArrayLen * StructSize, true);
	}

	CustomInstanceData_SetNum(NewArrayLen);
}

void UAllegroComponent::ClearInstances(bool bEmptyOrReset)
{
	if(GetAliveInstanceCount() > 0)
	{
		DestroyInstancesByRange(0, GetInstanceCount());
		CallCustomInstanceData_SetNum(0);
	}

	NumAliveInstance = 0;
	if (bEmptyOrReset)
	{
		IndexAllocator.Reset();
		InstancesData.Empty();
	}
	else
	{
		IndexAllocator.Reset();
		InstancesData.Reset();
	}

	NumAliveBlendFrame = 0;
	BlendFrameIndexAllocator.Reset();
	BlendFrameIndexAllocator.Allocate();
	InstancesData.BlendFrameInfo.Reset(); 
	FInstanceBlendFrameInfo& Info = InstancesData.BlendFrameInfo.AddDefaulted_GetRef();
	Info.Weight[0] = 1;
	Info.Weight[1] = 0;
	Info.Weight[2] = 0;
	Info.Weight[3] = 0;
	Info.FrameIndex[0] = 0;

	MarkRenderTransformDirty();
}


void UAllegroComponent::InstanceCopyFrom(int InstanceIndex, const UAllegroComponent* SrcComponent, int SrcInstanceIndex)
{
	if (IsValid(SrcComponent) && SrcComponent->IsInstanceValid(SrcInstanceIndex) && IsInstanceValid(InstanceIndex))
	{
		InstancesData.Locations[InstanceIndex] = SrcComponent->InstancesData.Locations[SrcInstanceIndex];
		InstancesData.Rotations[InstanceIndex] = SrcComponent->InstancesData.Rotations[SrcInstanceIndex];
		InstancesData.Scales[InstanceIndex] = SrcComponent->InstancesData.Scales[SrcInstanceIndex];
		InstancesData.Matrices[InstanceIndex] = SrcComponent->InstancesData.Matrices[SrcInstanceIndex];
		
		const FAllegroInstanceAnimState& SrcAS = SrcComponent->InstancesData.AnimationStates[SrcInstanceIndex];
		FAllegroInstanceAnimState& DstAS = InstancesData.AnimationStates[InstanceIndex];

		if (AnimCollection == SrcComponent->AnimCollection)
		{
			if (SrcAS.IsTransitionValid())
			{
				check(EnumHasAnyFlags(SrcComponent->InstancesData.Flags[SrcInstanceIndex], EAllegroInstanceFlags::EIF_AnimPlayingTransition));
				AnimCollection->IncTransitionRef(SrcAS.TransitionIndex);
			}

			if(DstAS.IsTransitionValid())
			{
				check(EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_AnimPlayingTransition));
				AnimCollection->DecTransitionRef(DstAS.TransitionIndex);
			}

			DstAS = SrcAS;
			InstancesData.FrameIndices[InstanceIndex] = SrcComponent->InstancesData.FrameIndices[SrcInstanceIndex];

			check(!EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_DynamicPose));
			const EAllegroInstanceFlags FlagsToKeep = EAllegroInstanceFlags::EIF_New;//instance must be kept new if it is
			const EAllegroInstanceFlags FlagsToTake = EAllegroInstanceFlags::EIF_Hidden | EAllegroInstanceFlags::EIF_New | EAllegroInstanceFlags::EIF_AllUserFlags | EAllegroInstanceFlags::EIF_AllAnimationFlags;
			InstancesData.Flags[InstanceIndex] &= FlagsToKeep;
			InstancesData.Flags[InstanceIndex] |= (SrcComponent->InstancesData.Flags[SrcInstanceIndex] & FlagsToTake) | EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate;
		}
		else //don't copy animation data if AnimCollections are not identical
		{
			const EAllegroInstanceFlags FlagsToKeep = EAllegroInstanceFlags::EIF_AllAnimationFlags | EAllegroInstanceFlags::EIF_New | EAllegroInstanceFlags::EIF_DynamicPose;//instance must be kept new if it is
			const EAllegroInstanceFlags FlagsToTake = EAllegroInstanceFlags::EIF_Hidden | EAllegroInstanceFlags::EIF_New | EAllegroInstanceFlags::EIF_AllUserFlags;
			InstancesData.Flags[InstanceIndex] &= FlagsToKeep;
			InstancesData.Flags[InstanceIndex] |= (SrcComponent->InstancesData.Flags[SrcInstanceIndex] & FlagsToTake) | EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate;
		}
		if (NumCustomDataFloats > 0 && NumCustomDataFloats <= SrcComponent->NumCustomDataFloats)
		{
			AllegroElementCopy(this->GetInstanceCustomDataFloats(InstanceIndex), SrcComponent->GetInstanceCustomDataFloats(SrcInstanceIndex), NumCustomDataFloats);
		}
		if (this->Submeshes.Num() > 0 && this->Submeshes.Num() == SrcComponent->Submeshes.Num() && this->MaxMeshPerInstance == SrcComponent->MaxMeshPerInstance)
		{
			AllegroElementCopy(this->GetInstanceMeshSlots(InstanceIndex), SrcComponent->GetInstanceMeshSlots(InstanceIndex), this->MaxMeshPerInstance + 1);
		}
	}
}


void UAllegroComponent::CallCustomInstanceData_Initialize(int InstanceIndex)
{
	CustomInstanceData_Initialize(InstanceIndex);
	for (int ListenerIndex = 0; ListenerIndex < this->NumListener; ListenerIndex++)
		this->ListenersPtr[ListenerIndex]->CustomInstanceData_Initialize(this->ListenersUserData[ListenerIndex], InstanceIndex);
}

void UAllegroComponent::CallCustomInstanceData_Destroy(int InstanceIndex)
{
	CustomInstanceData_Destroy(InstanceIndex);
	for (int ListenerIndex = 0; ListenerIndex < this->NumListener; ListenerIndex++)
		this->ListenersPtr[ListenerIndex]->CustomInstanceData_Destroy(this->ListenersUserData[ListenerIndex], InstanceIndex);
}

void UAllegroComponent::CallCustomInstanceData_Move(int DstIndex, int SrcIndex)
{
	CustomInstanceData_Move(DstIndex, SrcIndex);
	for (int ListenerIndex = 0; ListenerIndex < this->NumListener; ListenerIndex++)
		this->ListenersPtr[ListenerIndex]->CustomInstanceData_Move(this->ListenersUserData[ListenerIndex], DstIndex, SrcIndex);
}

void UAllegroComponent::CallCustomInstanceData_SetNum(int NewNum)
{
	CustomInstanceData_SetNum(NewNum);
	for (int ListenerIndex = 0; ListenerIndex < this->NumListener; ListenerIndex++)
		this->ListenersPtr[ListenerIndex]->CustomInstanceData_SetNum(this->ListenersUserData[ListenerIndex], NewNum);
}

void UAllegroComponent::BatchUpdateTransforms(int StartInstanceIndex, const TArray<FTransform3f>& NewTransforms)
{
	for (int i = 0; i < NewTransforms.Num(); i++)
		if(IsInstanceValid(i + StartInstanceIndex))
			SetInstanceTransform(i + StartInstanceIndex, NewTransforms[i]);
}

void UAllegroComponent::SetInstanceTransform(int InstanceIndex, const FTransform3f& NewTransform)
{
	check(IsInstanceValid(InstanceIndex));
	check(NewTransform.IsValid());
	InstancesData.Locations[InstanceIndex] = NewTransform.GetLocation();
	InstancesData.Rotations[InstanceIndex] = NewTransform.GetRotation();
	InstancesData.Scales[InstanceIndex]    = NewTransform.GetScale3D();
	OnInstanceTransformChange(InstanceIndex);
}

void UAllegroComponent::SetInstanceLocation(int InstanceIndex, const FVector3f& NewLocation)
{
	check(IsInstanceValid(InstanceIndex));
	InstancesData.Locations[InstanceIndex] = NewLocation;
	OnInstanceTransformChange(InstanceIndex);
}

void UAllegroComponent::SetInstanceRotator(int InstanceIndex, const FRotator3f& NewRotator)
{
	SetInstanceRotation(InstanceIndex, NewRotator.Quaternion());
}

void UAllegroComponent::SetInstanceRotation(int InstanceIndex, const FQuat4f& NewRotation)
{
	check(IsInstanceValid(InstanceIndex));
	InstancesData.Rotations[InstanceIndex] = NewRotation;
	OnInstanceTransformChange(InstanceIndex);
}

void UAllegroComponent::SetInstanceScale(int InstanceIndex, const FVector3f& NewScale)
{
	check(IsInstanceValid(InstanceIndex));
	InstancesData.Scales[InstanceIndex] = NewScale;
	OnInstanceTransformChange(InstanceIndex);
}

void UAllegroComponent::AddInstanceLocation(int InstanceIndex, const FVector3f& Offset)
{
	check(IsInstanceValid(InstanceIndex));
	InstancesData.Locations[InstanceIndex] += Offset;
	OnInstanceTransformChange(InstanceIndex);
}

void UAllegroComponent::SetInstanceLocationAndRotation(int InstanceIndex, const FVector3f& NewLocation, const FQuat4f& NewRotation)
{
	check(IsInstanceValid(InstanceIndex));
	check(!NewLocation.ContainsNaN() && !NewRotation.ContainsNaN());
	InstancesData.Locations[InstanceIndex] = NewLocation;
	InstancesData.Rotations[InstanceIndex] = NewRotation;
	OnInstanceTransformChange(InstanceIndex);
}

void UAllegroComponent::MoveAllInstances(const FVector3f& Offset)
{
	for(int i = 0; i < GetInstanceCount(); i++)
		if(IsInstanceAlive(i))
			AddInstanceLocation(i, Offset);
}

void UAllegroComponent::BatchAddUserFlags(const TArray<int>& InstanceIndices, int32 Flags)
{
	for (int InstanceIndex : InstanceIndices)
		if (IsInstanceValid(InstanceIndex))
			InstanceAddFlags(InstanceIndex, static_cast<EAllegroInstanceFlags>(Flags << InstaceUserFlagStart));
}

void UAllegroComponent::BatchRemoveUserFlags(const TArray<int>& InstanceIndices, int32 Flags)
{
	for (int InstanceIndex : InstanceIndices)
		if (IsInstanceValid(InstanceIndex))
			InstanceRemoveFlags(InstanceIndex, static_cast<EAllegroInstanceFlags>(Flags << InstaceUserFlagStart));
}

FTransform3f UAllegroComponent::GetInstanceTransform(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	return FTransform3f(InstancesData.Rotations[InstanceIndex], InstancesData.Locations[InstanceIndex], InstancesData.Scales[InstanceIndex]);
}

const FVector3f& UAllegroComponent::GetInstanceLocation(int InstanceIndex) const
{
	return InstancesData.Locations[InstanceIndex];
}

const FQuat4f& UAllegroComponent::GetInstanceRotation(int InstanceIndex) const
{
	return InstancesData.Rotations[InstanceIndex];
}

FRotator3f UAllegroComponent::GetInstanceRotator(int InstanceIndex) const
{
	return GetInstanceRotation(InstanceIndex).Rotator();
}

const FBoxCenterExtentFloat& UAllegroComponent::GetInstanceLocalBound(int InstanceIndex)
{
	check(IsInstanceValid(InstanceIndex));
	
	if (ShouldUseFixedInstanceBound())
		return AnimCollection->MeshesBBox;

	if (EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate))
	{
		EnumRemoveFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate);
		UpdateInstanceLocalBound(InstanceIndex);
	}
	
	return InstancesData.LocalBounds[InstanceIndex];
}


FBoxCenterExtentFloat UAllegroComponent::CalculateInstanceBound(int InstanceIndex)
{
	return GetInstanceLocalBound(InstanceIndex).TransformBy(InstancesData.Matrices[InstanceIndex]);
}

bool UAllegroComponent::ShouldUseFixedInstanceBound() const
{
	return bUseFixedInstanceBound || (AnimCollection && AnimCollection->bDontGenerateBounds);
}

void UAllegroComponent::OnInstanceTransformChange(int InstanceIndex)
{
	InstancesData.Matrices[InstanceIndex] = GetInstanceTransform(InstanceIndex).ToMatrixWithScale();
}

bool UAllegroComponent::IsInstanceHidden(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	return EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Hidden);
}

void UAllegroComponent::SetInstanceHidden(int InstanceIndex, bool bNewHidden)
{
	check(IsInstanceValid(InstanceIndex));
	const bool bIsHidden = EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Hidden);
	if (bNewHidden != bIsHidden)
	{
		if(bNewHidden)
			EnumAddFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Hidden);
		else
			EnumRemoveFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Hidden);

		EnumAddFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_New);
	}
}

void UAllegroComponent::ToggleInstanceVisibility(int InstanceIndex)
{
	check(IsInstanceValid(InstanceIndex));
	InstancesData.Flags[InstanceIndex] ^= EAllegroInstanceFlags::EIF_Hidden;
	InstancesData.Flags[InstanceIndex] |= EAllegroInstanceFlags::EIF_New;
}

void UAllegroComponent::SetInstanceStencil(int InstanceIndex, int32 Stencil)
{
	if (!IsInstanceValid(InstanceIndex))
		return;

	this->bRenderCustomDepth = 1;
	InstancesData.Stencil[InstanceIndex] = Stencil;
}

void UAllegroComponent::SetAnimCollectionAndSkeletalMesh(UAllegroAnimCollection* asset, USkeletalMesh* InMesh)
{
	this->Submeshes.Reset();
	this->SetAnimCollection(asset);

	if (InMesh)
	{
		auto& SubMesh = this->Submeshes.AddDefaulted_GetRef();
		SubMesh.SkeletalMesh = InMesh;

		this->ResetMeshSlots();
		this->CheckAssets_Internal();
	}
	else
	{
		this->InitSubmeshesFromAnimCollection();
	}
}

void UAllegroComponent::SetNoAnimStaticMesh(UStaticMesh* InMesh)
{
	this->Submeshes.Reset();
	this->SetAnimCollection(nullptr);

	if (InMesh)
	{
		auto& SubMesh = this->Submeshes.AddDefaulted_GetRef();
		SubMesh.StaticMesh = InMesh;

		this->ResetMeshSlots();
		//this->CheckAssets_Internal();
	}

	for (int i = 0; i < this->Submeshes.Num(); ++i)
	{
		//copy materials
		const int BaseMI = this->GetSubmeshBaseMaterialIndex(i);
		for (int MI = 0; MI < this->Submeshes[i].StaticMesh->GetStaticMaterials().Num(); MI++)
		{
			this->SetMaterial(MI, this->GetMaterial(BaseMI + MI));
		}
	}

	//UpdateBounds();
	MarkRenderStateDirty();
	MarkCachedMaterialParameterNameIndicesDirty();
	IStreamingManager::Get().NotifyPrimitiveUpdated(this);
}

float UAllegroComponent::InstancePlayAnimation(int InstanceIndex, FAllegroAnimPlayParams Params)
{
	if (!AnimCollection || !AnimCollection->bIsBuilt || !IsInstanceValid(InstanceIndex) || !Params.Animation)
		return -1;

	UAnimationAsset* AnimAsset = Params.Animation;
	EAllegroInstanceFlags& Flags = InstancesData.Flags[InstanceIndex];
	FAllegroInstanceAnimState& AnimState = InstancesData.AnimationStates[InstanceIndex];
	
	int32 TargetAnimSeqIndex = 0;
	EAnimAssetType AssetType = EAnimAssetType::AnimNull;


	bool IsBlendFrame = false;
	bool IsNewTransition = false;
	float AnimLength = 0.0f;
	
	if (auto AnimSequence = Cast<UAnimSequence>(AnimAsset))
	{
		int32* idx = AnimCollection->SequenceIndexMap.Find(Cast<UAnimSequenceBase>(AnimSequence));
		if (idx)
		{
			TargetAnimSeqIndex = *idx;
		}
		else
		{
			TargetAnimSeqIndex = INDEX_NONE;
		}
		AssetType = EAnimAssetType::AnimSequeue;
	}
	else if (auto Montage = Cast<UAnimMontage>(AnimAsset))
	{
		AssetType = EAnimAssetType::AnimMontage;
		
		FInstanceAnimStateExtend* Extend = AnimState.CreateNewInstanceExtend();
		AnimLength = Extend->SetMontage(Montage, Params,this);

		TargetAnimSeqIndex = Extend->CurrentSequence;
		Params.StartAt = Extend->Time;
	}
	else if (auto BlendSpace = Cast<UBlendSpace>(AnimAsset))
	{
		AssetType = EAnimAssetType::AnimBlendSpace;

		FInstanceAnimStateExtend* Extend = AnimState.CreateNewInstanceExtend();
		AnimLength = Extend->SetBlendSpace(BlendSpace, Params, this);

		TargetAnimSeqIndex = Extend->CurrentSequence;
		Params.StartAt = Extend->Time;

		IsBlendFrame = true;
	}
	
	if (TargetAnimSeqIndex == -1)
	{
		UE_LOG(LogAllegro, Warning, TEXT("AnimSequence is not finded in AnimCollection !"));
	}

	if (TargetAnimSeqIndex == -1 || EnumHasAnyFlags(Flags, EAllegroInstanceFlags::EIF_AnimPaused | EAllegroInstanceFlags::EIF_DynamicPose))
	{
		return -1;
	}

	const FAllegroSequenceDef& TargetSeq = AnimCollection->Sequences[TargetAnimSeqIndex];
	if (AssetType == EAnimAssetType::AnimSequeue)
	{
		AnimState.ResetAnimState();
		AnimLength = TargetSeq.GetSequenceLength();
		AnimAdvanceTime(Params.bLoop, float(0), Params.StartAt, AnimLength);	//clamp or wrap time
	}

	if (InstancesData.BlendFrameInfoIndex[InstanceIndex] > 0)
	{
		FreeBlendFrameIndex(InstancesData.BlendFrameInfoIndex[InstanceIndex]);
		InstancesData.BlendFrameInfoIndex[InstanceIndex] = 0;
	}

	AnimState.SetCurrentAnimAsset(TObjectPtr<UAnimationAsset>(AnimAsset), AssetType);

	const int TargetLocalFrameIndex = static_cast<int>(Params.StartAt * TargetSeq.SampleFrequencyFloat);
	const int TargetGlobalFrameIndex = TargetSeq.AnimationFrameIndex + TargetLocalFrameIndex;
	check(TargetLocalFrameIndex < TargetSeq.AnimationFrameCount);
	check(TargetGlobalFrameIndex < AnimCollection->FrameCountSequences);
	const bool bIsPlayingTransition = EnumHasAnyFlags(Flags, EAllegroInstanceFlags::EIF_AnimPlayingTransition);

	const bool bCanTransition = Params.TransitionDuration > 0 && AnimState.IsValid() && !bIsPlayingTransition;

#if ALLEGRO_GPU_TRANSITION
	this->UseGPUTransition = true;
#else
	this->UseGPUTransition = false;
#endif

	if (this->UseGPUTransition)
	{
#if ALLEGRO_ANIMTION_TICK_LOD
		if(AnimState.AnimtioneLOD < 1)
#endif
		{
			if (bCanTransition)
			{
				int TransitionFrameCount = static_cast<int>(TargetSeq.SampleFrequency * Params.TransitionDuration);
				if (TransitionFrameCount >= 3)
				{
					const FAllegroSequenceDef& CurrentSeqStruct = AnimCollection->Sequences[AnimState.CurrentSequence];

					int CurFrameIndex = InstancesData.FrameIndices[InstanceIndex];
					check(CurFrameIndex < AnimCollection->FrameCountSequences);
					int CurLocalFrameIndex = CurFrameIndex - CurrentSeqStruct.AnimationFrameIndex;
					check(CurLocalFrameIndex < CurrentSeqStruct.AnimationFrameCount);

					AnimState.GPUTransInfo.StartAnimSeqIndex = AnimState.CurrentSequence;
					AnimState.GPUTransInfo.BeginFrameIndex = CurFrameIndex;  //注意，用的是帧的绝对位置
					AnimState.GPUTransInfo.EndFrameIndex = CurFrameIndex + TransitionFrameCount;
					if (AnimState.GPUTransInfo.EndFrameIndex > CurrentSeqStruct.AnimationFrameCount + CurrentSeqStruct.AnimationFrameIndex)
					{
						AnimState.GPUTransInfo.EndFrameIndex = CurrentSeqStruct.AnimationFrameCount + CurrentSeqStruct.AnimationFrameIndex;
					}
				
					AnimState.GPUTransInfo.Time = 0.0f;
					AnimState.GPUTransInfo.BlendOption = Params.BlendOption;

					IsBlendFrame = true;
					IsNewTransition = true;
				}
			}
		}
	}
	else if (bCanTransition)	//blend should happen if a sequence is already being played without blend
	{
		//#TODO looped transition is not supported yet. 
		int TransitionFrameCount = static_cast<int>(TargetSeq.SampleFrequency * Params.TransitionDuration) /*+ 1*/;
		if (TransitionFrameCount >= 3)
		{
			int Remain = TargetSeq.AnimationFrameCount - TargetLocalFrameIndex;
			TransitionFrameCount = FMath::Min(TransitionFrameCount, Remain);

			if (TransitionFrameCount >= 3)
			{
				const FAllegroSequenceDef& CurrentSeqStruct = AnimCollection->Sequences[AnimState.CurrentSequence];

				int CurFrameIndex = InstancesData.FrameIndices[InstanceIndex];
				check(CurFrameIndex < AnimCollection->FrameCountSequences);
				int CurLocalFrameIndex = CurFrameIndex - CurrentSeqStruct.AnimationFrameIndex;
				check(CurLocalFrameIndex < CurrentSeqStruct.AnimationFrameCount);

				UAllegroAnimCollection::FTransitionKey TransitionKey;
				TransitionKey.FromSI = static_cast<uint16>(AnimState.CurrentSequence);
				TransitionKey.ToSI = static_cast<uint16>(TargetAnimSeqIndex);
				TransitionKey.FromFI = CurLocalFrameIndex;
				TransitionKey.ToFI = TargetLocalFrameIndex;
				TransitionKey.FrameCount = static_cast<uint16>(TransitionFrameCount);
				TransitionKey.BlendOption = Params.BlendOption;
				TransitionKey.bFromLoops = EnumHasAnyFlags(Flags, EAllegroInstanceFlags::EIF_AnimLoop);
				TransitionKey.bToLoops = Params.bLoop;

				auto [TransitionIndex, Result] = AnimCollection->FindOrCreateTransition(TransitionKey, Params.bIgnoreTransitionGeneration);
				if (TransitionIndex != -1)
				{
					EnumRemoveFlags(Flags, EAllegroInstanceFlags::EIF_BlendFrame | EAllegroInstanceFlags::EIF_AnimNoSequence | EAllegroInstanceFlags::EIF_AnimLoop | EAllegroInstanceFlags::EIF_AnimFinished);
					EnumAddFlags(Flags, EAllegroInstanceFlags::EIF_AnimSkipTick | EAllegroInstanceFlags::EIF_AnimPlayingTransition | EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate | (Params.bLoop ? EAllegroInstanceFlags::EIF_AnimLoop : EAllegroInstanceFlags::EIF_None));

					if (IsBlendFrame)
					{
						InstancesData.BlendFrameInfoIndex[InstanceIndex] = AllocateBlendFrameIndex();
						EnumAddFlags(Flags, EAllegroInstanceFlags::EIF_BlendFrame);
					}

					const UAllegroAnimCollection::FTransition& Transition = AnimCollection->Transitions[TransitionIndex];

					AnimState.Time = Params.StartAt;
					AnimState.PlayScale = Params.PlayScale;
					AnimState.CurrentSequence = static_cast<uint16>(TargetAnimSeqIndex);
					AnimState.TransitionIndex = static_cast<uint16>(TransitionIndex);

					InstancesData.FrameIndices[InstanceIndex] = Transition.FrameIndex;

					if (GAllegro_DebugTransitions)
						DebugDrawInstanceBound(InstanceIndex, 0, Result == UAllegroAnimCollection::ETR_Success_Found ? FColor::Green : FColor::Yellow, false, 0.3f);

					return AnimLength;
				}
				else
				{
				}
			}
			else
			{
				UE_LOG(LogAllegro, Warning, TEXT("Can't Transition From %d To %d. wrapping is not supported."), AnimState.CurrentSequence, TargetAnimSeqIndex);
			}
		}
		else
		{
			UE_LOG(LogAllegro, Warning, TEXT("Can't Transition From %d To %d. duration too low."), AnimState.CurrentSequence, TargetAnimSeqIndex);
		}
	}
	else if(bIsPlayingTransition)
	{
		//UE_LOG(LogAllegro, Warning, TEXT("Can't Transition ! transitioning ."));
	}
	
//NormalPlay:

	if (bIsPlayingTransition)
	{
		this->AnimCollection->DecTransitionRef(AnimState.TransitionIndex);
	}

	EnumRemoveFlags(Flags, EAllegroInstanceFlags::EIF_BlendFrame | EAllegroInstanceFlags::EIF_GPUTransition | EAllegroInstanceFlags::EIF_AnimPlayingTransition | EAllegroInstanceFlags::EIF_AnimNoSequence | EAllegroInstanceFlags::EIF_AnimLoop | EAllegroInstanceFlags::EIF_AnimFinished);
	EnumAddFlags(Flags, EAllegroInstanceFlags::EIF_AnimSkipTick | EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate | (Params.bLoop ? EAllegroInstanceFlags::EIF_AnimLoop : EAllegroInstanceFlags::EIF_None));

	if (IsBlendFrame)
	{
		int DataIdx = AllocateBlendFrameIndex();
		InstancesData.BlendFrameInfoIndex[InstanceIndex] = DataIdx;
		EnumAddFlags(Flags, EAllegroInstanceFlags::EIF_BlendFrame);
		if (DataIdx > 0)
		{
			FInstanceBlendFrameInfo& BlendInfo = InstancesData.BlendFrameInfo[DataIdx];
			BlendInfo.Weight[0] = 1.0f;
			BlendInfo.Weight[1] = 0.0f;
			BlendInfo.Weight[2] = 0.0f;
			BlendInfo.Weight[3] = 0.0f;
		}
	}
	if (IsNewTransition)
	{
		EnumAddFlags(Flags, EAllegroInstanceFlags::EIF_GPUTransition);
		int DataIdx = InstancesData.BlendFrameInfoIndex[InstanceIndex];
		if (DataIdx > 0)
		{
			FInstanceBlendFrameInfo& BlendInfo = InstancesData.BlendFrameInfo[DataIdx];
			BlendInfo.Weight[0] = 0.0f;
			BlendInfo.Weight[1] = 1.0f;
			BlendInfo.Weight[2] = 0.0f;
			BlendInfo.Weight[3] = 0.0f;

			BlendInfo.FrameIndex[0] = AnimState.GPUTransInfo.BeginFrameIndex;
		}
	}

	AnimState.Time = Params.StartAt;
	AnimState.PlayScale = Params.PlayScale;
	AnimState.CurrentSequence = static_cast<uint16>(TargetAnimSeqIndex);

	InstancesData.FrameIndices[InstanceIndex] = TargetGlobalFrameIndex;

	if (GAllegro_DebugTransitions && Params.TransitionDuration > 0) //show that instance failed to play transition
		DebugDrawInstanceBound(InstanceIndex, -2, FColor::Red, false, 0.3f);

	return AnimLength; 
}


float UAllegroComponent::InstancePlayAnimation(int InstanceIndex, UAnimationAsset* Animation, bool bLoop, float StartAt, float PlayScale, float TransitionDuration, EAlphaBlendOption BlendOption, bool bIgnoreTransitionGeneration)
{
	FAllegroAnimPlayParams Params;
	Params.Animation = Animation;
	Params.bLoop = bLoop;
	Params.StartAt = StartAt;
	Params.PlayScale = PlayScale;
	Params.TransitionDuration = TransitionDuration;
	Params.BlendOption = BlendOption;
	Params.bIgnoreTransitionGeneration = bIgnoreTransitionGeneration;
	return InstancePlayAnimation(InstanceIndex, Params);
}

void UAllegroComponent::PlayAnimationOnAll(UAnimationAsset* Animation, bool bLoop, float StartAt, float PlayScale, float TransitionDuration, EAlphaBlendOption BlendOption, bool bIgnoreTransitionGeneration)
{
	for (int InstanceIndex = 0; InstanceIndex < this->GetInstanceCount(); InstanceIndex++)
	{
		if (IsInstanceAlive(InstanceIndex))
			InstancePlayAnimation(InstanceIndex, Animation, bLoop, StartAt, PlayScale, TransitionDuration, BlendOption, bIgnoreTransitionGeneration);
	}
}


void UAllegroComponent::SetInstanceBlendSpacePosition(int InstanceIndex, float InX, float InY)
{

	if (!IsInstanceValid(InstanceIndex))
		return;


	FAllegroInstanceAnimState& AnimState = InstancesData.AnimationStates[InstanceIndex];
	if (AnimState.InstanceExtend)
	{
		AnimState.InstanceExtend->SetBlendSpacePosition(InX, InY);
	}
}


bool UAllegroComponent::InstanceMontageJumpToSectionName(int InstanceIndex, const FString& SectionName, bool bEndOfSection)
{
	if (!IsInstanceValid(InstanceIndex))
		return false;

	FAllegroInstanceAnimState& AnimState = InstancesData.AnimationStates[InstanceIndex];
	if (AnimState.InstanceExtend)
	{
		return AnimState.InstanceExtend->MontageJumpToSectionName(SectionName, bEndOfSection);
	}

	return false;
}

// void UAllegroComponent::StopInstanceAnimation(int InstanceIndex, bool bResetPose)
// {
// 	check(IsInstanceValid(InstanceIndex));
// 	
// 	if (!InstanceHasAnyFlag(InstanceIndex, EAllegroInstanceFlags::EIF_AnimNoSequence))
// 	{
// 		FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
// 		LeaveTransitionIfAny_Internal(InstanceIndex);
// 		InstanceRemoveFlags(InstanceIndex, EAllegroInstanceFlags::EIF_AllAnimationFlags);
// 		InstanceAddFlags(InstanceIndex, EAllegroInstanceFlags::EIF_AnimNoSequence);
// 		if(bResetPose)
// 		{
// 			AS.CurrentSequence = 0xFFff;
// 			InstancesData.FrameIndices[InstanceIndex] = 0;
// 		}
// 	}
// }


void UAllegroComponent::LeaveTransitionIfAny_Internal(int InstanceIndex)
{
	if (InstanceHasAnyFlag(InstanceIndex, EAllegroInstanceFlags::EIF_AnimPlayingTransition))
	{
		InstanceRemoveFlags(InstanceIndex, EAllegroInstanceFlags::EIF_AnimPlayingTransition);

		FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
		//switch to frame index in sequence range 
		InstancesData.FrameIndices[InstanceIndex] = Utils::TransitionFrameRangeToSeuqnceFrameRange<true>(AS, InstancesData.FrameIndices[InstanceIndex], AnimCollection);
		AnimCollection->DecTransitionRef(AS.TransitionIndex);
	}
}

void UAllegroComponent::PauseInstanceAnimation(int InstanceIndex, bool bPause)
{
	check(IsInstanceValid(InstanceIndex));
	if (bPause)
		EnumAddFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_AnimPaused);
	else
		EnumRemoveFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_AnimPaused);
}

bool UAllegroComponent::IsInstanceAnimationPaused(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	return EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_AnimPaused);
}

void UAllegroComponent::ToggleInstanceAnimationPause(int InstanceIndex)
{
	check(IsInstanceValid(InstanceIndex));
	InstancesData.Flags[InstanceIndex] ^= EAllegroInstanceFlags::EIF_AnimPaused;
}

void UAllegroComponent::SetInstanceAnimationLooped(int InstanceIndex, bool bLoop)
{
	check(IsInstanceValid(InstanceIndex));
	if (bLoop)
		EnumAddFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_AnimLoop);
	else
		EnumRemoveFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_AnimLoop);
}

bool UAllegroComponent::IsInstanceAnimationLooped(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	return EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_AnimLoop);
}

void UAllegroComponent::ToggleInstanceAnimationLoop(int InstanceIndex)
{
	check(IsInstanceValid(InstanceIndex));
	InstancesData.Flags[InstanceIndex] ^= EAllegroInstanceFlags::EIF_AnimLoop;
}


bool UAllegroComponent::IsInstancePlayingAnimation(int InstanceIndex, const UAnimSequenceBase* Animation) const
{
	check(IsInstanceValid(InstanceIndex));
	const FAllegroInstanceAnimState& animState = InstancesData.AnimationStates[InstanceIndex];
	const bool bPlaying = !EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_AnimNoSequence);

	int32* idx = AnimCollection->SequenceIndexMap.Find(Animation);
	bool IsCurrentSequence = (!idx) ? false : (*idx) == animState.CurrentSequence;
	return Animation && bPlaying && animState.IsValid() && IsCurrentSequence;
}

bool UAllegroComponent::IsInstancePlayingAnyAnimation(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	//#TODO should return false if its paused ?
	return !EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_AnimNoSequence);
}

UAnimSequenceBase* UAllegroComponent::GetInstanceCurrentAnimSequence(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	const FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
	if(AS.IsValid())
		return AnimCollection->Sequences[AS.CurrentSequence].Sequence;

	return nullptr;
}

void UAllegroComponent::SetInstancePlayScale(int InstanceIndex, float NewPlayScale)
{
	check(IsInstanceValid(InstanceIndex));
	InstancesData.AnimationStates[InstanceIndex].PlayScale = NewPlayScale;
}


float UAllegroComponent::GetInstancePlayLength(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	const FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
	return AS.IsValid() ? AnimCollection->Sequences[AS.CurrentSequence].GetSequenceLength() : 0;
}

float UAllegroComponent::GetInstancePlayTime(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	const FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
	return AS.Time;
}

float UAllegroComponent::GetInstancePlayTimeRemaining(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	const FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
	if (AS.IsValid())
	{
		float SL = this->AnimCollection->Sequences[AS.CurrentSequence].GetSequenceLength();
		float RT = SL - AS.Time;
		checkSlow(RT >= 0 && RT <= SL);
		return RT;
	}
	return 0;
}

float UAllegroComponent::GetInstancePlayTimeFraction(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	const FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
	if (AS.IsValid())
	{
		float SL = this->AnimCollection->Sequences[AS.CurrentSequence].GetSequenceLength();
		float PT = AS.Time / SL;
		checkSlow(PT >= 0.0f && PT <= 1.0f);
		return PT;
	}
	return 0;
}

float UAllegroComponent::GetInstancePlayTimeRemainingFraction(int InstanceIndex) const
{
	check(IsInstanceValid(InstanceIndex));
	const FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
	if (AS.IsValid())
	{
		float SL = this->AnimCollection->Sequences[AS.CurrentSequence].GetSequenceLength();
		float RPT = 1.0f - (AS.Time / SL);
		checkSlow(RPT >= 0.0f && RPT <= 1.0f);
		return RPT;
	}
	return 0;
}

void UAllegroComponent::CallOnAnimationFinished()
{
	OnAnimationFinished(AnimationFinishEvents);
	OnAnimationFinishedDelegate.Broadcast(this, AnimationFinishEvents);
	for (int ListenerIndex = 0; ListenerIndex < this->NumListener; ListenerIndex++)
		this->ListenersPtr[ListenerIndex]->OnAnimationFinished(this->ListenersUserData[ListenerIndex], AnimationFinishEvents);
}

void UAllegroComponent::CallOnAnimationNotify()
{
	OnAnimationNotify(AnimationNotifyEvents);
	OnAnimationNotifyDelegate.Broadcast(this, AnimationNotifyEvents);
	for (int ListenerIndex = 0; ListenerIndex < this->NumListener; ListenerIndex++)
		this->ListenersPtr[ListenerIndex]->OnAnimationNotify(this->ListenersUserData[ListenerIndex], AnimationNotifyEvents);
}

FTransform3f UAllegroComponent::GetInstanceBoneTransformCS(int instanceIndex, int boneIndex,bool InGameThread) const
{
	if (IsInstanceValid(instanceIndex) && AnimCollection && AnimCollection->IsBoneTransformCached(boneIndex))
	{
		if (InGameThread)
		{
			//#TODO should we block here ?
			AnimCollection->ConditionalFlushDeferredTransitions(InstancesData.FrameIndices[instanceIndex]);
		}

		if (EnumHasAnyFlags(InstancesData.Flags[instanceIndex], EAllegroInstanceFlags::EIF_BlendFrame))
		{
			int32 DataIndex = InstancesData.BlendFrameInfoIndex[instanceIndex];
			//check(DataIndex >= 0);

			if (DataIndex > 0)
			{
				const FInstanceBlendFrameInfo& BlendInfo = InstancesData.BlendFrameInfo[DataIndex];

				FTransform3f Trans = AnimCollection->GetBoneTransformFast(boneIndex, InstancesData.FrameIndices[instanceIndex]);
				if (BlendInfo.Weight[0] > 0.99f)
				{
					return Trans;
				}

				FMatrix44f M = Trans.ToMatrixNoScale();
				M *= BlendInfo.Weight[0];

				for (int i = 1; i < ALLEGRO_BLEND_FRAME_NUM_MAX; ++i)
				{
					if (BlendInfo.Weight[i] > 0.01f)
					{
						FTransform3f Trans1 = AnimCollection->GetBoneTransformFast(boneIndex, BlendInfo.FrameIndex[i - 1]);
						M += (Trans1.ToMatrixNoScale() * BlendInfo.Weight[i]);
					}
				}
				FTransform3f OutTrans(M);
				return OutTrans;
			}
		}
		return AnimCollection->GetBoneTransformFast(boneIndex, InstancesData.FrameIndices[instanceIndex]);
	}

	return FTransform3f::Identity;
}



FTransform3f UAllegroComponent::GetInstanceBoneTransformWS(int instanceIndex, int boneIndex) const
{
	const FTransform3f& boneTransform = GetInstanceBoneTransformCS(instanceIndex, boneIndex);
	return boneTransform * GetInstanceTransform(instanceIndex);
}

int32 UAllegroComponent::GetBoneIndex(FName BoneName) const
{
	int32 BoneIndex = INDEX_NONE;
	if (BoneName != NAME_None && AnimCollection && AnimCollection->Skeleton)
	{
		BoneIndex = AnimCollection->Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName);
	}
	return BoneIndex;
}



FName UAllegroComponent::GetBoneName(int32 BoneIndex) const
{
	if (AnimCollection && AnimCollection->Skeleton)
	{
		const FReferenceSkeleton& Ref = AnimCollection->Skeleton->GetReferenceSkeleton();
		if(Ref.IsValidIndex(BoneIndex))
			return Ref.GetBoneName(BoneIndex);
	}

	return NAME_None;
}

USkeletalMeshSocket* UAllegroComponent::FindSocket(FName SocketName, const USkeletalMesh* InMesh) const
{
	if (InMesh) //#TODO check if skeletons are identical ?
		return InMesh->FindSocket(SocketName);

	if(Submeshes.Num() == 1 && Submeshes[0].SkeletalMesh)
	{
		return Submeshes[0].SkeletalMesh->FindSocket(SocketName);
	}

	if (AnimCollection && AnimCollection->Skeleton)
		return AnimCollection->Skeleton->FindSocket(SocketName);

	return nullptr;
}

FTransform3f UAllegroComponent::GetInstanceSocketTransform(int instanceIndex, FName SocketName, USkeletalMesh* InMesh, bool bWorldSpace) const
{
	const USkeletalMeshSocket* Socket = FindSocket(SocketName, InMesh);
	if (Socket)
	{
		int32 boneIndex = GetBoneIndex(Socket->BoneName);
		if(boneIndex != INDEX_NONE)
		{
			const FTransform3f transform = FTransform3f(Socket->GetSocketLocalTransform()) * GetInstanceBoneTransformCS(instanceIndex, boneIndex);
			if(bWorldSpace)
				return transform * GetInstanceTransform(instanceIndex);
			else
				return transform;
		}
	}
	else
	{
		int32 boneIndex = GetBoneIndex(SocketName);
		if (boneIndex != INDEX_NONE)
		{
			return GetInstanceBoneTransform(instanceIndex, boneIndex, bWorldSpace);
		}
	}

	return FTransform3f::Identity;
	
}


void UAllegroComponent::GetInstancesSocketTransform(TArray<FTransform3f>& OutTransforms, FName SocketName, USkeletalMesh* InMesh, bool bWorldSpace) const
{
	if (AnimCollection)
	{
		const USkeletalMeshSocket* Socket = FindSocket(SocketName, InMesh);
		if (Socket)
		{
			int32 boneIndex = GetBoneIndex(Socket->BoneName);
			if (AnimCollection->IsBoneTransformCached(boneIndex))
			{
				FTransform3f SocketLocalTransform = FTransform3f(Socket->GetSocketLocalTransform());
				OutTransforms.SetNumUninitialized(GetInstanceCount());
				for (int i = 0; i < GetInstanceCount(); i++)
				{
					if (this->IsInstanceAlive(i))
					{
						int FrameIndex = InstancesData.FrameIndices[i];
						AnimCollection->ConditionalFlushDeferredTransitions(FrameIndex);
						OutTransforms[i] = SocketLocalTransform * AnimCollection->GetBoneTransformFast(boneIndex, FrameIndex);
						if (bWorldSpace)
							OutTransforms[i] = OutTransforms[i] * GetInstanceTransform(i);
					}
					else
					{
						OutTransforms[i] = FTransform3f::Identity;
					}
				}
			}
		}
		else
		{
			int32 boneIndex = GetBoneIndex(SocketName);
			if (AnimCollection->IsBoneTransformCached(boneIndex))
			{
				OutTransforms.SetNumUninitialized(GetInstanceCount());
				for (int i = 0; i < GetInstanceCount(); i++)
				{
					if (this->IsInstanceAlive(i))
					{
						int FrameIndex = InstancesData.FrameIndices[i];
						AnimCollection->ConditionalFlushDeferredTransitions(FrameIndex);
						OutTransforms[i] = AnimCollection->GetBoneTransformFast(boneIndex, FrameIndex);
						if (bWorldSpace)
							OutTransforms[i] = OutTransforms[i] * GetInstanceTransform(i);
					}
					else
					{
						OutTransforms[i] = FTransform3f::Identity;
					}
				}
			}

		}
	}
}

void UAllegroComponent::GetInstancesSocketTransform(const FName SocketName, USkeletalMesh* InMesh, const bool bWorldSpace, TFunctionRef<void(int InstanceIndex, const FTransform3f& T)> Proc) const
{
	if (AnimCollection)
	{
		const USkeletalMeshSocket* Socket = FindSocket(SocketName, InMesh);
		if (Socket)
		{
			int32 boneIndex = GetBoneIndex(Socket->BoneName);
			if (AnimCollection->IsBoneTransformCached(boneIndex))
			{
				FTransform3f SocketLocalTransform = FTransform3f(Socket->GetSocketLocalTransform());
				for (int i = 0; i < GetInstanceCount(); i++)
				{
					if (this->IsInstanceAlive(i))
					{
						FTransform3f ST = SocketLocalTransform * GetInstanceBoneTransformCS(i, boneIndex);
						if (bWorldSpace)
							ST = ST * GetInstanceTransform(i);

						Proc(i, ST);
					}
				}
			}
		}
		else
		{
			int32 boneIndex = GetBoneIndex(SocketName);
			if (AnimCollection->IsBoneTransformCached(boneIndex))
			{
				for (int i = 0; i < GetInstanceCount(); i++)
				{
					if (this->IsInstanceValid(i))
					{
						FTransform3f ST = GetInstanceBoneTransformCS(i, boneIndex);
						if (bWorldSpace)
							ST = ST * GetInstanceTransform(i);

						Proc(i, ST);
					}
				}
			}

		}
	}
}

UAllegroComponent::FSocketMinimalInfo UAllegroComponent::GetSocketMinimalInfo(FName InSocketName, USkeletalMesh* InMesh) const
{
	FSocketMinimalInfo Ret;

	const USkeletalMeshSocket* Socket = FindSocket(InSocketName, InMesh);
	if (Socket)
	{
		Ret.BoneIndex = GetBoneIndex(Socket->BoneName);
		if (Ret.BoneIndex != INDEX_NONE)
			Ret.LocalTransform = FTransform3f(Socket->GetSocketLocalTransform());
	}
	else
	{
		Ret.BoneIndex = GetBoneIndex(InSocketName);
	}

	return Ret;
}



FTransform3f UAllegroComponent::GetInstanceSocketTransform_Fast(int InstanceIndex, const FSocketMinimalInfo& SocketInfo, bool bWorldSpace) const
{
	check(IsInstanceValid(InstanceIndex));
	const FTransform3f transform = SocketInfo.LocalTransform * GetInstanceBoneTransformCS(InstanceIndex, SocketInfo.BoneIndex);
	if (bWorldSpace)
		return transform * GetInstanceTransform(InstanceIndex);
	else
		return transform;
}




// const FBoxCenterExtentFloat* UAllegroSingleMeshComponent::GetMeshLocalBounds() const
// {
// 	const FAllegroMeshDef& MeshDef = AnimCollection->Meshes[MeshIndex];
// 
// 	if (AnimCollection->Meshes.IsValidIndex(MeshDef.OwningBoundMeshIndex))
// 		return AnimCollection->Meshes[MeshDef.OwningBoundMeshIndex].Bounds.GetData();
// 
// 	return MeshDef.Bounds.GetData();
// }




void UAllegroComponent::OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport /*= ETeleportType::None*/)
{
	Super::OnUpdateTransform(UpdateTransformFlags, ETeleportType::None);
}

void UAllegroComponent::DetachFromComponent(const FDetachmentTransformRules& DetachmentRules)
{
	Super::DetachFromComponent(DetachmentRules);
}

void UAllegroComponent::InitializeComponent()
{
	Super::InitializeComponent();
}

void UAllegroComponent::UninitializeComponent()
{
	Super::UninitializeComponent();
}

void UAllegroComponent::ResetAnimationStates()
{
	if(AnimCollection)
	{
		for (int i = 0; i < GetInstanceCount(); i++)
		{
			if (IsInstanceAlive(i))
				ResetInstanceAnimationState(i);
		}
	}
}



// void UAllegroComponent::RegisterPerInstanceData(FName ArrayPropertyName)
// {
// 	FProperty* P = this->GetClass()->FindPropertyByName(ArrayPropertyName);
// 	if (!P)
// 	{
// 		UE_LOG(LogAllegro, Warning, TEXT("RegisterPerInstanceData failed. property %s not found"), *ArrayPropertyName.ToString());
// 		return;
// 	}
// 
// 	FArrayProperty* AP = CastField<FArrayProperty>(P);
// 	if (!AP)
// 	{
// 		UE_LOG(LogAllegro, Warning, TEXT("RegisterPerInstanceData failed. property %s is not an array"), *ArrayPropertyName.ToString());
// 		return;
// 	}
// 
// 	InstanceDataProperties.AddUnique(AP);
// 
// }

void UAllegroComponent::QueryLocationOverlappingSphere(const FVector3f& Center, float Radius, TArray<int>& OutIndices) const
{
	for (int i = 0; i < GetInstanceCount(); i++)
		if (IsInstanceAlive(i))
			if (FVector3f::DistSquared(InstancesData.Locations[i], Center) < (Radius * Radius))
				OutIndices.Add(i);
}

void UAllegroComponent::QueryLocationOverlappingBox(const FBox3f& Box, TArray<int>& OutIndices) const
{
	for (int i = 0; i < GetInstanceCount(); i++)
		if (IsInstanceAlive(i))
			if (Box.IsInside(InstancesData.Locations[i]))
				OutIndices.Add(i);
}




int UAllegroComponent::GetInstanceBoundIndex(int InstanceIndex) const
{
	const FAllegroInstanceAnimState& AS = InstancesData.AnimationStates[InstanceIndex];
	int FrameIndex = InstancesData.FrameIndices[InstanceIndex];
	if (FrameIndex < AnimCollection->FrameCountSequences) //playing sequence ?
	{
		return FrameIndex;
	}
	else if(AS.IsTransitionValid()) //playing transition ?
	{
		//we use bounding box generated for sequences
		return Utils::TransitionFrameRangeToSeuqnceFrameRange<true>(AS, FrameIndex, AnimCollection);
	}
	//#TODO what happens to dynamic pose instances ?
	return 0;
}



void UAllegroComponent::DebugDrawInstanceBound(int InstanceIndex, float BoundExtentOffset, FColor const& Color, bool bPersistentLines, float LifeTime, uint8 DepthPriority, float Thickness)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (IsInstanceValid(InstanceIndex) && AnimCollection && Submeshes.Num() && GetWorld())
	{
		FBoxCenterExtentFloat B = this->CalculateInstanceBound(InstanceIndex);
		DrawDebugBox(GetWorld(), FVector(B.Center), FVector(B.Extent) + BoundExtentOffset, FQuat(GetInstanceRotation(InstanceIndex)), Color, bPersistentLines, LifeTime, DepthPriority, Thickness);
	}
#endif
}

FAllegroDynamicData* UAllegroComponent::GenerateDynamicData_Internal()
{
	FAllegroDynamicData* DynamicData = FAllegroDynamicData::Allocate(this);
	const uint32 InstanceCount = GetInstanceCount();
	this->CalcInstancesBound(DynamicData->CompBound, DynamicData->Bounds);

	InstancesData.RemoveFlags(EAllegroInstanceFlags::EIF_New | EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate);

	//happens if all instances are hidden or destroyed. rare case !
	if (DynamicData->CompBound.IsForceInitValue())
	{
		DynamicData->CompBound = FBoxMinMaxFloat(FVector3f::ZeroVector, FVector3f::ZeroVector);
		DynamicData->InstanceCount = 0;
		DynamicData->AliveInstanceCount = 0;
		DynamicData->NumCells = 0;
		
	}
	else
	{
		DynamicData->CompBound.Expand(this->ComponentBoundExtent + 1.0f);

		if (DynamicData->NumCells != 0)
		{
			DynamicData->InitGrid();

			for (uint32 InstanceIndex = 0; InstanceIndex < InstanceCount; InstanceIndex++)	//for each instance
			{
				if (EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Destroyed | EAllegroInstanceFlags::EIF_Hidden))
					continue;

				const FBoxCenterExtentFloat& IB = DynamicData->Bounds[InstanceIndex];
				int CellIdx = DynamicData->LocationToCellIndex(IB.Center);
				DynamicData->Cells[CellIdx].Bound.Add(IB);
				DynamicData->Cells[CellIdx].AddValue(*DynamicData, InstanceIndex);
			}
		}
	}



	return DynamicData;

	//for (uint32 i = PrevDynamicDataInstanceCount; i < InstanceCount; i++)
	//{
	//	DynamicData->Flags[i] |= EAllegroInstanceFlags::EIF_New;
	//}
	//PrevDynamicDataInstanceCount = DynamicData->InstanceCount;
	//
	//ENQUEUE_RENDER_COMMAND(Allegro_SendRenderDynamicData)([=](FRHICommandListImmediate& RHICmdList) {
	//	AllegroProxy->SetDynamicDataRT(DynamicData);
	//});
}

void UAllegroComponent::CalcInstancesBound(FBoxMinMaxFloat& CompBound, FBoxCenterExtentFloat* InstancesBounds)
{
	ALLEGRO_SCOPE_CYCLE_COUNTER(UAllegroComponent_CalcInstancesBound);

	if (ShouldUseFixedInstanceBound())
	{
		const FBoxCenterExtentFloat FixedBound = AnimCollection->MeshesBBox;
		for (int32 InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)	//for each instance
		{
			if (EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Destroyed | EAllegroInstanceFlags::EIF_Hidden))
				continue;

			//#TODO why FixedBound.Center += Locations[InstanceIndex] is slower
			FBoxCenterExtentFloat IB = FixedBound.TransformBy(InstancesData.Matrices[InstanceIndex]);
			CompBound.Add(IB);
			if(InstancesBounds)
				InstancesBounds[InstanceIndex] = IB;
		}
	}
	else
	{
		if (InstancesData.LocalBounds.Num() != InstancesData.Flags.Num())
		{
			InstancesData.LocalBounds.SetNum(InstancesData.Flags.Num());
			this->TimeSinceLastLocalBoundUpdate = GAllegro_LocalBoundUpdateInterval;	//fore to update local bounds
		}

		const bool bTimeForLBUpdate = this->TimeSinceLastLocalBoundUpdate >= GAllegro_LocalBoundUpdateInterval;
		if (bTimeForLBUpdate)
		{
			this->TimeSinceLastLocalBoundUpdate = FMath::Fmod(this->TimeSinceLastLocalBoundUpdate, GAllegro_LocalBoundUpdateInterval);
		}
		
		const EAllegroInstanceFlags FlagToCheck = bTimeForLBUpdate ? EAllegroInstanceFlags::EIF_None : EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate;

		for (int32 InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)	//for each instance
		{
			if (EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Destroyed | EAllegroInstanceFlags::EIF_Hidden))
				continue;

			if (EnumHasAllFlags(InstancesData.Flags[InstanceIndex], FlagToCheck))
			{
				UpdateInstanceLocalBound(InstanceIndex);
			}
			
			FBoxCenterExtentFloat IB = InstancesData.LocalBounds[InstanceIndex].TransformBy(InstancesData.Matrices[InstanceIndex]);
			CompBound.Add(IB);
			if(InstancesBounds)
				InstancesBounds[InstanceIndex] = IB;
		}
	}
}

/*
void UAllegroComponent::CalcInstancesBound_Fixed(FAllegroDynamicData* DynamicData)
{
	const FBoxCenterExtentFloat FixedBound(this->FixedInstanceBound_Center, this->FixedInstanceBound_Extent);

	for (int32 InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)	//for each instance
	{
		if (EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Destroyed | EAllegroInstanceFlags::EIF_Hidden))
			continue;

		//#TODO why the fuck FixedBound.Center += Locations[InstanceIndex] is slower
		FBoxCenterExtentFloat IB = FixedBound.TransformBy(InstancesData.Matrices[InstanceIndex]);
		DynamicData->CompBound.Add(IB);
		DynamicData->Bounds[InstanceIndex] = IB;

	}
}

void UAllegroComponent::CalcInstancesBound(FAllegroDynamicData* DynamicData)
{
	const bool bTimeForLBUpdate = this->TimeSinceLastLocalBoundUpdate >= GAllegro_LocalBoundUpdateInterval;
	if (bTimeForLBUpdate)
	{
		this->TimeSinceLastLocalBoundUpdate -= GAllegro_LocalBoundUpdateInterval;
	}

	for (int32 InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)	//for each instance
	{
		if (EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Destroyed | EAllegroInstanceFlags::EIF_Hidden))
			continue;

		if (bTimeForLBUpdate || EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate))
		{
			UpdateInstanceLocalBound(InstanceIndex);
		}

		FBoxCenterExtentFloat IB = InstancesData.LocalBounds[InstanceIndex].TransformBy(InstancesData.Matrices[InstanceIndex]);
		DynamicData->CompBound.Add(IB);
		DynamicData->Bounds[InstanceIndex] = IB;
	}
}
*/

// void UAllegroComponent::Internal_UpdateInstanceLocalBoundMM(int InstanceIndex)
// {
// #if 0
// 	const uint8* MeshSlotIter = GetInstanceMeshSlots(InstanceIndex);
// 	if (*MeshSlotIter == 0xFF) //has no mesh ?
// 	{
// 		this->InstancesData.LocalBounds[InstanceIndex] = FBoxCenterExtentFloat(ForceInit);
// 		return;
// 	}
// 
// 	FBoxMinMaxFloat LocalBound(ForceInit);
// 
// 	do
// 	{
// 		const FAllegroSubmeshSlot& Slot = this->Submeshes[*MeshSlotIter];
// 		const FBoxMinMaxFloat MaxBox = this->AnimCollection->Meshes[Slot.MeshDefIndex].MaxBBox;
// 		LocalBound.Add(MaxBox);
// 		MeshSlotIter++;
// 
// 	} while (*MeshSlotIter != 0xFF);
// 
// 	LocalBound.ToCenterExtentBox(this->InstancesData.LocalBounds[InstanceIndex]);
// 
// #else
// 	const auto BoundIndex = GetInstanceBoundIndex(InstanceIndex);
// 	const uint8* MeshSlotIter = GetInstanceMeshSlots(InstanceIndex);
// 	if (*MeshSlotIter == 0xFF) //has no mesh ?
// 	{
// 		this->InstancesData.LocalBounds[InstanceIndex] = FBoxCenterExtentFloat(ForceInit);
// 		return;
// 	}
// 
// 	FBoxMinMaxFloat LocalBound(ForceInit);
// 
// 	do
// 	{
// 		const FAllegroSubmeshSlot& Slot = this->Submeshes[*MeshSlotIter];
// 		const TConstArrayView<FBoxCenterExtentFloat> MeshBounds = this->AnimCollection->GetMeshBounds(Slot.MeshDefIndex);
// 		LocalBound.Add(MeshBounds[BoundIndex]);
// 		MeshSlotIter++;
// 
// 	} while (*MeshSlotIter != 0xFF);
// 
// 	LocalBound.ToCenterExtentBox(this->InstancesData.LocalBounds[InstanceIndex]);
// #endif // 
// }

// void UAllegroComponent::Internal_UpdateInstanceLocalBoundSM(int InstanceIndex)
// {
// 	const auto BoundIndex = GetInstanceBoundIndex(InstanceIndex);
// 	const TConstArrayView<FBoxCenterExtentFloat> MeshBounds = this->AnimCollection->GetMeshBounds(this->Submeshes[0].MeshDefIndex);
// 	InstancesData.LocalBounds[InstanceIndex] = MeshBounds[BoundIndex];
// }

void UAllegroComponent::UpdateInstanceLocalBound(int InstanceIndex)
{
	check(IsInstanceValid(InstanceIndex) && !ShouldUseFixedInstanceBound() && this->InstancesData.LocalBounds.IsValidIndex(InstanceIndex));

	FBoxMinMaxFloat LocalBound(ForceInit);

	//staticmesh 特别处理
	if (!AnimCollection)
	{
		for (int i = 0;i < Submeshes.Num();++i)
		{
			if (Submeshes[i].StaticMesh)
			{
				FBox BoundingBox = this->Submeshes[i].StaticMesh->GetBoundingBox();
				FVector Center, Extern;
				BoundingBox.GetCenterAndExtents(Center, Extern);
				FVector3f C(Center);
				FVector3f E(Extern);
				LocalBound.Add(FBoxCenterExtentFloat(C, E));
				check(!LocalBound.IsForceInitValue());
			}
		}
		LocalBound.ToCenterExtentBox(this->InstancesData.LocalBounds[InstanceIndex]);
		return;
	}

	const auto BoundIndex = GetInstanceBoundIndex(InstanceIndex);
	const uint8* MeshSlotIter = GetInstanceMeshSlots(InstanceIndex);
	if (*MeshSlotIter == 0xFF) //has no mesh ?
	{
		this->InstancesData.LocalBounds[InstanceIndex] = FBoxCenterExtentFloat(ForceInit);
		return;
	}

	do
	{
		const FAllegroSubmeshSlot& Slot = this->Submeshes[*MeshSlotIter];
		const TConstArrayView<FBoxCenterExtentFloat> MeshBounds = this->AnimCollection->GetMeshBounds(Slot.MeshDefIndex);
		LocalBound.Add(MeshBounds[BoundIndex]);
		check(!LocalBound.IsForceInitValue());
		MeshSlotIter++;

	} while (*MeshSlotIter != 0xFF);

	LocalBound.ToCenterExtentBox(this->InstancesData.LocalBounds[InstanceIndex]);
}

void UAllegroComponent::UpdateLocalBounds()
{
	if(!ShouldUseFixedInstanceBound())
	{
		for (int32 InstanceIndex = 0; InstanceIndex < GetInstanceCount(); InstanceIndex++)
		{
			if (EnumHasAnyFlags(InstancesData.Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Destroyed | EAllegroInstanceFlags::EIF_Hidden))
				continue;

			UpdateInstanceLocalBound(InstanceIndex);
		}
	}
}



void UAllegroComponent::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);
}

void UAllegroComponent::PreDuplicate(FObjectDuplicationParameters& DupParams)
{
	Super::PreDuplicate(DupParams);
}

void UAllegroComponent::OnRegister()
{
	Super::OnRegister();
	CheckAssets_Internal();
}

void UAllegroComponent::OnUnregister()
{
	Super::OnUnregister();
}

FBoxSphereBounds UAllegroComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if(!AnimCollection || !bAnyValidSubmesh || Submeshes.Num() == 0 || GetAliveInstanceCount() == 0)
		return FBoxSphereBounds(ForceInit);

	UAllegroComponent* NonConst = const_cast<UAllegroComponent*>(this);
	FBoxMinMaxFloat CompBound(ForceInit);
	NonConst->CalcInstancesBound(CompBound, nullptr);

	//happens if all instances are hidden or destroyed. rare case !
	if (CompBound.IsForceInitValue())
	{
		return FBoxSphereBounds(ForceInit);
	}
	
	CompBound.Expand(NonConst->ComponentBoundExtent);
	return FBoxSphereBounds(CompBound.ToBoxSphereBounds());
}

FBoxSphereBounds UAllegroComponent::CalcLocalBounds() const
{
	return FBoxSphereBounds();
}

TStructOnScope<FActorComponentInstanceData> UAllegroComponent::GetComponentInstanceData() const
{
	//return Super::GetComponentInstanceData();
	return MakeStructOnScope<FActorComponentInstanceData, FAllegroComponentInstanceData>(this);
}


void UAllegroComponent::ApplyInstanceData(FAllegroComponentInstanceData& ID)
{
	InstancesData = ID.InstanceData;
	IndexAllocator = ID.IndexAllocator;
	FixInstanceData();
	MarkRenderStateDirty();
}


FAllegroComponentInstanceData::FAllegroComponentInstanceData(const UAllegroComponent* InComponent) : FPrimitiveComponentInstanceData(InComponent)
{
	InstanceData = InComponent->InstancesData;
	IndexAllocator = InComponent->IndexAllocator;
}

void FAllegroComponentInstanceData::ApplyToComponent(UActorComponent* Component, const ECacheApplyPhase CacheApplyPhase)
{
	Super::ApplyToComponent(Component, CacheApplyPhase);

	UAllegroComponent* allegro = CastChecked<UAllegroComponent>(Component);
	allegro->ApplyInstanceData(*this);
}

void FAllegroComponentInstanceData::AddReferencedObjects(FReferenceCollector& Collector)
{
    InstanceData.AddReferencedObjects(Collector);
	Super::AddReferencedObjects(Collector);
}





void FAllegroInstancesData::Reset()
{
	Flags.Reset();
	FrameIndices.Reset();
	AnimationStates.Reset();
	Locations.Reset();
	Rotations.Reset();
	Scales.Reset();
	Matrices.Reset();
	RenderCustomData.Reset();
	MeshSlots.Reset();
	LocalBounds.Reset();
	CustomPerInstanceStruct.Reset();
	BlendFrameInfoIndex.Reset();
	Stencil.Reset();
}

void FAllegroInstancesData::Empty()
{
	Flags.Empty();
	FrameIndices.Empty();
	AnimationStates.Empty();
	Locations.Empty();
	Rotations.Empty();
	Scales.Empty();
	Matrices.Empty();
	RenderCustomData.Empty();
	MeshSlots.Empty();
	LocalBounds.Empty();
	CustomPerInstanceStruct.Empty();
	BlendFrameInfoIndex.Empty();
	Stencil.Empty();
}

FArchive& operator<<(FArchive& Ar, FAllegroInstancesData& R)
{
	R.Flags.BulkSerialize(Ar);
	R.Locations.BulkSerialize(Ar);
	R.Rotations.BulkSerialize(Ar);
	R.Scales.BulkSerialize(Ar);
	R.RenderCustomData.BulkSerialize(Ar);
	//R.AnimationStates.BulkSerialize(Ar); //#TODO
	R.CustomPerInstanceStruct.BulkSerialize(Ar);
	return Ar;
}


void FAllegroInstancesData::RemoveFlags(EAllegroInstanceFlags FlagsToRemove)
{
	constexpr int NumEnumPerPack = sizeof(VectorRegister4Int) / sizeof(EAllegroInstanceFlags);
	constexpr int EnumNumBits = sizeof(EAllegroInstanceFlags) * 8;
	check(Flags.Num() % NumEnumPerPack == 0);
	const int MaskDW = ~(static_cast<int>(FlagsToRemove) | (static_cast<int>(FlagsToRemove) << EnumNumBits));
	AllegroArrayAndSSE(Flags.GetData(), Flags.Num() / NumEnumPerPack, MaskDW);
}


void FAllegroInstancesData::AddReferencedObjects(FReferenceCollector& Collector)
{
	for (int i=0;i<AnimationStates.Num();++i)
	{
		if (!EnumHasAnyFlags(Flags[i], EAllegroInstanceFlags::EIF_Destroyed))
		{
			AnimationStates[i].AddReferencedObjects(Collector);
		}
	}
}





//bool FAllegroInstancesData::operator==(const FAllegroInstancesData& Other) const
//{
//	return this->Flags.Num() == Other.Flags.Num()
//		&& this->FrameIndices.Num() == Other.FrameIndices.Num()
//		&& this->AnimationStates.Num() == Other.AnimationStates.Num()
//		&& this->Locations.Num() == Other.Locations.Num()
//		&& this->Rotations.Num() == Other.Rotations.Num()
//		&& this->Scales.Num() == Other.Scales.Num()
//		&& this->Matrices.Num() == Other.Matrices.Num()
//		&& this->RenderMatrices.Num() == Other.RenderMatrices.Num()
//		&& this->RenderCustomData.Num() == Other.RenderCustomData.Num()
//		&& this->MeshSlots.Num() == Other.MeshSlots.Num()
//		&& this->LocalBounds.Num() == Other.LocalBounds.Num();
//}

















//FPrimitiveSceneProxy* UAllegroSingleMeshComponent::CreateSceneProxy()
//{
//	if (GetAliveInstanceCount() > 0 && SkeletalMesh && AnimCollection && AnimCollection->Skeleton == SkeletalMesh->GetSkeleton() && MeshIndex != -1 && AnimCollection->Meshes[MeshIndex].MeshData)
//	{
//		return new FAllegroProxy(this, GetFName());
//	}
//
//	return nullptr;
//}

//FBoxSphereBounds UAllegroSingleMeshComponent::CalcBounds(const FTransform& LocalToWorld) const
//{
//	//bRenderDynamicDataDirty must be true because we mark both transform and dynamic data dirty at the same time, see UActorComponent::DoDeferredRenderUpdates_Concurrent
//	if(MeshIndex == -1)
//		return FBoxSphereBounds(ForceInit);
//
//	return FBoxSphereBounds(CalcInstancesBound(GetMeshLocalBounds()).ToBoxSphereBounds());
//}
//
//FBoxSphereBounds UAllegroSingleMeshComponent::CalcLocalBounds() const
//{
//	if(!SkeletalMesh)
//		return FBoxSphereBounds(ForceInit);
//	
//	return SkeletalMesh->GetBounds();
//}


/*
void UAllegroSingleMeshComponent::Internal_CheckAssets()
{
	check(IsInGameThread());
	MeshIndex = -1;
	PrevDynamicDataInstanceCount = 0;
	MarkRenderStateDirty();

	if (SkeletalMesh && AnimCollection)
	{
		if (AnimCollection->Skeleton == nullptr || SkeletalMesh->GetSkeleton() != AnimCollection->Skeleton)
		{
			//#TODO how to log bone mismatch ? logging error during cook will interrupt it
			//UE_LOG(LogAllegro, Error, TEXT("SkeletalMesh and AnimSet must use the same Skeleton. Component:%s AnimSet:%s SkeletalMesh:%s"), *GetName(), *AnimCollection->GetName(), *SkeletalMesh->GetName());
			//if new data is invalid (null AnimSet, mismatch skeleton, ...) we keep transforms but reset animation related data
			ResetAnimationStates();
			return;
		}

		if (WITH_EDITOR && FApp::CanEverRender())
		{
			AnimCollection->TryBuildAll();
		}

		int MeshDefIdx = AnimCollection->FindMeshDef(SkeletalMesh);
		if (MeshDefIdx == -1 || !AnimCollection->Meshes[MeshDefIdx].MeshData)
		{
			//UE_LOG(LogAllegro, Error, TEXT("SkeletalMesh [%s] is not registered in AnimSet [%s]"), *SkeletalMesh->GetName(), *AnimCollection->GetName());
			//if new data is invalid (null AnimSet, mismatch skeleton, ...) we keep transforms but reset animation related data
			ResetAnimationStates();
			return;
		}

		this->MeshIndex = MeshDefIdx;
	}
}*/


int UAllegroComponent::LineTraceInstanceAny(int InstanceIndex, const FVector& Start, const FVector& End) const
{
	check(IsInstanceValid(InstanceIndex));
#if 0
	if (MeshIndex != -1)
	{
		const FTransform T = FTransform(GetInstanceTransform(InstanceIndex));
		FVector LocalStart = T.InverseTransformPosition(Start);
		FVector LocalEnd = T.InverseTransformPosition(End);
		FVector LocalDir = LocalEnd - LocalStart;

		const FBoxCenterExtentFloat& LocalBound = GetInstanceLocalBound(InstanceIndex);
		if (!FMath::LineBoxIntersection(FBox(LocalBound.GetFBox()), LocalStart, LocalEnd, LocalDir))
		{
			return -1;
		}


		auto Len = LocalDir.Size();
		LocalDir /= Len;
		const FAllegroMeshDef& MeshDef = this->AnimCollection->Meshes[MeshIndex];
		return MeshDef.CompactPhysicsAsset.RayCastAny(AnimCollection, InstancesData.FrameIndices[InstanceIndex], LocalStart, LocalDir, Len);
	}
#endif // 
	return -1;
}

int UAllegroComponent::OverlapTestInstance(int InstanceIndex, const FVector& Point, float Thickness) const
{
	check(IsInstanceValid(InstanceIndex));
#if 0
	if (MeshIndex != -1)
	{
		const FTransform T = FTransform(GetInstanceTransform(InstanceIndex));
		FVector LocalPoint = T.InverseTransformPosition(Point);

		const FAllegroMeshDef& MeshDef = this->AnimCollection->Meshes[MeshIndex];
		return MeshDef.CompactPhysicsAsset.Overlap(AnimCollection, InstancesData.FrameIndices[InstanceIndex], LocalPoint, Thickness);
	}
#endif // 

	return -1;
}

int UAllegroComponent::LineTraceInstanceSingle(int InstanceIndex, const FVector& Start, const FVector& End, float Thickness, double& OutTime, FVector& OutPosition, FVector& OutNormal) const
{
	check(IsInstanceValid(InstanceIndex));
#if 0
	if (MeshIndex != -1)
	{
		const FTransform T = FTransform(GetInstanceTransform(InstanceIndex));
		FVector LocalStart = T.InverseTransformPosition(Start);
		FVector LocalEnd = T.InverseTransformPosition(End);
		FVector LocalDir = LocalEnd - LocalStart;
		auto Len = LocalDir.Size();
		LocalDir /= Len;

		const FBox LocalBound = FBox(GetInstanceLocalBound(InstanceIndex).GetFBox());
		Chaos::FReal unusedTime;
		Chaos::FVec3 unusedPos, unusedNormal;
		int unusedFaceIndex;

		if (!Chaos::FAABB3(LocalBound.Min, LocalBound.Max).Raycast(LocalStart, LocalDir, Len, Thickness, unusedTime, unusedPos, unusedNormal, unusedFaceIndex))
		{
			return -1;
		}

		const FAllegroMeshDef& MeshDef = this->AnimCollection->Meshes[MeshIndex];
		int HitBone = MeshDef.CompactPhysicsAsset.Raycast(AnimCollection, InstancesData.FrameIndices[InstanceIndex], LocalStart, LocalDir, Len, Thickness, OutTime, OutPosition, OutNormal);
		if (HitBone != -1)
		{
			OutPosition = T.TransformPosition(OutPosition);
			OutNormal = T.TransformVector(OutNormal);
		}
		return HitBone;
	}
#endif // 

	return -1;
}


int UAllegroComponent::LineTraceInstancesSingle(const TArrayView<int> InstanceIndices, const FVector& Start, const FVector& End, double Thickness, double& OutTime, FVector& OutPosition, FVector& OutNormal, int& OutBoneIndex) const
{
	int HitInstanceIndex = -1;
#if 0
	for (int InstanceIndex : InstanceIndices)
	{
		double TOI;
		FVector HitPos, HitNorm;
		int HitBone = LineTraceInstanceSingle(InstanceIndex, Start, End, Thickness, TOI, HitPos, HitNorm);
		if (HitBone != -1)
		{
			if (HitInstanceIndex == -1 || TOI < OutTime)
			{
				HitInstanceIndex = InstanceIndex;
				OutTime = TOI;
				OutPosition = HitPos;
				OutNormal = HitNorm;
				OutBoneIndex = HitBone;
			}
		}
	}
#endif

	return HitInstanceIndex;
}













bool UAllegroComponent::EnableInstanceDynamicPose(int InstanceIndex, bool bEnable)
{
	if(!IsInstanceValid(InstanceIndex))
		return false;

	const bool bIsDynamic = InstanceHasAnyFlag(InstanceIndex, EAllegroInstanceFlags::EIF_DynamicPose);
	if (bIsDynamic == bEnable)
		return true;

	
	if (bEnable)
	{
		int DynamicPoseIndex = AnimCollection->AllocDynamicPose();
		if (DynamicPoseIndex == -1)
		{
			UE_LOG(LogAllegro, Warning, TEXT("EnableInstanceDynamicPose Failed. Buffer Full! InstanceIndex:%d"), InstanceIndex);
			return false;
		}

		//release transition if any
		if (InstanceHasAnyFlag(InstanceIndex, EAllegroInstanceFlags::EIF_AnimPlayingTransition))
			AnimCollection->DecTransitionRef(InstancesData.AnimationStates[InstanceIndex].TransitionIndex);

		InstanceRemoveFlags(InstanceIndex, EAllegroInstanceFlags::EIF_AllAnimationFlags);
		InstanceAddFlags(InstanceIndex, EAllegroInstanceFlags::EIF_DynamicPose);
		InstancesData.FrameIndices[InstanceIndex] = AnimCollection->DynamicPoseIndexToFrameIndex(DynamicPoseIndex);
		InstancesData.AnimationStates[InstanceIndex] = FAllegroInstanceAnimState{};
	}
	else
	{
		ReleaseDynamicPose_Internal(InstanceIndex);
	}

	return true;
}

void UAllegroComponent::ReleaseDynamicPose_Internal(int InstanceIndex)
{
	InstanceRemoveFlags(InstanceIndex, EAllegroInstanceFlags::EIF_AllAnimationFlags | EAllegroInstanceFlags::EIF_DynamicPose);
	InstanceAddFlags(InstanceIndex, EAllegroInstanceFlags::EIF_AnimNoSequence);
	int& FrameIndex = InstancesData.FrameIndices[InstanceIndex];
	check(AnimCollection->IsDynamicPoseFrameIndex(FrameIndex));
	int DynamicPoseIndex = AnimCollection->FrameIndexToDynamicPoseIndex(FrameIndex);
	AnimCollection->FlipDynamicPoseSign(DynamicPoseIndex);
	AnimCollection->FreeDynamicPose(DynamicPoseIndex);
	
	FrameIndex = 0;
	InstancesData.AnimationStates[InstanceIndex] = FAllegroInstanceAnimState{};
}

/*
returned pointer must be filled before any new call.
*/
FMatrix3x4* UAllegroComponent::InstanceRequestDynamicPoseUpload(int InstanceIndex)
{
	check(AnimCollection && IsInstanceValid(InstanceIndex) && IsInstanceDynamicPose(InstanceIndex));
	int& FrameIndex = InstancesData.FrameIndices[InstanceIndex];
	check(AnimCollection->IsDynamicPoseFrameIndex(FrameIndex));
	int DynamicPoseIndex = AnimCollection->FrameIndexToDynamicPoseIndex(FrameIndex);
	FrameIndex = AnimCollection->FlipDynamicPoseSign(DynamicPoseIndex);
	return AnimCollection->RequestPoseUpload(FrameIndex);
}



namespace Utils
{
	void FillDynamicPoseFromComponent_Concurrent(UAllegroComponent * Self, USkeletalMeshComponent * MeshComp, int ScatterIdx, int InstanceIndex, TArrayView<FTransform> FullPose)
	{
		Self->SetInstanceTransform(InstanceIndex, FTransform3f(MeshComp->GetComponentTransform()));

		const TArray<FTransform>& Transforms = MeshComp->GetComponentSpaceTransforms();
		USkeleton* Skeleton = Self->AnimCollection->Skeleton;

		const FSkeletonToMeshLinkup* LinkupTable = nullptr;
		//LinkupCache lock scope
		{
			FScopeLock ScopeLock(&Skeleton->LinkupCacheLock);
			const int32 LinkupCacheIdx = Skeleton->GetMeshLinkupIndex(MeshComp->GetSkeletalMeshAsset());
			LinkupTable = &Skeleton->LinkupCache[LinkupCacheIdx];
		}

		for (FBoneIndexType BoneIndex : MeshComp->RequiredBones)
		{
			int SKBoneIndex = LinkupTable->MeshToSkeletonTable[BoneIndex];
			FullPose[SKBoneIndex] = Transforms[BoneIndex];
		}

		Self->AnimCollection->CurrentUpload.ScatterData[ScatterIdx] = Self->InstancesData.FrameIndices[InstanceIndex];
		FMatrix3x4* RenderMatrices = &Self->AnimCollection->CurrentUpload.PoseData[ScatterIdx * Self->AnimCollection->RenderBoneCount];

		Self->AnimCollection->CalcRenderMatrices(FullPose, RenderMatrices);
	}
};


void UAllegroComponent::FillDynamicPoseFromComponents()
{
	if (DynamicPoseInstancesTiedToSMC.Num() == 0)
		return;

	ALLEGRO_SCOPE_CYCLE_COUNTER(FillDynamicPoseFromComponents);
	
	int ScatterBaseIndex = AnimCollection->ReserveUploadData(DynamicPoseInstancesTiedToSMC.Num());
	int NumProcessed = 0;

	TArray<FTransform, TInlineAllocator<255>> FullPose;
	FullPose.Init(FTransform::Identity, AnimCollection->AnimationBoneCount);

	for (const auto& Pair : DynamicPoseInstancesTiedToSMC)
	{
		if (Pair.Value.MeshComponent)
		{
			int& FrameIndex = InstancesData.FrameIndices[Pair.Value.InstanceIndex];
			int DynamicPoseIndex = AnimCollection->FrameIndexToDynamicPoseIndex(FrameIndex);
			FrameIndex = AnimCollection->FlipDynamicPoseSign(DynamicPoseIndex); //#TODO is safe for concurrent ?

			FullPose.Init(FTransform::Identity, AnimCollection->AnimationBoneCount);
			Utils::FillDynamicPoseFromComponent_Concurrent(this, Pair.Value.MeshComponent, ScatterBaseIndex + NumProcessed, Pair.Value.InstanceIndex, FullPose);
			NumProcessed++;
		}
		else
		{
		}
	}

	if (NumProcessed != DynamicPoseInstancesTiedToSMC.Num())
	{
		AnimCollection->UploadDataSetNumUninitialized(ScatterBaseIndex + NumProcessed);
	}
}

void UAllegroComponent::FillDynamicPoseFromComponents_Concurrent()
{
	FillDynamicPoseFromComponents();
}

USkeletalMeshComponent* UAllegroComponent::GetInstanceTiedSkeletalMeshComponent(int InstanceIndex) const
{
	int DPI = AnimCollection->FrameIndexToDynamicPoseIndex(this->InstancesData.FrameIndices[InstanceIndex]);
	return DynamicPoseInstancesTiedToSMC.FindRef(DPI).MeshComponent;
}

void UAllegroComponent::TieDynamicPoseToComponent(int InstanceIndex, USkeletalMeshComponent* SrcComponent, int UserData)
{
	if(IsInstanceValid(InstanceIndex) && IsInstanceDynamicPose(InstanceIndex) && IsValid(SrcComponent))
	{
		InstanceAddFlags(InstanceIndex, EAllegroInstanceFlags::EIF_BoundToSMC);
		int DPI = AnimCollection->FrameIndexToDynamicPoseIndex(this->InstancesData.FrameIndices[InstanceIndex]);
		DynamicPoseInstancesTiedToSMC.FindOrAdd(DPI, FAllegroDynInsTieData { SrcComponent, InstanceIndex, UserData });
	}
}

void UAllegroComponent::UntieDynamicPoseFromComponent(int InstanceIndex)
{
	if(IsInstanceValid(InstanceIndex) && InstanceHasAllFlag(InstanceIndex, EAllegroInstanceFlags::EIF_DynamicPose | EAllegroInstanceFlags::EIF_BoundToSMC))
	{
		InstanceRemoveFlags(InstanceIndex, EAllegroInstanceFlags::EIF_BoundToSMC);
		int DPI = AnimCollection->FrameIndexToDynamicPoseIndex(this->InstancesData.FrameIndices[InstanceIndex]);
		DynamicPoseInstancesTiedToSMC.FindAndRemoveChecked(DPI);
	}
}

int UAllegroComponent::FindListener(IAllegroListenerInterface* InterfacePtr) const
{
	for (int i = 0; i < NumListener; i++)
		if (ListenersPtr[i] == InterfacePtr)
			return i;

	return -1;
}

int UAllegroComponent::FindListenerUserData(IAllegroListenerInterface* InterfacePtr) const
{
	for (int i = 0; i < NumListener; i++)
		if (ListenersPtr[i] == InterfacePtr)
			return ListenersUserData[i];

	return -1;
}

void UAllegroComponent::RemoveListener(IAllegroListenerInterface* InterfacePtr)
{
	int Index = FindListener(InterfacePtr);
	if (Index != -1)
	{
		//perform remove at swap
		NumListener--;
		ListenersPtr[Index] = ListenersPtr[NumListener];
		ListenersUserData[Index] = ListenersUserData[NumListener];
	}
}

int UAllegroComponent::AddListener(IAllegroListenerInterface* InterfacePtr, int UserData)
{
	if (InterfacePtr && CanAddListener())
	{
		ListenersPtr[NumListener] = InterfacePtr;
		ListenersUserData[NumListener] = UserData;
		return NumListener++;
	}
	return -1;
}

FPrimitiveSceneProxy* UAllegroComponent::CreateSceneProxy()
{
	if (AnimCollection && AnimCollection->bIsBuilt && Submeshes.Num() > 0 && bAnyValidSubmesh)
	{
		return new FAllegroProxy(this, GetFName());
	}
	else
	{
		if (Submeshes.Num() > 0 && Submeshes[0].StaticMesh)
		{
			return new FAllegroProxy(this, GetFName());
		}
	}

	//this->bRenderStateCreated will be still true but this->SceneProxy null
	return nullptr;
}

bool UAllegroComponent::ShouldCreateRenderState() const
{
	return true;
	//return AnimCollection != nullptr && Submeshes.Num() > 0;
}

void UAllegroComponent::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
}

// FBoxSphereBounds UAllegroComponent::CalcBounds(const FTransform& LocalToWorld) const
// {
// 	//bRenderDynamicDataDirty must be true because we mark both transform and dynamic data dirty at the same time, see UActorComponent::DoDeferredRenderUpdates_Concurrent
// 	if (Submeshes.Num() == 0 || !Submeshes[0].SkeletalMesh || Submeshes[0].MeshDefIndex == -1)
// 		return FBoxSphereBounds(ForceInit);
// 
// 
// 	return FBoxSphereBounds(CalcInstancesBound(GetMeshLocalBounds()).ToBoxSphereBounds());
// }
// 
// FBoxSphereBounds UAllegroComponent::CalcLocalBounds() const
// {
// 	return FBoxSphereBounds(ForceInit);
// }

void UAllegroComponent::CheckAssets_Internal()
{
	check(IsInGameThread());
	PrevDynamicDataInstanceCount = 0;
	for(FAllegroSubmeshSlot& MeshSlot : Submeshes)
	{
		MeshSlot.MeshDefIndex = -1;
	}
	bAnyValidSubmesh = false;

	if (AnimCollection)
	{
		if(AnimCollection->Skeleton)
		{
			for (FAllegroSubmeshSlot& MeshSlot : Submeshes)
			{
				if (!MeshSlot.SkeletalMesh)
				{
					continue;
				}

				if (MeshSlot.SkeletalMesh->GetSkeleton() != AnimCollection->Skeleton)
				{
					UE_LOG(LogAllegro, Warning, TEXT("MeshSlot.SkeletalMesh Skeleton does not match to AnimCollection"));
					continue;
				}

				int MeshDefIdx = AnimCollection->FindMeshDef(MeshSlot.SkeletalMesh);
				if (MeshDefIdx == -1 /*|| !AnimCollection->Meshes[MeshDefIdx].MeshData*/)
				{
					UE_LOG(LogAllegro, Warning, TEXT("MeshSlot.SkeletalMesh is not finded in AnimCollection"));
					continue;
				}

				MeshSlot.MeshDefIndex = MeshDefIdx;
				bAnyValidSubmesh = true;
			}
#if WITH_EDITOR
			if (FApp::CanEverRender() && bAnyValidSubmesh)
			{
				AnimCollection->TryBuildAll();
			}
#endif
		}
	}
	else
	{

	}

	//UpdateBounds();
	MarkRenderStateDirty();
	MarkCachedMaterialParameterNameIndicesDirty();
	IStreamingManager::Get().NotifyPrimitiveUpdated(this);
}

#if WITH_EDITORONLY_DATA
void UAllegroComponent::AddAllSkeletalMeshes()
{
	if (AnimCollection)
	{
		for (const FAllegroMeshDef& MD : AnimCollection->Meshes)
		{
			if (MD.Mesh && FindSubmeshIndex(MD.Mesh) == -1)
			{
				FAllegroSubmeshSlot& SM = Submeshes.AddDefaulted_GetRef();
				SM.SkeletalMesh = MD.Mesh;
			}
		}
		CheckAssets_Internal();
	}
}
#endif



int UAllegroComponent::FindSubmeshIndex(const USkeletalMesh* SubMeshAsset) const
{
	return Submeshes.IndexOfByPredicate([=](const FAllegroSubmeshSlot& Slot) { return Slot.SkeletalMesh == SubMeshAsset; });
}

int UAllegroComponent::FindSubmeshIndex(FName SubMeshName) const
{
	return Submeshes.IndexOfByPredicate([=](const FAllegroSubmeshSlot& Slot) { return Slot.Name == SubMeshName; });
}

int UAllegroComponent::FindSubmeshIndex(FName SubmeshName, const USkeletalMesh* OrSubmeshAsset) const
{
	return SubmeshName.IsNone() ? FindSubmeshIndex(OrSubmeshAsset) : FindSubmeshIndex(SubmeshName);
}

bool UAllegroComponent::InstanceAttachSubmeshByName(int InstanceIndex, FName SubMeshName, bool bAttach)
{
	if (IsInstanceValid(InstanceIndex) && !SubMeshName.IsNone())
	{
		int MeshIndex = FindSubmeshIndex(SubMeshName);
		if (MeshIndex != -1 && Submeshes[MeshIndex].MeshDefIndex != -1)
		{
			return InstanceAttachSubmeshByIndex_Internal(InstanceIndex, (uint8)MeshIndex, bAttach);
		}
	}
	return false;
}

bool UAllegroComponent::InstanceAttachSubmeshByAsset(int InstanceIndex, USkeletalMesh* InMesh, bool bAttach)
{
	if (IsInstanceValid(InstanceIndex) && InMesh)
	{
		int MeshIndex = FindSubmeshIndex(InMesh);
		if (MeshIndex != -1 && Submeshes[MeshIndex].MeshDefIndex != -1)
		{
			return InstanceAttachSubmeshByIndex_Internal(InstanceIndex, (uint8)MeshIndex, bAttach);
		}
	}
	return false;
}


bool UAllegroComponent::InstanceAttachSubmeshByIndex(int InstanceIndex, uint8 MeshIndex, bool bAttach)
{
	if(IsInstanceValid(InstanceIndex) && Submeshes.IsValidIndex(MeshIndex))
	{
		return InstanceAttachSubmeshByIndex_Internal(InstanceIndex, MeshIndex, bAttach);
	}
	return false;
}


bool UAllegroComponent::InstanceAttachSubmeshByIndex_Internal(int InstanceIndex, uint8 MeshIndex, bool bAttach)
{
	check(this->Submeshes.IsValidIndex(MeshIndex) && this->Submeshes[MeshIndex].MeshDefIndex != -1);
	InstanceAddFlags(InstanceIndex, EAllegroInstanceFlags::EIF_NeedLocalBoundUpdate);
	uint8* SlotBegin = GetInstanceMeshSlots(InstanceIndex);
	if (bAttach)
	{
		return AllegroTerminatedArrayAddUnique(SlotBegin, MaxMeshPerInstance, MeshIndex);
	}
	else
	{
		return AllegroTerminatedArrayRemoveShift(SlotBegin, MaxMeshPerInstance, MeshIndex);
	}
}

int UAllegroComponent::InstanceAttachSubmeshes(int InstanceIndex, const TArray<USkeletalMesh*>& InMeshes, bool bAttach)
{
	int NumAttach = 0;
	if (IsInstanceValid(InstanceIndex))
	{
		for (USkeletalMesh* SKMesh : InMeshes)
		{
			if(InstanceAttachSubmeshByAsset(InstanceIndex, SKMesh, bAttach))
				NumAttach++;
		}
	}
	return NumAttach;
}

void UAllegroComponent::InstanceDetachAllSubmeshes(int InstanceIndex)
{
	if(IsInstanceValid(InstanceIndex))
	{
		uint8* MS = this->GetInstanceMeshSlots(InstanceIndex);
		*MS = 0xFF;
	}
}

void UAllegroComponent::GetInstanceAttachedSkeletalMeshes(int InstanceIndex, TArray<USkeletalMesh*>& OutMeshes) const
{
	if(IsInstanceValid(InstanceIndex))
	{
		const uint8* SlotIter = this->GetInstanceMeshSlots(InstanceIndex);
		while(*SlotIter != 0xFF)
		{
			OutMeshes.Add(this->Submeshes[*SlotIter].SkeletalMesh);
			SlotIter++;
		}
	}
	
}

void UAllegroComponent::ResetMeshSlots()
{
	if (InstancesData.MeshSlots.Num())
	{
		FMemory::Memset(InstancesData.MeshSlots.GetData(), 0xFF, InstancesData.MeshSlots.Num() * InstancesData.MeshSlots.GetTypeSize());
	}
}

void UAllegroComponent::AddSubmesh(const FAllegroSubmeshSlot& InData)
{
	if (Submeshes.Num() >= ALLEGRO_MAX_SUBMESH)
	{
		UE_LOG(LogAllegro, Warning, TEXT("AddSubmesh failed. reached maximum."));
		return;
	}

	Submeshes.Emplace(InData);
	CheckAssets_Internal();
}



void UAllegroComponent::InitSubmeshesFromAnimCollection()
{
	if (this->AnimCollection)
	{
		const int Count = FMath::Min(ALLEGRO_MAX_SUBMESH, this->AnimCollection->Meshes.Num());
		this->Submeshes.Reset();
		this->Submeshes.SetNum(Count);

		for (int SubMeshIndex = 0; SubMeshIndex < Count; SubMeshIndex++)
		{
			this->Submeshes[SubMeshIndex].SkeletalMesh = this->AnimCollection->Meshes[SubMeshIndex].Mesh;
		}

		ResetMeshSlots();
		CheckAssets_Internal();
	}
	
}

void UAllegroComponent::SetSubmeshAsset(int SubmeshIndex, USkeletalMesh* InMesh)
{
	if (Submeshes.IsValidIndex(SubmeshIndex))
	{
		if (Submeshes[SubmeshIndex].SkeletalMesh != InMesh)
		{
			Submeshes[SubmeshIndex].SkeletalMesh = InMesh;
			CheckAssets_Internal();
		}
	}
}

int UAllegroComponent::GetSubmeshBaseMaterialIndex(int SubmeshIndex) const
{
	if (Submeshes.IsValidIndex(SubmeshIndex) )
	{
		if (Submeshes[SubmeshIndex].SkeletalMesh)
		{
			int Counter = 0;
			for (int i = 0; i < Submeshes.Num(); i++)
			{
				if (Submeshes[i].SkeletalMesh)
				{
					if (i == SubmeshIndex)
						return Counter;

					Counter += Submeshes[i].SkeletalMesh->GetMaterials().Num();
				}
			}
		}
		else if (Submeshes[SubmeshIndex].StaticMesh)
		{
			int Counter = 0;
			for (int i = 0; i < Submeshes.Num(); i++)
			{
				if (Submeshes[i].StaticMesh)
				{
					if (i == SubmeshIndex)
						return Counter;

					Counter += Submeshes[i].StaticMesh->GetStaticMaterials().Num();
				}
			}
		}
	}
	return -1;
}

int UAllegroComponent::GetSubmeshBaseMaterialIndexByAsset(USkeletalMesh* InMesh) const
{
	return GetSubmeshBaseMaterialIndex(FindSubmeshIndex(InMesh));
}

int UAllegroComponent::GetSubmeshBaseMaterialIndexByName(FName InName) const
{
	return GetSubmeshBaseMaterialIndex(FindSubmeshIndex(InName));
}

void UAllegroComponent::SetSubmeshMaterial(int SubmeshIndex, int MaterialIndex, UMaterialInterface* Material)
{
	if (Submeshes.IsValidIndex(SubmeshIndex) )
	{
		if (Submeshes[SubmeshIndex].SkeletalMesh)
		{
			const TArray<FSkeletalMaterial>& Mats = Submeshes[SubmeshIndex].SkeletalMesh->GetMaterials();
			if (Mats.IsValidIndex(MaterialIndex))
			{
				SetMaterial(GetSubmeshBaseMaterialIndex(SubmeshIndex) + MaterialIndex, Material);
				return;
			}
		}
		else if (Submeshes[SubmeshIndex].StaticMesh)
		{
			const TArray<FStaticMaterial>& Mats = Submeshes[SubmeshIndex].StaticMesh->GetStaticMaterials();
	
			if (Mats.IsValidIndex(MaterialIndex))
			{
				SetMaterial(GetSubmeshBaseMaterialIndex(SubmeshIndex) + MaterialIndex, Material);
				return;
			}
		}
	}
}

void UAllegroComponent::SetSubmeshMaterial(int SubmeshIndex, FName MaterialSlotName, UMaterialInterface* Material)
{
	if (Submeshes.IsValidIndex(SubmeshIndex) )
	{
		if (Submeshes[SubmeshIndex].SkeletalMesh)
		{
			const TArray<FSkeletalMaterial>& Mats = Submeshes[SubmeshIndex].SkeletalMesh->GetMaterials();
			for (int MaterialIndex = 0; MaterialIndex < Mats.Num(); MaterialIndex++)
			{
				if (Mats[MaterialIndex].MaterialSlotName == MaterialSlotName)
				{
					SetMaterial(GetSubmeshBaseMaterialIndex(SubmeshIndex) + MaterialIndex, Material);
					return;
				}
			}
		}else if (Submeshes[SubmeshIndex].StaticMesh)
		{
			const TArray<FStaticMaterial>& Mats = Submeshes[SubmeshIndex].StaticMesh->GetStaticMaterials();
			for (int MaterialIndex = 0; MaterialIndex < Mats.Num(); MaterialIndex++)
			{
				if (Mats[MaterialIndex].MaterialSlotName == MaterialSlotName)
				{
					SetMaterial(GetSubmeshBaseMaterialIndex(SubmeshIndex) + MaterialIndex, Material);
					return;
				}
			}
		}
	}
}

void UAllegroComponent::SetSubmeshMaterial(FName SubmeshName, USkeletalMesh* OrSubmeshAsset, FName MaterialSlotName, int OrMaterialIndex, UMaterialInterface* Material)
{
	if (!SubmeshName.IsNone())
	{
		if (!MaterialSlotName.IsNone())
		{
			SetSubmeshMaterial(FindSubmeshIndex(SubmeshName), MaterialSlotName, Material);
		}
		else
		{
			SetSubmeshMaterial(FindSubmeshIndex(SubmeshName), OrMaterialIndex, Material);
		}
	}
	else if (OrSubmeshAsset)
	{
		if (!MaterialSlotName.IsNone())
		{
			SetSubmeshMaterial(FindSubmeshIndex(OrSubmeshAsset), MaterialSlotName, Material);
		}
		else
		{
			SetSubmeshMaterial(FindSubmeshIndex(OrSubmeshAsset), OrMaterialIndex, Material);
		}
	}
}

TArray<FName> UAllegroComponent::GetSubmeshNames() const
{
	TArray<FName> Names;
	for (const FAllegroSubmeshSlot& S : Submeshes)
	{
		Names.AddUnique(S.Name);
	}
	return Names;
}


void UAllegroComponent::SetPreSkinPostionOffsetStaticMesh(int SlotIndex, UStaticMesh* InMesh)
{
	if (SlotIndex < Submeshes.Num())
	{
		if (InMesh)
		{
			Submeshes[SlotIndex].AdditionalStaticMesh = InMesh;
			Submeshes[SlotIndex].PreSkinPostionOffset = true;
		}
		else
		{
			Submeshes[SlotIndex].AdditionalStaticMesh = nullptr;
			Submeshes[SlotIndex].PreSkinPostionOffset = false;
		}
		MarkRenderStateDirty();
	}
}

void UAllegroComponent::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	UAllegroComponent* This = CastChecked<UAllegroComponent>(InThis);

	This->InstancesData.AddReferencedObjects(Collector);

	Super::AddReferencedObjects(This, Collector);
}