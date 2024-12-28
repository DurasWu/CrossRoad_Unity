// Copyright 2024 Lazy Marmot Games. All Rights Reserved.


#pragma once


#include "Engine/SkeletalMesh.h"
#include "VertexFactory.h"
#include "AllegroBase.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "RenderResource.h"
#include "SceneManagement.h"
#include "Allegro.h"
#include "AllegroResourcePool.h"
#include "StaticMeshResources.h"


class FAllegroSkinWeightVertexBuffer;
class UAllegroAnimCollection;
class FSkeletalMeshLODRenderData;
class FAllegroBoneIndexVertexBuffer;
struct FStaticMeshVertexBuffers;

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FAllegroVertexFactoryParameters, )
SHADER_PARAMETER(uint32, LODLevel)
SHADER_PARAMETER(uint32, BoneCount)
SHADER_PARAMETER(uint32, InstanceOffset)
SHADER_PARAMETER(uint32, InstanceEndOffset)
SHADER_PARAMETER(uint32, NumCustomDataFloats)
SHADER_PARAMETER_SRV(Buffer<float4>, AnimationBuffer)
SHADER_PARAMETER_SRV(Buffer<float4>, Instance_Transforms)
SHADER_PARAMETER_SRV(Buffer<uint>, Instance_AnimationFrameIndices)
SHADER_PARAMETER_SRV(Buffer<float>, Instance_CustomData)
SHADER_PARAMETER_SRV(Buffer<uint>, ElementIndices)
SHADER_PARAMETER_SRV(Buffer<uint>, Instance_BlendFrameIndex)
SHADER_PARAMETER_SRV(Buffer<float>, Instance_BlendFrameBuffer)
END_GLOBAL_SHADER_PARAMETER_STRUCT()

typedef TUniformBufferRef<FAllegroVertexFactoryParameters> FAllegroVertexFactoryBufferRef;


/*
*/
class FAllegroBaseVertexFactory : public FVertexFactory
{
public:
	typedef FVertexFactory Super;

	//DECLARE_VERTEX_FACTORY_TYPE(FAllegroBaseVertexFactory);

	struct FDataType : FStaticMeshDataType
	{
		/** The stream to read the bone indices from */
		FVertexStreamComponent BoneIndices;
		/** The stream to read the bone weights from */
		FVertexStreamComponent BoneWeights;

		FVertexStreamComponent ExtraBoneIndices;
		FVertexStreamComponent ExtraBoneWeights;

		FVertexStreamComponent PreSkinPostionOffset;  //extend
	};

	static FAllegroBaseVertexFactory* New(int InMaxBoneInfluence, bool PreSkinPostionOffset);

	FAllegroBaseVertexFactory(ERHIFeatureLevel::Type InFeatureLevel) : Super(InFeatureLevel)
	{
	}
	~FAllegroBaseVertexFactory()
	{
	}
	void FillData(FDataType& data, const FAllegroBoneIndexVertexBuffer* BoneIndexBuffer, const FSkeletalMeshLODRenderData* LODData, FStaticMeshVertexBuffers* AdditionalStaticMeshVB) const;
	
	void FillDataForStaticMesh(FDataType& data, const FStaticMeshLODResources* LODData,FStaticMeshVertexBuffers* AdditionalStaticMeshVB);

	void SetData(const FDataType& data);

	FString GetFriendlyName() const override { return TEXT("FAllegroBaseVertexFactory"); }

	uint32 MaxBoneInfluence;
};

enum EAllegroVerteFactoryMode
{
	EVF_BoneInfluence0,
	EVF_BoneInfluence1,
	EVF_BoneInfluence2,
	EVF_BoneInfluence3,
	EVF_BoneInfluence4,

	EVF_BoneInfluence5,
	EVF_BoneInfluence6,
	EVF_BoneInfluence7,
	EVF_BoneInfluence8,

	EVF_Max,
};


template<EAllegroVerteFactoryMode FactoryMode,bool PreSkinPostionOffset = false> 
class TAllegroVertexFactory : public FAllegroBaseVertexFactory
{
	
	DECLARE_VERTEX_FACTORY_TYPE(TAllegroVertexFactory);
	
	typedef FAllegroBaseVertexFactory Super;

	TAllegroVertexFactory(ERHIFeatureLevel::Type InFeatureLevel) : Super(InFeatureLevel)
	{
		this->MaxBoneInfluence = StaticMaxBoneInfluence();
	}
	static uint32 StaticMaxBoneInfluence()
	{
		return ((int)FactoryMode);  // + 1;
	}

	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
	static bool SupportsTessellationShaders() { return false; }
	static void ValidateCompiledResult(const FVertexFactoryType* Type, EShaderPlatform Platform, const FShaderParameterMap& ParameterMap, TArray<FString>& OutErrors);
};


