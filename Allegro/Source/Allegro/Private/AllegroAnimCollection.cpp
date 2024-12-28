// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroAnimCollection.h"
#include "AllegroComponent.h"
#include "Animation/AnimSequence.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "AllegroPrivate.h"
#include "Engine/SkeletalMeshSocket.h"
#include "DrawDebugHelpers.h"
#include "Misc/MemStack.h"
#include "EngineUtils.h"
#include "AllegroRender.h"
#include "MeshDrawShaderBindings.h"
#include "MeshMaterialShader.h"
#include "Engine/AssetManager.h"
#include "ShaderCore.h"
#include "Animation/AnimationPoseData.h"
#include "Async/ParallelFor.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IContentBrowserSingleton.h"
#include "DerivedDataCacheInterface.h"
#endif


#include "Chaos/Sphere.h"
#include "Chaos/Box.h"
#include "Chaos/Capsule.h"
#include "Styling/AppStyle.h"

#include "PhysicsEngine/PhysicsAsset.h"
#include "BoneContainer.h"
#include "Animation/AnimCurveTypes.h"
#include "BonePose.h"
#include "Interfaces/ITargetPlatform.h"


#include "AllegroObjectVersion.h"
#include "Misc/Fnv.h"
#include "Math/UnitConversion.h"
#include "AllegroPrivateUtils.h"
#include "AllegroSettings.h"

#include "../Private/AllegroRenderResources.h"
#include "Animation/AttributesRuntime.h"
#include "Animation/AnimNotifies/AnimNotify.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AllegroAnimCollection)

typedef TArray<FTransform, TFixedAllocator<256>> FixedTransformArray;
typedef TArray<FTransform, FAnimStackAllocator> TransformArrayAnimStack;

bool GGenerateSingleThread = true; //!!there are some problem when it related to CurveCaching 
int32 GAllegro_NumTransitionGeneratedThisFrame = 0;

ALLEGRO_AUTO_CVAR_DEBUG(bool, DisableTransitionGeneration, false, "", ECVF_Default);

namespace Utils
{
	//extract component space reference pose and its inverse from bone container
	void ExtractRefPose(const FBoneContainer& BoneContainer, TArray<FTransform>& OutPoseComponentSpace, TArray<FMatrix44f>& OutInverse)
	{
		OutPoseComponentSpace.SetNumUninitialized(BoneContainer.GetNumBones());
		OutInverse.SetNumUninitialized(BoneContainer.GetNumBones());

		OutPoseComponentSpace[0] = BoneContainer.GetRefPoseTransform(FCompactPoseBoneIndex(0));
		OutInverse[0] = ((FTransform3f)OutPoseComponentSpace[0]).Inverse().ToMatrixWithScale();

		for (FCompactPoseBoneIndex i(1); i < BoneContainer.GetNumBones(); ++i)
		{
			const FCompactPoseBoneIndex parentIdx = BoneContainer.GetParentBoneIndex(i);
			check(parentIdx < i);
			OutPoseComponentSpace[i.GetInt()] = BoneContainer.GetRefPoseTransform(i) * OutPoseComponentSpace[parentIdx.GetInt()];
			OutInverse[i.GetInt()] = ((FTransform3f)OutPoseComponentSpace[i.GetInt()]).Inverse().ToMatrixWithScale();
		}
	}

	//
	TArray<FTransform> ConvertMeshPoseToSkeleton(USkeletalMesh* Mesh)
	{
		const FReferenceSkeleton& SkelRefPose = Mesh->GetSkeleton()->GetReferenceSkeleton();
		const FReferenceSkeleton& MeshRefPose = Mesh->GetRefSkeleton();

		TArray<FTransform> Pose = SkelRefPose.GetRefBonePose();

		for (int MeshBoneIndex = 0; MeshBoneIndex < MeshRefPose.GetNum(); MeshBoneIndex++)
		{
			int SkelBoneIndex = Mesh->GetSkeleton()->GetSkeletonBoneIndexFromMeshBoneIndex(Mesh, MeshBoneIndex);
			if (SkelBoneIndex != -1)
			{
				FTransform MeshT = MeshRefPose.GetRefBonePose()[MeshBoneIndex];
				Pose[SkelBoneIndex] = MeshT;
			}
		}

		return Pose;
	}

// 	//New
// 	template<typename A> void ExtractCompactPose(const FCompactPose& Pose, const TArrayView<FTransform>& InvRefPose, TArray<FTransform, A>& OutLocalSpace, TArray<FTransform, A>& OutShaderSpace)
// 	{
// 		OutLocalSpace.SetNumUninitialized(Pose.GetNumBones());
// 		OutShaderSpace.SetNumUninitialized(Pose.GetNumBones());
// 
// 		OutLocalSpace[0] = Pose[FCompactPoseBoneIndex(0)];
// 		OutShaderSpace[0] = InvRefPose[0] * OutLocalSpace[0];
// 
// 		for (FCompactPoseBoneIndex i(1); i < Pose.GetNumBones(); ++i)
// 		{
// 			const FCompactPoseBoneIndex parentIdx = Pose.GetParentBoneIndex(i);
// 			check(parentIdx < i);
// 			OutLocalSpace[i.GetInt()] = Pose.GetRefPose(i) * OutLocalSpace[parentIdx.GetInt()];
// 			OutShaderSpace[i.GetInt()] = InvRefPose[i.GetInt()] * OutLocalSpace[i.GetInt()];
// 		}
// 	}

	void LocalPoseToComponent(const FCompactPose& Pose, FTransform* OutComponentSpace)
	{
		OutComponentSpace[0] = Pose[FCompactPoseBoneIndex(0)];

		for (FCompactPoseBoneIndex CompactIdx(1); CompactIdx < Pose.GetNumBones(); ++CompactIdx)
		{
			const FCompactPoseBoneIndex ParentIdx = Pose.GetParentBoneIndex(CompactIdx);
			OutComponentSpace[CompactIdx.GetInt()] = Pose[CompactIdx] * OutComponentSpace[ParentIdx.GetInt()];
		}
	}
	void LocalPoseToComponent(const FCompactPose& Pose, const FTransform* InLocalSpace, FTransform* OutComponentSpace)
	{
		OutComponentSpace[0] = InLocalSpace[0];

		for (FCompactPoseBoneIndex CompactIdx(1); CompactIdx < Pose.GetNumBones(); ++CompactIdx)
		{
			const FCompactPoseBoneIndex ParentIdx = Pose.GetParentBoneIndex(CompactIdx);
			OutComponentSpace[CompactIdx.GetInt()] = InLocalSpace[CompactIdx.GetInt()] * OutComponentSpace[ParentIdx.GetInt()];
		}
	}

	FString CheckMeshIsQualified(const USkeletalMesh* SKMesh, int InBaseLOD)
	{
		const FSkeletalMeshRenderData* RenderResource = SKMesh->GetResourceForRendering();
		check(RenderResource);

		for (int32 LODIndex = InBaseLOD; LODIndex < RenderResource->LODRenderData.Num(); ++LODIndex)
		{
			const FSkeletalMeshLODRenderData& Data = RenderResource->LODRenderData[LODIndex];
			if (Data.GetSkinWeightVertexBuffer()->GetBoneInfluenceType() != DefaultBoneInfluence || Data.GetVertexBufferMaxBoneInfluences() > FAllegroMeshDataEx::MAX_INFLUENCE)
			{
				return FString::Printf(TEXT("Can't build SkeletalMesh %s at LOD %d. MaxBoneInfluence supported is <= %d."), *SKMesh->GetName(), LODIndex, FAllegroMeshDataEx::MAX_INFLUENCE);
			}
		}

		return FString();
	}

	void HelperBlendPose(const TransformArrayAnimStack& A, const TransformArrayAnimStack& B, float Alpha, TransformArrayAnimStack& Out)
	{
		check(Alpha >= 0 && Alpha <= 1);
		for (int i = 0; i < A.Num(); i++)
		{
			Out[i].Blend(A[i], B[i], Alpha);
		}
	}


#if WITH_EDITOR
	//
	FBox HelperCalcSkeletalMeshBoundCPUSkin(USkeletalMesh* SKMesh, const TArrayView<FTransform>& BoneTransforms, bool bDrawDebug, int LODIndex)
	{
		TArray<FMatrix, TFixedAllocator<1024>> BoneMatrices;
		for (FTransform T : BoneTransforms)
			BoneMatrices.Add(T.ToMatrixWithScale());

		const FSkeletalMeshLODRenderData& LODData = SKMesh->GetResourceForRendering()->LODRenderData[LODIndex];

		const int NumVertex = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
		const FSkinWeightVertexBuffer* SkinWeightVB = LODData.GetSkinWeightVertexBuffer();
		const int MaxBoneInf = SkinWeightVB->GetMaxBoneInfluences();
		//TArray<FSkinWeightInfo> SkinWeights;
		//SkinWeightVB->GetSkinWeights(SkinWeights);

		FBox Bound(ForceInit);

		for (int VertexIndex = 0; VertexIndex < NumVertex; VertexIndex++)
		{
			const FVector VertexPos = FVector(LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex));
			int SectionIndex, SectionVertexIndex;
			LODData.GetSectionFromVertexIndex(VertexIndex, SectionIndex, SectionVertexIndex);
			const FSkelMeshRenderSection& SectionInfo = LODData.RenderSections[SectionIndex];

			FMatrix BlendMatrix;
			FMemory::Memzero(BlendMatrix);

			for (int InfluenceIndex = 0; InfluenceIndex < MaxBoneInf; InfluenceIndex++)
			{
				const uint32 BondeIndex = SkinWeightVB->GetBoneIndex(VertexIndex, InfluenceIndex); //SkinWeights[VertexIndex].InfluenceBones[InfluenceIndex];
				const uint16 Weight = SkinWeightVB->GetBoneWeight(VertexIndex, InfluenceIndex);	// SkinWeights[VertexIndex].InfluenceWeights[InfluenceIndex];
				if (Weight == 0)
					continue;

				check(BondeIndex <= 255 && Weight <= 0xFFFF);

				const float WeighScaler = Weight / (65535.0f);
				check(WeighScaler >= 0 && WeighScaler <= 1.0f);
				FBoneIndexType RealBoneIndex = SectionInfo.BoneMap[BondeIndex];
				BlendMatrix += BoneMatrices[RealBoneIndex] * WeighScaler;
			}

			FVector SkinnedVertex = BlendMatrix.TransformPosition(VertexPos);
			Bound += SkinnedVertex;
			//if(bDrawDebug)
			//	DrawDebugPoint(GWorld, SkinnedVertex, 4, FColor::MakeRandomColor(), false, 20);
		}

		return Bound;
	}
