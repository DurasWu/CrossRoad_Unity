// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroRender.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "AllegroComponent.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "AllegroPrivate.h"
#include "AllegroAnimCollection.h"
#include "RendererInterface.h"
#include "Async/ParallelFor.h"

#include "Materials/MaterialRenderProxy.h"
#include "ConvexVolume.h"
#include "SceneView.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "UnrealEngine.h"
#include "SceneInterface.h"
#include "MaterialShared.h"

#include "AllegroPrivateUtils.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"

#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

bool GAllegro_DrawInstanceBounds = false;
FAutoConsoleVariableRef CVar_DrawInstanceBounds(TEXT("allegro.DrawInstanceBounds"), GAllegro_DrawInstanceBounds, TEXT(""), ECVF_Default);

bool GAllegro_DrawCells = false;
FAutoConsoleVariableRef CVar_DrawCells(TEXT("allegro.DrawCells"), GAllegro_DrawCells, TEXT(""), ECVF_Default);

int GAllegro_ForcedAnimFrameIndex = -1;
FAutoConsoleVariableRef CVar_ForcedAnimFrameIndex(TEXT("allegro.ForcedAnimFrameIndex"), GAllegro_ForcedAnimFrameIndex, TEXT("force all instances to use the specified animation frame index. -1 to disable"), ECVF_Default);

int GAllegro_ForceLOD = -1;
FAutoConsoleVariableRef CVar_ForceLOD(TEXT("allegro.ForceLOD"), GAllegro_ForceLOD, TEXT("similar to r.ForceLOD, -1 to disable"), ECVF_Default);

int GAllegro_ShadowForceLOD = -1;
FAutoConsoleVariableRef CVar_ShadowForceLOD(TEXT("allegro.ShadowForceLOD"), GAllegro_ShadowForceLOD, TEXT(""), ECVF_Default);

int GAllegro_MaxTrianglePerInstance = -1;
FAutoConsoleVariableRef CVar_MaxTrianglePerInstance(TEXT("allegro.MaxTrianglePerInstance"), GAllegro_MaxTrianglePerInstance, TEXT("limits the per instance triangle counts, used for debug/profile purposes. <= 0 to disable"), ECVF_Default);

int GAllegro_FroceMaxBoneInfluence = -1;
FAutoConsoleVariableRef CVar_FroceMaxBoneInfluence(TEXT("allegro.FroceMaxBoneInfluence"), GAllegro_FroceMaxBoneInfluence, TEXT("limits the MaxBoneInfluence for all instances, -1 to disable"), ECVF_Default);

float GAllegro_DistanceScale = 1;
FAutoConsoleVariableRef CVar_DistanceScale(TEXT("allegro.DistanceScale"), GAllegro_DistanceScale, TEXT("scale used for distance based LOD. higher value results in higher LOD."), ECVF_Default);


bool GAllegro_DisableFrustumCull = false;
FAutoConsoleVariableRef CVar_DisableFrustumCull(TEXT("allegro.DisableFrustumCull"), GAllegro_DisableFrustumCull, TEXT(""), ECVF_Default);

bool GAllegro_DisableGridCull = false;
FAutoConsoleVariableRef CVar_DisableGriding(TEXT("allegro.DisableGridCull"), GAllegro_DisableGridCull, TEXT(""), ECVF_Default);

int GAllegro_NumInstancePerGridCell = 256;
FAutoConsoleVariableRef CVar_NumInstancePerGridCell(TEXT("allegro.NumInstancePerGridCell"), GAllegro_NumInstancePerGridCell, TEXT(""), ECVF_Default);

bool GAllegro_DisableSectionsUnification = false;
FAutoConsoleVariableRef CVar_DisableSectionsUnification(TEXT("allegro.DisableSectionsUnification"), GAllegro_DisableSectionsUnification, TEXT(""), ECVF_Default);

float GAllegro_CullScreenSize = 0.0001f;
#if ALLEGRO_USE_LOD_SCREEN_SIZE
FAutoConsoleVariableRef CVar_CullScreenSize(TEXT("allegro.CullScreenSize"), GAllegro_CullScreenSize, TEXT("min screen size for culling."), ECVF_Default);
#endif



ALLEGRO_AUTO_CVAR_DEBUG(bool, DebugForceNoPrevFrameData, false, "", ECVF_Default);

#include "AllegroBatchGenerator.h"


void CalcBoundExtentFactor(TArray<FBoxCenterExtentFloat> Extents, TArray<float>& Factors)
{
	Factors.Reset(Extents.Num());
	FBoxMinMaxFloat Bound(ForceInit);

	for (auto& Extent : Extents)
	{
		Bound.Add(Extent);
		Factors.Add_GetRef(1.0f);
	}

	FBox3f BoundBox = Bound.ToBox();
	FVector3f BoundSize = BoundBox.GetSize();
	float BoundSphere = FMath::Max(FMath::Max(BoundSize.Y, BoundSize.Z), BoundSize.X);

	for(int i=0;i< Extents.Num();++i)
	{
		FBox3f SubBox = Extents[i].GetFBox();
		FVector3f SubSize = SubBox.GetSize();
		float SubSphere = FMath::Max(FMath::Max(SubSize.Y, SubSize.Z), SubSize.X);
		Factors[i] = SubSphere / BoundSphere;
	}
}


