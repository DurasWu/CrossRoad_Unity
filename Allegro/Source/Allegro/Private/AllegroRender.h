// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "PrimitiveSceneProxy.h"
#include "VertexFactory.h"
#include "Components.h"
#include "AllegroComponent.h"
#include "AllegroRenderResources.h"
#include "Containers/TripleBuffer.h"
#include "Containers/CircularQueue.h"

class FAllegroProxy;




struct FAllegroDynamicData
{
	struct alignas(16) FCell
	{
		static const uint32 MAX_INSTANCE_PER_CELL = 128 - 1;

		struct alignas(PLATFORM_CACHE_LINE_SIZE) FCellPage
		{
			int NextPage;
			uint32 Indices[MAX_INSTANCE_PER_CELL];
		};

		int PageTail, PageHead;
		uint32 Counter, Unused;
		FBoxMinMaxFloat Bound;

		FCell() : PageTail(-1), PageHead(-1), Counter(MAX_INSTANCE_PER_CELL), Unused(0), Bound(ForceInit)
		{}

		bool IsEmpty() const { return PageTail == -1; }

		void AddValue(FAllegroDynamicData& Owner, uint32 InValue);

		//
		uint8* CopyToArray(const FAllegroDynamicData& Owner, uint8* DstMem) const;

		template<typename TProc> void ForEachInstance(const FAllegroDynamicData& Owner, TProc&& Proc) const
		{
			int IterIdx = PageHead;
			while (IterIdx != -1)
			{
				const FCellPage* Page = Owner.CellPagePool + IterIdx;
				const uint32 NumElem = (Page->NextPage == -1 ? this->Counter : MAX_INSTANCE_PER_CELL);

				for (uint32 Idx = 0; Idx < NumElem; Idx++)
					Proc(Page->Indices[Idx]);

				IterIdx = Page->NextPage;
			}
		}
	};

	FBoxMinMaxFloat CompBound { ForceInit };

	EAllegroInstanceFlags* Flags = nullptr;
	FMatrix44f* Transforms = nullptr;
	FBoxCenterExtentFloat* Bounds = nullptr;
	uint32* FrameIndices = nullptr;

	uint32  NumBlendFrame = 0;
	uint32* BlendFrameInfoIndex = nullptr;
	FInstanceBlendFrameInfo* BlendFrameInfoData = nullptr;

	int16* Stencil = nullptr;
	
	float* CustomData = nullptr;
	uint8* MeshSlots = nullptr;

	uint32 InstanceCount = 0;
	uint32 AliveInstanceCount = 0;
	uint32 CreationNumber = 0;
	

	FCell::FCellPage* CellPagePool = nullptr;
	uint32 CellPageCounter = 0;
	uint32 MaxCellPage = 0;

	FCell* Cells = nullptr;
	uint32 NumCells = 0;


	FIntPoint GridSize = FIntPoint::NoneValue;	//number of cell in x y axis
	FVector2f BoundMin = FVector2f::ZeroVector;
	FVector2f GridCellSize = FVector2f::ZeroVector;
	FVector2f GridInvCellSize = FVector2f::ZeroVector;

	
	FCell& GetCellAtCoord(int X, int Y)
	{
		return Cells[CellCoordToCellIndex(X, Y)];
	}
	int LocationToCellIndex(const FVector3f& C) const
	{
		int CellX = static_cast<int>((C.X - BoundMin.X) * GridInvCellSize.X);
		int CellY = static_cast<int>((C.Y - BoundMin.Y) * GridInvCellSize.Y);
		check(IsCellCoordValid(CellX, CellY));
		return CellCoordToCellIndex(CellX, CellY);
	}
	int CellCoordToCellIndex(int X, int Y) const
	{
		return Y * GridSize.X + X;
	}
	bool IsCellCoordValid(int X, int Y) const
	{
		return X >= 0 && Y >= 0 && X < GridSize.X && Y < GridSize.Y;
	}

	void InitGrid();

	static FAllegroDynamicData* Allocate(UAllegroComponent* Comp);

	void operator delete(void* ptr) { return FMemory::Free(ptr); }


};


struct FProxyLODData
{
	uint8 bSameMaterials : 1;	//true if all sections are using same material (compared by pointer)
	uint8 bSameMaxBoneInfluence : 1;	//true if all sections have the same MBI
	uint8 bSameCastShadow : 1;	//true if all sections bCastShadow is identical
	uint8 bMeshUnificationApplicable : 1;
	uint8 bAllSectionsCastShadow : 1;
	uint8 bAnySectionCastShadow : 1;
	uint8 bHasAnyTranslucentMaterial : 1;	//true if any section of this LOD has trans material
	

	int SectionsMaxBoneInfluence = 0;
	int SectionsNumTriangle = 0;
	int SectionsNumVertices = 0;

