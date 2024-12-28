// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "AllegroRenderResources.h"
#include "ConvexVolume.h"
#include "SceneView.h"
#include "SceneManagement.h"
#include "Materials/MaterialRenderProxy.h"
#include "Async/ParallelFor.h"
#include "Allegro.h"



struct alignas(16) FAllegroMeshGeneratorBase
{
	static uint32 OverrideNumPrimitive(uint32 Value)
	{
		if (!UE_BUILD_SHIPPING)
			return GAllegro_MaxTrianglePerInstance > 0 ? FMath::Min((uint32)GAllegro_MaxTrianglePerInstance, Value) : Value;

		return Value;
	}
	static uint32 OverrideMaxBoneInfluence(uint32 Value)
	{
		if (!UE_BUILD_SHIPPING)
			return GAllegro_FroceMaxBoneInfluence > 0 ? FMath::Min(uint32(FAllegroMeshDataEx::MAX_INFLUENCE), (uint32)GAllegro_FroceMaxBoneInfluence) : Value;

		return Value;
	}
	static uint32 OverrideAnimFrameIndex(uint32 Value)
	{
		if (!UE_BUILD_SHIPPING && GAllegro_ForcedAnimFrameIndex >= 0)
			return (uint32)GAllegro_ForcedAnimFrameIndex;

		return Value;
	};
	static EAllegroVerteFactoryMode GetTargetVFMode(int MeshMaxBoneInf)
	{
		return (EAllegroVerteFactoryMode)(MeshMaxBoneInf); //(MeshMaxBoneInf-1)
	}

	static uint32 PackFrameIndices(uint16 cur, uint16 prev) { return static_cast<uint32>(cur) | (static_cast<uint32>(prev) << 16); }

	static void SimpleFNV(uint32& Hash, uint32 Value)
	{
		Hash = (Hash ^ Value) * 0x01000193; //from FFnv1aHash::Add
	}

	static void OffsetPlane(FPlane& P, const FVector& Offset)
	{
		P.W = (P.GetOrigin() - Offset) | P.GetNormal();
	}


	//////////////////////////////////////////////////////////////////////////
	struct FIndexCollector
	{
		//each cache line (64 bytes) == UINT32[16]

		static const uint32 NUM_DW_PER_LINE = PLATFORM_CACHE_LINE_SIZE / sizeof(uint32);
		static const uint32 MAX_DW_INDEX = (NUM_DW_PER_LINE * 4) - (sizeof(void*) / sizeof(uint32));
		static const uint32 MAX_WORD_INDEX = MAX_DW_INDEX * 2;
		static const uint32 PAGE_DATA_SIZE_IN_BYTES = MAX_DW_INDEX * sizeof(uint32);

		struct alignas(PLATFORM_CACHE_LINE_SIZE) FPageData
		{
			FPageData* NextPage = nullptr;
			union
			{
				uint32 ValuesUINT32[MAX_DW_INDEX];
				uint16 ValuesUINT16[MAX_WORD_INDEX];
			};
		};
		static_assert((sizeof(FPageData) % PLATFORM_CACHE_LINE_SIZE) == 0);

		FPageData* PageHead;
		FPageData* PageTail;
		//uint32 BatchHash = 0x811c9dc5; //copied from FFnv1aHash
		uint32 Counter;	//counter for number of elements in tail block

		void Init(bool bUseUint32)
		{
			PageHead = PageTail = nullptr;
			Counter = bUseUint32 ? MAX_DW_INDEX : MAX_WORD_INDEX;
		}
	
		template<typename T> void AddValue(FAllegroMeshGeneratorBase& Gen, T InValue)
		{
			constexpr uint32 MAX_IDX = (sizeof(T) == sizeof(uint32)) ? MAX_DW_INDEX : MAX_WORD_INDEX;
			if (Counter == MAX_IDX)
			{
				Counter = 0;
				FPageData* NewPage = Gen.template MempoolNew<FPageData>();
				if (PageTail)
				{
					PageTail->NextPage = NewPage;
					PageTail = NewPage;
				}
				else
				{
					PageHead = PageTail = NewPage;
				}
			}

			if constexpr (sizeof(T) == sizeof(uint32))
				PageTail->ValuesUINT32[Counter++] = InValue;
			else
				PageTail->ValuesUINT16[Counter++] = InValue;
		}
		
		template<typename T> void CopyToArray(T* DstMem) const
		{
			constexpr uint32 MAX_IDX = (sizeof(T) == sizeof(uint32)) ? MAX_DW_INDEX : MAX_WORD_INDEX;

			const FPageData* PageIter = PageHead;
			while (PageIter)
			{
				const uint32 Num = PageIter->NextPage ? MAX_IDX : this->Counter;
				FMemory::Memcpy(DstMem, PageIter->ValuesUINT32, Num * sizeof(T));
				DstMem += Num;
				PageIter = PageIter->NextPage;
			}
		}
	};

	struct BatchData
	{
		uint32 NumInstance = 0;
		uint32 InstanceOffset;
		int16  Stencil;

		TArray<uint8> DataArray;

		void AddValue(void* value, bool UseInt32) //or Int16
		{
			uint32 OldNumInstance = NumInstance;
			++NumInstance;
			uint32 DataSize = UseInt32 ? 4 : 2;
			DataArray.AddUninitialized(DataSize);
			uint8* ptr = DataArray.GetData() + (OldNumInstance * DataSize);
			memcpy(ptr, value, DataSize);
		}
	};

	struct RunArrayInfo
	{
		int16  Stencil;
		TArray<uint32, SceneRenderingAllocator> Idxs;
		TArray<uint32, SceneRenderingAllocator> RunArray;

		void Handle()
		{
			RunArray.Reset(0);
			if (Idxs.Num() == 0)
			{
				return;
			}

			Idxs.Sort([](const uint32& LHS, const uint32& RHS)
				{
					return LHS < RHS;
				}
			);

			uint32 LastValue = 0;
			for (int i = 0; i < Idxs.Num(); ++i)
			{
				if (i == 0)
				{
					RunArray.Add(Idxs[i]);
					LastValue = Idxs[i];
				}
				else
				{
					if (Idxs[i] - LastValue == 1)
					{
						LastValue = Idxs[i];
					}
					else
					{
						RunArray.Add(LastValue);
						RunArray.Add(Idxs[i]);
						LastValue = Idxs[i];
					}
				}
			}
			RunArray.Add(LastValue);
			Idxs.Reset(0);
		}
	};





	struct FLODData
	{
		//normal batch
		uint32 NumInstance;
		uint32 InstanceOffset;
		FIndexCollector IndexCollector;

		TArray<BatchData> BatchData;   //extern for stencil!
		bool UseUIN32;

		TArray<RunArrayInfo, SceneRenderingAllocator> RunArrayInfo;

		void Init(bool bUseUIN32)
		{
			UseUIN32 = bUseUIN32;
			NumInstance = InstanceOffset = 0;
			IndexCollector.Init(bUseUIN32);
			BatchData.Reset();
		}

		template<typename T> void AddElem(FAllegroMeshGeneratorBase& Gen, T Value, int16 Stencil)
		{

#if ALLEGRO_USE_GPU_SCENE
			if (RunArrayInfo.Num() < 1)
			{
				auto& Array = RunArrayInfo.AddDefaulted_GetRef();
				Array.Stencil = -1;
			}
			if (Stencil < 0)
			{
				RunArrayInfo[0].Idxs.Add(Value);
			}
			else
			{

				bool Finded = false;
				for (int i = 1; i < RunArrayInfo.Num(); ++i)
				{
					if (RunArrayInfo[i].Stencil == Stencil)
					{
						Finded = true;
						RunArrayInfo[i].Idxs.Add(Value);
						break;
					}
				}
				if (!Finded)
				{
					auto& Array = RunArrayInfo.AddDefaulted_GetRef();
					Array.Stencil = Stencil;
					Array.Idxs.Add(Value);
				}
			}
#else
			if (Stencil < 0)
			{
				NumInstance++;
				IndexCollector.template AddValue<T>(Gen, Value);
			}
			else
			{
				bool Finded = false;
				for (auto& data : BatchData)
				{
					if (data.Stencil == Stencil)
					{
						Finded = true;
						data.AddValue(&Value, UseUIN32);
						break;
					}
				}
				if (!Finded)
				{
					auto& data = BatchData.AddDefaulted_GetRef();
					data.Stencil = Stencil;

					data.InstanceOffset = 0;
					data.NumInstance = 0;
					data.DataArray.Reserve(32);
					data.AddValue(&Value, UseUIN32);
				}
			}
		
#endif
		}

	};

	struct FSubMeshData
	{
		FLODData LODs[ALLEGRO_MAX_LOD];
		bool bHasAnyLOD;

		void Init(bool bUseUIN32)
		{
			for (FLODData& L : LODs)
				L.Init(bUseUIN32);

			bHasAnyLOD = false;
		}
	};

	struct FSubMeshInfo
	{
		uint32 MaxDrawDist = ~0u;
		uint32 OverrideDist = ~0u;
		uint8 OverrideMeshIdx = 0xFF;
		bool bIsValid = false;
		uint8 LODRemap[ALLEGRO_MAX_LOD] = {};
	};

#pragma region stack allocator
	static constexpr uint32 STACK_CAPCITY = ((sizeof(FSubMeshInfo) + sizeof(FSubMeshData)) * ALLEGRO_MAX_SUBMESH) + (sizeof(uint8) * ALLEGRO_MAX_SUBMESH) + sizeof(FConvexVolume) + 128;

	uint8 StackBytes[STACK_CAPCITY];
	uint8* StackMemSeek = nullptr;
	const uint8* GetStackTail() const { return &StackBytes[STACK_CAPCITY - 1]; }

	uint8* StackAlloc(const size_t SizeInBytes, const size_t InAlign = 4)
	{
		uint8* addr = Align(StackMemSeek, InAlign);
		StackMemSeek = addr + SizeInBytes;
		check(StackMemSeek <= GetStackTail());
		return addr;
	}
	template<typename T> T* StackNewArray(uint32 Count)
	{
		uint8* addr = StackAlloc(sizeof(T) * Count);
		DefaultConstructItems<T>(addr, Count);
		return (T*)addr;
	}
#pragma endregion


#pragma region mempool
	uint8* MempoolPtr = nullptr;
	uint8* MempoolSeek = nullptr;
	uint8* MempoolEnd = nullptr;

	template <typename T> T* MempoolAlloc(const size_t SizeInBytes, const size_t InAlign = 4)
	{
		uint8* addr = Align(MempoolSeek, InAlign);
		MempoolSeek = addr + SizeInBytes;
		check(MempoolSeek <= MempoolEnd);
		return (T*)addr;
	};
	template<typename T> T* MempoolNew()
	{
		return new (MempoolAlloc<void>(sizeof(T), alignof(T))) T();
	}
#pragma endregion

	const FConvexVolume* EditedViewFrustum = nullptr;
	uint32* VisibleInstances = nullptr;	//instance index of visible instances
	uint32* Distances = nullptr; //distance of instances (float is treated as uint32 for faster comparison)