#endif


	void NotifyError(const UAllegroAnimCollection* AC, const FString& InString)
	{
#if WITH_EDITOR
		if (GEditor)
		{
			FString ErrString = FString::Printf(TEXT("%s Build Failed: %s"), *AC->GetName(), *InString);
			FNotificationInfo Info(FText::FromString(ErrString));
			Info.Image = FAppStyle::GetBrush(TEXT("Icons.Error"));
			Info.FadeInDuration = 0.1f;
			Info.FadeOutDuration = 0.5f;
			Info.ExpireDuration = 7;
			Info.bUseThrobber = false;
			Info.bUseSuccessFailIcons = true;
			Info.bUseLargeFont = true;
			Info.bFireAndForget = false;
			Info.bAllowThrottleWhenFrameRateIsLow = false;

			TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
			if (NotificationItem)
			{
				NotificationItem->SetCompletionState(SNotificationItem::CS_Fail);
				NotificationItem->ExpireAndFadeout();
				//GEditor->PlayEditorSound(TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue.CompileSuccess_Cue"));
			}

			UE_LOG(LogAllegro, Error, TEXT("%s"), *ErrString);
		}
#endif
	}

};







TArray<FName> UAllegroAnimCollection::GetValidBones() const
{
	TArray<FName> BoneNames;
	if (Skeleton)
	{
		for (const FMeshBoneInfo& BoneInfo : Skeleton->GetReferenceSkeleton().GetRefBoneInfo())
			BoneNames.Add(BoneInfo.Name);
	}
	return BoneNames;
}

UAllegroAnimCollection::UAllegroAnimCollection()
{
}

UAllegroAnimCollection::~UAllegroAnimCollection()
{

}

const FTransform3f& UAllegroAnimCollection::GetBoneTransform(uint32 SkeletonBoneIndex, uint32 FrameIndex)
{
	if (IsBoneTransformCached(SkeletonBoneIndex))
	{
		if (IsAnimationFrameIndex(FrameIndex))
			return GetBoneTransformFast(SkeletonBoneIndex, FrameIndex);

		ConditionalFlushDeferredTransitions(FrameIndex);
		return GetBoneTransformFast(SkeletonBoneIndex, FrameIndex);
	}
	return FTransform3f::Identity;
}

void UAllegroAnimCollection::PostInitProperties()
{
	Super::PostInitProperties();

	if(!IsTemplate())
	{
#if WITH_EDITOR
		FCoreUObjectDelegates::OnObjectModified.AddUObject(this, &UAllegroAnimCollection::ObjectModifiedEvent);
#endif
		
		FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates.AddUObject(this, &UAllegroAnimCollection::OnPreSendAllEndOfFrameUpdates);
		FCoreDelegates::OnBeginFrame.AddUObject(this, &UAllegroAnimCollection::OnBeginFrame);
		FCoreDelegates::OnEndFrame.AddUObject(this, &UAllegroAnimCollection::OnEndFrame);
		FCoreDelegates::OnBeginFrameRT.AddUObject(this, &UAllegroAnimCollection::OnBeginFrameRT);
		FCoreDelegates::OnEndFrameRT.AddUObject(this, &UAllegroAnimCollection::OnEndFrameRT);

	}
}

void UAllegroAnimCollection::PostLoad()
{
	Super::PostLoad();

	if(Skeleton)
		Skeleton->ConditionalPostLoad();
	
	for(int MeshIndex = 0; MeshIndex < Meshes.Num(); MeshIndex++)
	{
		FAllegroMeshDef& MeshDef = this->Meshes[MeshIndex];
		if(MeshDef.Mesh)
			MeshDef.Mesh->ConditionalPostLoad();
	}

	for (FAllegroSequenceDef& SeqDef : Sequences)
	{
		if (SeqDef.Sequence)
			SeqDef.Sequence->ConditionalPostLoad();
	}

	SequenceIndexMap.Reset();
	for (int i=0;i < Sequences.Num();++i)
	{
		SequenceIndexMap.Add(Sequences[i].Sequence, i);
	}
	
	if(!IsTemplate() && !UE_SERVER)
	{

		FString ErrString;
		if (!CheckCanBuild(ErrString))
		{
#if WITH_EDITOR
			if(!ErrString.IsEmpty())
				Utils::NotifyError(this, ErrString);
#endif
		}
		else
		{
			if (!BuildData())
			{
				DestroyBuildData();
			}
		}

	}
}

#if WITH_EDITOR
void UAllegroAnimCollection::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	FName PRPName = PropertyChangedEvent.Property->GetFName();

	bNeedRebuild = true;
}

void UAllegroAnimCollection::PreEditChange(FProperty* PropertyThatWillChange)
{
	Super::PreEditChange(PropertyThatWillChange);

	FName PRPName = PropertyThatWillChange->GetFName();

	if(PRPName == GET_MEMBER_NAME_CHECKED(UAllegroAnimCollection, Meshes) || PropertyThatWillChange == nullptr)
	{
		EnqueueReleaseResources();
		FlushRenderingCommands();
		DestroyBuildData();
	}
}


bool UAllegroAnimCollection::IsObjectRelatedToThis(const UObject* Other) const
{
	if(Skeleton == Other)
		return true;

	for(const FAllegroMeshDef& MD : Meshes)
		if(MD.Mesh == Other)
			return true;

	for(const FAllegroSequenceDef& SD : Sequences)
		if(SD.Sequence == Other)
			return true;

	return false;
}

void UAllegroAnimCollection::ObjectModifiedEvent(UObject* Object)
{
	if(!bNeedRebuild && IsObjectRelatedToThis(Object))
	{
		bNeedRebuild = true;
	}
}

#endif



void UAllegroAnimCollection::BeginDestroy()
{
	Super::BeginDestroy();

	if(!IsTemplate())
	{
#if WITH_EDITOR
		FCoreUObjectDelegates::OnObjectModified.RemoveAll(this);
#endif

		if(FApp::CanEverRender())
		{
			EnqueueReleaseResources();
			ReleaseResourcesFence.BeginFence();
		}
	}

	FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates.RemoveAll(this);
	FCoreDelegates::OnBeginFrame.RemoveAll(this);
	FCoreDelegates::OnEndFrame.RemoveAll(this);
	FCoreDelegates::OnBeginFrameRT.RemoveAll(this);
	FCoreDelegates::OnEndFrameRT.RemoveAll(this);
}

bool UAllegroAnimCollection::IsReadyForFinishDestroy()
{
	return Super::IsReadyForFinishDestroy() && (IsTemplate() || ReleaseResourcesFence.IsFenceComplete());
}

void UAllegroAnimCollection::FinishDestroy()
{
	Super::FinishDestroy();

	DestroyBuildData();
// 	Meshes.Empty();
// 	Sequences.Empty();
// 	AnimationBuffer.DestroyBuffer();
// 	BonesTransform.Empty();
// 	PoseCount = FrameCountSequences = PoseCountBlends = BoneCount = 0;
}

void UAllegroAnimCollection::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);
	//#TODO 
	auto MatrixSize = bHighPrecision ? sizeof(float[3][4]) : sizeof(uint16[3][4]);
	CumulativeResourceSize.AddDedicatedVideoMemoryBytes(TotalFrameCount * RenderBoneCount * MatrixSize);
}


void UAllegroAnimCollection::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	
	Ar.UsingCustomVersion(FAllegroObjectVersion::GUID);
	

	if(!IsTemplate() && (Ar.IsCooking() || FPlatformProperties::RequiresCookedData()))
	{
		FStripDataFlags StripFlags(Ar);
		const bool bIsServerOnly = Ar.IsSaving() ? Ar.CookingTarget()->IsServerOnly() : FPlatformProperties::IsServerOnly();
		
		for (FAllegroMeshDef& MeshDef : Meshes)
		{
			if(!bIsServerOnly)  //dedicated server doesn't need skin weight
			{
				MeshDef.SerializeCooked(Ar);
			}
		}
	}
}

#if WITH_EDITOR
void UAllegroAnimCollection::BeginCacheForCookedPlatformData(const ITargetPlatform* TargetPlatform)
{
	Super::BeginCacheForCookedPlatformData(TargetPlatform);

	//if (!IsTemplate())
	//{
	//	if(Skeleton && Meshes.Num())
	//	{
	//		InitSkeletonData();
	//		BuildMeshData();
	//	}
	//}
}

void UAllegroAnimCollection::ClearAllCachedCookedPlatformData()
{
	Super::ClearAllCachedCookedPlatformData();

	//DestroyBuildData();
}

#endif


#if WITH_EDITOR

void UAllegroAnimCollection::Rebuild()
{
	FGlobalComponentReregisterContext ReregisterContext;
	InternalBuildAll();
// 	if (FApp::CanEverRender() && this->bIsBuilt)
// 	{
// 		ENQUEUE_RENDER_COMMAND(ReleaseResoruces)([=](FRHICommandListImmediate& RHICmdList) {
// 			this->AnimationBuffer.ReleaseResource();
// 			this->ReleaseMeshDataResources();
// 		});
// 
// 		FlushRenderingCommands();
// 	}
// 
// 	DestroyBuildData();
// 
// 	FString ErrString;
// 	if(!CheckCanBuild(ErrString))
// 	{
// 		if(!ErrString.IsEmpty())
// 			HelperNotifyError(this, ErrString);
// 
// 		return;
// 	}
// 
// 	if (!BuildData())
// 	{
// 		DestroyBuildData();
// 	}
// 
// 	FlushRenderingCommands();
}

// void UAllegroAnimCollection::RemoveBlends()
// {
// 	for(FAllegroSequenceDef& Seq : Sequences)
// 	{
// 		Seq.Blends.Empty();
// 	}
// 	MarkPackageDirty();
// 	bNeedRebuild = true;
// }

// void UAllegroAnimCollection::AddAllBlends()
// {
// 	for (FAllegroSequenceDef& SeqFrom : Sequences)
// 	{
// 		for (FAllegroSequenceDef& SeqTo : Sequences)
// 		{
// 			if(&SeqFrom == &SeqTo)
// 				continue;
// 				
// 			if(SeqFrom.IndexOfBlendDef(SeqTo.Sequence) != -1)
// 				continue;
// 
// 			FAllegroBlendDef& BlendDef = SeqFrom.Blends.AddDefaulted_GetRef();
// 			BlendDef.BlendTo = SeqTo.Sequence;
// 		}
// 	}
// 	MarkPackageDirty();
// 	bNeedRebuild = true;
// }