FAllegroProxy::FAllegroProxy(const UAllegroComponent* Component, FName ResourceName)
	: FPrimitiveSceneProxy(Component, ResourceName)
	, MaterialRelevance(Component->GetMaterialRelevance(GetScene().GetFeatureLevel()))
	, AminCollection(Component->AnimCollection)
	, InstanceMaxDrawDistance(Component->InstanceMaxDrawDistance)
	, InstanceMinDrawDistance(Component->InstanceMinDrawDistance)
	, DistanceScale(Component->LODDistanceScale)
	, NumCustomDataFloats(Component->NumCustomDataFloats)
	//, MinLODIndex(Component->MinLODIndex)
	//, MaxLODIndex(Component->MaxLODIndex)
	, ShadowLODBias(Component->ShadowLODBias)
	, StartShadowLODBias(Component->StartShadowLODBias)
	//, SortMode(Component->SortMode)
	, bNeedCustomDataForShadowPass(Component->bNeedCustomDataForShadowPass)
	, bHasAnyTranslucentMaterial(false)
	, MaxMeshPerInstance(Component->MaxMeshPerInstance)
	, MaxBatchCountPossible(0)
	, DynamicData(nullptr)
	, OldDynamicData(nullptr)
	, SpecialCustomDepthStencilValue(Component->SpecialCustomDepthStencilValue)
{

	NumBlendFramePerInstance = ALLEGRO_BLEND_FRAME_NUM_MAX;
	//{
	//	//if (MinLODIndex > MaxLODIndex)
	//	//	Swap(MinLODIndex, MaxLODIndex);
	//
	//	//uint8 LODCount = MaxLODIndex - MinLODIndex;
	//	this->ShadowLODBias = Component->ShadowLODBias;
	//	this->StartShadowLODBias = Component->StartShadowLODBias;
	//}
	
	TArray<UMaterialInterface*> CompMaterials = Component->GetMaterials();
	check(CompMaterials.Num() <= 0xFFff);
	MaterialsProxy.SetNum(CompMaterials.Num());
	
	for (int i = 0; i < CompMaterials.Num(); i++)
	{
		UMaterialInterface* Material = CompMaterials[i];
		//if(GForceDefaultMaterial) GForceDefaultMaterial is not exported
		//	Material = UMaterial::GetDefaultMaterial(MD_Surface);

		if (Material)
		{
			if (!Material->CheckMaterialUsage_Concurrent(MATUSAGE_SkeletalMesh))
			{
				UE_LOG(LogAllegro, Error, TEXT("Material %s with missing usage flag was applied to %s"), *Material->GetName(), *Component->GetName());
				Material = UMaterial::GetDefaultMaterial(MD_Surface);
			}
		}
		else
		{
			Material = UMaterial::GetDefaultMaterial(MD_Surface);
		}

		MaterialsProxy[i] = Material->GetRenderProxy();
	}

	this->bHasDeformableMesh = true;
	this->bGoodCandidateForCachedShadowmap = false;
	this->bVFRequiresPrimitiveUniformBuffer = true;

	for (int i = 0; i < ALLEGRO_MAX_LOD - 1; i++)
		this->LODDistances[i] = Component->LODDistances[i];

	//#TODO should we detect and set them ?
	//this->bEvaluateWorldPositionOffset = true;
	//this->bAnyMaterialHasWorldPositionOffset = true;

	check(Component->Submeshes.Num() > 0 && Component->Submeshes.Num() < 0xFF && this->MaxMeshPerInstance > 0);
	int SKMNum = 0;
	int SMNum = 0;

	for (int i = 0; i < Component->Submeshes.Num(); ++i)
	{
		if (Component->Submeshes[i].SkeletalMesh)
		{
			++SKMNum;
		}
		else if (Component->Submeshes[i].StaticMesh)
		{
			++SMNum;
		}
	}
	
	int MaterialCounter = 0;
	int SectionCounter = 0;

	FBoxMinMaxFloat MaxBound(ForceInit);

	//两种不同时使用
	if (SKMNum > 0)
	{
		this->bAlwaysHasVelocity = true;
		this->SubMeshes.SetNum(SKMNum);

		TArray<FBoxCenterExtentFloat> Extents;
		TArray<float> Factors;
		Extents.Reset(SKMNum);

		//initialize sub meshes. the rest are initialized in CreateRenderThreadResources.
		for (int MeshIdx = 0; MeshIdx < Component->Submeshes.Num(); MeshIdx++)
		{
			const FAllegroSubmeshSlot& CompMeshSlot = Component->Submeshes[MeshIdx];
			auto& Extent = Extents.AddDefaulted_GetRef();

			if (!CompMeshSlot.SkeletalMesh)
				continue;

			int MeshDefIdx = AminCollection->FindMeshDef(CompMeshSlot.SkeletalMesh);
			if (MeshDefIdx == -1)
				continue;

			FProxyMeshData& MD = this->SubMeshes[MeshIdx];
			MD.SkeletalMesh = CompMeshSlot.SkeletalMesh;
			MD.SkeletalRenderData = CompMeshSlot.SkeletalMesh->GetResourceForRendering();
			MD.PreSkinPostionOffset = CompMeshSlot.PreSkinPostionOffset;

			if (CompMeshSlot.AdditionalStaticMesh)
			{
				MD.AdditionalStaticRenderData = CompMeshSlot.AdditionalStaticMesh->GetRenderData();
			}

			check(MD.SkeletalRenderData);
			MD.MeshDefIndex = MeshDefIdx;
			MD.MeshDataEx = AminCollection->Meshes[MeshDefIdx].MeshData;
			check(MD.MeshDataEx);
			MD.MeshDefBaseLOD = AminCollection->Meshes[MeshDefIdx].BaseLOD;
			MD.BaseMaterialIndex = MaterialCounter;

			MD.MaxDrawDistance = CompMeshSlot.MaxDrawDistance;

			if (CompMeshSlot.OverrideDistance > 0)
			{
				int OverrideIdx = Component->FindSubmeshIndex(CompMeshSlot.OverrideSubmeshName);
				if (OverrideIdx != -1)
				{
					MD.OverrideDistance = CompMeshSlot.OverrideDistance;
					MD.OverrideMeshIndex = (uint8)OverrideIdx;
				}
			}

			check(MD.SkeletalRenderData->LODRenderData.Num() > 0);
			MD.MinLODIndex = FMath::Clamp(CompMeshSlot.MinLODIndex, MD.MeshDefBaseLOD, static_cast<uint8>(MD.SkeletalRenderData->LODRenderData.Num() - 1));

			const int LODCount = MD.SkeletalRenderData->LODRenderData.Num() - (int)MD.MeshDefBaseLOD;
			MaxBatchCountPossible += LODCount;
			MaterialCounter += MD.SkeletalMesh->GetNumMaterials();

			for (int LODIndex = 0; LODIndex < MD.SkeletalRenderData->LODRenderData.Num(); LODIndex++)
			{
				const FSkeletalMeshLODRenderData& SKMeshLOD = MD.SkeletalRenderData->LODRenderData[LODIndex];
				SectionCounter += SKMeshLOD.RenderSections.Num();
			}

			TArray<FSkeletalMeshLODInfo>& LodInfos = MD.SkeletalMesh->GetLODInfoArray();
			MD.LodNum = LodInfos.Num();
			for (int LodIdx = 0; LodIdx < LodInfos.Num(); ++LodIdx)
			{
				MD.LODScreenSize[LodIdx] = LodInfos[LodIdx].ScreenSize.GetDefault();
				MD.LODHysteresis[LodIdx] = LodInfos[LodIdx].LODHysteresis;
			}

			const TConstArrayView<FBoxCenterExtentFloat> MeshBounds = AminCollection->GetMeshBounds(MeshDefIdx);
			//0 is default pos
			Extent = MeshBounds[0];

			MaxBound.Add(Extent);
		}

#if ALLEGRO_LOD_PRE_SUBMESH_FACTOR
		CalcBoundExtentFactor(Extents, Factors);
		for (int MeshIdx = 0; MeshIdx < Component->Submeshes.Num(); MeshIdx++)
		{
			FProxyMeshData& MD = this->SubMeshes[MeshIdx];
			MD.ExtentFactor = Factors[MeshIdx];
		}
#endif
	}
	else if (SMNum > 0)
	{
		if (Component->IsAttachment)
			this->bAlwaysHasVelocity = true;
		else
			this->bAlwaysHasVelocity = false;

		this->SubStaticMeshes.SetNum(SMNum);

		TArray<FBoxCenterExtentFloat> Extents;
		TArray<float> Factors;
		Extents.Reset(SMNum);

		for (int MeshIdx = 0; MeshIdx < Component->Submeshes.Num(); MeshIdx++)
		{
			const FAllegroSubmeshSlot& CompMeshSlot = Component->Submeshes[MeshIdx];

			auto& Extent = Extents.AddDefaulted_GetRef();

			if (!CompMeshSlot.StaticMesh)
				continue;

			FProxyStaticMeshData& MD = this->SubStaticMeshes[MeshIdx];
			MD.StaticMesh = CompMeshSlot.StaticMesh;
			MD.StaticMeshData = CompMeshSlot.StaticMesh->GetRenderData();
			MD.PreSkinPostionOffset = CompMeshSlot.PreSkinPostionOffset;

			if (CompMeshSlot.AdditionalStaticMesh)
			{
				MD.AdditionalStaticRenderData = CompMeshSlot.AdditionalStaticMesh->GetRenderData();
			}

			MD.MeshDefIndex = 0;
			MD.MeshDefBaseLOD = MD.StaticMesh->GetMinLOD().GetDefault();
			MD.BaseMaterialIndex = MaterialCounter;

			MD.MaxDrawDistance = CompMeshSlot.MaxDrawDistance;

			if (CompMeshSlot.OverrideDistance > 0)
			{
				int OverrideIdx = Component->FindSubmeshIndex(CompMeshSlot.OverrideSubmeshName);
				if (OverrideIdx != -1)
				{
					MD.OverrideDistance = CompMeshSlot.OverrideDistance;
					MD.OverrideMeshIndex = (uint8)OverrideIdx;
				}
			}

			MD.MinLODIndex = FMath::Clamp(CompMeshSlot.MinLODIndex, MD.MeshDefBaseLOD, static_cast<uint8>(MD.StaticMeshData->LODResources.Num() - 1));

			const int LODCount = MD.StaticMeshData->LODResources.Num() - (int)MD.MeshDefBaseLOD;
			MaxBatchCountPossible += LODCount;
			MaterialCounter += MD.StaticMesh->GetStaticMaterials().Num();

			for (int LODIndex = 0; LODIndex < MD.StaticMeshData->LODResources.Num(); LODIndex++)
			{
				const FStaticMeshLODResources& MeshLod = MD.StaticMeshData->LODResources[LODIndex];
				SectionCounter += MeshLod.Sections.Num();
			}

			MD.LodNum = MAX_STATIC_MESH_LODS;
			for (int LodIdx = 0; LodIdx < MD.LodNum; ++LodIdx)
			{
				MD.LODScreenSize[LodIdx] = MD.StaticMeshData->ScreenSize[LodIdx].GetDefault();
				MD.LODHysteresis[LodIdx] = 0.0f;
			}

			FBox BoundingBox = CompMeshSlot.StaticMesh->GetBoundingBox();
			FVector C, E;
			BoundingBox.GetCenterAndExtents(C, E);
			Extent = FBoxCenterExtentFloat(FVector3f(C), FVector3f(E));

			MaxBound.Add(Extent);
		}

#if ALLEGRO_LOD_PRE_SUBMESH_FACTOR
		CalcBoundExtentFactor(Extents, Factors);
		for (int MeshIdx = 0; MeshIdx < Component->Submeshes.Num(); MeshIdx++)
		{
			FProxyStaticMeshData& MD = this->SubStaticMeshes[MeshIdx];
			MD.ExtentFactor = Factors[MeshIdx];
		}
#endif
	}

	this->MaterialIndicesArray.SetNumZeroed(SectionCounter);
	this->FixBound = MaxBound;

}