class FAllegroBoneIndexVertexBuffer : public FVertexBuffer
{
public:
	FStaticMeshVertexDataInterface* BoneData = nullptr;
	bool bIs16BitBoneIndex = false;
	int MaxBoneInfluences = 0;
	int NumVertices = 0;

	~FAllegroBoneIndexVertexBuffer()
	{
		if (BoneData)
		{
			delete BoneData;
			BoneData = nullptr;
		}
	}

	void Serialize(FArchive& Ar);

	void InitRHI(FRHICommandListBase& RHICmdList) override;
	void ReleaseRHI() override;
	
	void ResizeBuffer()
	{
		if(!BoneData)
			BoneData = new TStaticMeshVertexData<uint8>();

		BoneData->ResizeBuffer(NumVertices * MaxBoneInfluences * (bIs16BitBoneIndex ? 2 : 1));
		FMemory::Memzero(BoneData->GetDataPointer(), BoneData->GetResourceSize());
	}
	void SetBoneIndex(uint32 VertexIdx, uint32 InfluenceIdx, uint32 BoneIdx)
	{
		if(bIs16BitBoneIndex)
		{
			FBoneIndex16* Data = ((FBoneIndex16*)BoneData->GetDataPointer());
			Data[VertexIdx * MaxBoneInfluences + InfluenceIdx] = static_cast<FBoneIndex16>(BoneIdx);
		}
		else
		{
			FBoneIndex8* Data = ((FBoneIndex8*)BoneData->GetDataPointer());
			Data[VertexIdx * MaxBoneInfluences + InfluenceIdx] = static_cast<FBoneIndex8>(BoneIdx);
		}
	}

	uint32 GetBufferSizeInBytes() const
	{
		return NumVertices * MaxBoneInfluences * (bIs16BitBoneIndex ? 2u : 1u);
	}
	
};

//vertex buffer containing bone transforms of all baked animations
class FAllegroAnimationBuffer : public FRenderResource
{
public:
	FStaticMeshVertexDataInterface* Transforms = nullptr;

	FBufferRHIRef Buffer;
	FShaderResourceViewRHIRef ShaderResourceViewRHI;
	FUnorderedAccessViewRHIRef UAV;
	bool bHighPrecision = false;
	
	~FAllegroAnimationBuffer();
	void InitRHI(FRHICommandListBase& RHICmdList) override;
	void ReleaseRHI() override;
	void Serialize(FArchive& Ar);

	void AllocateBuffer();
	void InitBuffer(const TArrayView<FTransform> InTransforms, bool InHightPrecision);
	void InitBuffer(uint32 NumMatrix, bool InHightPrecision, bool bFillIdentity);
	void DestroyBuffer();

	FMatrix3x4* GetDataPointerHP() const;
	FMatrix3x4Half* GetDataPointerLP() const;

	//void SetPoseTransform(uint32 PoseIndex, uint32 BoneCount, const FTransform* BoneTransforms);
	//void SetPoseTransform(uint32 PoseIndex, uint32 BoneCount, const FMatrix3x4* BoneTransforms);

};


/*
*/
class FAllegroMeshDataEx :  public TSharedFromThis<FAllegroMeshDataEx>
{
public:
	static const int MAX_INFLUENCE = 8;

	struct FLODData
	{
		FAllegroBoneIndexVertexBuffer BoneData;
		TUniquePtr<FAllegroBaseVertexFactory> VertexFactories[MAX_INFLUENCE + 1];
		const FSkeletalMeshLODRenderData* SkelLODData = nullptr;

		FLODData(ERHIFeatureLevel::Type InFeatureLevel);
		FAllegroBaseVertexFactory* GetVertexFactory(int MaxBoneInfluence);
		void InitResources(FRHICommandListBase& RHICmdList);
		void ReleaseResources();

		void Serialize(FArchive& Ar);
	};

	//accessed by [SkeletalMeshLODIndex - BaseLOD]
	TArray<FLODData, TFixedAllocator<ALLEGRO_MAX_LOD>> LODs;

	#if WITH_EDITOR
	void InitFromMesh(int InBaseLOD, USkeletalMesh* SKMesh, const UAllegroAnimCollection* AnimSet);
	#endif
	void InitResources(FRHICommandListBase& RHICmdList);
	void ReleaseResouces();
	void Serialize(FArchive& Ar);

	void InitMeshData(const FSkeletalMeshRenderData* SKRenderData, int InBaseLOD);

	uint32 GetTotalBufferSize() const;

};

typedef TSharedPtr<FAllegroMeshDataEx, ESPMode::ThreadSafe> FAllegroMeshDataExPtr;


struct FAllegroInstanceBuffer : TSharedFromThis<FAllegroInstanceBuffer>
{
	static const uint32 SizeAlign = 4096;	//must be pow2	InstanceCount is aligned to this 