	TArray<uint8>* VisibleInstanceLODLevel = nullptr;

	uint32 NumVisibleInstance = 0;
	bool Use32BitElementIndex() const { return NumVisibleInstance >= 0xFFFF; }
	auto GetElementIndexSize() const { return Use32BitElementIndex() ? 4u : 2u; }

	uint32 TotalElementCount = 0;	//
	uint32 TotalBatch = 0;	//number of FMeshBatch we draw
	uint32 NumSubMesh = 0;
	FSubMeshData* SubMeshes_Data = nullptr;
	FSubMeshInfo* SubMeshes_Info = nullptr;


	FAllegroProxy* Proxy = nullptr;
	const FSceneViewFamily* ViewFamily = nullptr;
	const FSceneView* View = nullptr;
	FMeshElementCollector* Collector = nullptr;
	int ViewIndex = 0;
	FVector3f ResolvedViewLocation;
	uint32 LODDrawDistances[ALLEGRO_MAX_LOD] = {};	//sorted from low to high. value is binary form of float. in other words we use: reinterpret_cast<uint32>(floatA) < reinterpret_cast<uint32>(floatB)
	uint32 MinDrawDist = 0;
	uint32 MaxDrawDist = ~0u;
	uint8 MaxMeshPerInstance = 0;
	bool bWireframe = false;
	FColoredMaterialRenderProxy* WireframeMaterialInstance = nullptr;
	FAllegroInstanceBufferPtr InstanceBuffer;
	FAllegroCIDBufferPtr CIDBuffer;
	FAllegroElementIndexBufferPtr ElementIndexBuffer;
	FAllegroBlendFrameBufferPtr BlendFrameBuffer;

	static const uint32 DISTANCING_NUM_FLOAT_PER_REG = 4;

	virtual ~FAllegroMeshGeneratorBase()
	{
		if (this->VisibleInstanceLODLevel)
		{
			delete[] this->VisibleInstanceLODLevel;
			this->VisibleInstanceLODLevel = nullptr;
		}
	}
};


struct FAllegroElementRunArrayOFR : FOneFrameResource
{
	TArray<uint32, SceneRenderingAllocator> RunArray;
};


template<bool bShaddowCollector>
struct FAllegroMultiMeshGenerator : public FAllegroMeshGeneratorBase
{
	FAllegroMultiMeshGenerator(const FAllegroProxy* InProxy, const FSceneViewFamily* InViewFamily, const FSceneView* InView, FMeshElementCollector* InCollector, int InViewIndex,  int SubMeshNum)
	{
		Proxy = const_cast<FAllegroProxy*>(InProxy);
		ViewFamily = InViewFamily;
		View = InView;
		Collector = InCollector;
		ViewIndex = InViewIndex;

		StackMemSeek = StackBytes;
		check(SubMeshNum > 0 && InProxy->MaxMeshPerInstance > 0);  //check(InProxy->SubMeshes.Num() > 0 && InProxy->MaxMeshPerInstance > 0);
		this->NumSubMesh = SubMeshNum;							   //InProxy->SubMeshes.Num();
		this->MaxMeshPerInstance = InProxy->MaxMeshPerInstance;
		this->SubMeshes_Data = this->StackNewArray<FSubMeshData>(NumSubMesh);
		this->SubMeshes_Info = this->StackNewArray<FSubMeshInfo>(NumSubMesh);
	}

	virtual ~FAllegroMultiMeshGenerator()
	{
		if(MempoolPtr)
			FMemory::Free(MempoolPtr);
	}

	void DoGenerate()
	{
		GenerateData();

		if (this->TotalElementCount == 0)
			return;

		GenerateBatches();

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		if (GAllegro_DrawInstanceBounds && !bShaddowCollector)
		{
			for (uint32 VisIndex = 0; VisIndex < NumVisibleInstance; VisIndex++)
			{
				RenderInstanceBound(this->VisibleInstances[VisIndex]);
			}
		}
		if (GAllegro_DrawCells && !bShaddowCollector)
		{
			DrawCellBounds();
		}
#endif
	}