FAllegroProxy::~FAllegroProxy()
{
	if (DynamicData)
	{
		delete DynamicData;
		DynamicData = nullptr;
	}

	if (OldDynamicData)
	{
		delete OldDynamicData;
		OldDynamicData = nullptr;
	}
}


void FAllegroProxy::OnTransformChanged()
{
	if (!bHasPerInstanceLocalBounds)
	{
		check(InstanceLocalBounds.Num() <= 1);
		InstanceLocalBounds.SetNumUninitialized(1);
		
		SetInstanceLocalBounds(0, FRenderBounds(this->FixBound.GetMin(), this->FixBound.GetMax()));
	}
}


bool FAllegroProxy::CanBeOccluded() const
{
	return !MaterialRelevance.bDisableDepthTest && !ShouldRenderCustomDepth();
}

void FAllegroProxy::ApplyWorldOffset(FVector InOffset)
{
	Super::ApplyWorldOffset(InOffset);

	FVector3f InOffset3f(InOffset); //#TODO precision loss ?
	if (DynamicData)
	{
		for (uint32 InstanceIndex = 0; InstanceIndex < DynamicData->InstanceCount; InstanceIndex++)
		{
			auto& M = DynamicData->Transforms[InstanceIndex];
			M.SetOrigin(M.GetOrigin() + InOffset3f);
		}
	}
	if (OldDynamicData)
	{
		for (uint32 InstanceIndex = 0; InstanceIndex < OldDynamicData->InstanceCount; InstanceIndex++)
		{
			auto& M = OldDynamicData->Transforms[InstanceIndex];
			M.SetOrigin(M.GetOrigin() + InOffset3f);
		}
	}
}