	//as size of FSkeletalMeshLODRenderData.RenderSections.Num(), accessed by section index.
	//value is index for FAllegroProxy.MaterialsProxy
	//reason behind this is FSkelMeshRenderSection.MaterialIndex is not index for USkeletalMesh.Materials[] it needs to be remapped through USkeletalMesh.LODInfo[]
	uint16* SectionsMaterialIndices = nullptr;	

	TUniquePtr<FAllegroBaseVertexFactory> VertexFactories[FAllegroMeshDataEx::MAX_INFLUENCE + 1];
};


struct FProxyMeshDataBase
{
	FStaticMeshRenderData* AdditionalStaticRenderData = nullptr; //extern
	int BaseMaterialIndex = 0;
	float MaxDrawDistance = 0;
	float OverrideDistance = 0;
	bool bHasAnyTranslucentMaterial = false;
	bool PreSkinPostionOffset = false;  //extern

	uint8 OverrideMeshIndex = 0;
	uint8 MeshDefBaseLOD = 0;
	uint8 MeshDefIndex = 0;
	uint8 MinLODIndex = 0;

	FProxyLODData LODs[ALLEGRO_MAX_LOD];

	//extern
	uint8 LodNum;
	float LODScreenSize[ALLEGRO_MAX_LOD];
	float LODHysteresis[ALLEGRO_MAX_LOD];

	float ExtentFactor = 1.0f;
};

struct FProxyMeshData : public FProxyMeshDataBase
{
	USkeletalMesh* SkeletalMesh = nullptr; //not touched during rendering
	const FSkeletalMeshRenderData* SkeletalRenderData = nullptr;
	FAllegroMeshDataExPtr MeshDataEx;
};

struct FProxyStaticMeshData : public FProxyMeshDataBase
{
	UStaticMesh* StaticMesh = nullptr;
	const FStaticMeshRenderData* StaticMeshData = nullptr;
};

class FAllegroProxy : public FPrimitiveSceneProxy
{
public:
	typedef FPrimitiveSceneProxy Super;


	FMaterialRelevance MaterialRelevance;
	UAllegroAnimCollection* AminCollection;

	float InstanceMaxDrawDistance;
	float InstanceMinDrawDistance;
	float LODDistances[ALLEGRO_MAX_LOD - 1];

	float DistanceScale;
	int NumCustomDataFloats;
	int NumBlendFramePerInstance;

	//uint8 MinLODIndex;
	//uint8 MaxLODIndex;
	uint8 ShadowLODBias;
	uint8 StartShadowLODBias;
	bool bNeedCustomDataForShadowPass;
	bool bHasAnyTranslucentMaterial;	//true if we any of the LODS have any translucent section
	uint8 MaxMeshPerInstance;
	uint32 MaxBatchCountPossible;
	FAllegroDynamicData* DynamicData;
	FAllegroDynamicData* OldDynamicData;
	TArray<FProxyMeshData> SubMeshes;
	TArray<FMaterialRenderProxy*> MaterialsProxy;
	TArray<uint16> MaterialIndicesArray;

	TArray<FProxyStaticMeshData>  SubStaticMeshes;

	FBoxMinMaxFloat FixBound;


	FAllegroProxy(const UAllegroComponent* Component, FName ResourceName);
	~FAllegroProxy();


	void OnTransformChanged() override;

	bool CanBeOccluded() const override;
	void ApplyWorldOffset(FVector InOffset) override;
	const TArray<FBoxSphereBounds>* GetOcclusionQueries(const FSceneView* View) const override { return nullptr; }
	void AcceptOcclusionResults(const FSceneView* View, TArray<bool>* Results, int32 ResultsStart, int32 NumResults) override { }
	bool HasSubprimitiveOcclusionQueries() const override { return false; }
	void GetLightRelevance(const FLightSceneProxy* LightSceneProxy, bool& bDynamic, bool& bRelevant, bool& bLightMapped, bool& bShadowMapped) const override;
	FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	SIZE_T GetTypeHash() const override;
	void CreateRenderThreadResources() override;
	void DestroyRenderThreadResources() override;
	uint32 GetMemoryFootprint(void) const override;
	uint32 GetAllocatedSize(void) const;
	void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

	void SetDynamicDataRT(FAllegroDynamicData* pData);

	bool IsMultiMesh() const { return SubMeshes.Num() > 1 && MaxMeshPerInstance > 0; }


	FAllegroBaseVertexFactory* GetVertexFactory(int SubMeshIndex, int LodIndex, const FAllegroBoneIndexVertexBuffer* BoneIndexBuffer, const FSkeletalMeshLODRenderData* LODData, int MaxBoneInfluence, FStaticMeshVertexBuffers* AdditionalStaticMeshVB);
	FAllegroBaseVertexFactory* GetStaticVertexFactory(int SubMeshIndex, int LodIndex, const FStaticMeshLODResources* LODData, FStaticMeshVertexBuffers* AdditionalStaticMeshVB);

	void SetHasStencil(bool HasStencil);

private:

	bool bHasStencil = false;
	int  SpecialCustomDepthStencilValue = 0;
};