void UAllegroAnimCollection::AddAllMeshes()
{
	IAssetRegistry* AR = IAssetRegistry::Get();
	if(AR)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
		Filter.TagsAndValues.Add(FName(TEXT("Skeleton")), GetSkeletonTagValue());

		TArray<FAssetData> AssetData;
		AR->GetAssets(Filter, AssetData);

		for(const FAssetData& AD : AssetData)
		{
			AddMeshUnique(AD);
		}
	}
}

void UAllegroAnimCollection::AddAllAnimations()
{
	IAssetRegistry* AR = IAssetRegistry::Get();
	if (AR)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
		Filter.TagsAndValues.Add(FName(TEXT("Skeleton")), GetSkeletonTagValue());

		TArray<FAssetData> AssetData;
		AR->GetAssets(Filter, AssetData);

		for (const FAssetData& AD : AssetData)
		{
			AddAnimationUnique(AD);
		}
	}
}

void UAllegroAnimCollection::AddSelectedAssets()
{
	TArray<FAssetData> SelectedAssets;
	IContentBrowserSingleton::Get().GetSelectedAssets(SelectedAssets);

	for (const FAssetData& AD : SelectedAssets)
	{
		FString Tag = AD.GetTagValueRef<FString>(TEXT("Skeleton"));
		if (Tag != GetSkeletonTagValue())
			continue;

		if(AD.IsInstanceOf<USkeletalMesh>())
		{
			AddMeshUnique(AD);
		}
		else if (AD.IsInstanceOf<UAnimSequenceBase>())
		{
			AddAnimationUnique(AD);
		}
	}
}

void UAllegroAnimCollection::AddMeshUnique(const FAssetData& InAssetData)
{
	if (this->FindMeshDefByPath(InAssetData.GetSoftObjectPath()) == -1)
	{
		this->Meshes.AddDefaulted();
		this->Meshes.Last().Mesh = Cast<USkeletalMesh>(InAssetData.GetAsset());
	}
}

void UAllegroAnimCollection::AddAnimationUnique(const FAssetData& InAssetData)
{
	if (this->FindSequenceDefByPath(InAssetData.GetSoftObjectPath()) == -1)
	{
		this->Sequences.AddDefaulted();
		this->Sequences.Last().Sequence = Cast<UAnimSequence>(InAssetData.GetAsset());
	}
}

FString UAllegroAnimCollection::GetSkeletonTagValue() const
{
	return Skeleton ? FString::Printf(TEXT("%s'%s'"), *Skeleton->GetClass()->GetPathName(), *Skeleton->GetPathName()) : FString();
}




void UAllegroAnimCollection::TryBuildAll()
{
 	//if we have not build any data then build it now
 	if (bNeedRebuild)
 	{
		InternalBuildAll();
 	}
}


void UAllegroAnimCollection::InternalBuildAll()
{
#if WITH_EDITOR
	if (FApp::CanEverRender() && this->bIsBuilt)
	{
		EnqueueReleaseResources();
		FlushRenderingCommands();
	}

	DestroyBuildData();

	FString ErrString;
	if (!CheckCanBuild(ErrString))
	{
		if (!ErrString.IsEmpty())
			Utils::NotifyError(this, ErrString);

		return;
	}

	if (!BuildData())
	{
		DestroyBuildData();
	}

	SequenceIndexMap.Reset();
	for (int i = 0; i < Sequences.Num(); ++i)
	{
		SequenceIndexMap.Add(Sequences[i].Sequence, i);
	}

	FlushRenderingCommands();
#endif // 
}

#endif

void UAllegroAnimCollection::DestroyBuildData()
{
	check(IsInGameThread());

	AnimationBuffer = nullptr;

	for(FAllegroSequenceDef& SeqDef : Sequences)
	{
		SeqDef.AnimationFrameIndex = SeqDef.AnimationFrameCount = 0;
		SeqDef.SampleFrequencyFloat = SeqDef.SequenceLength = 0;
		SeqDef.Notifies.Empty();
	}
	for (FAllegroMeshDef& MeshDef : Meshes)
	{
		MeshDef.MeshData = nullptr;
		MeshDef.OwningBounds.Empty();
		MeshDef.MaxBBox = FBoxMinMaxFloat(ForceInit);
		MeshDef.CompactPhysicsAsset = FAllegroCompactPhysicsAsset();
	}

	CachedTransforms.Empty();
	SkeletonBoneIndexTo_CachedTransforms.Empty();
	BonesToCache_Indices.Empty();

	RefPoseComponentSpace.Empty();
	RefPoseInverse.Empty();

	RenderRequiredBones.Empty();

	AnimationBoneContainer = FBoneContainer();

	TotalFrameCount = FrameCountSequences = RenderBoneCount = AnimationBoneCount = TotalAnimationBufferSize = TotalMeshBonesBufferSize = 0;
	NumTransitionFrameAllocated = 0;

	MeshesBBox = FBoxCenterExtentFloat(ForceInit);

	
	Transitions.Empty();
	TransitionsHashTable.Free();
	TransitionPoseAllocator.Empty();
	
	ZeroRCTransitions.Empty();
	NegativeRCTransitions.Empty();
	DeferredTransitions.Reset();
	DeferredTransitions_FrameCount = 0;

	DynamicPoseAllocator.Empty();
	DynamicPoseFlipFlags.Empty();

	CurrentUpload = FPoseUploadData{};
	ScatterBuffer = FScatterUploadBuffer{};

	bNeedRebuild = true;
	bIsBuilt = false;
	
}



FArchive& operator<<(FArchive& Ar, FAllegroCompactPhysicsAsset& PA)
{
	PA.Capsules.BulkSerialize(Ar);
	PA.Spheres.BulkSerialize(Ar);
	PA.Boxes.BulkSerialize(Ar);
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FAllegroCompactPhysicsAsset::FShapeSphere& Shape)
{
	Ar << Shape.Center << Shape.Radius << Shape.BoneIndex;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FAllegroCompactPhysicsAsset::FShapeBox& Shape)
{
	Ar << Shape.Rotation << Shape.Center << Shape.BoxMin << Shape.BoxMax << Shape.BoneIndex << Shape.bHasTransform;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FAllegroCompactPhysicsAsset::FShapeCapsule& Shape)
{
	Ar << Shape.A << Shape.B << Shape.BoneIndex << Shape.Radius;
	return Ar;
}


int UAllegroAnimCollection::FindSequenceDef(const UAnimSequenceBase* Animation) const
{
	return Sequences.IndexOfByPredicate([=](const FAllegroSequenceDef& Item){ return Item.Sequence == Animation; });
}

int UAllegroAnimCollection::FindSequenceDefByPath(const FSoftObjectPath& AnimationPath) const
{
	return Sequences.IndexOfByPredicate([=](const FAllegroSequenceDef& Item) { return Item.Sequence && FSoftObjectPath(Item.Sequence) == AnimationPath; });
}

int UAllegroAnimCollection::FindMeshDef(const USkeletalMesh* Mesh) const
{
	return Meshes.IndexOfByPredicate([=](const FAllegroMeshDef& Item){ return Item.Mesh == Mesh; });
}

int UAllegroAnimCollection::FindMeshDefByPath(const FSoftObjectPath& MeshPath) const
{
	return Meshes.IndexOfByPredicate([=](const FAllegroMeshDef& Item) { return Item.Mesh && FSoftObjectPath(Item.Mesh) == MeshPath; });
}

FAllegroMeshDataExPtr UAllegroAnimCollection::FindMeshData(const USkeletalMesh* InMesh) const
{
	for (const FAllegroMeshDef& MeshDef : Meshes)
		if(MeshDef.Mesh == InMesh && MeshDef.MeshData)
			return MeshDef.MeshData;

	return nullptr;
}

UAnimSequenceBase* UAllegroAnimCollection::GetRandomAnimSequence() const
{
	if (Sequences.Num())
		return Sequences[FMath::RandHelper(Sequences.Num())].Sequence;
	return nullptr;
}

UAnimSequenceBase* UAllegroAnimCollection::FindAnimByName(const FString& ContainingName)
{
	for (FAllegroSequenceDef& SequenceIter : Sequences)
	{
		if (SequenceIter.Sequence && SequenceIter.Sequence->GetName().Contains(ContainingName))
			return SequenceIter.Sequence;
	}

	return nullptr;
}

USkeletalMesh* UAllegroAnimCollection::FindMeshByName(const FString& ContainingName)
{
	for (FAllegroMeshDef& MeshIter : Meshes)
	{
		if (MeshIter.Mesh && MeshIter.Mesh->GetName().Contains(ContainingName))
			return MeshIter.Mesh;
	}
	return nullptr;
}

USkeletalMesh* UAllegroAnimCollection::GetRandomMesh() const
{
	if(Meshes.Num())
		return Meshes[FMath::RandHelper(Meshes.Num())].Mesh;
	return nullptr;
}

USkeletalMesh* UAllegroAnimCollection::GetRandomMeshFromStream(const FRandomStream& RandomSteam) const
{
	if (Meshes.Num())
		return Meshes[RandomSteam.RandHelper(Meshes.Num())].Mesh;
	return nullptr;
}

void UAllegroAnimCollection::InitMeshDataResources(FRHICommandListBase& RHICmdList)
{
	check(IsInRenderingThread());
	for (const FAllegroMeshDef& MeshDef : Meshes)
	{
		if (MeshDef.MeshData)
		{
			check(MeshDef.Mesh && MeshDef.Mesh->GetResourceForRendering());
			MeshDef.MeshData->InitMeshData(MeshDef.Mesh->GetResourceForRendering(), MeshDef.BaseLOD);
			MeshDef.MeshData->InitResources(RHICmdList);
		}
	}
}

void UAllegroAnimCollection::ReleaseMeshDataResources()
{
	check(IsInRenderingThread());
	for (FAllegroMeshDef& MeshDef : Meshes)
	{
		if (MeshDef.MeshData)
		{
			MeshDef.MeshData->ReleaseResouces();
			MeshDef.MeshData = nullptr;
		}
	}
}

void UAllegroAnimCollection::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);
#if WITH_EDITOR
	DestroyBuildData();
#endif
}