void FAllegroProxy::GetLightRelevance(const FLightSceneProxy* LightSceneProxy, bool& bDynamic, bool& bRelevant, bool& bLightMapped, bool& bShadowMapped) const
{
	bDynamic = true;
	bRelevant = true;
	bLightMapped = false;
	bShadowMapped = true;
}

void FAllegroProxy::SetHasStencil(bool HasStencil)
{
#if ALLEGRO_USE_STENCIL
	this->bHasStencil = HasStencil;

	if (this->bHasStencil)
	{
		//注意，需要修改引擎源码，导出SetCustomDepthStencilValue_RenderThread函数,否则link不过
		//this->SetCustomDepthStencilValue_RenderThread(SpecialCustomDepthStencilValue);
	}
	else
	{
		//this->SetCustomDepthStencilValue_RenderThread(0);
	}
#endif
}

FPrimitiveViewRelevance FAllegroProxy::GetViewRelevance(const FSceneView* View) const
{
	bool bHasData = DynamicData && DynamicData->AliveInstanceCount > 0;
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = bHasData && IsShown(View) && View->Family->EngineShowFlags.SkeletalMeshes != 0;
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bRenderInDepthPass = ShouldRenderInDepthPass();
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = (this->bHasStencil && ShouldRenderCustomDepth());
	Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque == 1 && Result.bRenderInMainPass;

	return Result;
}

void FAllegroProxy::SetDynamicDataRT(FAllegroDynamicData* pData)
{
	check(IsInRenderingThread());
	if (OldDynamicData)
	{
		delete OldDynamicData;
	}

	OldDynamicData = DynamicData;
	DynamicData = pData;

	pData->CreationNumber = GFrameNumberRenderThread;

	//if(OldDynamicData)
	//{
	//	for (uint32 i = OldDynamicData->InstanceCount; i < DynamicData->InstanceCount; i++)
	//	{
	//		DynamicData->Flags[i] |= EAllegroInstanceFlags::EIF_New;
	//	}
	//}

#if ALLEGRO_USE_GPU_SCENE
	bSupportsInstanceDataBuffer = true;
	InstanceSceneData.SetNumZeroed(DynamicData->InstanceCount);

	for (uint32 Idx = 0; Idx < DynamicData->InstanceCount; ++Idx)
	{
		FInstanceSceneData& SceneData = InstanceSceneData[Idx];
		SceneData.LocalToPrimitive = DynamicData->Transforms[Idx];
	}
#endif
}

SIZE_T FAllegroProxy::GetTypeHash() const
{
	static SIZE_T UniquePointer;
	return reinterpret_cast<SIZE_T>(&UniquePointer);
}