	FBufferRHIRef TransformVB;
	FShaderResourceViewRHIRef TransformSRV;

	FBufferRHIRef FrameIndexVB;
	FShaderResourceViewRHIRef FrameIndexSRV;

	FBufferRHIRef BlendFrameIndexVB;
	FShaderResourceViewRHIRef BlendFrameIndexmSRV;

	uint32 CreationFrameNumber = 0;
	uint32 InstanceCount = 0;

	AllegroShaderMatrixT* MappedTransforms = nullptr;
	uint32* MappedFrameIndices = nullptr;
	uint32* MappedBlendFrameIndices = nullptr;

	void LockBuffers();
	void UnlockBuffers();
	bool IsLocked() const { return MappedTransforms != nullptr; }
	uint32 GetSize() const { return InstanceCount; }

	static TSharedPtr<FAllegroInstanceBuffer> Create(uint32 InstanceCount);
};
typedef TSharedPtr<FAllegroInstanceBuffer> FAllegroInstanceBufferPtr;




typedef TBufferAllocatorSingle<FAllegroInstanceBufferPtr> FAllegroInstanceBufferAllocator;
extern FAllegroInstanceBufferAllocator GAllegroInstanceBufferPool;



struct FAllegroCIDBuffer : TSharedFromThis<FAllegroCIDBuffer>
{
	static const uint32 SizeAlign = 4096;	//must be pow2	InstanceCount is aligned to this 

	FBufferRHIRef CustomDataBuffer;
	FShaderResourceViewRHIRef CustomDataSRV;
	
	uint32 CreationFrameNumber = 0;
	uint32 NumberOfFloat = 0;

	float* MappedData = nullptr;

	void LockBuffers();
	void UnlockBuffers();
	bool IsLocked() const { return MappedData != nullptr; }
	uint32 GetSize() const { return NumberOfFloat; }

	static TSharedPtr<FAllegroCIDBuffer> Create(uint32 InNumberOfFloat);
};
typedef TSharedPtr<FAllegroCIDBuffer> FAllegroCIDBufferPtr;

typedef TBufferAllocatorSingle<FAllegroCIDBufferPtr> FAllegroCIDBufferAllocator;
extern FAllegroCIDBufferAllocator GAllegroCIDBufferPool;


struct FAllegroBlendFrameBuffer : TSharedFromThis<FAllegroBlendFrameBuffer>
{
	static const uint32 SizeAlign = 4096;	

	FBufferRHIRef BlendFrameDataBuffer;
	FShaderResourceViewRHIRef BlendFrameDataSRV;

	float* MappedData = nullptr;
	uint32 NumberOfFloat = 0;

	uint32 NumBlendFrame = 0;

	void LockBuffers();
	void UnlockBuffers();
	bool IsLocked() const { return MappedData != nullptr; }
	uint32 GetSize() const { return NumberOfFloat; }

	static TSharedPtr<FAllegroBlendFrameBuffer> Create(uint32 NumOfFloat);
};
typedef TSharedPtr<FAllegroBlendFrameBuffer> FAllegroBlendFrameBufferPtr;


typedef TBufferAllocatorSingle<FAllegroBlendFrameBufferPtr> FAllegroBlendFrameBufferAllocator;
extern FAllegroBlendFrameBufferAllocator GAllegroBlendFrameBufferPool;

struct FAllegroBatchElementOFR : FOneFrameResource
{
	uint32 MaxBoneInfluences;
	FAllegroBaseVertexFactory* VertexFactory;
	FAllegroVertexFactoryBufferRef UniformBuffer;	//uniform buffer holding data for drawing a LOD
};


struct FAllegroProxyOFR : FOneFrameResource
{
	FAllegroCIDBufferPtr CDIShadowBuffer;

	~FAllegroProxyOFR();
};



struct FAllegroElementIndexBuffer : TSharedFromThis<FAllegroElementIndexBuffer>
{
	static const uint32 SizeAlign = 4096;

	FBufferRHIRef ElementIndexBuffer;
	FShaderResourceViewRHIRef ElementIndexUIN16SRV;
	FShaderResourceViewRHIRef ElementIndexUIN32SRV;
	uint32 CreationFrameNumber = 0;
	uint32 SizeInBytes = 0;

	void* MappedData = nullptr;

	void LockBuffers();
	void UnlockBuffers();
	bool IsLocked() const { return MappedData != nullptr; }
	uint32 GetSize() const { return SizeInBytes; }

	static TSharedPtr<FAllegroElementIndexBuffer> Create(uint32 InSizeInBytes);
};

typedef TSharedPtr<FAllegroElementIndexBuffer> FAllegroElementIndexBufferPtr;
typedef TBufferAllocatorSingle<FAllegroElementIndexBufferPtr> FAllegroElementIndexBufferAllocator;

extern FAllegroElementIndexBufferAllocator GAllegroElementIndexBufferPool;