//generate skeleton related data 
void UAllegroAnimCollection::InitSkeletonData()
{
	{
		this->TransitionsHashTable.Clear(FMath::RoundUpToPowerOfTwo(this->Sequences.Num() * 3));
		if(this->MaxTransitionPose > 0)
		{
			this->TransitionPoseAllocator.Init(this->MaxTransitionPose);
		}

		if(this->MaxDynamicPose > 0)
		{
			this->DynamicPoseAllocator.Init(this->MaxDynamicPose);
			this->DynamicPoseFlipFlags.Init(false, this->MaxDynamicPose);
		}
	}

	for (FAllegroSequenceDef& SD : Sequences)
	{
		SD.Notifies.Empty();
		if (SD.Sequence)
		{
			//cache animation notifies
			for (const FAnimNotifyEvent& Notify : SD.Sequence->Notifies)
			{
				if (Notify.NotifyStateClass == nullptr) //only name only notification is supported
				{
					SD.Notifies.Emplace(FAllegroSimpleAnimNotifyEvent{Notify.GetTriggerTime(), Notify.NotifyName,Notify.Notify });
				}
				else
				{
					//check(false);
					UE_LOG(LogAllegro, Warning, TEXT("UAllegroAnimCollection Init Notify Error!  Notify.NotifyStateClass != nullptr "));
				}
			}
		}
	}

	this->CachedTransforms.Empty();
	this->SkeletonBoneIndexTo_CachedTransforms.Empty();
	this->BonesToCache_Indices.Empty();

	this->RenderRequiredBones.Empty();
	this->RefPoseComponentSpace.Empty();
	this->RefPoseInverse.Empty();

	if (Skeleton)
	{
		//try add bones that are used by physics assets
		if (bCachePhysicsAssetBones)
		{
			for (const FAllegroMeshDef& MeshDef : Meshes)
			{
				if (MeshDef.Mesh && MeshDef.Mesh->GetPhysicsAsset())
				{
					const UPhysicsAsset* PhysAsset = MeshDef.Mesh->GetPhysicsAsset();
					for (const USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
					{
						this->BonesToCache.Add(BodySetup->BoneName);
					}
				}
			}
		}
		//find bone indices by name
		this->BonesToCache_Indices.Reserve(this->BonesToCache.Num());
		for (FName BoneName : BonesToCache)
		{
			int SkelBoneIndex = this->Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName);
			if (SkelBoneIndex != INDEX_NONE)
			{
				this->BonesToCache_Indices.AddUnique((FBoneIndexType)SkelBoneIndex);
			}
			else
			{
				UE_LOG(LogAllegro, Warning, TEXT("Bone [%s] listed in UAllegroAnimCollection.BonesToCache not exist in reference skeleton. AnimCollection:%s"), *BoneName.ToString(), *GetFullName());
			}
		}

		InitBoneContainer();


		//init compact physics assets
		for (FAllegroMeshDef& MeshDef : Meshes)
		{
			const UPhysicsAsset* PXAsset = MeshDef.Mesh ? MeshDef.Mesh->GetPhysicsAsset() : nullptr;
			MeshDef.CompactPhysicsAsset = FAllegroCompactPhysicsAsset();
			if (PXAsset)
			{
				MeshDef.CompactPhysicsAsset.Init(this->Skeleton, PXAsset);
			}
		}
	}

	

}

void UAllegroAnimCollection::CachePose(int PoseIndex, const TArrayView<FTransform>& PoseComponentSpace)
{
	CachePoseBounds(PoseIndex, PoseComponentSpace);
	CachePoseBones(PoseIndex, PoseComponentSpace);
}

void UAllegroAnimCollection::CachePoseBounds(int PoseIndex, const TArrayView<FTransform>& PoseComponentSpace)
{
	check(PoseIndex < this->FrameCountSequences);
	//generate and cache bounds
	for (FAllegroMeshDef& MeshDef : this->Meshes)
	{
		if (MeshDef.Mesh && MeshDef.OwningBoundMeshIndex == -1)	//check if has mesh and needs bound
		{
			FBox3f Box;
			if (MeshDef.Mesh->GetPhysicsAsset())
			{
				Box = (FBox3f)CalcPhysicsAssetBound(MeshDef.Mesh->GetPhysicsAsset(), PoseComponentSpace, false);
			}
			else
			{
				Box = (FBox3f)MeshDef.Mesh->GetBounds().GetBox();
			}

			check(Box.IsValid);

			FBoxCenterExtentFloat BoundCE(Box.ExpandBy(MeshDef.BoundExtent));
			MeshDef.MaxBBox.Add(BoundCE);
			if (MeshDef.OwningBounds.Num())
				MeshDef.OwningBounds[PoseIndex] = BoundCE;

			//MeshDef.MaxCoveringRadius = FMath::Max(MeshDef.MaxCoveringRadius, GetBoxCoveringRadius(MeshBound));
		}
	}
}

void UAllegroAnimCollection::CachePoseBones(int PoseIndex, const TArrayView<FTransform>& PoseComponentSpace)
{
	for (FBoneIndexType SkelBoneIndex : this->BonesToCache_Indices)
	{
		FTransform3f& BoneT = this->GetBoneTransformFast(SkelBoneIndex, PoseIndex);
		FCompactPoseBoneIndex CompactBoneIdx = this->AnimationBoneContainer.GetCompactPoseIndexFromSkeletonPoseIndex(FSkeletonPoseBoneIndex(SkelBoneIndex));
		BoneT = (FTransform3f)PoseComponentSpace[CompactBoneIdx.GetInt()];
	}
}

bool UAllegroAnimCollection::CheckCanBuild(FString& OutErrorString) const
{
	if (!Skeleton || Meshes.Num() == 0)
		return false;

	bool bAnyMesh = false;

	//check meshes are valid
	for (const FAllegroMeshDef& MeshDef : this->Meshes)
	{
		if (MeshDef.Mesh)
		{
			if (MeshDef.Mesh->GetSkeleton() != Skeleton)
			{
				OutErrorString = FString::Printf(TEXT("Skeleton Mismatch. AnimSet:%s SkeletalMesh:%s"), *GetName(), *MeshDef.Mesh->GetName());
				return false;
			}
		
			OutErrorString = Utils::CheckMeshIsQualified(MeshDef.Mesh, MeshDef.BaseLOD);
			if (!OutErrorString.IsEmpty())
				return false;

			bAnyMesh = true;
		}
	}

	int FrameCounter = 1;
	
	for (int SequenceIndex = 0; SequenceIndex < this->Sequences.Num(); SequenceIndex++)
	{
		const FAllegroSequenceDef& SequenceStruct = this->Sequences[SequenceIndex];
		if (!SequenceStruct.Sequence)
			continue;

		const int MaxFrame = SequenceStruct.CalcFrameCount();
		FrameCounter += MaxFrame;
	}

	return bAnyMesh;
}

bool UAllegroAnimCollection::BuildData()
{
	const double BuildStartTime = FPlatformTime::Seconds();

	bool bDone = BuildAnimationData();
	if(!bDone)
		return false;

	double ExecutionTime = FPlatformTime::Seconds() - BuildStartTime;
	UE_LOG(LogAllegro, Log, TEXT("%s animations build finished in %f seconds"), *GetName(), ExecutionTime);

	BuildMeshData();

	bNeedRebuild = false;
	bIsBuilt = true;

	if (FApp::CanEverRender())
	{
		ENQUEUE_RENDER_COMMAND(InitAnimationBuffer)([this](FRHICommandListImmediate& RHICmdList) {
			this->AnimationBuffer->InitResource(RHICmdList);
			this->InitMeshDataResources(RHICmdList);
		});

#if WITH_EDITOR
		if (GEditor)
		{
			FNotificationInfo Info(FText::FromString(FString::Printf(TEXT("%s: Build Done!"), *this->GetName())));
			Info.Image = FAppStyle::GetBrush(TEXT("LevelEditor.RecompileGameCode"));
			Info.FadeInDuration = 0.1f;
			Info.FadeOutDuration = 0.5f;
			Info.ExpireDuration = 1.5f;
			Info.bUseThrobber = false;
			Info.bUseSuccessFailIcons = true;
			Info.bUseLargeFont = true;
			Info.bFireAndForget = false;
			Info.bAllowThrottleWhenFrameRateIsLow = false;
			TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
			if (NotificationItem)
			{
				NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
				NotificationItem->ExpireAndFadeout();
				//GEditor->PlayEditorSound(TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue.CompileSuccess_Cue"));
			}
		}
#endif

	}


	return true;
}

bool UAllegroAnimCollection::BuildAnimationData()
{
	if(!Skeleton || Meshes.Num() == 0)
		return false;

	this->InitSkeletonData();
	
	const FReferenceSkeleton& ReferenceSkelton = this->Skeleton->GetReferenceSkeleton();
	
	int FrameCounter = 1;
	//for each sequence 
	for (int SequenceIndex = 0; SequenceIndex < this->Sequences.Num(); SequenceIndex++)
	{
		FAllegroSequenceDef& SequenceStruct = this->Sequences[SequenceIndex];
		if (!SequenceStruct.Sequence)
			continue;

		const int MaxFrame = SequenceStruct.CalcFrameCount();
		SequenceStruct.SequenceLength = SequenceStruct.Sequence->GetPlayLength();
		//SequenceStruct.SampleFrequency = SequenceStruct.Sequence->GetSamplingFrameRate().Numerator;

		SequenceStruct.AnimationFrameCount = MaxFrame;
		SequenceStruct.AnimationFrameIndex = FrameCounter;
		SequenceStruct.SampleFrequencyFloat = static_cast<float>(SequenceStruct.SampleFrequency);

		FrameCounter += MaxFrame;
	}

	this->FrameCountSequences = FrameCounter;
	this->TotalFrameCount = this->FrameCountSequences + this->MaxTransitionPose + (this->MaxDynamicPose * 2);
	this->RenderBoneCount = this->RenderRequiredBones.Num(); //ReferenceSkelton.GetNum();
	this->TotalAnimationBufferSize = this->RenderBoneCount * this->TotalFrameCount * this->GetRenderMatrixSize();
	//LexToString(FUnitConversion::QuantizeUnitsToBestFit((this->RenderBoneCount * this->PoseCount * this->GetRenderMatrixSize()), EUnit::Bytes));

	this->AnimationBuffer = MakeUnique<FAllegroAnimationBuffer>();
	this->AnimationBuffer->InitBuffer(this->RenderBoneCount * this->TotalFrameCount, this->bHighPrecision, true);

	//reserve bounds
	{
		for (FAllegroMeshDef& MeshDef : this->Meshes)
		{
			MeshDef.MaxBBox = FBoxMinMaxFloat(ForceInit);
			MeshDef.OwningBounds.Empty();

			if (!this->bDontGenerateBounds && MeshDef.Mesh && !this->Meshes.IsValidIndex(MeshDef.OwningBoundMeshIndex))
			{
				MeshDef.OwningBounds.SetNumUninitialized(this->FrameCountSequences);
			}
		}
	}

	//reserve cached transforms
	{
		this->CachedTransforms.Init(FTransform3f::Identity, this->TotalFrameCount * this->BonesToCache_Indices.Num());
		this->SkeletonBoneIndexTo_CachedTransforms.SetNumZeroed(ReferenceSkelton.GetNum());
		for (int i = 0; i < this->BonesToCache_Indices.Num(); i++)
		{
			FBoneIndexType BoneIndex = this->BonesToCache_Indices[i];
			SkeletonBoneIndexTo_CachedTransforms[BoneIndex] = &this->CachedTransforms[i];
		}
	}

	//0 index is identity data (default pose)
	CachePose(0, this->RefPoseComponentSpace);

	//build animation sequences
	ParallelFor(Sequences.Num(), [this](int SI) {
		this->BuildSequence(SI);

	}, GGenerateSingleThread ? EParallelForFlags::ForceSingleThread : EParallelForFlags::None);


	{
		this->MeshesBBox = FBoxCenterExtentFloat(ForceInit);
		FBoxMinMaxFloat MaxPossibleBound(ForceInit);

		for (FAllegroMeshDef& MeshDef : this->Meshes)
		{
			if(MeshDef.Mesh)
			{
				MeshDef.BoundsView = MeshDef.OwningBounds;
				if (this->Meshes.IsValidIndex(MeshDef.OwningBoundMeshIndex)) //get from other MeshDef if its not independent
				{
					MeshDef.BoundsView = this->Meshes[MeshDef.OwningBoundMeshIndex].OwningBounds;
					MeshDef.MaxBBox = this->Meshes[MeshDef.OwningBoundMeshIndex].MaxBBox;
				}

				MaxPossibleBound.Add(MeshDef.MaxBBox);
			}
		}
		if (!MaxPossibleBound.IsForceInitValue())
			MaxPossibleBound.ToCenterExtentBox(this->MeshesBBox);
	}


	return true;
}