void FAllegroProxy::CreateRenderThreadResources()
{
	uint16* MatIndexIter = this->MaterialIndicesArray.GetData();
	this->bHasAnyTranslucentMaterial = false;

	for (int MeshIdx = 0; MeshIdx < SubMeshes.Num(); MeshIdx++)
	{
		FProxyMeshData& MD = SubMeshes[MeshIdx];

		if (MD.SkeletalRenderData)
		{
			check(MD.SkeletalRenderData->LODRenderData.Num() <= ALLEGRO_MAX_LOD);

			int MiniLod = MD.SkeletalMesh->GetMinLod().GetDefault();
			//initialize Proxy LODData
			for (int LODIndex = 0; LODIndex < MD.SkeletalRenderData->LODRenderData.Num(); LODIndex++)
			{
				if (LODIndex < MiniLod)
				{
					continue;
				}

				const FSkeletalMeshLODRenderData& SkelLODData = MD.SkeletalRenderData->LODRenderData[LODIndex];
				check(SkelLODData.RenderSections.Num());

		
				FProxyLODData& ProxyLODData = MD.LODs[LODIndex];

				ProxyLODData.bAnySectionCastShadow = false;
				ProxyLODData.bAllSectionsCastShadow = true;
				ProxyLODData.bSameCastShadow = true;
				ProxyLODData.bMeshUnificationApplicable = true;
				ProxyLODData.bSameMaterials = true;
				ProxyLODData.bHasAnyTranslucentMaterial = false;
				ProxyLODData.bSameMaxBoneInfluence = true;
				ProxyLODData.SectionsMaxBoneInfluence = 0;
				ProxyLODData.SectionsNumTriangle = 0;
				ProxyLODData.SectionsNumVertices = 0;


				const FSkeletalMeshLODInfo* SKMeshLODInfo = MD.SkeletalMesh->GetLODInfo(LODIndex);

				ProxyLODData.SectionsMaterialIndices = MatIndexIter;
				MatIndexIter += SkelLODData.RenderSections.Num();
				check(MatIndexIter <= (this->MaterialIndicesArray.GetData() + this->MaterialIndicesArray.Num()));

				for (int SectionIndex = 0; SectionIndex < SkelLODData.RenderSections.Num(); SectionIndex++)
				{
					const FSkelMeshRenderSection& SectionInfo = SkelLODData.RenderSections[SectionIndex];

					int SolvedMI = MD.BaseMaterialIndex + SectionInfo.MaterialIndex;
					if (SKMeshLODInfo && SKMeshLODInfo->LODMaterialMap.IsValidIndex(SectionIndex) && SKMeshLODInfo->LODMaterialMap[SectionIndex] != INDEX_NONE)
					{
						SolvedMI = MD.BaseMaterialIndex + SKMeshLODInfo->LODMaterialMap[SectionIndex];
					}
					ProxyLODData.SectionsMaterialIndices[SectionIndex] = SolvedMI;

					const FMaterialRenderProxy* SectionMaterialProxy = this->MaterialsProxy[SolvedMI];

					checkf(IsInRenderingThread(), TEXT("GetIncompleteMaterialWithFallback must be called from render thread"));
					const FMaterial& Material = SectionMaterialProxy->GetIncompleteMaterialWithFallback(this->GetScene().GetFeatureLevel());

					check(SectionMaterialProxy);
					ProxyLODData.bAnySectionCastShadow |= SectionInfo.bCastShadow;
					ProxyLODData.bAllSectionsCastShadow &= SectionInfo.bCastShadow;
					ProxyLODData.bSameCastShadow &= SectionInfo.bCastShadow == SkelLODData.RenderSections[0].bCastShadow;
					ProxyLODData.bHasAnyTranslucentMaterial |= IsTranslucentBlendMode(Material.GetBlendMode());
					MD.bHasAnyTranslucentMaterial |= ProxyLODData.bHasAnyTranslucentMaterial;

					const FMaterialRenderProxy* Section0Material = this->MaterialsProxy[ProxyLODData.SectionsMaterialIndices[0]];
					const bool bSameMaterial = SectionMaterialProxy == Section0Material;
					ProxyLODData.bSameMaterials &= bSameMaterial;
					const bool bUseUnifiedMesh = Material.WritesEveryPixel() && !Material.IsTwoSided() && !IsTranslucentBlendMode(Material.GetBlendMode()) && !Material.MaterialModifiesMeshPosition_RenderThread();
					ProxyLODData.bMeshUnificationApplicable &= bUseUnifiedMesh;
					ProxyLODData.bSameMaxBoneInfluence &= (SectionInfo.MaxBoneInfluences == SkelLODData.RenderSections[0].MaxBoneInfluences);

					ProxyLODData.SectionsMaxBoneInfluence = FMath::Max(ProxyLODData.SectionsMaxBoneInfluence, SectionInfo.MaxBoneInfluences);
					ProxyLODData.SectionsNumTriangle += SectionInfo.NumTriangles;
					ProxyLODData.SectionsNumVertices += SectionInfo.NumVertices;
				}
			}
		}

		this->bHasAnyTranslucentMaterial |= MD.bHasAnyTranslucentMaterial;
	}


	MatIndexIter = this->MaterialIndicesArray.GetData();
	for (int MeshIdx = 0; MeshIdx < SubStaticMeshes.Num(); MeshIdx++)
	{
		FProxyStaticMeshData& MD = SubStaticMeshes[MeshIdx];

		if (MD.StaticMesh)
		{
			check(MD.StaticMeshData->LODResources.Num() <= ALLEGRO_MAX_LOD);

			int MiniLod = MD.StaticMesh->GetMinLOD().GetDefault();

			//initialize Proxy LODData
			for (int LODIndex = 0; LODIndex < MD.StaticMeshData->LODResources.Num(); LODIndex++)
			{
				if (LODIndex < MiniLod)
				{
					continue;
				}

				const FStaticMeshLODResources& LODData = MD.StaticMeshData->LODResources[LODIndex];
				check(LODData.Sections.Num());

				FProxyLODData& ProxyLODData = MD.LODs[LODIndex];

				ProxyLODData.bAnySectionCastShadow = false;
				ProxyLODData.bAllSectionsCastShadow = true;
				ProxyLODData.bSameCastShadow = true;
				ProxyLODData.bMeshUnificationApplicable = true;
				ProxyLODData.bSameMaterials = true;
				ProxyLODData.bHasAnyTranslucentMaterial = false;
				ProxyLODData.bSameMaxBoneInfluence = true;
				ProxyLODData.SectionsMaxBoneInfluence = 0;
				ProxyLODData.SectionsNumTriangle = 0;
				ProxyLODData.SectionsNumVertices = 0;

				ProxyLODData.SectionsMaterialIndices = MatIndexIter;
				MatIndexIter += LODData.Sections.Num();
				check(MatIndexIter <= (this->MaterialIndicesArray.GetData() + this->MaterialIndicesArray.Num()));

				for (int SectionIndex = 0; SectionIndex < LODData.Sections.Num(); SectionIndex++)
				{
					const FStaticMeshSection& SectionInfo = LODData.Sections[SectionIndex];

					int SolvedMI = MD.BaseMaterialIndex + SectionInfo.MaterialIndex;
				
					ProxyLODData.SectionsMaterialIndices[SectionIndex] = SolvedMI;

					const FMaterialRenderProxy* SectionMaterialProxy = this->MaterialsProxy[SolvedMI];

					checkf(IsInRenderingThread(), TEXT("GetIncompleteMaterialWithFallback must be called from render thread"));
					const FMaterial& Material = SectionMaterialProxy->GetIncompleteMaterialWithFallback(this->GetScene().GetFeatureLevel());

					check(SectionMaterialProxy);
					ProxyLODData.bAnySectionCastShadow |= SectionInfo.bCastShadow;
					ProxyLODData.bAllSectionsCastShadow &= SectionInfo.bCastShadow;
					ProxyLODData.bSameCastShadow &= SectionInfo.bCastShadow == SectionInfo.bCastShadow;
					ProxyLODData.bHasAnyTranslucentMaterial |= IsTranslucentBlendMode(Material.GetBlendMode());
					MD.bHasAnyTranslucentMaterial |= ProxyLODData.bHasAnyTranslucentMaterial;

					const FMaterialRenderProxy* Section0Material = this->MaterialsProxy[ProxyLODData.SectionsMaterialIndices[0]];
					const bool bSameMaterial = SectionMaterialProxy == Section0Material;
					ProxyLODData.bSameMaterials &= bSameMaterial;
					const bool bUseUnifiedMesh = Material.WritesEveryPixel() && !Material.IsTwoSided() && !IsTranslucentBlendMode(Material.GetBlendMode()) && !Material.MaterialModifiesMeshPosition_RenderThread();
					ProxyLODData.bMeshUnificationApplicable &= bUseUnifiedMesh;
					ProxyLODData.bSameMaxBoneInfluence &= true;

					ProxyLODData.SectionsMaxBoneInfluence = 0;
					ProxyLODData.SectionsNumTriangle += SectionInfo.NumTriangles;
					ProxyLODData.SectionsNumVertices += SectionInfo.MaxVertexIndex;
				}
			}
		}

		this->bHasAnyTranslucentMaterial |= MD.bHasAnyTranslucentMaterial;
	}
}