	//////////////////////////////////////////////////////////////////////////
	void RenderInstanceBound(uint32 InstanceIndex) const
	{
		check(InstanceIndex < static_cast<int>(Proxy->DynamicData->InstanceCount));
		const FMatrix InstanceMatrix = FMatrix(Proxy->DynamicData->Transforms[InstanceIndex]);
		FPrimitiveDrawInterface* PDI = Collector->GetPDI(ViewIndex);
		const ESceneDepthPriorityGroup DrawBoundsDPG = SDPG_World;
		uint16 InstanceAnimationFrameIndex = Proxy->DynamicData->FrameIndices[InstanceIndex];
		const FBoxCenterExtentFloat& InstanceBound = Proxy->DynamicData->Bounds[InstanceIndex];
		DrawWireBox(PDI, FBox(InstanceBound.GetFBox()), FLinearColor::Green, DrawBoundsDPG);
		//draw axis
		{
			FVector AxisLoc = InstanceMatrix.GetOrigin();
			FVector X, Y, Z;
			InstanceMatrix.GetScaledAxes(X, Y, Z);
			const float Scale = 50;
			PDI->DrawLine(AxisLoc, AxisLoc + X * Scale, FColor::Red, DrawBoundsDPG, 1);
			PDI->DrawLine(AxisLoc, AxisLoc + Y * Scale, FColor::Green, DrawBoundsDPG, 1);
			PDI->DrawLine(AxisLoc, AxisLoc + Z * Scale, FColor::Blue, DrawBoundsDPG, 1);
		}
	}
	//////////////////////////////////////////////////////////////////////////
	void RenderCircleBound(const FVector& InBoundOrigin, float InBoundSphereRadius, FColor DrawColor = FColor::Yellow) const
	{
		FPrimitiveDrawInterface* PDI = Collector->GetPDI(ViewIndex);
		const ESceneDepthPriorityGroup DrawBoundsDPG = SDPG_World;
		DrawCircle(PDI, InBoundOrigin, FVector(1, 0, 0), FVector(0, 1, 0), DrawColor, InBoundSphereRadius, 32, DrawBoundsDPG);
		DrawCircle(PDI, InBoundOrigin, FVector(1, 0, 0), FVector(0, 0, 1), DrawColor, InBoundSphereRadius, 32, DrawBoundsDPG);
		DrawCircle(PDI, InBoundOrigin, FVector(0, 1, 0), FVector(0, 0, 1), DrawColor, InBoundSphereRadius, 32, DrawBoundsDPG);
	}
	//////////////////////////////////////////////////////////////////////////
	void RenderBox(const FBox& Box, FColor DrawColor = FColor::Green) const
	{
		FPrimitiveDrawInterface* PDI = Collector->GetPDI(ViewIndex);
		const ESceneDepthPriorityGroup DrawBoundsDPG = SDPG_World;
		DrawWireBox(PDI, Box, DrawColor, DrawBoundsDPG);
	}
	//////////////////////////////////////////////////////////////////////////
	void DrawCellBounds()
	{
		for (uint32 CellIndex = 0; CellIndex < this->Proxy->DynamicData->NumCells; CellIndex++)
		{
			const FAllegroDynamicData::FCell& Cell = this->Proxy->DynamicData->Cells[CellIndex];
			if (Cell.IsEmpty())
				continue;

			FPrimitiveDrawInterface* PDI = Collector->GetPDI(ViewIndex);
			FColor Color = FColor::MakeRandomSeededColor(CellIndex);
			DrawWireBox(PDI, FBox(Cell.Bound.ToBox()), Color, SDPG_World, 3);
			if(0)
			{
				Cell.ForEachInstance(*this->Proxy->DynamicData, [&](uint32 InstanceIndex) {
					FVector3f Center = this->Proxy->DynamicData->Bounds[InstanceIndex].Center;
					PDI->DrawPoint(FVector(Center), Color, 8, SDPG_World);
				});
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	TUniformBufferRef<FAllegroVertexFactoryParameters> CreateUniformBuffer(uint32 InstanceOffset, uint32 NumInstance,uint32 LODLevel)
	{
		FAllegroVertexFactoryParameters UniformParams;
		UniformParams.LODLevel = LODLevel;
		UniformParams.BoneCount = (this->Proxy->AminCollection)?this->Proxy->AminCollection->RenderBoneCount:0;
		UniformParams.InstanceOffset = InstanceOffset;						//LODData.InstanceOffset;
		UniformParams.InstanceEndOffset = InstanceOffset + NumInstance - 1; //LODData.InstanceOffset + LODData.NumInstance - 1;
		UniformParams.NumCustomDataFloats = 0;

		UniformParams.AnimationBuffer = (this->Proxy->AminCollection) ? (this->Proxy->AminCollection->AnimationBuffer->ShaderResourceViewRHI): GNullVertexBuffer.VertexBufferSRV;
		UniformParams.Instance_CustomData = GNullVertexBuffer.VertexBufferSRV;//#TODO proper SRV ?
		
		if (this->CIDBuffer) //do we have any per instance custom data 
		{
			UniformParams.Instance_CustomData = this->CIDBuffer->CustomDataSRV;
			UniformParams.NumCustomDataFloats = this->Proxy->NumCustomDataFloats;
		}

		UniformParams.Instance_Transforms = this->InstanceBuffer->TransformSRV;
		UniformParams.Instance_AnimationFrameIndices = this->InstanceBuffer->FrameIndexSRV;

		UniformParams.ElementIndices = this->Use32BitElementIndex() ? this->ElementIndexBuffer->ElementIndexUIN32SRV : this->ElementIndexBuffer->ElementIndexUIN16SRV;

		UniformParams.Instance_BlendFrameIndex = InstanceBuffer->BlendFrameIndexmSRV;
		UniformParams.Instance_BlendFrameBuffer = GNullVertexBuffer.VertexBufferSRV;
		if (BlendFrameBuffer)
		{
			UniformParams.Instance_BlendFrameBuffer = BlendFrameBuffer->BlendFrameDataSRV;
		}

		return FAllegroVertexFactoryBufferRef::CreateUniformBufferImmediate(UniformParams, UniformBuffer_SingleFrame);//will take from pool , no worry

	}
	//////////////////////////////////////////////////////////////////////////
	FMeshBatch& AllocateMeshBatch(const FSkeletalMeshLODRenderData& SkelLODData, uint32 SubMeshIndex, uint32 LODIndex, uint32 SectionIndex, 
		FAllegroBatchElementOFR* BatchUserData, int32 NumInstances)
	{
		FMeshBatch& Mesh = this->Collector->AllocateMesh();
		Mesh.ReverseCulling = false;//IsLocalToWorldDeterminantNegative();
		Mesh.Type = PT_TriangleList;
		Mesh.DepthPriorityGroup = this->Proxy->GetDepthPriorityGroup(View);
		Mesh.bCanApplyViewModeOverrides = true;
		Mesh.bSelectable = false;
		Mesh.bUseForMaterial = true;
		Mesh.bUseSelectionOutline = false;
		Mesh.LODIndex = static_cast<int8>(LODIndex);	//?
		Mesh.SegmentIndex = static_cast<uint8>(SectionIndex);
		//its useless, MeshIdInPrimitive is set by Collector->AddMesh()
		//Mesh.MeshIdInPrimitive = static_cast<uint16>(LODIndex); //static_cast<uint16>((LODIndex << 8) | SectionIndex);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		Mesh.VisualizeLODIndex = static_cast<int8>(LODIndex);
#endif
		Mesh.VertexFactory = BatchUserData->VertexFactory;

		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.UserData = BatchUserData;

#if ALLEGRO_USE_GPU_SCENE

#else
		BatchElement.PrimitiveIdMode = PrimID_ForceZero;
#endif


		BatchElement.IndexBuffer = SkelLODData.MultiSizeIndexContainer.GetIndexBuffer();
		BatchElement.UserIndex = 0;
		//BatchElement.PrimitiveUniformBufferResource = &PrimitiveUniformBuffer->UniformBuffer; //&GIdentityPrimitiveUniformBuffer; 
		BatchElement.PrimitiveUniformBuffer = this->Proxy->GetUniformBuffer();

		BatchElement.NumInstances = NumInstances; // this->SubMeshes_Data[SubMeshIndex].LODs[LODIndex].NumInstance;
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		BatchElement.VisualizeElementIndex = static_cast<int32>(SectionIndex);
#endif



		return Mesh;
	}


	//////////////////////////////////////////////////////////////////////////
	void Cull()
	{
		{
			ALLEGRO_SCOPE_CYCLE_COUNTER(FirstCull);

			const FAllegroDynamicData* DynData = this->Proxy->DynamicData;
			uint32* VisibleInstancesIter = this->VisibleInstances;

			if(DynData->NumCells > 0 && !GAllegro_DisableFrustumCull)
			{
				//cells intersection with frustum
				for (uint32 CellIndex = 0; CellIndex < DynData->NumCells; CellIndex++)
				{
					const FAllegroDynamicData::FCell& Cell = DynData->Cells[CellIndex];
					if (Cell.IsEmpty())
						continue;

					bool bFullyInside;
					FBoxCenterExtentFloat CellBound;
					Cell.Bound.ToCenterExtentBox(CellBound);
					if (!EditedViewFrustum->IntersectBox(FVector(CellBound.Center), FVector(CellBound.Extent), bFullyInside))
						continue;

					if (bFullyInside)
					{
						VisibleInstancesIter = (uint32*)Cell.CopyToArray(*DynData, (uint8*)VisibleInstancesIter);
					}
					else
					{
						Cell.ForEachInstance(*DynData, [&](uint32 InstanceIndex) {
							const FBoxCenterExtentFloat& IB = DynData->Bounds[InstanceIndex];
							if (EditedViewFrustum->IntersectBox(FVector(IB.Center), FVector(IB.Extent)))
							{
								*VisibleInstancesIter++ = InstanceIndex;
							}
						});
					}
				}
			}
			else
			{
				//per instance frustum cull 
				for (uint32 InstanceIndex = 0; InstanceIndex < DynData->InstanceCount; InstanceIndex++)
				{
					if (EnumHasAnyFlags(DynData->Flags[InstanceIndex], EAllegroInstanceFlags::EIF_Destroyed | EAllegroInstanceFlags::EIF_Hidden))
						continue;

					if(!GAllegro_DisableFrustumCull)
					{
						const FBoxCenterExtentFloat& Bound = DynData->Bounds[InstanceIndex];
						if (!EditedViewFrustum->IntersectBox(FVector(Bound.Center), FVector(Bound.Extent)))	//frustum cull
							continue;
					}

					*VisibleInstancesIter++ = InstanceIndex;
				}
			}

			this->NumVisibleInstance = VisibleInstancesIter - this->VisibleInstances;

			if (this->NumVisibleInstance == 0)
				return;
			
		}

		{
			//fix the remaining data because we go SIMD only
			uint32 AVI = Align(this->NumVisibleInstance, DISTANCING_NUM_FLOAT_PER_REG);
			for (uint32 i = this->NumVisibleInstance; i < AVI; i++)
				this->VisibleInstances[i] = this->VisibleInstances[0];

			MempoolAlloc<uint32>(AVI * sizeof(uint32)); //tricky! pass over VisibleInstances

#if ALLEGRO_USE_LOD_SCREEN_SIZE

			if (this->NumSubMesh > 0)
			{
				if (this->VisibleInstanceLODLevel)
				{
					delete[] this->VisibleInstanceLODLevel;
					this->VisibleInstanceLODLevel = nullptr;
				}

				uint32 NumLodMesh = 1;

#if ALLEGRO_LOD_PRE_SUBMESH
				NumLodMesh = this->NumSubMesh;
#endif

				this->VisibleInstanceLODLevel = new TArray<uint8>[NumLodMesh];
				for (uint32 i = 0; i < NumLodMesh; ++i)
				{
					this->VisibleInstanceLODLevel[i].Reset(this->NumVisibleInstance);
					this->VisibleInstanceLODLevel[i].AddUninitialized(this->NumVisibleInstance);
				}

				this->UpdateLODLevel(NumLodMesh);

			}
#else
			this->Distances = MempoolAlloc<uint32>(AVI * sizeof(uint32), 16);

			CollectDistances();
#endif

		}


#if ALLEGRO_USE_LOD_SCREEN_SIZE

#else
		//#TODO maybe sort with Algo::Sort when NumVisibleInstance is small
		//sort the instances from front to rear
		if(NumVisibleInstance > 1)
		{
			ALLEGRO_SCOPE_CYCLE_COUNTER(SortInstances);

			uint32* Distances_Sorted = MempoolAlloc<uint32>(NumVisibleInstance * sizeof(uint32));
			uint32* VisibleInstances_Sorted = MempoolAlloc<uint32>(NumVisibleInstance * sizeof(uint32));

			if (NumVisibleInstance > 0xFFff)
				AllegroRadixSort32(VisibleInstances_Sorted, this->VisibleInstances, Distances_Sorted, this->Distances, static_cast<uint32>(NumVisibleInstance));
			else
				AllegroRadixSort32(VisibleInstances_Sorted, this->VisibleInstances, Distances_Sorted, this->Distances, static_cast<uint16>(NumVisibleInstance));

			this->VisibleInstances = VisibleInstances_Sorted;
			this->Distances = Distances_Sorted;
		}

		if(MaxDrawDist != ~0u)
		{

			uint32* DistIter = this->Distances + this->NumVisibleInstance;
			do
			{
				DistIter--;
				if (*DistIter <= this->MaxDrawDist)
				{
					DistIter++;
					break;
				}
			} while (DistIter != this->Distances);

			
			this->NumVisibleInstance = DistIter - this->Distances;

			if (this->NumVisibleInstance == 0)
				return;
		}

		if(MinDrawDist > 0)
		{
			uint32 i = 0;
			for (; i < this->NumVisibleInstance; i++)
			{
				if(this->Distances[i] >= this->MinDrawDist)
					break;
			}
			this->VisibleInstances += i;
			this->Distances += i;
			this->NumVisibleInstance -= i;

			if (this->NumVisibleInstance == 0)
				return;
		}
#endif

		{
			if (NumVisibleInstance > 0xFFff)
				SecondCull<uint32>();
			else
				SecondCull<uint16>();
		}

	}

	
	void UpdateLODLevelImpl(TArray<FProxyMeshDataBase*>& MDArray, TArray<uint8>* OutLodArray, int NumCalc)
	{
		const FAllegroDynamicData* DynData = this->Proxy->DynamicData;
		static const auto* SkeletalMeshLODRadiusScale = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.SkeletalMeshLODRadiusScale"));
		float LODScale = FMath::Clamp(SkeletalMeshLODRadiusScale->GetValueOnRenderThread(), 0.25f, 1.0f);
		 
		//const int32 CurrentLODLevel = 0;
		//const float HysteresisOffset = 0.f;

		uint32* VisInstance = this->VisibleInstances; 
		const FSceneView* V = this->View;
		const uint8* InstancesMeshSlots = this->Proxy->DynamicData->MeshSlots;
		uint32 MaxMeshPerInst = this->MaxMeshPerInstance;
		float CullScreenSize = GAllegro_CullScreenSize;
		uint32 NumMesh = this->NumSubMesh;
		
		ParallelFor(TEXT("ParallelForLODLevel"), NumVisibleInstance, 400, [VisInstance, DynData, V, 
			LODScale, MDArray, OutLodArray, InstancesMeshSlots, MaxMeshPerInst, CullScreenSize, NumMesh, NumCalc](int i) {

			for (int SubIdx = 0; SubIdx < NumCalc; ++SubIdx)
			{
				TArray<uint8>& OutLod = OutLodArray[SubIdx];
				FProxyMeshDataBase* MD = MDArray[SubIdx];

				OutLod[i] = 0xff; //default is cull

				const uint8* MeshSlotIter = InstancesMeshSlots + i * (MaxMeshPerInst + 1);
				for (uint32 n = 0; n < (MaxMeshPerInst + 1); ++n)
				{
					uint8 SubMeshIdx = *MeshSlotIter++;

					if (SubMeshIdx == 0xFF) //data should be terminated with 0xFF
						break;

					if (SubMeshIdx != SubIdx || SubMeshIdx >= NumMesh)
					{
						continue;
					}

					int32 NewLODLevel = 0;
					uint32 InstanceIndex = VisInstance[i];
					const FBoxCenterExtentFloat& IB = DynData->Bounds[InstanceIndex];
					FVector4 Origin(IB.Center.X, IB.Center.Y, IB.Center.Z, 0);
					float SphereRadius = FMath::Max(FMath::Max(IB.Extent.Y, IB.Extent.Z), IB.Extent.X);

#if ALLEGRO_LOD_PRE_SUBMESH_FACTOR
					check(MD->ExtentFactor > 0.0f);
					SphereRadius *= MD->ExtentFactor;
#endif

					float ScreenRadiusSquared = ComputeBoundsScreenRadiusSquared(Origin, SphereRadius, *(V)) * LODScale * LODScale;
					if (CullScreenSize > ScreenRadiusSquared)
					{
						OutLod[i] = 0xff;
					}
					else
					{
						if (V->Family && 1 == V->Family->EngineShowFlags.LOD)
						{
							// Iterate from worst to best LOD
							for (int32 LODLevel = MD->LodNum - 1; LODLevel > 0; LODLevel--)
							{
								// Get ScreenSize for this LOD
								float ScreenSize = MD->LODScreenSize[LODLevel];

								// If we are considering shifting to a better (lower) LOD, bias with hysteresis.
								//if (LODLevel <= CurrentLODLevel)
								//{
								//	ScreenSize += MD->SKMLODHysteresis[LODLevel];
								//}

								// If have passed this boundary, use this LOD
								if (FMath::Square(ScreenSize * 0.5f) > ScreenRadiusSquared)
								{
									NewLODLevel = LODLevel;
									break;
								}
							}
						}
						OutLod[i] = NewLODLevel;
					}

				}
			}

		});

		
	}


	virtual void UpdateLODLevel(int NumCalc)
	{
		TArray<FProxyMeshDataBase*> MeshDataBaseArray;
		MeshDataBaseArray.Reset(this->Proxy->SubMeshes.Num());
		for (int i = 0; i < this->Proxy->SubMeshes.Num(); ++i)
		{
			auto& Ref = MeshDataBaseArray.AddDefaulted_GetRef();
			Ref = &(this->Proxy->SubMeshes[i]);
		}
		this->UpdateLODLevelImpl(MeshDataBaseArray, this->VisibleInstanceLODLevel , NumCalc);
	}


	//////////////////////////////////////////////////////////////////////////
	void CollectDistances()
	{
		ALLEGRO_SCOPE_CYCLE_COUNTER(CollectDistances);

		auto CamPos = VectorLoad(&ResolvedViewLocation.X);
		auto CamX = VectorReplicate(CamPos, 0);
		auto CamY = VectorReplicate(CamPos, 1);
		auto CamZ = VectorReplicate(CamPos, 2);

		const uint32 AlignedNumVis = Align(this->NumVisibleInstance, DISTANCING_NUM_FLOAT_PER_REG);
		const FBoxCenterExtentFloat* Bounds = this->Proxy->DynamicData->Bounds;

		//collect distances of visible indices
		for (uint32 VisIndex = 0; VisIndex < AlignedNumVis; VisIndex += DISTANCING_NUM_FLOAT_PER_REG)
		{
			auto CentersA = VectorLoad(&Bounds[this->VisibleInstances[VisIndex + 0]].Center.X);
			auto CentersB = VectorLoad(&Bounds[this->VisibleInstances[VisIndex + 1]].Center.X);
			auto CentersC = VectorLoad(&Bounds[this->VisibleInstances[VisIndex + 2]].Center.X);
			auto CentersD = VectorLoad(&Bounds[this->VisibleInstances[VisIndex + 3]].Center.X);

			AllegroVectorTranspose4x4(CentersA, CentersB, CentersC, CentersD);

			auto XD = VectorSubtract(CamX, CentersA);
			auto YD = VectorSubtract(CamY, CentersB);
			auto ZD = VectorSubtract(CamZ, CentersC);

			auto LenSQ = VectorMultiplyAdd(XD, XD, VectorMultiplyAdd(YD, YD, VectorMultiply(ZD, ZD)));

			VectorStoreAligned(LenSQ, reinterpret_cast<float*>(this->Distances + VisIndex));
		}
	}
	//////////////////////////////////////////////////////////////////////////
	template<typename TVisIndex> void SecondCull()
	{
		ALLEGRO_SCOPE_CYCLE_COUNTER(SecondCull);

		//set initial values of batches
		for (uint32 MeshIdx = 0; MeshIdx < this->NumSubMesh; MeshIdx++)
		{
			this->SubMeshes_Data[MeshIdx].Init(sizeof(TVisIndex) == sizeof(uint32));
		}

		//fix the align for allocations of pages
		this->MempoolSeek = Align(this->MempoolSeek, PLATFORM_CACHE_LINE_SIZE);

		const uint8* InstancesMeshSlots = this->Proxy->DynamicData->MeshSlots;

		uint32 CurLODIndex = 0;
		uint32 NextLODDist = LODDrawDistances[CurLODIndex];

		uint8* SubMeshCurLOD = this->StackAlloc(this->NumSubMesh);
		for (uint32 MeshIdx = 0; MeshIdx < this->NumSubMesh; MeshIdx++)
			SubMeshCurLOD[MeshIdx] = this->SubMeshes_Info[MeshIdx].LODRemap[CurLODIndex];

		const FAllegroDynamicData* DynamicData = this->Proxy->DynamicData;

		//second culling + batching 
		for (uint32 VisIndex = 0; VisIndex < this->NumVisibleInstance; VisIndex++)
		{
			const uint32 InstanceIndex = this->VisibleInstances[VisIndex];

#if ALLEGRO_USE_LOD_SCREEN_SIZE

			if (this->VisibleInstanceLODLevel)
			{
				for (uint32 MeshIdx = 0; MeshIdx < this->NumSubMesh; MeshIdx++)
				{
#if ALLEGRO_LOD_PRE_SUBMESH
					CurLODIndex = this->VisibleInstanceLODLevel[MeshIdx][VisIndex];
#else
					CurLODIndex = this->VisibleInstanceLODLevel[0][VisIndex];
#endif
					if (0xff != CurLODIndex)
					{
						SubMeshCurLOD[MeshIdx] = this->SubMeshes_Info[MeshIdx].LODRemap[CurLODIndex];
					}
					else
					{
						SubMeshCurLOD[MeshIdx] = 0xff;
					}
				}
			}

#else
			const uint32 DistanceSQ = this->Distances[VisIndex];
			while (DistanceSQ > NextLODDist)
			{
				CurLODIndex++;
				NextLODDist = this->LODDrawDistances[CurLODIndex];

				for (uint32 MeshIdx = 0; MeshIdx < this->NumSubMesh; MeshIdx++)
					SubMeshCurLOD[MeshIdx] = this->SubMeshes_Info[MeshIdx].LODRemap[CurLODIndex];

			}
#endif
	
			const uint8* MeshSlotIter = InstancesMeshSlots + InstanceIndex * (this->MaxMeshPerInstance + 1);
			for (int n = 0; n < (this->MaxMeshPerInstance + 1); ++n)
			{
				uint8 SubMeshIdx = *MeshSlotIter++;

				if (SubMeshIdx == 0xFF) //data should be terminated with 0xFF
					break;

				//check(SubMeshIdx < this->NumSubMesh);
				if (SubMeshIdx >= this->NumSubMesh)
				{
					continue;
				}

				const uint8 CurLod = SubMeshCurLOD[SubMeshIdx];

#if ALLEGRO_USE_LOD_SCREEN_SIZE
				
				if (CurLod == 0xff)
				{
					//cull
					continue;
				}

#else
				if (DistanceSQ > SubMeshes_Info[SubMeshIdx].MaxDrawDist)
					continue;

				if (DistanceSQ > SubMeshes_Info[SubMeshIdx].OverrideDist)
				{
					SubMeshIdx = SubMeshes_Info[SubMeshIdx].OverrideMeshIdx;

					if (DistanceSQ > SubMeshes_Info[SubMeshIdx].MaxDrawDist)
						continue;
				}
#endif
				if (bShaddowCollector)
				{
					//##TODO optimize. in shadow pass sub meshes with same skeletal mesh can be merge if materials have same VS (default vertex shader)
					//SubMeshIdx = this->ShadowSubmeshRemap[SubMeshIdx];
				}

				FLODData& LODData = this->SubMeshes_Data[SubMeshIdx].LODs[CurLod];
				if (bShaddowCollector)
				{
					LODData.template AddElem<TVisIndex>(*this, static_cast<TVisIndex>(VisIndex),-1);
				}
				else
				{
#if	ALLEGRO_USE_STENCIL
					int16 Stencil = DynamicData->Stencil[InstanceIndex];
#else
					int16 Stencil = -1;
#endif
					LODData.template AddElem<TVisIndex>(*this, static_cast<TVisIndex>(VisIndex), Stencil);
				}
				
			}

		}
		

#if ALLEGRO_USE_LOD_SCREEN_SIZE
		if (this->VisibleInstanceLODLevel)
		{
			delete[] this->VisibleInstanceLODLevel;
			this->VisibleInstanceLODLevel = nullptr;
		}
#endif

		//assign offsets and count total
		{
			this->TotalElementCount = 0;
			this->TotalBatch = 0;
			bool bHasStencil = false;

#if ALLEGRO_USE_GPU_SCENE
			//nothing
			this->TotalElementCount = 1;
#else
			for (uint32 MeshIdx = 0; MeshIdx < this->NumSubMesh; MeshIdx++)
			{
				if(!this->SubMeshes_Info[MeshIdx].bIsValid)
					continue;

				for (uint32 LODIndex = 0; LODIndex < ALLEGRO_MAX_LOD; LODIndex++)
				{
					FLODData& LODData = this->SubMeshes_Data[MeshIdx].LODs[LODIndex];
					if (LODData.NumInstance > 0)
					{
						this->SubMeshes_Data[MeshIdx].bHasAnyLOD = true;

						LODData.InstanceOffset = this->TotalElementCount;
						this->TotalElementCount += LODData.NumInstance;
						this->TotalBatch++;
					}

					for (auto& batch : LODData.BatchData)
					{
						if (batch.NumInstance > 0)
						{
							if (batch.Stencil > -1)
							{
								bHasStencil = true;
							}

							this->SubMeshes_Data[MeshIdx].bHasAnyLOD = true;

							batch.InstanceOffset = this->TotalElementCount;
							this->TotalElementCount += batch.NumInstance;
							//this->TotalBatch++;
						}
					}
				}
			}
			check(this->TotalBatch <= this->Proxy->MaxBatchCountPossible);
#endif
			if (!bShaddowCollector)
			{
				this->Proxy->SetHasStencil(bHasStencil);
			}
		}
	}
	//////////////////////////////////////////////////////////////////////////
	void FillBuffers()
	{
		const FAllegroDynamicData* DynamicData = this->Proxy->DynamicData;
		const FAllegroDynamicData* OldDynamicData = this->Proxy->OldDynamicData;

		if (OldDynamicData && !GAllegro_DebugForceNoPrevFrameData)
		{
			const int CFN = GFrameNumberRenderThread;
			if ((CFN - OldDynamicData->CreationNumber) == 1)	//must belong to the previous frame exactly
			{

			}
			else
			{
				OldDynamicData = DynamicData;
			}
		}
		else
		{
			OldDynamicData = DynamicData;
		}

		//EIF_New means instance just added this frame, so it doesn't have previous frame data
		check((int)EAllegroInstanceFlags::EIF_New == 2);
		const FAllegroDynamicData* PrevDynamicDataLUT[3] = { OldDynamicData, OldDynamicData, DynamicData };

		//fill instance buffers
		{
			AllegroShaderMatrixT* RESTRICT DstInstanceTransform = this->InstanceBuffer->MappedTransforms;
			uint32* RESTRICT DstPackedFrameIndex = this->InstanceBuffer->MappedFrameIndices;
			uint32* RESTRICT DstBlendFrameIndices = InstanceBuffer->MappedBlendFrameIndices;
			float* RESTRICT DstBlendFrameData = BlendFrameBuffer? BlendFrameBuffer->MappedData:nullptr;
			check(IsAligned(DstInstanceTransform, 16));


			float* RESTRICT DstCustomDatas = nullptr;
			if (this->CIDBuffer)
			{
				DstCustomDatas = this->CIDBuffer->MappedData;
			}
			const uint32 NumCustomDataFloats = DstCustomDatas ? Proxy->NumCustomDataFloats : 0;

			//#Note stores instance data from front to rear
			//for each visible instance
			for (uint32 VisIdx = 0; VisIdx < NumVisibleInstance; VisIdx++)
			{
				uint32 InstanceIndex = this->VisibleInstances[VisIdx];
				check(InstanceIndex < DynamicData->InstanceCount);

				const FAllegroDynamicData* PrevFrameDynamicData = PrevDynamicDataLUT[static_cast<uint16>(DynamicData->Flags[InstanceIndex] & EAllegroInstanceFlags::EIF_New)];
				check(InstanceIndex < PrevFrameDynamicData->InstanceCount);

				//converts from Matrix4x4f
				DstInstanceTransform[VisIdx * 2 + 0] = DynamicData->Transforms[InstanceIndex];
				DstInstanceTransform[VisIdx * 2 + 1] = PrevFrameDynamicData->Transforms[InstanceIndex];

				DstPackedFrameIndex[VisIdx * 2 + 0] = OverrideAnimFrameIndex(DynamicData->FrameIndices[InstanceIndex]);
				DstPackedFrameIndex[VisIdx * 2 + 1] = OverrideAnimFrameIndex(PrevFrameDynamicData->FrameIndices[InstanceIndex]);

				//#TODO optimize
				for (uint32 FloatIndex = 0; FloatIndex < NumCustomDataFloats; FloatIndex++)
				{
					DstCustomDatas[VisIdx * NumCustomDataFloats + FloatIndex] = DynamicData->CustomData[InstanceIndex * NumCustomDataFloats + FloatIndex];
				}

				DstBlendFrameIndices[VisIdx * 2] = DynamicData->BlendFrameInfoIndex[InstanceIndex];
				DstBlendFrameIndices[VisIdx * 2 + 1] = PrevFrameDynamicData->BlendFrameInfoIndex[InstanceIndex];
			}

			if (BlendFrameBuffer)
			{
				const FAllegroDynamicData* PrevFrameDynamicData = OldDynamicData;
				int DataSize = 2 * Proxy->NumBlendFramePerInstance - 1;

				FInstanceBlendFrameInfo nullInfo;
				nullInfo.Weight[0] = 1;
				nullInfo.Weight[1] = 0;
				nullInfo.Weight[2] = 0;
				nullInfo.Weight[3] = 0;

				for (uint32 i = 0; i < DynamicData->NumBlendFrame; ++i)
				{
					if(DynamicData->BlendFrameInfoData)
						FMemory::Memcpy(DstBlendFrameData + (i * 2 * DataSize), &(DynamicData->BlendFrameInfoData[i]),  sizeof(FInstanceBlendFrameInfo));
					else
						FMemory::Memcpy(DstBlendFrameData + (i * 2 * DataSize), &nullInfo, sizeof(FInstanceBlendFrameInfo));

					if (PrevFrameDynamicData->BlendFrameInfoData && PrevFrameDynamicData->NumBlendFrame > i)
						FMemory::Memcpy(DstBlendFrameData + (i * 2 * DataSize + DataSize), &(PrevFrameDynamicData->BlendFrameInfoData[i]), sizeof(FInstanceBlendFrameInfo));
					else
						FMemory::Memcpy(DstBlendFrameData + (i * 2 * DataSize + DataSize), DstBlendFrameData + (i * 2 * DataSize), sizeof(FInstanceBlendFrameInfo));
				}

				BlendFrameBuffer->UnlockBuffers();
			}

			InstanceBuffer->UnlockBuffers();
			if (CIDBuffer)
				CIDBuffer->UnlockBuffers();
		}
	}
	//////////////////////////////////////////////////////////////////////////
	void FillShadowBuffers()
	{
		const FAllegroDynamicData* DynamicData = this->Proxy->DynamicData;

		AllegroShaderMatrixT* RESTRICT DstInstanceTransform = this->InstanceBuffer->MappedTransforms;
		uint32* RESTRICT DstFrameIndex = this->InstanceBuffer->MappedFrameIndices;
		uint32* RESTRICT DstBlendFrameIndices = InstanceBuffer->MappedBlendFrameIndices;
		float* RESTRICT DstBlendFrameData = BlendFrameBuffer ? BlendFrameBuffer->MappedData : nullptr;
		check(IsAligned(DstInstanceTransform, 16));

		float* RESTRICT DstCustomDatas = nullptr;
		if (this->CIDBuffer) //could be null for shadow pass
		{
			DstCustomDatas = this->CIDBuffer->MappedData;
		}
		const uint32 NumCustomDataFloats = DstCustomDatas ? Proxy->NumCustomDataFloats : 0;

		//for each visible instance, copy their data to the buffer
		for (uint32 VisIdx = 0; VisIdx < NumVisibleInstance; VisIdx++)
		{
			uint32 InstanceIndex = this->VisibleInstances[VisIdx];
			check(InstanceIndex < DynamicData->InstanceCount);

			DstInstanceTransform[VisIdx] = DynamicData->Transforms[InstanceIndex];
			DstFrameIndex[VisIdx] = OverrideAnimFrameIndex(DynamicData->FrameIndices[InstanceIndex]);

			for (uint32 FloatIndex = 0; FloatIndex < NumCustomDataFloats; FloatIndex++)
			{
				DstCustomDatas[VisIdx * NumCustomDataFloats + FloatIndex] = DynamicData->CustomData[InstanceIndex * NumCustomDataFloats + FloatIndex];
			}
			DstBlendFrameIndices[VisIdx] = DynamicData->BlendFrameInfoIndex[InstanceIndex];
		}

		if (BlendFrameBuffer)
		{
			if (DynamicData->BlendFrameInfoData)
			{
				FMemory::Memcpy(DstBlendFrameData, (DynamicData->BlendFrameInfoData), sizeof(FInstanceBlendFrameInfo) * DynamicData->NumBlendFrame);
			}
			else
			{
				int DataSize = 2 * Proxy->NumBlendFramePerInstance - 1;
				FInstanceBlendFrameInfo nullInfo;
				nullInfo.Weight[0] = 1;
				nullInfo.Weight[1] = 0;
				nullInfo.Weight[2] = 0;
				nullInfo.Weight[3] = 0;

				for (uint32 i = 0; i < DynamicData->NumBlendFrame; ++i)
				{
					FMemory::Memcpy(DstBlendFrameData + (i * DataSize), &nullInfo, sizeof(FInstanceBlendFrameInfo));
				}
			}
			BlendFrameBuffer->UnlockBuffers();
		}

		this->InstanceBuffer->UnlockBuffers();

		if (this->CIDBuffer)
			this->CIDBuffer->UnlockBuffers();
	}
	//////////////////////////////////////////////////////////////////////////
	template<typename TVisIndex> void FillElementsBuffer()
	{

#if ALLEGRO_USE_GPU_SCENE
		//nothing
#else

		void* MappedElementVB = this->ElementIndexBuffer->MappedData;

		for (uint32 MeshIdx = 0; MeshIdx < this->NumSubMesh; MeshIdx++)
		{
			if (!this->SubMeshes_Data[MeshIdx].bHasAnyLOD)
				continue;

			for (uint32 LODIndex = 0; LODIndex < ALLEGRO_MAX_LOD; LODIndex++)
			{
				const FLODData& LODData = this->SubMeshes_Data[MeshIdx].LODs[LODIndex];

				TVisIndex* DstMem = reinterpret_cast<TVisIndex*>(MappedElementVB) + LODData.InstanceOffset;
				LODData.IndexCollector.template CopyToArray<TVisIndex>(DstMem);

				for (auto& batch : LODData.BatchData)
				{
					DstMem = reinterpret_cast<TVisIndex*>(MappedElementVB) + batch.InstanceOffset;
					//batch.IndexCollector.template CopyToArray<TVisIndex>(DstMem);
					memcpy(DstMem, batch.DataArray.GetData(), batch.DataArray.Num());
				}
			}
		}
#endif

		this->ElementIndexBuffer->UnlockBuffers();
	}
	//////////////////////////////////////////////////////////////////////////
	
	virtual bool InitMeshLODData(uint32 SubMeshIdx, FProxyMeshDataBase** MeshDataBasePtr, uint8& CurrentFirstLODIdx, uint8& LODRenderData)
	{
		*MeshDataBasePtr = (FProxyMeshDataBase*)(&(this->Proxy->SubMeshes[SubMeshIdx]));

		FSubMeshInfo& GenSubMesh = this->SubMeshes_Info[SubMeshIdx];
		const FProxyMeshData& ProxySubMesh = this->Proxy->SubMeshes[SubMeshIdx];

		GenSubMesh.bIsValid = ProxySubMesh.SkeletalRenderData != nullptr;
		if (!GenSubMesh.bIsValid)
			return false;

		CurrentFirstLODIdx = ProxySubMesh.SkeletalRenderData->CurrentFirstLODIdx;
		LODRenderData = static_cast<uint8>((ProxySubMesh.SkeletalRenderData)->LODRenderData.Num());
		return true;
	}


	void InitLODData()
	{
		const float DistanceFactor = (1.0f / (GAllegro_DistanceScale * Proxy->DistanceScale));


		//init instance data
		{
			if (Proxy->InstanceMinDrawDistance > 0)
			{
				float MinDD = FMath::Square(Proxy->InstanceMinDrawDistance * DistanceFactor);
				this->MinDrawDist = *reinterpret_cast<uint32*>(&MinDD);
			}
			if(Proxy->InstanceMaxDrawDistance > 0)
			{
				float MaxDD = FMath::Square(Proxy->InstanceMaxDrawDistance * DistanceFactor);
				this->MaxDrawDist = *reinterpret_cast<uint32*>(&MaxDD);
			}

			for (int LODIndex = 0; LODIndex < ALLEGRO_MAX_LOD - 1; LODIndex++)
			{
				//we use our own global variables instead :|
				//const FCachedSystemScalabilityCVars& CVars = GetCachedScalabilityCVars();
				//CVars.ViewDistanceScale
				//float LDF = View->LODDistanceFactor;
				//float VS = CVars.ViewDistanceScale;

				float LODDrawDist = FMath::Square(Proxy->LODDistances[LODIndex] * DistanceFactor);
				LODDrawDistances[LODIndex] = *reinterpret_cast<uint32*>(&LODDrawDist);
			};
		}


		LODDrawDistances[ALLEGRO_MAX_LOD - 1] = ~0u;

		//init sub mesh data
		for (uint32 SubMeshIdx = 0; SubMeshIdx < NumSubMesh; SubMeshIdx++)
		{
			FSubMeshInfo& GenSubMesh = this->SubMeshes_Info[SubMeshIdx];
			FProxyMeshDataBase* ProxySubMesh = nullptr;

			uint8 CurrentFirstLODIdx = 0;
			uint8 LODRenderData = 1;

			if (!this->InitMeshLODData(SubMeshIdx, &ProxySubMesh, CurrentFirstLODIdx, LODRenderData))
			{
				continue;
			}

			if (ProxySubMesh->MaxDrawDistance > 0)
			{
				float flt = FMath::Square(ProxySubMesh->MaxDrawDistance * DistanceFactor);
				GenSubMesh.MaxDrawDist = *reinterpret_cast<uint32*>(&flt);
			}

			if (ProxySubMesh->OverrideDistance > 0)
			{
				float flt = FMath::Square(ProxySubMesh->OverrideDistance * DistanceFactor);
				GenSubMesh.OverrideDist = *reinterpret_cast<uint32*>(&flt);
				GenSubMesh.OverrideMeshIdx = ProxySubMesh->OverrideMeshIndex;
				check(GenSubMesh.OverrideMeshIdx != 0xFF);
			}

			
			const uint8 MinPossibleLOD = FMath::Max(CurrentFirstLODIdx, ProxySubMesh->MinLODIndex);
			const uint8 MaxPossibleLOD = LODRenderData - 1; 
			
			check(MaxPossibleLOD <= MaxPossibleLOD);

			uint8 MinLOD = MinPossibleLOD;
			uint8 MaxLOD = MaxPossibleLOD;

			if (GAllegro_ForceLOD >= 0)
			{
				MinLOD = MaxLOD = FMath::Clamp((uint8)GAllegro_ForceLOD, MinPossibleLOD, MaxPossibleLOD);
			}

			if (bShaddowCollector && GAllegro_ShadowForceLOD >= 0)	//GAllegro_ShadowForceLOD overrides GAllegro_ForceLOD if its collecting for shadow
			{
				MinLOD = MaxLOD = FMath::Clamp((uint8)GAllegro_ShadowForceLOD, MinPossibleLOD, MaxPossibleLOD);
			}
			
			for (uint8 LODIndex = 0; LODIndex < static_cast<uint8>(ALLEGRO_MAX_LOD); LODIndex++)
			{
				const uint8 MeshLODBias = bShaddowCollector ? (LODIndex >= this->Proxy->StartShadowLODBias ? this->Proxy->ShadowLODBias : 0) : 0;
				GenSubMesh.LODRemap[LODIndex] = FMath::Clamp(LODIndex + MeshLODBias, MinLOD, MaxLOD);
			}

		}


		ResolvedViewLocation = (FVector3f)View->CullingOrigin.GridSnap(16);	//OverrideLODViewOrigin ?
	}
	//////////////////////////////////////////////////////////////////////////
	void GenerateData()
	{
		InitLODData();

		const uint32 TotalInstances = Proxy->DynamicData->AliveInstanceCount;
		check(TotalInstances > 0);

		//find maximum amount of needed memory and allocate it at once
		{
			const uint32 ATI = Align(TotalInstances, DISTANCING_NUM_FLOAT_PER_REG);
			const uint32 Size_Distances = sizeof(uint32) * ATI * 2;
			const uint32 size_VisibleInstances = sizeof(uint32) * ATI * 2;
			check(MaxMeshPerInstance > 0);
			const uint32 ElementVBMaxPossibleSizeInBytes = TotalInstances * MaxMeshPerInstance * (TotalInstances > 0xFFFF ? 4u : 2u);
			const uint32 MaxPageNeeded = this->Proxy->MaxBatchCountPossible + (ElementVBMaxPossibleSizeInBytes / FIndexCollector::PAGE_DATA_SIZE_IN_BYTES) + 2;
			const uint32 SizePageMemory = sizeof(typename FIndexCollector::FPageData) * MaxPageNeeded;

			auto TotalBlockSize = Size_Distances + size_VisibleInstances + SizePageMemory + PLATFORM_CACHE_LINE_SIZE;

			this->MempoolPtr = this->MempoolSeek = (uint8*)FMemory::Malloc(TotalBlockSize);
			this->MempoolEnd = MempoolSeek + TotalBlockSize;

			this->VisibleInstances = (uint32*)this->MempoolSeek;
		}

		if (bShaddowCollector)	//is it collecting for shadow ?
		{
			//SCOPE_CYCLE_COUNTER(STAT_ALLEGRO_ShadowCullTime);
			this->EditedViewFrustum = View->GetDynamicMeshElementsShadowCullFrustum();
			const bool bShadowNeedTranslation = !View->GetPreShadowTranslation().IsZero();
			if (bShadowNeedTranslation)
			{
				auto NewPlanes = View->GetDynamicMeshElementsShadowCullFrustum()->Planes;
				//apply offset to the planes
				for (FPlane& P : NewPlanes)
					P.W = (P.GetOrigin() - View->GetPreShadowTranslation()) | P.GetNormal();

				//regenerate Permuted planes
				uint8* FrustumMem = this->StackAlloc(sizeof(FConvexVolume), alignof(FConvexVolume));
				this->EditedViewFrustum = new (FrustumMem) FConvexVolume(NewPlanes);
			}

			Cull();

			INC_DWORD_STAT_BY(STAT_ALLEGRO_ShadowNumCulled, TotalInstances - this->NumVisibleInstance);
		}
		else
		{
			//SCOPE_CYCLE_COUNTER(STAT_ALLEGRO_CullTime);
			EditedViewFrustum = &View->CullingFrustum;

			Cull();
			INC_DWORD_STAT_BY(STAT_ALLEGRO_ViewNumCulled, TotalInstances - this->NumVisibleInstance);
		}

		if (this->TotalElementCount == 0)
			return;

		if (bShaddowCollector)
		{
			INC_DWORD_STAT_BY(STAT_ALLEGRO_ShadowNumVisible, this->NumVisibleInstance);
		}
		else
		{
			INC_DWORD_STAT_BY(STAT_ALLEGRO_ViewNumVisible, this->NumVisibleInstance);
		}

		//allocate vertex buffers
		//#Note because PreRenderDelegateEx is called before InitShadow we can't share a global vertex buffer for all proxies :(
		{
			this->ElementIndexBuffer = GAllegroElementIndexBufferPool.Alloc(this->TotalElementCount * this->GetElementIndexSize());
			this->ElementIndexBuffer->LockBuffers();

			//need custom per instance float ?
			if (Proxy->NumCustomDataFloats > 0 && (!bShaddowCollector || Proxy->bNeedCustomDataForShadowPass))
			{
				this->CIDBuffer = GAllegroCIDBufferPool.Alloc(NumVisibleInstance * Proxy->NumCustomDataFloats);
				this->CIDBuffer->LockBuffers();
			}

			this->InstanceBuffer = GAllegroInstanceBufferPool.Alloc(this->NumVisibleInstance * (bShaddowCollector ? 1 : 2));	//#Note shadow pass doesn't need pref frame data
			this->InstanceBuffer->LockBuffers();

			uint32 NumBlendFrame = std::max(Proxy->DynamicData->NumBlendFrame, (Proxy->OldDynamicData)? Proxy->OldDynamicData->NumBlendFrame:0);
			
			if(NumBlendFrame > 1)
			{
				uint32 DataSize = NumBlendFrame * (Proxy->NumBlendFramePerInstance * 2 - 1);
				if (!bShaddowCollector)
				{
					BlendFrameBuffer = GAllegroBlendFrameBufferPool.Alloc(DataSize * 2); //cur + prev
				}
				else
				{
					BlendFrameBuffer = GAllegroBlendFrameBufferPool.Alloc(DataSize); //cur
				}

				BlendFrameBuffer->NumBlendFrame = NumBlendFrame;
				BlendFrameBuffer->LockBuffers();
			}
			else
			{
				BlendFrameBuffer.Reset();
			}
		}

		//fill mapped buffers
		{
			ALLEGRO_SCOPE_CYCLE_COUNTER(BufferFilling);

			if (bShaddowCollector)
				FillShadowBuffers();
			else
				FillBuffers();

			if (Use32BitElementIndex())
				FillElementsBuffer<uint32>();
			else
				FillElementsBuffer<uint16>();
		}

	}
	//////////////////////////////////////////////////////////////////////////
	void GenerateBatches()
	{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		this->bWireframe = AllowDebugViewmodes() && this->ViewFamily->EngineShowFlags.Wireframe;
		if (this->bWireframe)
		{
			this->WireframeMaterialInstance = new FColoredMaterialRenderProxy(GEngine->WireframeMaterial ? GEngine->WireframeMaterial->GetRenderProxy() : NULL, FLinearColor(0, 0.5f, 1.f));
			this->Collector->RegisterOneFrameMaterialProxy(WireframeMaterialInstance);
		}
#endif
		ALLEGRO_SCOPE_CYCLE_COUNTER(GenerateBatches);

		bool bSKM = this->Proxy->SubMeshes.Num() > 0;
		bool bSM = this->Proxy->SubStaticMeshes.Num() > 0;

		for (uint32 SubMeshIdx = 0; SubMeshIdx < this->NumSubMesh; SubMeshIdx++)
		{
			const FSubMeshData& SubMesh = this->SubMeshes_Data[SubMeshIdx];
			//if (!SubMesh.bHasAnyLOD)
			//	continue;

			//better to add opaque meshes from near to far ? :thinking:
			for (uint32 LODIndex = 0; LODIndex < ALLEGRO_MAX_LOD; LODIndex++)
			{
				const FLODData& Data = SubMesh.LODs[LODIndex];
				if (Data.NumInstance == 0 && Data.BatchData.Num() == 0 && Data.RunArrayInfo.Num() == 0)
					continue;

				if (bSKM)
				{
					if (!this->Proxy->SubMeshes[SubMeshIdx].LODs[LODIndex].bHasAnyTranslucentMaterial)
						GenerateLODBatch(SubMeshIdx, LODIndex, false);
				}
				else if (bSM)
				{
					if (!this->Proxy->SubStaticMeshes[SubMeshIdx].LODs[LODIndex].bHasAnyTranslucentMaterial)
						GenerateLODBatch(SubMeshIdx, LODIndex, false);
				}
			}

			//translucent need to be added from far to near
			for (uint32 LODIndex = ALLEGRO_MAX_LOD; LODIndex--; )
			{
				const FLODData& Data = SubMesh.LODs[LODIndex];
				if (Data.NumInstance == 0 && Data.BatchData.Num() == 0)
					continue;

				if (bSKM)
				{
					if (this->Proxy->SubMeshes[SubMeshIdx].LODs[LODIndex].bHasAnyTranslucentMaterial)
						GenerateLODBatch(SubMeshIdx, LODIndex, true);
				}
				else if (bSM)
				{
					if (this->Proxy->SubStaticMeshes[SubMeshIdx].LODs[LODIndex].bHasAnyTranslucentMaterial)
						GenerateLODBatch(SubMeshIdx, LODIndex, true);
				}
			}
		}
	};

	//////////////////////////////////////////////////////////////////////////

	virtual void GenerateLODBatchEx(uint32 SubMeshIdx, uint32 LODIndex, bool bAnyTranslucentMaterial,uint32 NumInstance,uint32 InstanceOffset,int16 Stencil, TArray<uint32, SceneRenderingAllocator>& RunArray)
	{
		if (NumInstance == 0 && RunArray.Num() == 0)
		{
			return;
		}

		const FProxyMeshData& ProxyMD = this->Proxy->SubMeshes[SubMeshIdx];
		const FSkeletalMeshLODRenderData& SkelLODData = ProxyMD.SkeletalRenderData->LODRenderData[LODIndex];
		const FProxyLODData& ProxyLODData = ProxyMD.LODs[LODIndex];

		//FAllegroVertexFactoryBufferRef UniformBuffer = this->CreateUniformBuffer(InstanceOffset, NumInstance);
		FAllegroBatchElementOFR* LastOFRS[(int)EAllegroVerteFactoryMode::EVF_Max] = {};

#if ALLEGRO_USE_GPU_SCENE
		FAllegroElementRunArrayOFR& RunArrayOFR = Collector->AllocateOneFrameResource<FAllegroElementRunArrayOFR>();
		RunArrayOFR.RunArray = MoveTemp(RunArray);
#endif
		//when all materials are the same, one FMeshBatch can be used for all passes, we draw them with MaxBoneInfluence of the LOD (will result the same for depth/material pass)
		//same MaxBoneInfleunce must be used for depth and material pass, different value causes depth mismatch, because of floating point precision, denorm-flush, ...

		//unify as whole if all materials are same, or just try merge depth batches if possible.

		const bool bTryUnifySections = GAllegro_DisableSectionsUnification == false;
		const bool bIdenticalMaterials = bTryUnifySections && ProxyLODData.bSameMaterials && ProxyLODData.bSameCastShadow && SkelLODData.RenderSections.Num() > 1 /*&& ProxyLODData.bSameMaxBoneInfluence*/;
		const int NumSection = bIdenticalMaterials ? 1 : SkelLODData.RenderSections.Num();
		const bool bUseUnifiedMeshForDepth = bTryUnifySections && NumSection > 1 && ProxyLODData.bMeshUnificationApplicable && ProxyLODData.bSameCastShadow /*&& ProxyLODData.bSameMaxBoneInfluence*/;
		for (int32 SectionIndex = 0; SectionIndex < NumSection; SectionIndex++) //for each section
		{
			const FSkelMeshRenderSection& SectionInfo = SkelLODData.RenderSections[SectionIndex];

			//int SolvedMaterialSection = SectionInfo.MaterialIndex;
			//const FSkeletalMeshLODInfo* SKMLODInfo = ProxyMD.SkeletalMesh->GetLODInfo(LODIndex);
			//if (SKMLODInfo && SKMLODInfo->LODMaterialMap.IsValidIndex(SectionInfo.MaterialIndex))
			//{
			//	SolvedMaterialSection = SKMLODInfo->LODMaterialMap[SectionInfo.MaterialIndex];
			//}
			const uint16 MaterialIndex = ProxyLODData.SectionsMaterialIndices[SectionIndex];
			FMaterialRenderProxy* MaterialProxy = bWireframe ? WireframeMaterialInstance : Proxy->MaterialsProxy[MaterialIndex];

			const uint32 MaxBoneInfluence = OverrideMaxBoneInfluence((bIdenticalMaterials || bUseUnifiedMeshForDepth) ? ProxyLODData.SectionsMaxBoneInfluence : SectionInfo.MaxBoneInfluences);
			check(MaxBoneInfluence > 0 && MaxBoneInfluence <= FAllegroMeshDataEx::MAX_INFLUENCE);

			const EAllegroVerteFactoryMode VFMode = GetTargetVFMode(MaxBoneInfluence);
			FAllegroBatchElementOFR*& BatchUserData = LastOFRS[VFMode];	//FAllegroBatchElementOFR with same MaxBoneInfluence can be shared for sections
			if (!BatchUserData)
			{
				BatchUserData = &Collector->AllocateOneFrameResource<FAllegroBatchElementOFR>();
				//initialize OFR
				BatchUserData->MaxBoneInfluences = MaxBoneInfluence;

				int NewLodIndex = LODIndex - ProxyMD.MeshDefBaseLOD;
				FStaticMeshVertexBuffers* AdditionalStaticMeshVB = nullptr;
				if (ProxyMD.AdditionalStaticRenderData)
				{
					if (LODIndex < (uint32)ProxyMD.AdditionalStaticRenderData->LODResources.Num())
					{
						AdditionalStaticMeshVB = &(ProxyMD.AdditionalStaticRenderData->LODResources[LODIndex].VertexBuffers);
					}
				}

				if (ProxyMD.PreSkinPostionOffset && AdditionalStaticMeshVB)
				{
					BatchUserData->VertexFactory = Proxy->GetVertexFactory(SubMeshIdx, LODIndex, 
						&(ProxyMD.MeshDataEx->LODs[NewLodIndex].BoneData), ProxyMD.MeshDataEx->LODs[NewLodIndex].SkelLODData, MaxBoneInfluence, AdditionalStaticMeshVB);
				}
				else
				{
					BatchUserData->VertexFactory = ProxyMD.MeshDataEx->LODs[NewLodIndex].GetVertexFactory(MaxBoneInfluence);
				}
				
				BatchUserData->UniformBuffer = this->CreateUniformBuffer(InstanceOffset, NumInstance, NewLodIndex);//UniformBuffer;
			}

			// Draw the mesh.
			FMeshBatch& Mesh = AllocateMeshBatch(SkelLODData, SubMeshIdx, LODIndex, SectionIndex, BatchUserData, NumInstance);
#if	ALLEGRO_USE_STENCIL
			Mesh.Stencil = Stencil;
#endif
			FMeshBatchElement& BatchElement = Mesh.Elements[0];
			Mesh.bWireframe = bWireframe;
			Mesh.MaterialRenderProxy = MaterialProxy;

			if (bUseUnifiedMeshForDepth)
			{
				Mesh.bUseAsOccluder = Mesh.bUseForDepthPass = Mesh.CastShadow = false; //this batch must be material only
			}
			else
			{
				Mesh.bUseForDepthPass = Proxy->ShouldRenderInDepthPass();
				Mesh.bUseAsOccluder = Proxy->ShouldUseAsOccluder();
				Mesh.CastShadow = SectionInfo.bCastShadow;
			}

			if (bIdenticalMaterials)
			{
				BatchElement.FirstIndex = 0;
				BatchElement.NumPrimitives = OverrideNumPrimitive(ProxyLODData.SectionsNumTriangle);
				BatchElement.MinVertexIndex = 0;
				BatchElement.MaxVertexIndex = SkelLODData.GetNumVertices() - 1; //SectionInfo.GetVertexBufferIndex() + SectionInfo.GetNumVertices() - 1; //
			}
			else
			{
				BatchElement.FirstIndex = SectionInfo.BaseIndex;
				BatchElement.NumPrimitives = OverrideNumPrimitive(SectionInfo.NumTriangles);
				BatchElement.MinVertexIndex = SectionInfo.BaseVertexIndex;
				BatchElement.MaxVertexIndex = SkelLODData.GetNumVertices() - 1; //SectionInfo.GetVertexBufferIndex() + SectionInfo.GetNumVertices() - 1; //
			}

#if ALLEGRO_USE_GPU_SCENE
			if (RunArrayOFR.RunArray.Num() > 0)
			{
				BatchElement.NumInstances = RunArrayOFR.RunArray.Num() / 2;
				BatchElement.InstanceRuns = &RunArrayOFR.RunArray[0];
				BatchElement.bIsInstanceRuns = true;
			}
#endif
			//BatchElement.InstancedLODIndex = LODIndex;
			Collector->AddMesh(ViewIndex, Mesh);
		}

		if (bUseUnifiedMeshForDepth)
		{
			const uint32 MaxBoneInfluence = OverrideMaxBoneInfluence(ProxyLODData.SectionsMaxBoneInfluence);
			const EAllegroVerteFactoryMode VFMode = GetTargetVFMode(MaxBoneInfluence);
			FAllegroBatchElementOFR*& BatchUserData = LastOFRS[VFMode];
			check(BatchUserData);

			FMeshBatch& Mesh = AllocateMeshBatch(SkelLODData, SubMeshIdx, LODIndex, 0, BatchUserData, NumInstance);
#if	ALLEGRO_USE_STENCIL
			Mesh.Stencil = Stencil;
#endif
			FMeshBatchElement& BatchElement = Mesh.Elements[0];
			Mesh.bUseForMaterial = false;
			Mesh.bUseForDepthPass = Proxy->ShouldRenderInDepthPass();
			Mesh.bUseAsOccluder = Proxy->ShouldUseAsOccluder();
			Mesh.CastShadow = ProxyLODData.bAllSectionsCastShadow;
			Mesh.MaterialRenderProxy = UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();
			BatchElement.FirstIndex = 0;
			BatchElement.NumPrimitives = OverrideNumPrimitive(ProxyLODData.SectionsNumTriangle);
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = SkelLODData.GetNumVertices() - 1;

#if ALLEGRO_USE_GPU_SCENE
			if (RunArrayOFR.RunArray.Num() > 0)
			{
				BatchElement.NumInstances = RunArrayOFR.RunArray.Num() / 2;
				BatchElement.InstanceRuns = &RunArrayOFR.RunArray[0];
				BatchElement.bIsInstanceRuns = true;
			}
#endif
			Collector->AddMesh(ViewIndex, Mesh);
		}
	}

	void GenerateLODBatch(uint32 SubMeshIdx, uint32 LODIndex, bool bAnyTranslucentMaterial)
	{
		FLODData& LodData = this->SubMeshes_Data[SubMeshIdx].LODs[LODIndex];

#if ALLEGRO_USE_GPU_SCENE
		for (int i = 0; i < LodData.RunArrayInfo.Num(); ++i)
		{
			auto& Info = LodData.RunArrayInfo[i];
			Info.Handle();

			GenerateLODBatchEx(SubMeshIdx, LODIndex, bAnyTranslucentMaterial, 0, 0, Info.Stencil, Info.RunArray);
		}
#else
		TArray<uint32, SceneRenderingAllocator> RunArray;
		GenerateLODBatchEx(SubMeshIdx, LODIndex, bAnyTranslucentMaterial, LodData.NumInstance, LodData.InstanceOffset, -1, RunArray);
		for (auto& data : LodData.BatchData)
		{
			GenerateLODBatchEx(SubMeshIdx, LODIndex, bAnyTranslucentMaterial, data.NumInstance, data.InstanceOffset,data.Stencil, RunArray);
		}
#endif
	}
};



template<bool bShaddowCollector>
struct FAllegroStaticMultiMeshGenerator : public FAllegroMultiMeshGenerator<bShaddowCollector>
{

	FAllegroStaticMultiMeshGenerator(const FAllegroProxy* InProxy, const FSceneViewFamily* InViewFamily, const FSceneView* InView, FMeshElementCollector* InCollector, int InViewIndex, int SubMeshNum)
		:FAllegroMultiMeshGenerator<bShaddowCollector>(InProxy, InViewFamily, InView, InCollector, InViewIndex, SubMeshNum)
	{
		
	}

	bool InitMeshLODData(uint32 SubMeshIdx, FProxyMeshDataBase** MeshDataBasePtr, uint8& CurrentFirstLODIdx, uint8& LODRenderData) override
	{
		*MeshDataBasePtr = (FProxyMeshDataBase*)(&(this->Proxy->SubStaticMeshes[SubMeshIdx]));
		auto& GenSubMesh = this->SubMeshes_Info[SubMeshIdx];

		const FProxyStaticMeshData& ProxySubMesh = this->Proxy->SubStaticMeshes[SubMeshIdx];

		GenSubMesh.bIsValid = ProxySubMesh.StaticMeshData != nullptr;
		if (!GenSubMesh.bIsValid)
			return false;

		CurrentFirstLODIdx = ProxySubMesh.StaticMeshData->CurrentFirstLODIdx;
		LODRenderData = static_cast<uint8>((ProxySubMesh.StaticMeshData)->LODResources.Num());

		return true;
	}

	void UpdateLODLevel(int NumCalc) override
	{
		TArray<FProxyMeshDataBase*> MeshDataBaseArray;
		MeshDataBaseArray.Reset(this->Proxy->SubStaticMeshes.Num());
		for (int i = 0; i < this->Proxy->SubStaticMeshes.Num(); ++i)
		{
			auto& Ref = MeshDataBaseArray.AddDefaulted_GetRef();
			Ref = &(this->Proxy->SubStaticMeshes[i]);
		}
		this->UpdateLODLevelImpl(MeshDataBaseArray, this->VisibleInstanceLODLevel, NumCalc);
	}


	FMeshBatch& AllocateStaticMeshBatch(const FStaticMeshLODResources& StaticMeshLODData, uint32 SubMeshIndex, uint32 LODIndex, uint32 SectionIndex,
		FAllegroBatchElementOFR* BatchUserData, int32 NumInstances)
	{
		FMeshBatch& Mesh = this->Collector->AllocateMesh();
		Mesh.ReverseCulling = false;//IsLocalToWorldDeterminantNegative();
		Mesh.Type = PT_TriangleList;
		Mesh.DepthPriorityGroup = this->Proxy->GetDepthPriorityGroup(this->View);
		Mesh.bCanApplyViewModeOverrides = true;
		Mesh.bSelectable = false;
		Mesh.bUseForMaterial = true;
		Mesh.bUseSelectionOutline = false;
		Mesh.LODIndex = static_cast<int8>(LODIndex);	//?
		Mesh.SegmentIndex = static_cast<uint8>(SectionIndex);
		//its useless, MeshIdInPrimitive is set by Collector->AddMesh()
		//Mesh.MeshIdInPrimitive = static_cast<uint16>(LODIndex); //static_cast<uint16>((LODIndex << 8) | SectionIndex);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		Mesh.VisualizeLODIndex = static_cast<int8>(LODIndex);
#endif
		Mesh.VertexFactory = BatchUserData->VertexFactory;

		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.UserData = BatchUserData;

#if ALLEGRO_USE_GPU_SCENE

#else
		BatchElement.PrimitiveIdMode = PrimID_ForceZero;
#endif

		BatchElement.IndexBuffer = &StaticMeshLODData.IndexBuffer;
		BatchElement.UserIndex = 0;
		//BatchElement.PrimitiveUniformBufferResource = &PrimitiveUniformBuffer->UniformBuffer; //&GIdentityPrimitiveUniformBuffer; 
		BatchElement.PrimitiveUniformBuffer = this->Proxy->GetUniformBuffer();

		BatchElement.NumInstances = NumInstances; // this->SubMeshes_Data[SubMeshIndex].LODs[LODIndex].NumInstance;
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		BatchElement.VisualizeElementIndex = static_cast<int32>(SectionIndex);
#endif

		return Mesh;
	}

	void GenerateLODBatchEx(uint32 SubMeshIdx, uint32 LODIndex, bool bAnyTranslucentMaterial, uint32 NumInstance, uint32 InstanceOffset, int16 Stencil, TArray<uint32, SceneRenderingAllocator>& RunArray) override
	{
		if (NumInstance == 0 && RunArray.Num() == 0)
		{
			return;
		}

		const FProxyStaticMeshData& ProxyMD = this->Proxy->SubStaticMeshes[SubMeshIdx];
		const FStaticMeshLODResources& LODDataResource = ProxyMD.StaticMeshData->LODResources[LODIndex];
		const FProxyLODData& ProxyLODData = ProxyMD.LODs[LODIndex];

		//FAllegroVertexFactoryBufferRef UniformBuffer = this->CreateUniformBuffer(InstanceOffset, NumInstance,0, GNullVertexBuffer.VertexBufferSRV);
		FAllegroBatchElementOFR* LastOFRS[(int)EAllegroVerteFactoryMode::EVF_Max] = {};

#if ALLEGRO_USE_GPU_SCENE
		FAllegroElementRunArrayOFR& RunArrayOFR = this->Collector->AllocateOneFrameResource<FAllegroElementRunArrayOFR>();
		RunArrayOFR.RunArray = MoveTemp(RunArray);
#endif
		const bool bTryUnifySections = GAllegro_DisableSectionsUnification == false;
		const bool bIdenticalMaterials = bTryUnifySections && ProxyLODData.bSameMaterials && ProxyLODData.bSameCastShadow && LODDataResource.Sections.Num() > 1;
		const int NumSection = bIdenticalMaterials ? 1 : LODDataResource.Sections.Num();
		const bool bUseUnifiedMeshForDepth = bTryUnifySections && NumSection > 1 && ProxyLODData.bMeshUnificationApplicable && ProxyLODData.bSameCastShadow;
		
		for (int32 SectionIndex = 0; SectionIndex < NumSection; SectionIndex++) //for each section
		{
			const FStaticMeshSection& SectionInfo = LODDataResource.Sections[SectionIndex];
			const uint16 MaterialIndex = ProxyLODData.SectionsMaterialIndices[SectionIndex];
			FMaterialRenderProxy* MaterialProxy = this->bWireframe ? this->WireframeMaterialInstance : this->Proxy->MaterialsProxy[MaterialIndex];

			const uint32 MaxBoneInfluence = 0;
			const EAllegroVerteFactoryMode VFMode = this->GetTargetVFMode(MaxBoneInfluence);
			FAllegroBatchElementOFR*& BatchUserData = LastOFRS[VFMode];
			if (!BatchUserData)
			{
				BatchUserData = &this->Collector->AllocateOneFrameResource<FAllegroBatchElementOFR>();
				BatchUserData->MaxBoneInfluences = MaxBoneInfluence;
		
				int NewLodIndex = LODIndex - ProxyMD.MeshDefBaseLOD;
				FStaticMeshVertexBuffers* AdditionalStaticMeshVB = nullptr;
				if (ProxyMD.AdditionalStaticRenderData)
				{
					if (LODIndex < (uint32)ProxyMD.AdditionalStaticRenderData->LODResources.Num())
					{
						AdditionalStaticMeshVB = &(ProxyMD.AdditionalStaticRenderData->LODResources[LODIndex].VertexBuffers);
					}
				}

				BatchUserData->VertexFactory = this->Proxy->GetStaticVertexFactory(SubMeshIdx, LODIndex,
					 &LODDataResource, ProxyMD.PreSkinPostionOffset?AdditionalStaticMeshVB:nullptr);

				BatchUserData->UniformBuffer = this->CreateUniformBuffer(InstanceOffset, NumInstance, NewLodIndex); //UniformBuffer;
			}

			FMeshBatch& Mesh = AllocateStaticMeshBatch(LODDataResource, SubMeshIdx, LODIndex, SectionIndex, BatchUserData, NumInstance);
#if	ALLEGRO_USE_STENCIL
			Mesh.Stencil = Stencil;
#endif
			FMeshBatchElement& BatchElement = Mesh.Elements[0];
			Mesh.bWireframe = this->bWireframe;
			Mesh.MaterialRenderProxy = MaterialProxy;

			if (bUseUnifiedMeshForDepth)
			{
				Mesh.bUseAsOccluder = Mesh.bUseForDepthPass = Mesh.CastShadow = false; //this batch must be material only
			}
			else
			{
				Mesh.bUseForDepthPass = this->Proxy->ShouldRenderInDepthPass();
				Mesh.bUseAsOccluder = this->Proxy->ShouldUseAsOccluder();
				Mesh.CastShadow = SectionInfo.bCastShadow;
			}

			if (bIdenticalMaterials)
			{
				BatchElement.FirstIndex = 0;
				BatchElement.NumPrimitives = this->OverrideNumPrimitive(ProxyLODData.SectionsNumTriangle);
				BatchElement.MinVertexIndex = SectionInfo.MinVertexIndex;
				BatchElement.MaxVertexIndex = SectionInfo.MaxVertexIndex; //SectionInfo.GetVertexBufferIndex() + SectionInfo.GetNumVertices() - 1; //
			}
			else
			{
				BatchElement.FirstIndex = SectionInfo.FirstIndex;
				BatchElement.NumPrimitives = this->OverrideNumPrimitive(SectionInfo.NumTriangles);
				BatchElement.MinVertexIndex = SectionInfo.MinVertexIndex;
				BatchElement.MaxVertexIndex = SectionInfo.MaxVertexIndex; //SectionInfo.GetVertexBufferIndex() + SectionInfo.GetNumVertices() - 1; //

			}

#if ALLEGRO_USE_GPU_SCENE
			if (RunArrayOFR.RunArray.Num() > 0)
			{
				BatchElement.NumInstances = RunArrayOFR.RunArray.Num() / 2;
				BatchElement.InstanceRuns = &RunArrayOFR.RunArray[0];
				BatchElement.bIsInstanceRuns = true;
			}
#endif
			this->Collector->AddMesh(this->ViewIndex, Mesh);
		}

		if (bUseUnifiedMeshForDepth)
		{
			const uint32 MaxBoneInfluence = 0;// OverrideMaxBoneInfluence(ProxyLODData.SectionsMaxBoneInfluence);
			const EAllegroVerteFactoryMode VFMode = this->GetTargetVFMode(MaxBoneInfluence);
			FAllegroBatchElementOFR*& BatchUserData = LastOFRS[VFMode];
			check(BatchUserData);

			FMeshBatch& Mesh = AllocateStaticMeshBatch(LODDataResource, SubMeshIdx, LODIndex, 0, BatchUserData, NumInstance);
#if	ALLEGRO_USE_STENCIL
			Mesh.Stencil = Stencil;
#endif
			FMeshBatchElement& BatchElement = Mesh.Elements[0];
			Mesh.bUseForMaterial = false;
			Mesh.bUseForDepthPass = this->Proxy->ShouldRenderInDepthPass();
			Mesh.bUseAsOccluder = this->Proxy->ShouldUseAsOccluder();
			Mesh.CastShadow = ProxyLODData.bAllSectionsCastShadow;
			Mesh.MaterialRenderProxy = UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();
			BatchElement.FirstIndex = 0;
			BatchElement.NumPrimitives = this->OverrideNumPrimitive(ProxyLODData.SectionsNumTriangle);
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = LODDataResource.GetNumVertices()-1;//SkelLODData.GetNumVertices() - 1;

#if ALLEGRO_USE_GPU_SCENE
			if (RunArrayOFR.RunArray.Num() > 0)
			{
				BatchElement.NumInstances = RunArrayOFR.RunArray.Num() / 2;
				BatchElement.InstanceRuns = &RunArrayOFR.RunArray[0];
				BatchElement.bIsInstanceRuns = true;
			}
#endif

			this->Collector->AddMesh(this->ViewIndex, Mesh);
		}

	}

};