void UAllegroAnimCollection::BuildSequence(int SequenceIndex)
{
	FMemMark MemMarker(FMemStack::Get());	//animation structures use FMemMemStack so we need marker

	FAllegroSequenceDef& SequenceStruct = this->Sequences[SequenceIndex];
	UAnimSequenceBase* AnimSequence = SequenceStruct.Sequence;
	if (!AnimSequence)
		return;

	TransformArrayAnimStack PoseComponentSpace;
	PoseComponentSpace.SetNumUninitialized(this->AnimationBoneContainer.GetCompactPoseNumBones());

	FMatrix3x4* RenderMatricesFloat = this->bHighPrecision ? this->AnimationBuffer->GetDataPointerHP() : nullptr;
	FMatrix3x4Half* RenderMatricesHalf = !this->bHighPrecision ? this->AnimationBuffer->GetDataPointerLP() : nullptr;

	FCompactPose CompactPose;
	CompactPose.SetBoneContainer(&this->AnimationBoneContainer);
	FBlendedCurve InCurve;
	InCurve.InitFrom(AnimationBoneContainer);
	UE::Anim::FStackAttributeContainer InAttributes;
	FAnimationPoseData poseData(CompactPose, InCurve, InAttributes);

	const double FrameTime = 1.0f / SequenceStruct.SampleFrequency;

	//for each frame
	for (int SeqFrameIndex = 0; SeqFrameIndex < SequenceStruct.AnimationFrameCount; SeqFrameIndex++)
	{
		const double SampleTime = SeqFrameIndex * FrameTime;
		const int AnimBufferFrameIndex = SequenceStruct.AnimationFrameIndex + SeqFrameIndex;

		//CompactPose.ResetToRefPose();
		AnimSequence->GetAnimationPose(poseData, FAnimExtractContext(SampleTime, this->bExtractRootMotion));
		Utils::LocalPoseToComponent(CompactPose, PoseComponentSpace.GetData());
		CachePose(AnimBufferFrameIndex, PoseComponentSpace);

		if(this->bHighPrecision)
		{
			CalcRenderMatrices(PoseComponentSpace, RenderMatricesFloat + (AnimBufferFrameIndex * this->RenderBoneCount));
		}
		else
		{
			CalcRenderMatrices(PoseComponentSpace, RenderMatricesHalf + (AnimBufferFrameIndex * this->RenderBoneCount));
		}
	}
}

void UAllegroAnimCollection::BuildMeshData()
{
#if WITH_EDITOR
	double StartTime = FPlatformTime::Seconds();
	//build mesh data
	ParallelFor(Meshes.Num(), [this](int MeshDefIdx) {
		//build mesh data
		FAllegroMeshDef& MeshDef = this->Meshes[MeshDefIdx];
		if (MeshDef.Mesh)
		{
			check(!MeshDef.MeshData);
			MeshDef.MeshData = MakeShared<FAllegroMeshDataEx>();

			/*
			FString MeshDataDDCKey = this->GetDDCKeyFoMeshBoneIndex(MeshDef);
			TArray<uint8> DDCData;
			if (GetDerivedDataCacheRef().GetSynchronous(*MeshDataDDCKey, DDCData, this->GetPathName()))
			{
				//有cache 从cache里读
				FMemoryReader DDCReader(DDCData);
				MeshDef.MeshData->Serialize(DDCReader);
				
				if (MeshDef.Mesh->GetMinLod().Default > MeshDef.BaseLOD)
				{
					MeshDef.BaseLOD = MeshDef.Mesh->GetMinLod().Default;
				}
			}
			else
			*/
			{
				
				if (MeshDef.Mesh->GetMinLod().Default > MeshDef.BaseLOD)
				{
					MeshDef.BaseLOD = MeshDef.Mesh->GetMinLod().Default;
				}

				MeshDef.MeshData->InitFromMesh(MeshDef.BaseLOD, MeshDef.Mesh, this);
				
				//没cache 组织数据，写到cache
				/*
				FMemoryWriter DDCWriter(DDCData);
				MeshDef.MeshData->Serialize(DDCWriter);
				GetDerivedDataCacheRef().Put(*MeshDataDDCKey, DDCData, this->GetPathName());
				*/
			}
		}

	}, GGenerateSingleThread ? EParallelForFlags::ForceSingleThread : EParallelForFlags::None);

	double ExecutionTime = FPlatformTime::Seconds() - StartTime;
	UE_LOG(LogAllegro, Log, TEXT("%s mesh builds finished in %f seconds"), *GetName(), ExecutionTime);
#endif

	this->TotalMeshBonesBufferSize = 0;
	for (FAllegroMeshDef& MeshDef : this->Meshes)
		if (MeshDef.Mesh && MeshDef.MeshData)
			this->TotalMeshBonesBufferSize += MeshDef.MeshData->GetTotalBufferSize();

}

void UAllegroAnimCollection::InitBoneContainer()
{
	const FReferenceSkeleton& SkelRefPose = Skeleton->GetReferenceSkeleton();
	TArray<bool> RequiredBones; //index is Skeleton bone index
	RequiredBones.Init(false, SkelRefPose.GetNum());

	for (const FAllegroMeshDef& MeshDef : this->Meshes)
	{
		if (!MeshDef.Mesh)
			continue;

		const FReferenceSkeleton& MeshRefPose = MeshDef.Mesh->GetRefSkeleton();
		const FSkeletalMeshRenderData* RenderData = MeshDef.Mesh->GetResourceForRendering();
		check(RenderData && RenderData->LODRenderData.Num());
		//bones that are not skinned will be excluded
		for (int LODIndex = MeshDef.BaseLOD; LODIndex < RenderData->LODRenderData.Num(); LODIndex++)
		{
			const FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[LODIndex];
			for (FBoneIndexType MeshBoneIndex : LODRenderData.ActiveBoneIndices)
			{
				int SkelBoneIndex = this->Skeleton->GetSkeletonBoneIndexFromMeshBoneIndex(MeshDef.Mesh, MeshBoneIndex);
				check(SkelBoneIndex != -1);
				RequiredBones[SkelBoneIndex] = true;
			}
		}
	}

	this->RenderRequiredBones.Reserve(RequiredBones.Num());
	this->SkeletonBoneToRenderBone.Init(-1, RequiredBones.Num());

	for (int SkelBoneIndex = 0; SkelBoneIndex < RequiredBones.Num(); SkelBoneIndex++)
	{
		if (RequiredBones[SkelBoneIndex])
		{
			this->SkeletonBoneToRenderBone[SkelBoneIndex] = RenderRequiredBones.Num();
			RenderRequiredBones.Add(SkelBoneIndex);
		}
	}

	//we need all bones so our compact pose bone index is same as skeleton bone index
	TArray<FBoneIndexType> AnimationRequiredBones;
	AnimationRequiredBones.SetNumUninitialized(SkelRefPose.GetNum());
	for (int SkelBoneIndex = 0; SkelBoneIndex < SkelRefPose.GetNum(); SkelBoneIndex++)
		AnimationRequiredBones[SkelBoneIndex] = SkelBoneIndex;

	this->AnimationBoneCount = AnimationRequiredBones.Num();
	this->RenderBoneCount = this->RenderRequiredBones.Num();


	UE::Anim::FCurveFilterSettings CurFillter(UE::Anim::ECurveFilterMode::DisallowAll);
	this->AnimationBoneContainer.InitializeTo(MoveTemp(AnimationRequiredBones), CurFillter, *this->Skeleton);
	this->AnimationBoneContainer.SetDisableRetargeting(this->bDisableRetargeting);
	if (this->RefPoseOverrideMesh && this->RefPoseOverrideMesh->GetSkeleton() == this->Skeleton)
	{
		InitRefPoseOverride();
	}
	else
	{
		Utils::ExtractRefPose(this->AnimationBoneContainer, this->RefPoseComponentSpace, this->RefPoseInverse);
	}
}



void UAllegroAnimCollection::InitRefPoseOverride()
{
	const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
	TArray<FTransform> NewRefPose = Utils::ConvertMeshPoseToSkeleton(this->RefPoseOverrideMesh);
	check(NewRefPose.Num() == RefSkel.GetNum());

	TSharedPtr<FSkelMeshRefPoseOverride> RefPoseOverride = MakeShared<FSkelMeshRefPoseOverride>();
	RefPoseOverride->RefBonePoses = NewRefPose;
	this->AnimationBoneContainer.SetRefPoseOverride(RefPoseOverride);

	Utils::ExtractRefPose(this->AnimationBoneContainer, this->RefPoseComponentSpace, this->RefPoseInverse);
	RefPoseOverride->RefBasesInvMatrix = RefPoseInverse;
}