void FAllegroProxy::DestroyRenderThreadResources()
{
	for (int MeshIdx = 0; MeshIdx < SubMeshes.Num(); MeshIdx++)
	{
		FProxyMeshData& MD = SubMeshes[MeshIdx];

		for (int LodIdx = 0; LodIdx < ALLEGRO_MAX_LOD; ++LodIdx)
		{
			for (TUniquePtr<FAllegroBaseVertexFactory>& VF : MD.LODs[LodIdx].VertexFactories)
			{
				if (VF)
				{
					VF->ReleaseResource();
					VF = nullptr;
				}
			}
		}
	}

	for (int MeshIdx = 0; MeshIdx < SubStaticMeshes.Num(); MeshIdx++)
	{
		FProxyStaticMeshData& MD = SubStaticMeshes[MeshIdx];

		for (int LodIdx = 0; LodIdx < ALLEGRO_MAX_LOD; ++LodIdx)
		{
			for (TUniquePtr<FAllegroBaseVertexFactory>& VF : MD.LODs[LodIdx].VertexFactories)
			{
				if (VF)
				{
					VF->ReleaseResource();
					VF = nullptr;
				}
			}
		}
	}
}

FAllegroBaseVertexFactory* FAllegroProxy::GetVertexFactory(int SubMeshIndex, int LodIndex, const FAllegroBoneIndexVertexBuffer* BoneIndexBuffer, const FSkeletalMeshLODRenderData* LODData, int MaxBoneInfluence, FStaticMeshVertexBuffers* AdditionalStaticMeshVB)
{
	check(MaxBoneInfluence > 0 && MaxBoneInfluence <= FAllegroMeshDataEx::MAX_INFLUENCE);
	check(SubMeshIndex < this->SubMeshes.Num() && LodIndex < ALLEGRO_MAX_LOD);

	FProxyLODData& LOD = this->SubMeshes[SubMeshIndex].LODs[LodIndex];

	if (LOD.VertexFactories[MaxBoneInfluence])
	{
		return LOD.VertexFactories[MaxBoneInfluence].Get();
	}

	{
		check(IsInRenderingThread() && LODData);

		FAllegroBaseVertexFactory* VF = FAllegroBaseVertexFactory::New(MaxBoneInfluence, true);
		FAllegroBaseVertexFactory::FDataType VFData;
		VF->FillData(VFData, BoneIndexBuffer, LODData, AdditionalStaticMeshVB);
		VF->SetData(VFData);
		VF->InitResource(FRHICommandListImmediate::Get());

		LOD.VertexFactories[MaxBoneInfluence] = TUniquePtr<FAllegroBaseVertexFactory>(VF);
	}
	return LOD.VertexFactories[MaxBoneInfluence].Get();

}

FAllegroBaseVertexFactory* FAllegroProxy::GetStaticVertexFactory(int SubMeshIndex, int LodIndex, const FStaticMeshLODResources* LODData, FStaticMeshVertexBuffers* AdditionalStaticMeshVB)
{
	check(SubMeshIndex < this->SubStaticMeshes.Num() && LodIndex < ALLEGRO_MAX_LOD);
	FProxyLODData& LOD = this->SubStaticMeshes[SubMeshIndex].LODs[LodIndex];
	int MaxBoneInfluence = 0;

	if (LOD.VertexFactories[MaxBoneInfluence])
	{
		return LOD.VertexFactories[MaxBoneInfluence].Get();
	}

	{
		check(IsInRenderingThread() && LODData);

		FAllegroBaseVertexFactory* VF = FAllegroBaseVertexFactory::New(MaxBoneInfluence, AdditionalStaticMeshVB?true:false);
		FAllegroBaseVertexFactory::FDataType VFData;
		VF->FillDataForStaticMesh(VFData, LODData, AdditionalStaticMeshVB);
		VF->SetData(VFData);
		VF->InitResource(FRHICommandListImmediate::Get());

		LOD.VertexFactories[MaxBoneInfluence] = TUniquePtr<FAllegroBaseVertexFactory>(VF);
	}
	return LOD.VertexFactories[MaxBoneInfluence].Get();
}

uint32 FAllegroProxy::GetMemoryFootprint(void) const
{
	return (sizeof(*this) + GetAllocatedSize());
}

uint32 FAllegroProxy::GetAllocatedSize(void) const
{
	return FPrimitiveSceneProxy::GetAllocatedSize() + this->SubMeshes.GetAllocatedSize() + this->MaterialsProxy.GetAllocatedSize() + this->MaterialIndicesArray.GetAllocatedSize();
}

void FAllegroProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	ALLEGRO_SCOPE_CYCLE_COUNTER(GetDynamicMeshElements);

	if (!DynamicData || DynamicData->AliveInstanceCount == 0)
	{
		return;
	}

	//SKM 与 SM 只能二选一使用
	const bool bHasData = SubMeshes.Num() > 0;
	const bool bStaticMesh = SubStaticMeshes.Num() > 0;
	if (!bHasData && !bStaticMesh)
	{
		return;
	}

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			const FSceneView* View = Views[ViewIndex];

			if (View->GetDynamicMeshElementsShadowCullFrustum())
			{
				check(Views.Num() == 1);
				if (bHasData)
				{
					FAllegroMultiMeshGenerator<true> generator(this, &ViewFamily, View, &Collector, ViewIndex, this->SubMeshes.Num());
					generator.DoGenerate();
				}
				else if (bStaticMesh)
				{
					FAllegroStaticMultiMeshGenerator<true> generator(this, &ViewFamily, View, &Collector, ViewIndex, this->SubStaticMeshes.Num());
					generator.DoGenerate();
				}
			}
			else
			{
				bool bIgnoreView = (View->bIsInstancedStereoEnabled && View->StereoPass == EStereoscopicPass::eSSP_SECONDARY);
				if (!bIgnoreView)
				{
					if (bHasData)
					{
						FAllegroMultiMeshGenerator<false> generator(this, &ViewFamily, View, &Collector, ViewIndex, this->SubMeshes.Num());
						generator.DoGenerate();
					}
					else if (bStaticMesh)
					{
						FAllegroStaticMultiMeshGenerator<false> generator(this, &ViewFamily, View, &Collector, ViewIndex, this->SubStaticMeshes.Num());
						generator.DoGenerate();
					}
				}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
				// Render bounds
				RenderBounds(Collector.GetPDI(ViewIndex), ViewFamily.EngineShowFlags, GetBounds(), IsSelected());
#endif
			}
		}
	}
}
#if 0
void FProxyMeshData::Init(FAllegroProxy* Owner)
{
	this->bHasAnyTranslucentMaterial = false;

	if(this->SkeletalRenderData)
	{
		check(this->SkeletalRenderData->LODRenderData.Num() <= Allegro_MAX_LOD);
		//initialize Proxy LODData
		for (int LODIndex = 0; LODIndex < this->SkeletalRenderData->LODRenderData.Num(); LODIndex++)
		{
			const FSkeletalMeshLODRenderData& SkelLODData = this->SkeletalRenderData->LODRenderData[LODIndex];
			check(SkelLODData.RenderSections.Num());

			FProxyLODData& ProxyLODData = this->LODs[LODIndex];

			ProxyLODData.bAnySectionCastShadow = false;
			ProxyLODData.bAllSectionsCastShadow = true;
			ProxyLODData.bUseUnifiedMesh = true;
			ProxyLODData.bAllMaterialsSame = true;
			ProxyLODData.SectionsMaxBoneInfluence = 0;
			ProxyLODData.SectionsNumTriangle = 0;
			ProxyLODData.SectionsNumVertices = 0;
			ProxyLODData.bHasAnyTranslucentMaterial = false;
			ProxyLODData.bSameMaxBoneInfluence = true;

			const FSkeletalMeshLODInfo* SKMeshLODInfo = this->SkeletalMesh->GetLODInfo(LODIndex);

			for (int SectionIndex = 0; SectionIndex < SkelLODData.RenderSections.Num(); SectionIndex++)
			{
				const FSkelMeshRenderSection& SectionInfo = SkelLODData.RenderSections[SectionIndex];
				const FMaterialRenderProxy* SectionMaterialProxy = Owner->MaterialsProxy[this->BaseMaterialIndex + SectionInfo.MaterialIndex];
				const FMaterial& Material = SectionMaterialProxy->GetIncompleteMaterialWithFallback(Owner->GetScene().GetFeatureLevel());

				check(SectionMaterialProxy);
				ProxyLODData.bAnySectionCastShadow |= SectionInfo.bCastShadow;
				ProxyLODData.bAllSectionsCastShadow &= SectionInfo.bCastShadow;
				ProxyLODData.SectionsMaxBoneInfluence = FMath::Max(ProxyLODData.SectionsMaxBoneInfluence, SectionInfo.MaxBoneInfluences);
				ProxyLODData.SectionsNumTriangle += SectionInfo.NumTriangles;
				ProxyLODData.SectionsNumVertices += SectionInfo.NumVertices;
				ProxyLODData.bHasAnyTranslucentMaterial |= IsTranslucentBlendMode(Material.GetBlendMode());
				this->bHasAnyTranslucentMaterial |= ProxyLODData.bHasAnyTranslucentMaterial;

				if (SectionIndex != 0)
				{
					bool bSameMaterial = SectionMaterialProxy == Owner->MaterialsProxy[this->BaseMaterialIndex + SkelLODData.RenderSections[0].MaterialIndex];
					ProxyLODData.bAllMaterialsSame &= bSameMaterial;
					bool bUseUnifiedMesh = Material.WritesEveryPixel() && !Material.IsTwoSided() && !IsTranslucentBlendMode(Material.GetBlendMode()) && !Material.MaterialModifiesMeshPosition_RenderThread();
					ProxyLODData.bUseUnifiedMesh &= bUseUnifiedMesh;
					ProxyLODData.bSameMaxBoneInfluence &= SectionInfo.MaxBoneInfluences == SkelLODData.RenderSections[0].MaxBoneInfluences;
				}

				
				int SolvedMI = SectionInfo.MaterialIndex;
				if (SKMeshLODInfo && SKMeshLODInfo->LODMaterialMap.IsValidIndex(SectionIndex) && SKMeshLODInfo->LODMaterialMap[SectionIndex] != INDEX_NONE)
				{
					SolvedMI = SKMeshLODInfo->LODMaterialMap[SectionIndex];
				}

				ProxyLODData.SectionsMaterialIndices[SectionIndex] = this->BaseMaterialIndex + SolvedMI;
			}
		}
	}

	Owner->bHasAnyTranslucentMaterial |= bHasAnyTranslucentMaterial;

	
}
#endif

void FAllegroDynamicData::InitGrid()
{
	FVector3f InCompBoundSize;
	CompBound.GetSize(InCompBoundSize);

	AllegroCalcGridSize(InCompBoundSize.X, InCompBoundSize.Y, this->NumCells, GridSize.X, GridSize.Y);

	GridCellSize.X = (InCompBoundSize.X / GridSize.X);
	GridCellSize.Y = (InCompBoundSize.Y / GridSize.Y);
	GridInvCellSize = FVector2f(1.0f) / GridCellSize;

	BoundMin = FVector2f(CompBound.GetMin());
}