FBox UAllegroAnimCollection::CalcPhysicsAssetBound(const UPhysicsAsset* PhysAsset, const TArrayView<FTransform>& PoseComponentSpace, bool bConsiderAllBodiesForBounds)
{
	//see UPhysicsAsset::CalcAABB for reference

	FBox AssetBound{ ForceInit };

	auto LCalcBodyBound = [&](const UBodySetup* BodySetup) {
		uint32 SkelBoneIndex = this->Skeleton->GetReferenceSkeleton().FindBoneIndex(BodySetup->BoneName);
		if (SkelBoneIndex == INDEX_NONE)
			return;

		FCompactPoseBoneIndex CompactBoneIndex = this->AnimationBoneContainer.GetCompactPoseIndexFromSkeletonPoseIndex(FSkeletonPoseBoneIndex(SkelBoneIndex));
		FBox BodyBound = BodySetup->AggGeom.CalcAABB(PoseComponentSpace[CompactBoneIndex.GetInt()]);	//Min Max might be reversed if transform has negative scale
		BodyBound = FBox(BodyBound.Min.ComponentMin(BodyBound.Max), BodyBound.Min.ComponentMax(BodyBound.Max));
		AssetBound += BodyBound;
		};

	if (bConsiderAllBodiesForBounds)
	{
		for (UBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
		{
			if (BodySetup)
				LCalcBodyBound(BodySetup);
		}
	}
	else
	{
		for (int BodyIndex : PhysAsset->BoundsBodies)
		{
			const UBodySetup* BodySetup = PhysAsset->SkeletalBodySetups[BodyIndex];
			if (BodySetup && BodySetup->bConsiderForBounds)
				LCalcBodyBound(BodySetup);
		}
	}

	check(AssetBound.IsValid && !AssetBound.Min.ContainsNaN() && !AssetBound.Max.ContainsNaN());
	return AssetBound;
}

//void UAllegroAnimCollection::ExtractCompactPose(const FCompactPose& Pose, FTransform* OutComponentSpace, FMatrix44f* OutShaderSpace)
//{
// 	OutComponentSpace[0] = Pose[FCompactPoseBoneIndex(0)];
// 
// 	for (FCompactPoseBoneIndex CompactIdx(1); CompactIdx < Pose.GetNumBones(); ++CompactIdx)
// 	{
// 		const FCompactPoseBoneIndex ParentIdx = Pose.GetParentBoneIndex(CompactIdx);
// 		OutComponentSpace[CompactIdx.GetInt()] = Pose[CompactIdx] * OutComponentSpace[ParentIdx.GetInt()];
// 	}
// 
// 	for (int i = 0; i < RenderRequiredBones.Num(); i++)
// 	{
// 		FBoneIndexType CompactBoneIdx = RenderRequiredBones[i];
// 		FMatrix44f BoneMatrix = ((FTransform3f)OutComponentSpace[CompactBoneIdx]).ToMatrixWithScale();
// 		OutShaderSpace[i] = this->RefPoseInverse[CompactBoneIdx] * BoneMatrix;
// 	}
//}


void UAllegroAnimCollection::CalcRenderMatrices(const TArrayView<FTransform> PoseComponentSpace, FMatrix3x4* OutMatrices) const
{
	for (int i = 0; i < RenderRequiredBones.Num(); i++)
	{
		FBoneIndexType CompactBoneIdx = RenderRequiredBones[i];
		FMatrix44f BoneMatrix = static_cast<FTransform3f>(PoseComponentSpace[CompactBoneIdx]).ToMatrixWithScale();
		AllegroSetMatrix3x4Transpose(OutMatrices[i], this->RefPoseInverse[CompactBoneIdx] * BoneMatrix);
	}
}

void UAllegroAnimCollection::CalcRenderMatrices(const TArrayView<FTransform> PoseComponentSpace, FMatrix3x4Half* OutMatrices) const
{
	for (int i = 0; i < RenderRequiredBones.Num(); i++)
	{
		FBoneIndexType CompactBoneIdx = RenderRequiredBones[i];
		FMatrix44f BoneMatrix = static_cast<FTransform3f>(PoseComponentSpace[CompactBoneIdx]).ToMatrixWithScale();
		FMatrix3x4 FloatMatrix;
		AllegroSetMatrix3x4Transpose(FloatMatrix, this->RefPoseInverse[CompactBoneIdx] * BoneMatrix);
		OutMatrices[i] = FloatMatrix;
	}
}
// 
// void UAllegroAnimCollection::CalcRenderMatrices(const TArrayView<FTransform> PoseComponentSpace, FMatrix44f* OutMatrices) const
// {
// 	for (int i = 0; i < RenderRequiredBones.Num(); i++)
// 	{
// 		FBoneIndexType CompactBoneIdx = RenderRequiredBones[i];
// 		FMatrix44f BoneMatrix = ((FTransform3f)PoseComponentSpace[CompactBoneIdx]).ToMatrixWithScale();
// 		OutMatrices[i] = (this->RefPoseInverse[CompactBoneIdx] * BoneMatrix).GetTransposed();
// 	}
// }

uint32 UAllegroAnimCollection::GetRenderMatrixSize() const
{
	return bHighPrecision ? sizeof(FMatrix3x4) : sizeof(FMatrix3x4Half);
}

void UAllegroAnimCollection::EnqueueReleaseResources()
{
	ENQUEUE_RENDER_COMMAND(ReleaseResoruces)([this](FRHICommandListImmediate& RHICmdList) {
		if(this->AnimationBuffer)
			this->AnimationBuffer->ReleaseResource();

		this->ReleaseMeshDataResources();
		this->ScatterBuffer.Release();
	});
}

UAnimSequenceBase* UAllegroAnimCollection::GetRandomAnimSequenceFromStream(const FRandomStream& RandomSteam) const
{
	if (Sequences.Num())
		return Sequences[RandomSteam.RandHelper(Sequences.Num())].Sequence;
	return nullptr;
}

#if WITH_EDITOR
FString UAllegroAnimCollection::GetDDCKeyFoMeshBoneIndex(const FAllegroMeshDef& MeshDef) const
{
	uint32 BonesHash = FFnv::MemFnv32(this->RenderRequiredBones.GetData(), this->RenderRequiredBones.Num() * this->RenderRequiredBones.GetTypeSize());
	TStringBuilder<800> SBuilder;
	SBuilder.Appendf(TEXT("_MeshBoneIndices_%d_%d_%s"), BonesHash, MeshDef.BaseLOD, *MeshDef.Mesh->GetDerivedDataKey());

	return FDerivedDataCacheInterface::BuildCacheKey(TEXT("ALLEGRO"), TEXT("4"), SBuilder.GetData());
}
#endif

int UAllegroAnimCollection::CalcFrameIndex(const FAllegroSequenceDef& SequenceStruct, float SequencePlayTime) const
{
	int LocalFrameIndex = static_cast<int>(SequencePlayTime * SequenceStruct.SampleFrequencyFloat);
	check(LocalFrameIndex < SequenceStruct.AnimationFrameCount);
	int SequenceBufferFrameIndex = SequenceStruct.AnimationFrameIndex + LocalFrameIndex;
	check(SequenceBufferFrameIndex < this->FrameCountSequences);
	return static_cast<int>(SequenceBufferFrameIndex);
}




int UAllegroAnimCollection::ReserveUploadData(int FrameCount)
{
	int ScatterIdx = this->CurrentUpload.ScatterData.AddUninitialized(FrameCount);
	this->CurrentUpload.PoseData.AddUninitialized(FrameCount * this->RenderBoneCount);
	return ScatterIdx;
}

void UAllegroAnimCollection::UploadDataSetNumUninitialized(int N)
{
	CurrentUpload.ScatterData.SetNumUninitialized(N, false);
	CurrentUpload.PoseData.SetNumUninitialized(N * this->RenderBoneCount, false);
}

void UAllegroAnimCollection::RemoveUnusedTransition(AllegroTransitionIndex UnusedTI)
{
	FTransition& T = this->Transitions[UnusedTI];
	check(T.IsUnused());
	check(!T.IsDeferred());

	this->TransitionsHashTable.Remove(T.GetKeyHash(), UnusedTI);
	this->TransitionPoseAllocator.Free(T.BlockOffset, T.FrameCount);
	this->Transitions.RemoveAt(UnusedTI);
}

void UAllegroAnimCollection::RemoveAllUnusedTransitions()
{
	for (AllegroTransitionIndex UnusedTI : NegativeRCTransitions)
	{
		RemoveUnusedTransition(UnusedTI);
	}
	
	NegativeRCTransitions.Reset();
}



TPair<int, UAllegroAnimCollection::ETransitionResult> UAllegroAnimCollection::FindOrCreateTransition(const FTransitionKey& Key, bool bIgonreTransitionGeneration)
{
	check(IsInGameThread());

	ALLEGRO_SCOPE_CYCLE_COUNTER(UAllegroAnimCollection_FindOrCreateTransition);

	const uint32 KeyHash = Key.GetKeyHash();
	
	for (uint32 TransitionIndex = this->TransitionsHashTable.First(KeyHash); this->TransitionsHashTable.IsValid(TransitionIndex); TransitionIndex = this->TransitionsHashTable.Next(TransitionIndex))
	{
		FTransition& Trn = this->Transitions[TransitionIndex];
		if (Trn.KeysEqual(Key))
		{
			IncTransitionRef(TransitionIndex);
			return { TransitionIndex, ETR_Success_Found };
		}
	}
	


	//developer settings with console variable was not loading properly so we use CDO instead of CVar
	if (bIgonreTransitionGeneration || GAllegro_NumTransitionGeneratedThisFrame >= GetDefault<UAllegroDeveloperSettings>()->MaxTransitionGenerationPerFrame || GAllegro_DisableTransitionGeneration)
		return { -1, ETR_Failed_RateLimitReached };

	if (Transitions.Num() >= 0xFFff) //transition index is uint16
		return { -1, ETR_Failed_BufferFull };

	int BlockOffset = this->TransitionPoseAllocator.Alloc(Key.FrameCount);
	if (BlockOffset == -1) //if pool is full free unused transitions 
	{
		while (NegativeRCTransitions.Num())
		{
			//try remove several elements at once
			int N = FMath::Min(8, NegativeRCTransitions.Num());
			while (N > 0)
			{
				AllegroTransitionIndex UnusedTransitionIndex = NegativeRCTransitions.Pop();
				RemoveUnusedTransition(UnusedTransitionIndex);
				N--;
			};

			BlockOffset = this->TransitionPoseAllocator.Alloc(Key.FrameCount);
			if (BlockOffset != -1)
				break;
		};
		
		if (BlockOffset == -1)
		{
			return { -1, ETR_Failed_BufferFull };
		}
	}
	
	NumTransitionFrameAllocated = this->TransitionPoseAllocator.AllocSize;
	GAllegro_NumTransitionGeneratedThisFrame++;

	uint32 NewTransitionIndex = Transitions.Add(FTransition{});
	FTransition& NewTransition = Transitions[NewTransitionIndex];
	static_cast<FTransitionKey&>(NewTransition) = Key;
	NewTransition.BlockOffset = BlockOffset;
	NewTransition.FrameIndex = this->FrameCountSequences + BlockOffset;

	//push it for concurrent end of frame generation
	//#Note CachedTransforms of the transitions contain invalid value
	NewTransition.DeferredIndex = this->DeferredTransitions.Add(NewTransitionIndex);
	this->DeferredTransitions_FrameCount += Key.FrameCount;

	this->TransitionsHashTable.Add(KeyHash, NewTransitionIndex);

	return { NewTransitionIndex, ETR_Success_NewlyCreated };
}

void UAllegroAnimCollection::IncTransitionRef(AllegroTransitionIndex TransitionIndex)
{
	check(IsInGameThread());
	FTransition& T = this->Transitions[TransitionIndex];

	if (T.RefCount > 0)
	{
		T.RefCount++;
	}
	else
	{
		check(T.RefCount == 0 || T.RefCount == -1);

		TArray<AllegroTransitionIndex>& TargetArray = T.RefCount == 0 ? ZeroRCTransitions : NegativeRCTransitions;
		TargetArray[T.StateIndex] = TargetArray.Last();
		this->Transitions[TargetArray.Last()].StateIndex = T.StateIndex;
		TargetArray.Pop();

		T.StateIndex = ~AllegroTransitionIndex(0);
		T.RefCount = 1;
	}
}

void UAllegroAnimCollection::DecTransitionRef(AllegroTransitionIndex& TransitionIndex)
{
	check(IsInGameThread());
	FTransition& T = this->Transitions[TransitionIndex];
	check(T.RefCount > 0)
	T.RefCount--;
	if(T.RefCount == 0)
	{
		T.StateIndex = this->ZeroRCTransitions.Add(TransitionIndex);
	}
	TransitionIndex = ~AllegroTransitionIndex(0);
}

void UAllegroAnimCollection::ReleasePendingTransitions()
{
	int BaseIndex = NegativeRCTransitions.AddUninitialized(ZeroRCTransitions.Num());
	for (int i = 0; i < ZeroRCTransitions.Num(); i++)
	{
		AllegroTransitionIndex TI = this->ZeroRCTransitions[i];
		FTransition& T = Transitions[TI];
		T.RefCount = -1;
		T.StateIndex = static_cast<AllegroTransitionIndex>(BaseIndex + i);
		NegativeRCTransitions[T.StateIndex] = TI;
	}

	this->ZeroRCTransitions.Reset();

}

void UAllegroAnimCollection::GenerateTransition_Concurrent(uint32 TransitionIndex, uint32 ScatterIdx)
{
	const FTransition& Trs = this->Transitions[TransitionIndex];
	const FAllegroSequenceDef& SequenceStructFrom = this->Sequences[Trs.FromSI];
	const FAllegroSequenceDef& SequenceStructTo = this->Sequences[Trs.ToSI];

	const int TransitionFrameCount = Trs.FrameCount;
	const double TransitionFrameTime = 1.0f / SequenceStructTo.SampleFrequency;
	const double FrameTime = 1.0f / SequenceStructFrom.SampleFrequency;

	INC_DWORD_STAT_BY(STAT_ALLEGRO_NumTransitionPoseGenerated, TransitionFrameCount);

	for (int i = 0; i < TransitionFrameCount; i++)
		this->CurrentUpload.ScatterData[ScatterIdx + i] = static_cast<uint32>(Trs.FrameIndex + i);

	FMatrix3x4* UploadMatrices = &this->CurrentUpload.PoseData[ScatterIdx * this->RenderBoneCount];

	FMemMark MemMarker(FMemStack::Get());

	FCompactPose CompactPoseFrom, CompactPoseTo;
	CompactPoseFrom.SetBoneContainer(&this->AnimationBoneContainer);
	CompactPoseTo.SetBoneContainer(&this->AnimationBoneContainer);
	
	FBlendedCurve InCurve;
	InCurve.InitFrom(AnimationBoneContainer);
	UE::Anim::FStackAttributeContainer InAttributes;
	FAnimationPoseData PoseDataFrom(CompactPoseFrom, InCurve, InAttributes);
	FAnimationPoseData PoseDataTo(CompactPoseTo, InCurve, InAttributes);

	{
		const double SampleStartTimeA = Trs.FromFI * FrameTime;
		const double SampleStartTimeB = Trs.ToFI * TransitionFrameTime;

		TransformArrayAnimStack PoseComponentSpace;
		PoseComponentSpace.SetNumUninitialized(this->AnimationBoneContainer.GetCompactPoseNumBones());

		for (int TransitionFrameIndex = 0; TransitionFrameIndex < TransitionFrameCount; TransitionFrameIndex++)
		{
			const double LocalTime = TransitionFrameIndex * TransitionFrameTime;
			const double SampleTimeA = SampleStartTimeA + LocalTime;
			const double SampleTimeB = SampleStartTimeB + LocalTime;
			
			const float TransitionAlpha = (TransitionFrameIndex + 1) / static_cast<float>(TransitionFrameCount + 1);
			check(TransitionAlpha != 0 && TransitionAlpha != 1); //not having any blend is waste
			const float FinalAlpha = FAlphaBlend::AlphaToBlendOption(TransitionAlpha, Trs.BlendOption);
			const int TransitionPoseIndex = Trs.FrameIndex + TransitionFrameIndex;

			SequenceStructFrom.Sequence->GetAnimationPose(PoseDataFrom, FAnimExtractContext(SampleTimeA, this->bExtractRootMotion,{}, Trs.bFromLoops));
			SequenceStructTo.Sequence->GetAnimationPose(PoseDataTo, FAnimExtractContext(SampleTimeB, this->bExtractRootMotion, {}, Trs.bToLoops));

			for (int TransformIndex = 0; TransformIndex < CompactPoseFrom.GetNumBones(); TransformIndex++)
			{
				CompactPoseFrom.GetMutableBones()[TransformIndex].BlendWith(CompactPoseTo.GetMutableBones()[TransformIndex], FinalAlpha);
			}

			Utils::LocalPoseToComponent(CompactPoseFrom, PoseComponentSpace.GetData());
			CachePoseBones(TransitionPoseIndex, PoseComponentSpace);
			CalcRenderMatrices(PoseComponentSpace, UploadMatrices + (TransitionFrameIndex * this->RenderBoneCount));
		}
	}
}

void UAllegroAnimCollection::FlushDeferredTransitions()
{
	check(IsInGameThread());
	ALLEGRO_SCOPE_CYCLE_COUNTER(UAllegroAnimCollection_FlushDeferredTransitions);

	if (this->DeferredTransitions.Num())
	{
		FMemMark MemMarker(FMemStack::Get());
		int* ScatterIndices = New<int>(FMemStack::Get(), this->DeferredTransitions.Num());

		int ScatterIdx = ReserveUploadData(this->DeferredTransitions_FrameCount);
		for (int i = 0; i < this->DeferredTransitions.Num(); i++)
		{
			FTransition& T = this->Transitions[DeferredTransitions[i]];
			check(T.IsDeferred());
			T.DeferredIndex = -1;
			ScatterIndices[i] = ScatterIdx;
			ScatterIdx += T.FrameCount;
		}

		ParallelFor(DeferredTransitions.Num(), [this, ScatterIndices](int Index) {
			this->GenerateTransition_Concurrent(this->DeferredTransitions[Index], ScatterIndices[Index]);
		});

		this->DeferredTransitions.Reset();
		this->DeferredTransitions_FrameCount = 0;
	}
	
}

void UAllegroAnimCollection::ApplyScatterBufferRT(FRHICommandList& RHICmdList, const FPoseUploadData& UploadData)
{
	check(IsInRenderingThread());

	if (UploadData.ScatterData.Num())
	{
		ALLEGRO_SCOPE_CYCLE_COUNTER(UAllegroAnimCollection_ApplyScatterBufferRT);

		const uint32 MatrixSize = sizeof(FMatrix3x4);
		const uint32 PoseSizeBytes = this->RenderBoneCount * MatrixSize;
		this->ScatterBuffer.Init(UploadData.ScatterData, PoseSizeBytes, true, TEXT("AnimCollectionScatter"));
		FMemory::Memcpy(this->ScatterBuffer.UploadData, UploadData.PoseData.GetData(), UploadData.PoseData.Num() * MatrixSize);

		FRWBuffer buffData;
		buffData.Buffer = this->AnimationBuffer->Buffer;
		buffData.SRV = this->AnimationBuffer->ShaderResourceViewRHI;
		buffData.UAV = this->AnimationBuffer->UAV;
		this->ScatterBuffer.ResourceUploadTo(RHICmdList, buffData);
	}
}


FMatrix3x4* UAllegroAnimCollection::RequestPoseUpload(int PoseIndex)
{
	int ScatterIdx = ReserveUploadData(1);
	this->CurrentUpload.ScatterData[ScatterIdx] = PoseIndex;
	return &this->CurrentUpload.PoseData[ScatterIdx * RenderBoneCount];
}

void UAllegroAnimCollection::RequestPoseUpload(int PoseIndex, const TArrayView<FMatrix3x4> PoseTransforms)
{
	check(PoseTransforms.Num() == RenderBoneCount);
	FMatrix3x4* Dst = RequestPoseUpload(PoseIndex);
	FMemory::Memcpy(Dst, PoseTransforms.GetData(), sizeof(FMatrix3x4) * RenderBoneCount);
}
 


FAllegroMeshDef::FAllegroMeshDef()
{
	Mesh = nullptr;
	BaseLOD = 0;
	BoundExtent = FVector3f::ZeroVector;
	MaxBBox = FBoxMinMaxFloat(ForceInit);
	//MaxCoveringRadius = 0;
	OwningBoundMeshIndex = -1;

}

const FSkeletalMeshLODRenderData& FAllegroMeshDef::GetBaseLODRenderData() const
{
	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	check(RenderData && RenderData->LODRenderData.Num());
	return RenderData->LODRenderData[FMath::Min(this->BaseLOD, static_cast<uint8>(RenderData->LODRenderData.Num() - 1))];
}

void FAllegroMeshDef::SerializeCooked(FArchive& Ar)
{
	if (Ar.IsSaving())
	{
		bool bHasMeshData = MeshData.IsValid() && MeshData->LODs.Num();
		Ar << bHasMeshData;
		if (bHasMeshData)
		{
			MeshData->Serialize(Ar);
		}
	}
	else
	{
		bool bHasMeshData = false;
		Ar << bHasMeshData;
		if(bHasMeshData)
		{
			MeshData = MakeShared<FAllegroMeshDataEx>();
			MeshData->Serialize(Ar);
		}
	}
}

FAllegroSequenceDef::FAllegroSequenceDef()
{
	Sequence = nullptr;
	SampleFrequency = 30;
	SampleFrequencyFloat = 30;
	AnimationFrameIndex = 0;
	AnimationFrameCount = 0;
	SequenceLength = 0;
}

int FAllegroSequenceDef::CalcFrameIndex(float time) const
{
	check(time >= 0 && time <= SequenceLength);
	int seqIdx = (int)(time * SampleFrequencyFloat);
	check(seqIdx < AnimationFrameCount);
	return AnimationFrameIndex + seqIdx;
}

int FAllegroSequenceDef::CalcFrameCount() const
{
	const int MaxFrame = static_cast<int>(SampleFrequency * Sequence->GetPlayLength()) + 1;
	return MaxFrame;
}

// const FAllegroBlendDef* FAllegroSequenceDef::FindBlendDef(const UAnimSequenceBase* Anim) const
// {
// 	if (!Anim)
// 		return nullptr;
// 	return Blends.FindByPredicate([&](const FAllegroBlendDef& BlendDef) { return BlendDef.BlendTo == Anim; });
// }
// 
// int FAllegroSequenceDef::IndexOfBlendDef(const UAnimSequenceBase* Anim) const
// {
// 	if (!Anim)
// 		return -1;
// 	return Blends.IndexOfByPredicate([&](const FAllegroBlendDef& BlendDef) { return BlendDef.BlendTo == Anim; });
// }
// 
// int FAllegroSequenceDef::IndexOfBlendDefByPath(const FSoftObjectPath& AnimationPath) const
// {
// 	return Blends.IndexOfByPredicate([&](const FAllegroBlendDef& BlendDef) { return BlendDef.BlendTo && FSoftObjectPath(BlendDef.BlendTo) == AnimationPath; });
// }
// 


void FAllegroCompactPhysicsAsset::Init(const USkeleton* Skeleton, const UPhysicsAsset* PhysAsset)
{
	for (const USkeletalBodySetup* Body : PhysAsset->SkeletalBodySetups)
	{
		int BoneIndex = Skeleton->GetReferenceSkeleton().FindBoneIndex(Body->BoneName);
		check(BoneIndex != -1);

		for (const FKSphereElem& Shape : Body->AggGeom.SphereElems)
		{
			Spheres.Emplace(FShapeSphere{ Shape.Center, Shape.Radius, BoneIndex });
		}
		for (const FKSphylElem& Shape : Body->AggGeom.SphylElems)
		{
			const FVector Axis = Shape.Rotation.RotateVector(FVector(0, 0, Shape.Length * 0.5f));
			Capsules.Emplace(FShapeCapsule { Shape.Center - Axis, Shape.Center + Axis , Shape.Radius, BoneIndex });
		}
		for (const FKBoxElem& Shape : Body->AggGeom.BoxElems)
		{
			const FVector HalfExtent = FVector(Shape.X, Shape.Y, Shape.Z) * 0.5f;
			const FQuat BoxQuat = Shape.Rotation.Quaternion();

			if (BoxQuat.IsIdentity()) //see ChaosInterface.CreateGeometry  AABB can handle translations internally but if we have a rotation we need to wrap it in a transform
			{
				Boxes.Emplace(FShapeBox{ FQuat::Identity, FVector::ZeroVector, Shape.Center - HalfExtent, Shape.Center + HalfExtent, BoneIndex, false });
			}
			else
			{
				Boxes.Emplace(FShapeBox{ BoxQuat, Shape.Center, -HalfExtent, HalfExtent, BoneIndex, true });
			}

			
		}
	}

}

int FAllegroCompactPhysicsAsset::RayCastAny(const UAllegroAnimCollection* AnimCollection, int FrameIndex, const FVector& Start, const FVector& Dir, Chaos::FReal Len) const
{
	check(Dir.IsNormalized() && Len > 0);

	FVector P, D;
	auto InvertByBone = [&](int BoneIndex){
		//
		const FTransform& BoneT = (FTransform)AnimCollection->GetBoneTransformFast(BoneIndex, FrameIndex);
		P = BoneT.InverseTransformPosition(Start);
		D = BoneT.InverseTransformVector(Dir);
	};

	for (const FShapeCapsule& Capsule : Capsules)
	{
		InvertByBone(Capsule.BoneIndex);
		
		FVector End = P + (D * Len);
		FVector PT1, PT2;
		FMath::SegmentDistToSegment(P, End, Capsule.A, Capsule.B, PT1, PT2);
		if (FVector::DistSquared(PT1, PT2) < FMath::Square(Capsule.Radius))
			return Capsule.BoneIndex;

	}

	for (const FShapeSphere& Sphere : Spheres)
	{
		InvertByBone(Sphere.BoneIndex);
		
		if (FMath::LineSphereIntersection(P, D, Len, Sphere.Center, (double)Sphere.Radius))
			return Sphere.BoneIndex;
	}

	for (const FShapeBox& Box : Boxes)
	{
		InvertByBone(Box.BoneIndex);

		if(Box.bHasTransform)
		{
			FTransform BT(Box.Rotation, Box.Center);
			P = BT.InverseTransformPositionNoScale(P);
			D = BT.InverseTransformVectorNoScale(D);
		}

		FVector End = P + (D * Len);
		FVector StartToEnd = End - P;
		if (FMath::LineBoxIntersection(FBox(Box.BoxMin, Box.BoxMax), P, End, StartToEnd))
			return Box.BoneIndex;
	}

	return -1;
}

int FAllegroCompactPhysicsAsset::Overlap(const UAllegroAnimCollection* AnimCollection, int FrameIndex, const FVector& Point, Chaos::FReal Thickness) const
{
	const auto ThicknessSQ = Thickness * Thickness;

	auto InvertByBone = [&](int BoneIndex) {
		FTransform BoneT = (FTransform)AnimCollection->GetBoneTransformFast(BoneIndex, FrameIndex);
		return BoneT.InverseTransformPosition(Point);
	};

	for (const FShapeCapsule& Capsule : Capsules)
	{
		FVector P = InvertByBone(Capsule.BoneIndex);
		if (FMath::PointDistToSegmentSquared(P, Capsule.A, Capsule.B) <= ThicknessSQ)
			return Capsule.BoneIndex;
	}

	for (const FShapeSphere& Sphere : Spheres)
	{
		FVector P = InvertByBone(Sphere.BoneIndex);
		if (FVector::DistSquared(Sphere.Center, P) <= FMath::Square(Thickness + Sphere.Radius))
			return Sphere.BoneIndex;
	}

	for (const FShapeBox& Box : Boxes)
	{
		FVector P = InvertByBone(Box.BoneIndex);
		if (Box.bHasTransform)
		{
			P = FTransform(Box.Rotation, Box.Center).InverseTransformPositionNoScale(P);
		}

		if (FMath::SphereAABBIntersection(P, ThicknessSQ, FBox(Box.BoxMin, Box.BoxMax)))
			return Box.BoneIndex;
	}

	return -1;
}



int FAllegroCompactPhysicsAsset::Raycast(const UAllegroAnimCollection* AnimCollection, int FrameIndex, const FVector& StartPoint, const FVector& Dir, Chaos::FReal Length, Chaos::FReal Thickness, Chaos::FReal& OutTime, FVector& OutPosition, FVector& OutNormal) const
{
	FVector LocalStart, LocalDir;

	auto InvertByBone = [&](int BoneIndex) {
		FTransform BoneT = (FTransform)AnimCollection->GetBoneTransformFast(BoneIndex, FrameIndex);
		LocalStart = BoneT.InverseTransformPosition(StartPoint);
		LocalDir = BoneT.InverseTransformVector(Dir);
	};

	Chaos::FVec3 OutLocalPos, OutLocalNormal;
	int FaceIndex = -1;
	Chaos::FReal MinTime = TNumericLimits<Chaos::FReal>::Max();
	int HitBoneIndex = -1;

	auto RevertLocals = [&](int BoneIndex) {
		if(OutTime < MinTime)
		{
			MinTime = OutTime;
			HitBoneIndex = BoneIndex;

			FTransform BoneT = (FTransform)AnimCollection->GetBoneTransformFast(BoneIndex, FrameIndex);
			OutPosition = BoneT.TransformPosition(OutLocalPos);
			OutNormal = BoneT.TransformVector(OutLocalNormal);
		}
	};



	for (const FShapeCapsule& Capsule : Capsules)
	{
		InvertByBone(Capsule.BoneIndex);

		if(Chaos::FCapsule(Capsule.A, Capsule.B, Capsule.Radius).Raycast(LocalStart, LocalDir, Length, Thickness, OutTime, OutLocalPos, OutLocalNormal, FaceIndex))
		{
			RevertLocals(Capsule.BoneIndex);
		}
	}

	for (const FShapeSphere& Sphere : Spheres)
	{
		InvertByBone(Sphere.BoneIndex);
		
		if(Chaos::FImplicitSphere3(Sphere.Center, Sphere.Radius).Raycast(LocalStart, LocalDir, Length, Thickness, OutTime, OutLocalPos, OutLocalNormal, FaceIndex))
		{
			RevertLocals(Sphere.BoneIndex);
		}
	}

	for (const FShapeBox& Box : Boxes)
	{
		InvertByBone(Box.BoneIndex);

		if (Box.bHasTransform)
		{
			FTransform BT(Box.Rotation, Box.Center);
			LocalStart = BT.InverseTransformPositionNoScale(LocalStart);
			LocalDir = BT.InverseTransformVectorNoScale(LocalDir);
		}
		

		if (Chaos::FAABB3(Box.BoxMin, Box.BoxMax).Raycast(LocalStart, LocalDir, Length, Thickness, OutTime, OutLocalPos, OutLocalNormal, FaceIndex))
		{
			if (Box.bHasTransform)
			{
				FTransform BT(Box.Rotation, Box.Center);
				OutLocalPos = BT.TransformPositionNoScale(OutLocalPos);
				OutLocalNormal = BT.TransformVectorNoScale(OutLocalNormal);
			}

			RevertLocals(Box.BoneIndex);
		}
	}
	return HitBoneIndex;
}


void UAllegroAnimCollection::OnPreSendAllEndOfFrameUpdates(UWorld* World)
{
	FlushDeferredTransitions();

	if (this->CurrentUpload.ScatterData.Num())
	{
		ENQUEUE_RENDER_COMMAND(ScatterUpdate)([this, UploadData = MoveTemp(CurrentUpload)](FRHICommandListImmediate& RHICmdList) {
			
			this->ApplyScatterBufferRT(RHICmdList, UploadData);
		});
	}
}
void UAllegroAnimCollection::OnEndFrame()
{
}

void UAllegroAnimCollection::OnBeginFrameRT()
{

}

void UAllegroAnimCollection::OnEndFrameRT()
{

}

void UAllegroAnimCollection::OnBeginFrame()
{
	ReleasePendingTransitions();

}