FAllegroDynamicData* FAllegroDynamicData::Allocate(UAllegroComponent* Comp)
{
	ALLEGRO_SCOPE_CYCLE_COUNTER(FAllegroDynamicData_Allocate);

	uint32 InstanceCount = Comp->GetInstanceCount();
	uint32 MaxNumCell = Comp->GetAliveInstanceCount() / GAllegro_NumInstancePerGridCell;
	MaxNumCell = MaxNumCell >= 6 ? MaxNumCell : 0;	//zero if we don't need grid
	if(GAllegro_DisableGridCull)
		MaxNumCell = 0;

	const size_t MemSizeTransforms = sizeof(*Transforms) * InstanceCount;
	const size_t MemSizeBounds = sizeof(*Bounds) * InstanceCount;
	const size_t MemSizeFrameIndices = sizeof(*FrameIndices) * InstanceCount;
	const size_t MemSizeFlags = sizeof(EAllegroInstanceFlags) * InstanceCount;
	const size_t MemSizeCustomData = Comp->NumCustomDataFloats > 0 ? (Comp->NumCustomDataFloats * sizeof(float) * InstanceCount) : 0;
	const size_t MemSizeMeshSlots = (Comp->MaxMeshPerInstance + 1) * sizeof(uint8) * InstanceCount;

	const size_t MemSizeStencilData = sizeof(int16) * InstanceCount;

	const size_t MemSizeBlendAnimInfoIndex = sizeof(uint32) * InstanceCount;
	const size_t InstanceBlendFrameNum = Comp->InstancesData.BlendFrameInfo.Num();
	const size_t MemSizeBlendAnimInfo = InstanceBlendFrameNum > 1 ? sizeof(FInstanceBlendFrameInfo) * InstanceBlendFrameNum : 0;

	size_t MemSizeCells = 0;
	uint32 MaxCellPageNeeded = 0;
	if (MaxNumCell > 0)
	{
		MemSizeCells = MaxNumCell * sizeof(FCell);
		MaxCellPageNeeded = FMath::DivideAndRoundUp((uint32)Comp->GetAliveInstanceCount(), FCell::MAX_INSTANCE_PER_CELL) + MaxNumCell;
	}

	const size_t MemSizeCellPages = MaxCellPageNeeded * sizeof(FCell::FCellPage);

	const size_t OverallSize = sizeof(FAllegroDynamicData) + MemSizeTransforms + MemSizeBounds + MemSizeFrameIndices + MemSizeFlags + MemSizeCustomData + MemSizeMeshSlots \
		+ MemSizeStencilData + MemSizeBlendAnimInfoIndex + MemSizeBlendAnimInfo \
		+ MemSizeCells + MemSizeCellPages + 256;

	uint8* MemBlock = (uint8*)FMemory::Malloc(OverallSize);
	FAllegroDynamicData* DynData = new (MemBlock) FAllegroDynamicData();
	uint8* DataIter = (uint8*)(DynData + 1);

	auto TakeMem = [&](size_t SizeInBytes, size_t InAlign = 4) {
		uint8* cur = Align(DataIter, InAlign);
		DataIter = cur + SizeInBytes;
		check(DataIter <= (MemBlock + OverallSize));
		return cur;
	};

	DynData->InstanceCount = InstanceCount;
	DynData->AliveInstanceCount = Comp->GetAliveInstanceCount();

	DynData->Flags = (EAllegroInstanceFlags*)TakeMem(MemSizeFlags);
	DynData->Bounds = (FBoxCenterExtentFloat*)TakeMem(MemSizeBounds);
	DynData->Transforms = (FMatrix44f*)TakeMem(MemSizeTransforms, 16);
	DynData->FrameIndices = (uint32*)TakeMem(MemSizeFrameIndices);
	DynData->CustomData = MemSizeCustomData ? (float*)TakeMem(MemSizeCustomData) : nullptr;
	DynData->MeshSlots = MemSizeMeshSlots ? (uint8*)TakeMem(MemSizeMeshSlots) : nullptr;

	DynData->Stencil = (int16*)TakeMem(MemSizeStencilData);
	DynData->NumBlendFrame = InstanceBlendFrameNum;
	DynData->BlendFrameInfoIndex = (uint32*)TakeMem(MemSizeBlendAnimInfoIndex);
	DynData->BlendFrameInfoData = InstanceBlendFrameNum > 1 ?(FInstanceBlendFrameInfo*)TakeMem(MemSizeBlendAnimInfo):nullptr;


	if (MaxNumCell > 0)	//use grid culling ?
	{
		DynData->NumCells = MaxNumCell;
		DynData->MaxCellPage = MaxCellPageNeeded;
		DynData->Cells = (FCell*)TakeMem(MemSizeCells, alignof(FCell));
		DynData->CellPagePool = (FCell::FCellPage*)TakeMem(MemSizeCellPages, PLATFORM_CACHE_LINE_SIZE);

		DefaultConstructItems<FCell>(DynData->Cells, MaxNumCell);
	}

	//copy data
	FMemory::Memcpy(DynData->Flags, Comp->InstancesData.Flags.GetData(), MemSizeFlags);
	FMemory::Memcpy(DynData->FrameIndices, Comp->InstancesData.FrameIndices.GetData(), MemSizeFrameIndices);
	FMemory::Memcpy(DynData->Transforms, Comp->InstancesData.Matrices.GetData(), MemSizeTransforms);
	if (MemSizeCustomData )
		FMemory::Memcpy(DynData->CustomData, Comp->InstancesData.RenderCustomData.GetData(), MemSizeCustomData);
	
	FMemory::Memcpy(DynData->MeshSlots, Comp->InstancesData.MeshSlots.GetData(), MemSizeMeshSlots);

	FMemory::Memcpy(DynData->Stencil, Comp->InstancesData.Stencil.GetData(), MemSizeStencilData);
	FMemory::Memcpy(DynData->BlendFrameInfoIndex, Comp->InstancesData.BlendFrameInfoIndex.GetData(), MemSizeBlendAnimInfoIndex);
	if(InstanceBlendFrameNum > 1)
		FMemory::Memcpy(DynData->BlendFrameInfoData, Comp->InstancesData.BlendFrameInfo.GetData(), MemSizeBlendAnimInfo);

	return DynData;
}

void FAllegroDynamicData::FCell::AddValue(FAllegroDynamicData& Owner, uint32 InValue)
{
	if (Counter == MAX_INSTANCE_PER_CELL)
	{
		Counter = 1;
		FCellPage* CellPage = new (Owner.CellPagePool + Owner.CellPageCounter) FCellPage();
		CellPage->NextPage = -1;
		CellPage->Indices[0] = InValue;

		if (PageTail != -1)
		{
			Owner.CellPagePool[PageTail].NextPage = Owner.CellPageCounter;
			PageTail = Owner.CellPageCounter;
		}
		else
		{
			PageHead = PageTail = Owner.CellPageCounter;
		}

		Owner.CellPageCounter++;
		check(Owner.CellPageCounter <= Owner.MaxCellPage);
	}
	else
	{
		Owner.CellPagePool[PageTail].Indices[this->Counter++] = InValue;
	}
}

uint8* FAllegroDynamicData::FCell::CopyToArray(const FAllegroDynamicData& Owner, uint8* DstMem) const
{
	int IterIdx = PageHead;
	while (IterIdx != -1)
	{
		const FCellPage* CellData = Owner.CellPagePool + IterIdx;
		const uint32 DataSize = (CellData->NextPage == -1 ? this->Counter : MAX_INSTANCE_PER_CELL) * sizeof(uint32);
		//#TODO can't be optimized by just copying it all ? 
		FMemory::Memcpy(DstMem, CellData->Indices, DataSize);
		DstMem += DataSize;
		IterIdx = CellData->NextPage;
	}

	return DstMem;
}
