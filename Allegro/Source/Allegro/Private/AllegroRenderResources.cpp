// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroRenderResources.h"
#include "AllegroRender.h"
#include "AllegroAnimCollection.h"
#include "MeshDrawShaderBindings.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Allegro.h"
#include "RenderUtils.h"
#include "ShaderCore.h"
#include "MeshMaterialShader.h"
#include "MaterialDomain.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalRenderResources.h"
#include "AllegroPrivateUtils.h"
#include "Animation/Skeleton.h"
#include "StaticMeshResources.h"

FAllegroBaseVertexFactory* FAllegroBaseVertexFactory::New(int InMaxBoneInfluence, bool PreSkinPostionOffset)
{
	if (PreSkinPostionOffset)
	{
		switch (InMaxBoneInfluence)
		{
			case 0: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence0, true>(GMaxRHIFeatureLevel);
			case 1: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence1, true>(GMaxRHIFeatureLevel);
			case 2: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence2, true>(GMaxRHIFeatureLevel);
			case 3: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence3, true>(GMaxRHIFeatureLevel);
			case 4: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence4, true>(GMaxRHIFeatureLevel);
			case 5: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence5, true>(GMaxRHIFeatureLevel);
			case 6: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence6, true>(GMaxRHIFeatureLevel);
			case 7: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence7, true>(GMaxRHIFeatureLevel);
			case 8: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence8, true>(GMaxRHIFeatureLevel);
		};
	}
	else
	{
		switch (InMaxBoneInfluence)
		{
			case 0: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence0, false>(GMaxRHIFeatureLevel);
			case 1: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence1, false>(GMaxRHIFeatureLevel);
			case 2: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence2, false>(GMaxRHIFeatureLevel);
			case 3: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence3, false>(GMaxRHIFeatureLevel);
			case 4: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence4, false>(GMaxRHIFeatureLevel);
			case 5: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence5, false>(GMaxRHIFeatureLevel);
			case 6: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence6, false>(GMaxRHIFeatureLevel);
			case 7: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence7, false>(GMaxRHIFeatureLevel);
			case 8: return new TAllegroVertexFactory<EAllegroVerteFactoryMode::EVF_BoneInfluence8, false>(GMaxRHIFeatureLevel);
		};
	}
	check(false);
	return nullptr;
}

void FAllegroBaseVertexFactory::FillData(FDataType& data, const FAllegroBoneIndexVertexBuffer* BoneIndexBuffer, const FSkeletalMeshLODRenderData* LODData, FStaticMeshVertexBuffers* AdditionalStaticMeshVB) const
{
	const FStaticMeshVertexBuffers& smvb = LODData->StaticVertexBuffers;

	smvb.PositionVertexBuffer.BindPositionVertexBuffer(this, data);
	smvb.StaticMeshVertexBuffer.BindTangentVertexBuffer(this, data);
	smvb.StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(this, data);
	smvb.ColorVertexBuffer.BindColorVertexBuffer(this, data);

	//const uint32 weightOffset = sizeof(uint8[4]);
	//data.BoneIndices = FVertexStreamComponent(weightBuffer, 0, stride, VET_UByte4);
	//data.BoneWeights = FVertexStreamComponent(weightBuffer, weightOffset, stride, VET_UByte4N);
	//see InitGPUSkinVertexFactoryComponents we can take weights from the mesh since we just have different bone indices
	
	const FSkinWeightDataVertexBuffer* LODSkinData = LODData->GetSkinWeightVertexBuffer()->GetDataVertexBuffer();
	check(LODSkinData->GetMaxBoneInfluences() == BoneIndexBuffer->MaxBoneInfluences);
	//check(LODSkinData->GetMaxBoneInfluences() <= this->MaxBoneInfluence);

	{
		EVertexElementType ElemType = LODSkinData->Use16BitBoneWeight() ? VET_UShort4N : VET_UByte4N;
		uint32 Stride = LODSkinData->GetConstantInfluencesVertexStride();
		data.BoneWeights = FVertexStreamComponent(LODSkinData, LODSkinData->GetConstantInfluencesBoneWeightsOffset(), Stride, ElemType);
		if(LODSkinData->GetMaxBoneInfluences() > 4)
		{
			data.ExtraBoneWeights = FVertexStreamComponent(LODSkinData, LODSkinData->GetConstantInfluencesBoneWeightsOffset() + (LODSkinData->GetBoneWeightByteSize() * 4), Stride, ElemType);
		}
		else
		{
			data.ExtraBoneWeights = FVertexStreamComponent(&GNullVertexBuffer, 0, 0, ElemType);
		}
	}

	{
		uint32 Stride = (BoneIndexBuffer->bIs16BitBoneIndex ? 2 : 1) * LODSkinData->GetMaxBoneInfluences();
		EVertexElementType ElemType = BoneIndexBuffer->bIs16BitBoneIndex ? VET_UShort4 : VET_UByte4;
		data.BoneIndices = FVertexStreamComponent(BoneIndexBuffer, 0, Stride, ElemType);
		if(LODSkinData->GetMaxBoneInfluences() > 4)
		{
			data.ExtraBoneIndices = FVertexStreamComponent(BoneIndexBuffer, Stride / 2, Stride, ElemType);
		}
		else
		{
			data.ExtraBoneIndices = FVertexStreamComponent(&GNullVertexBuffer, 0, 0, ElemType);
		}
	}

	if (AdditionalStaticMeshVB)
	{
		data.PreSkinPostionOffset = FVertexStreamComponent(&AdditionalStaticMeshVB->PositionVertexBuffer, 0, (AdditionalStaticMeshVB->PositionVertexBuffer).GetStride(), VET_Float3);
	}
}

void FAllegroBaseVertexFactory::FillDataForStaticMesh(FDataType& data,const FStaticMeshLODResources* LODData, FStaticMeshVertexBuffers* AdditionalStaticMeshVB)
{
	const FStaticMeshVertexBuffers& smvb = LODData->VertexBuffers;

	smvb.PositionVertexBuffer.BindPositionVertexBuffer(this, data);
	smvb.StaticMeshVertexBuffer.BindTangentVertexBuffer(this, data);
	smvb.StaticMeshVertexBuffer.BindPackedTexCoordVertexBuffer(this, data);
	smvb.ColorVertexBuffer.BindColorVertexBuffer(this, data);

	EVertexElementType ElemType = VET_UShort4N;
	data.ExtraBoneWeights = FVertexStreamComponent(&GNullVertexBuffer, 0, 0, ElemType);
	data.ExtraBoneIndices = FVertexStreamComponent(&GNullVertexBuffer, 0, 0, ElemType);
	
	if (AdditionalStaticMeshVB)
	{
		data.PreSkinPostionOffset = FVertexStreamComponent(&AdditionalStaticMeshVB->PositionVertexBuffer, 0, (AdditionalStaticMeshVB->PositionVertexBuffer).GetStride(), VET_Float3);
	}
}

void FAllegroBaseVertexFactory::SetData(const FDataType& data)
{
	FVertexDeclarationElementList OutElements;
	//position
	OutElements.Add(AccessStreamComponent(data.PositionComponent, 0));
	// tangent basis vector decls
	OutElements.Add(AccessStreamComponent(data.TangentBasisComponents[0], 1));
	OutElements.Add(AccessStreamComponent(data.TangentBasisComponents[1], 2));

	// Texture coordinates
	if (data.TextureCoordinates.Num())
	{
		const uint8 BaseTexCoordAttribute = 5;
		for (int32 CoordinateIndex = 0; CoordinateIndex < data.TextureCoordinates.Num(); ++CoordinateIndex)
		{
			OutElements.Add(AccessStreamComponent(
				data.TextureCoordinates[CoordinateIndex],
				BaseTexCoordAttribute + CoordinateIndex  
			));

			if (BaseTexCoordAttribute + CoordinateIndex > 6)
			{
				check(false);
			}
		}

		/*for (int32 CoordinateIndex = data.TextureCoordinates.Num(); CoordinateIndex < MAX_TEXCOORDS; ++CoordinateIndex)
		{
	
			OutElements.Add(AccessStreamComponent(
				data.TextureCoordinates[data.TextureCoordinates.Num() - 1],
				BaseTexCoordAttribute + CoordinateIndex
			));
		}*/
	}

	OutElements.Add(AccessStreamComponent(data.ColorComponent, 13));

	// bone indices decls
	if(data.BoneIndices.VertexBuffer)
		OutElements.Add(AccessStreamComponent(data.BoneIndices, 3));

	// bone weights decls
	if(data.BoneWeights.VertexBuffer)
		OutElements.Add(AccessStreamComponent(data.BoneWeights, 4));

	if (MaxBoneInfluence > 4)
	{
		OutElements.Add(AccessStreamComponent(data.ExtraBoneIndices, 14));
		OutElements.Add(AccessStreamComponent(data.ExtraBoneWeights, 15));
	}

	if (data.PreSkinPostionOffset.VertexBuffer)
	{
		OutElements.Add(AccessStreamComponent(data.PreSkinPostionOffset,7));
	}

#if ALLEGRO_USE_GPU_SCENE
	AddPrimitiveIdStreamElement(EVertexInputStreamType::Default, OutElements, 16, 0xff);
#endif

	InitDeclaration(OutElements);

	check(GetDeclaration());

}

template<EAllegroVerteFactoryMode FactoryMode, bool PreSkinPostionOffset> 
bool TAllegroVertexFactory<FactoryMode, PreSkinPostionOffset>::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	return Parameters.MaterialParameters.MaterialDomain == MD_Surface && (Parameters.MaterialParameters.bIsUsedWithSkeletalMesh || Parameters.MaterialParameters.bIsSpecialEngineMaterial);

}

template<EAllegroVerteFactoryMode FactoryMode, bool PreSkinPostionOffset> 
void TAllegroVertexFactory<FactoryMode, PreSkinPostionOffset>::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	const FStaticFeatureLevel MaxSupportedFeatureLevel = GetMaxSupportedFeatureLevel(Parameters.Platform);
	
#if ALLEGRO_USE_GPU_SCENE
	const bool bUseGPUScene = UseGPUScene(Parameters.Platform, MaxSupportedFeatureLevel) && (MaxSupportedFeatureLevel > ERHIFeatureLevel::ES3_1);
#else
	const bool bUseGPUScene = false;
#endif
	const bool bSupportsPrimitiveIdStream = Parameters.VertexFactoryType->SupportsPrimitiveIdStream();



	OutEnvironment.SetDefine(TEXT("USE_INSTANCING"), 1);	
	OutEnvironment.SetDefine(TEXT("MAX_BONE_INFLUENCE"), StaticMaxBoneInfluence());
	OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), bSupportsPrimitiveIdStream && bUseGPUScene);
	OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_SPEEDTREE_WIND"), 0);
	OutEnvironment.SetDefine(TEXT("USE_DITHERED_LOD_TRANSITION_FOR_INSTANCED"), 0);
	OutEnvironment.SetDefine(TEXT("VF_ALLEGRO"), 1);
	OutEnvironment.SetDefine(TEXT("USE_DITHERED_LOD_TRANSITION"), 0);

	OutEnvironment.SetDefine(TEXT("PRESKIN_POSITION_OFFSET"), PreSkinPostionOffset?1:0);
}

template<EAllegroVerteFactoryMode FactoryMode, bool PreSkinPostionOffset> 
void TAllegroVertexFactory<FactoryMode, PreSkinPostionOffset>::ValidateCompiledResult(const FVertexFactoryType* Type, EShaderPlatform Platform, const FShaderParameterMap& ParameterMap, TArray<FString>& OutErrors)
{
}



class FAllegroShaderParameters : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FAllegroShaderParameters, NonVirtual);

public:
	void Bind(const FShaderParameterMap& ParameterMap)
	{
	}
	void GetElementShaderBindings(const class FSceneInterface* Scene, const FSceneView* View, const FMeshMaterialShader* Shader, const EVertexInputStreamType InputStreamType, ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory, const FMeshBatchElement& BatchElement, class FMeshDrawSingleShaderBindings& ShaderBindings, FVertexInputStreamArray& VertexStreams) const
	{
		const FAllegroBaseVertexFactory* vf = static_cast<const FAllegroBaseVertexFactory*>(VertexFactory);
		const FAllegroBatchElementOFR* userData = (const FAllegroBatchElementOFR*)BatchElement.UserData;

		EShaderFrequency SF = Shader->GetFrequency();
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FAllegroVertexFactoryParameters>(), userData->UniformBuffer);
	}

};

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FAllegroVertexFactoryParameters, "AllegroVF");


IMPLEMENT_TYPE_LAYOUT(FAllegroShaderParameters);

constexpr EVertexFactoryFlags VFFlags = EVertexFactoryFlags::UsedWithMaterials | EVertexFactoryFlags::SupportsDynamicLighting | EVertexFactoryFlags::SupportsPrecisePrevWorldPos
#if ALLEGRO_USE_GPU_SCENE
| EVertexFactoryFlags::SupportsPrimitiveIdStream
#endif
;


//#TODO maybe just supporting 1 and 4 Bone influence is enough ?
using AllegroVertexFactory0 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence0, false>;
using AllegroVertexFactory1 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence1, false>;
using AllegroVertexFactory2 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence2, false>;
using AllegroVertexFactory3 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence3, false>;
using AllegroVertexFactory4 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence4, false>;
using AllegroVertexFactory5 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence5, false>;
using AllegroVertexFactory6 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence6, false>;
using AllegroVertexFactory7 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence7, false>;
using AllegroVertexFactory8 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence8, false>;

using AllegroVertexFactory10 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence0, true>;
using AllegroVertexFactory11 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence1, true>;
using AllegroVertexFactory12 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence2, true>;
using AllegroVertexFactory13 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence3, true>;
using AllegroVertexFactory14 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence4, true>;
using AllegroVertexFactory15 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence5, true>;
using AllegroVertexFactory16 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence6, true>;
using AllegroVertexFactory17 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence7, true>;
using AllegroVertexFactory18 = TAllegroVertexFactory < EAllegroVerteFactoryMode::EVF_BoneInfluence8, true>;

IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory0, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory1, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory2, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory3, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory4, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory5, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory6, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory7, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory8, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);

IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory10, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory11, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory12, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory13, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory14, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory15, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory16, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory17, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);
IMPLEMENT_TEMPLATE_VERTEX_FACTORY_TYPE(template<>, AllegroVertexFactory18, "/Plugin/Allegro/Private/AllegroVertexFactory.ush", VFFlags);


IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory0, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory1, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory2, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory3, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory4, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory5, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory6, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory7, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory8, SF_Vertex, FAllegroShaderParameters);

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory0, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory1, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory2, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory3, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory4, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory5, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory6, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory7, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory8, SF_Pixel, FAllegroShaderParameters);


IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory10, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory11, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory12, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory13, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory14, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory15, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory16, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory17, SF_Vertex, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory18, SF_Vertex, FAllegroShaderParameters);

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory10, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory11, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory12, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory13, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory14, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory15, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory16, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory17, SF_Pixel, FAllegroShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(AllegroVertexFactory18, SF_Pixel, FAllegroShaderParameters);

#if 0
void FAllegroSkinWeightVertexBuffer::Serialize(FArchive& Ar)
{
	bool bAnyData = WeightData != nullptr;
	Ar << bAnyData;
	if (Ar.IsSaving())
	{
		if (WeightData)
			WeightData->Serialize(Ar);
	}
	else
	{
		if (bAnyData)
		{
			AllocBuffer();
			WeightData->Serialize(Ar);
		}
	}
}

void FAllegroSkinWeightVertexBuffer::InitRHI()
{
	uint32 size = WeightData->Num() * sizeof(FBoneWeight);
	FRHIResourceCreateInfo info(TEXT("FAllegroSkinWeightVertexBuffer"), WeightData->GetResourceArray());
	VertexBufferRHI = RHICreateVertexBuffer(size, BUF_Static, info);
	delete WeightData;
	WeightData = nullptr;
	//VertexBufferRHI = CreateRHIBuffer<true>(WeightData, 1, EBufferUsageFlags::Static | EBufferUsageFlags::VertexBuffer, TEXT("FAllegroSkinWeightVertexBuffer"));
}

void FAllegroSkinWeightVertexBuffer::ReleaseRHI()
{
	VertexBufferRHI.SafeRelease();
}

void FAllegroSkinWeightVertexBuffer::AllocBuffer()
{
	if (WeightData)
		delete WeightData;

	WeightData = new TStaticMeshVertexData<FBoneWeight>();
}

void FAllegroSkinWeightVertexBuffer::InitBuffer(const TArray<FSkinWeightInfo>& weightsArray)
{
	AllocBuffer();
	WeightData->ResizeBuffer(weightsArray.Num());

	FBoneWeight* dst = (FBoneWeight*)WeightData->GetDataPointer();
	for (int VertexIndex = 0; VertexIndex < weightsArray.Num(); VertexIndex++)
	{
		for (int InfluenceIndex = 0; InfluenceIndex < MAX_INFLUENCE; InfluenceIndex++)
		{
			check(weightsArray[VertexIndex].InfluenceBones[InfluenceIndex] <= 255);
			dst[VertexIndex].BoneIndex[InfluenceIndex] = static_cast<uint8>(weightsArray[VertexIndex].InfluenceBones[InfluenceIndex]);
			uint8 newWeight = static_cast<uint8>(weightsArray[VertexIndex].InfluenceWeights[InfluenceIndex] >> 8); //convert to 8bit skin weight
			dst[VertexIndex].BoneWeight[InfluenceIndex] = newWeight;
		}
	}
}

FAllegroSkinWeightVertexBuffer::~FAllegroSkinWeightVertexBuffer()
{
	if (WeightData)
	{
		delete WeightData;
		WeightData = nullptr;
	}
}
#endif


FAllegroAnimationBuffer::~FAllegroAnimationBuffer()
{
	DestroyBuffer();
}

void FAllegroAnimationBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	const uint32 Stride = bHighPrecision ? sizeof(float[4]) : sizeof(uint16[4]);
	FRHIResourceCreateInfo info(TEXT("FAllegroAnimationBuffer"), Transforms->GetResourceArray());
	ERHIAccess AM = ERHIAccess::Unknown;// ERHIAccess::SRVGraphics | ERHIAccess::UAVCompute | ERHIAccess::CopyDest;
	Buffer = RHICmdList.CreateVertexBuffer(Transforms->Num() * Transforms->GetStride(), BUF_UnorderedAccess | BUF_ShaderResource, AM, info);
	ShaderResourceViewRHI = RHICmdList.CreateShaderResourceView(Buffer, Stride, bHighPrecision ? PF_A32B32G32R32F : PF_FloatRGBA);
	UAV = RHICmdList.CreateUnorderedAccessView(Buffer, bHighPrecision ? PF_A32B32G32R32F : PF_FloatRGBA);

	delete Transforms;
	Transforms = nullptr;
}


void FAllegroAnimationBuffer::ReleaseRHI()
{
	ShaderResourceViewRHI.SafeRelease();
	Buffer.SafeRelease();
}

void FAllegroAnimationBuffer::Serialize(FArchive& Ar)
{
	Ar << bHighPrecision;
	bool bAnyData = Transforms != nullptr;
	Ar << bAnyData;
	if (Ar.IsSaving())
	{
		if (Transforms)
			Transforms->Serialize(Ar);
	}
	else
	{
		if (bAnyData)
		{
			AllocateBuffer();
			Transforms->Serialize(Ar);
		}
	}
}

void FAllegroAnimationBuffer::AllocateBuffer()
{
	if (Transforms)
		delete Transforms;

	if (bHighPrecision)
		Transforms = new TStaticMeshVertexData<FMatrix3x4>();
	else
		Transforms = new TStaticMeshVertexData<FMatrix3x4Half>();
}

void FAllegroAnimationBuffer::InitBuffer(const TArrayView<FTransform> InTransforms, bool InHightPrecision)
{
	InitBuffer(InTransforms.Num(), InHightPrecision, false);

	if (bHighPrecision)
	{
		FMatrix3x4* Dst = (FMatrix3x4*)Transforms->GetDataPointer();
		for (int i = 0; i < InTransforms.Num(); i++)
			Dst[i].SetMatrixTranspose(InTransforms[i].ToMatrixWithScale());

	}
	else
	{
		FMatrix3x4Half* Dst = (FMatrix3x4Half*)Transforms->GetDataPointer();
		for (int i = 0; i < InTransforms.Num(); i++)
			Dst[i].SetMatrixTranspose(FMatrix44f(InTransforms[i].ToMatrixWithScale()));

	}
}

void FAllegroAnimationBuffer::InitBuffer(uint32 NumMatrix, bool InHightPrecision, bool bFillIdentity)
{
	this->bHighPrecision = InHightPrecision;
	this->AllocateBuffer();
	this->Transforms->ResizeBuffer(NumMatrix);

	if(bFillIdentity)
	{
		if (InHightPrecision)
		{
			FMatrix3x4 IdentityMatrix;
			IdentityMatrix.SetMatrixTranspose(FMatrix::Identity);

			FMatrix3x4* Dst = (FMatrix3x4*)Transforms->GetDataPointer();
			for (uint32 i = 0; i < NumMatrix; i++)
				Dst[i] = IdentityMatrix;
		}
		else
		{
			FMatrix3x4Half IdentityMatrix;
			IdentityMatrix.SetMatrixTranspose(FMatrix44f::Identity);

			FMatrix3x4Half* Dst = (FMatrix3x4Half*)Transforms->GetDataPointer();
			for (uint32 i = 0; i < NumMatrix; i++)
				Dst[i] = IdentityMatrix;
		}
	}
}

void FAllegroAnimationBuffer::DestroyBuffer()
{
	if (Transforms)
	{
		delete Transforms;
		Transforms = nullptr;
	}
}




FMatrix3x4Half* FAllegroAnimationBuffer::GetDataPointerLP() const
{
	check(!this->bHighPrecision);
	return (FMatrix3x4Half*)this->Transforms->GetDataPointer();
}

FMatrix3x4* FAllegroAnimationBuffer::GetDataPointerHP() const
{
	check(this->bHighPrecision);
	return (FMatrix3x4*)this->Transforms->GetDataPointer();
}

#if WITH_EDITOR

void FAllegroMeshDataEx::InitFromMesh(int InBaseLOD, USkeletalMesh* SKMesh, const UAllegroAnimCollection* AnimSet)
{
	const FSkeletalMeshRenderData* SKMRenderData = SKMesh->GetResourceForRendering();

	this->LODs.Reserve(SKMRenderData->LODRenderData.Num() - InBaseLOD);

	for (int LODIndex = InBaseLOD; LODIndex < SKMRenderData->LODRenderData.Num(); LODIndex++)	//for each LOD
	{
		const FSkeletalMeshLODRenderData& SKMLODData = SKMRenderData->LODRenderData[LODIndex];
		const FSkinWeightVertexBuffer* SkinVB = SKMLODData.GetSkinWeightVertexBuffer();
		check(SKMLODData.GetVertexBufferMaxBoneInfluences() <= MAX_INFLUENCE);
		check(SkinVB->GetBoneInfluenceType() == DefaultBoneInfluence);
		check(SkinVB->GetVariableBonesPerVertex() == false);
		check(SkinVB->GetMaxBoneInfluences() == 4 || SkinVB->GetMaxBoneInfluences() == 8);

		FAllegroMeshDataEx::FLODData& AllegroLODData = this->LODs.Emplace_GetRef(GMaxRHIFeatureLevel);
		const bool bNeeds16BitBoneIndex = SkinVB->Use16BitBoneIndex() || SKMesh->GetSkeleton()->GetReferenceSkeleton().GetNum() > 255;
		AllegroLODData.BoneData.bIs16BitBoneIndex = bNeeds16BitBoneIndex;
		AllegroLODData.BoneData.MaxBoneInfluences = SkinVB->GetMaxBoneInfluences();
		AllegroLODData.BoneData.NumVertices = SkinVB->GetNumVertices();
		AllegroLODData.BoneData.ResizeBuffer();
		
		for (uint32 VertexIndex = 0; VertexIndex < SkinVB->GetNumVertices(); VertexIndex++)	//for each vertex
		{
			int SectionIndex, SectionVertexIndex;
			SKMLODData.GetSectionFromVertexIndex(VertexIndex, SectionIndex, SectionVertexIndex);

			const FSkelMeshRenderSection& SectionInfo = SKMLODData.RenderSections[SectionIndex];
			for (uint32 InfluenceIndex = 0; InfluenceIndex < (uint32)SectionInfo.MaxBoneInfluences; InfluenceIndex++)
			{
				uint32 BoneIndex = SkinVB->GetBoneIndex(VertexIndex, InfluenceIndex);
				uint16 BoneWeight = SkinVB->GetBoneWeight(VertexIndex, InfluenceIndex);
				FBoneIndexType MeshBoneIndex = SectionInfo.BoneMap[BoneIndex];
				int SkeletonBoneIndex = AnimSet->Skeleton->GetSkeletonBoneIndexFromMeshBoneIndex(SKMesh, MeshBoneIndex);
				check(SkeletonBoneIndex != INDEX_NONE);
				int RenderBoneIndex = AnimSet->SkeletonBoneToRenderBone[SkeletonBoneIndex];
				check(RenderBoneIndex != INDEX_NONE);

				AllegroLODData.BoneData.SetBoneIndex(VertexIndex, InfluenceIndex, RenderBoneIndex);
			}
		}
	}
}
#endif

void FAllegroMeshDataEx::InitResources(FRHICommandListBase& RHICmdList)
{
	check(IsInRenderingThread());
	for (FLODData& LODData : LODs)
	{
		LODData.InitResources(RHICmdList);
	}
}

void FAllegroMeshDataEx::ReleaseResouces()
{
	check(IsInRenderingThread());
	for (FLODData& LODData : LODs)
	{
		LODData.ReleaseResources();
	}
}

void FAllegroMeshDataEx::Serialize(FArchive& Ar)
{
	int NumLOD = this->LODs.Num();
	Ar << NumLOD;

	if (Ar.IsSaving())
	{
		for (int i = 0; i < NumLOD; i++)
			this->LODs[i].Serialize(Ar);
	}
	else
	{
		this->LODs.Reserve(NumLOD);
		for (int i = 0; i < NumLOD; i++)
		{
			this->LODs.Emplace(GMaxRHIFeatureLevel);
			this->LODs.Last().Serialize(Ar);
		}
	}
}

void FAllegroMeshDataEx::InitMeshData(const FSkeletalMeshRenderData* SKRenderData, int InBaseLOD)
{
	check(this->LODs.Num() <= SKRenderData->LODRenderData.Num());
	
	for (int LODIndex = 0; LODIndex < this->LODs.Num(); LODIndex++)
	{
		this->LODs[LODIndex].SkelLODData = &SKRenderData->LODRenderData[LODIndex + InBaseLOD];
	}
}

uint32 FAllegroMeshDataEx::GetTotalBufferSize() const
{
	uint32 Size = 0;
	for (const FLODData& L : LODs)
		Size += L.BoneData.GetBufferSizeInBytes();

	return Size;
}

FAllegroMeshDataEx::FLODData::FLODData(ERHIFeatureLevel::Type InFeatureLevel) 
{

}

FAllegroBaseVertexFactory* FAllegroMeshDataEx::FLODData::GetVertexFactory(int MaxBoneInfluence)
{
	check(MaxBoneInfluence >= 0 && MaxBoneInfluence <= FAllegroMeshDataEx::MAX_INFLUENCE);
	
	if (!VertexFactories[MaxBoneInfluence])
	{
		check(IsInRenderingThread() && this->SkelLODData);

		FAllegroBaseVertexFactory* VF = FAllegroBaseVertexFactory::New(MaxBoneInfluence, false);
		FAllegroBaseVertexFactory::FDataType VFData;
		VF->FillData(VFData, &BoneData, SkelLODData, nullptr);
		VF->SetData(VFData);
		VF->InitResource(FRHICommandListImmediate::Get());

		VertexFactories[MaxBoneInfluence] = TUniquePtr<FAllegroBaseVertexFactory>(VF);
	}

	return VertexFactories[MaxBoneInfluence].Get();
}

void FAllegroMeshDataEx::FLODData::InitResources(FRHICommandListBase& RHICmdList)
{
	check(IsInRenderingThread());
	BoneData.InitResource(RHICmdList);

}

void FAllegroMeshDataEx::FLODData::ReleaseResources()
{
	check(IsInRenderingThread());
	
	

	for (TUniquePtr<FAllegroBaseVertexFactory>& VF : VertexFactories)
	{
		if (VF)
		{
			VF->ReleaseResource();
			VF = nullptr;
		}
	}

	BoneData.ReleaseResource();
}


void FAllegroMeshDataEx::FLODData::Serialize(FArchive& Ar)
{
	//SkinWeight.Serialize(Ar);
	BoneData.Serialize(Ar);
}

void FAllegroInstanceBuffer::LockBuffers()
{
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();
	check(MappedTransforms == nullptr && MappedFrameIndices == nullptr);
	MappedTransforms = (AllegroShaderMatrixT*) RHICmdList.LockBuffer(TransformVB, 0, InstanceCount * sizeof(AllegroShaderMatrixT), RLM_WriteOnly);
	MappedFrameIndices = (uint32*)RHICmdList.LockBuffer(FrameIndexVB, 0, InstanceCount * sizeof(uint32), RLM_WriteOnly);
	MappedBlendFrameIndices = (uint32*)RHICmdList.LockBuffer(BlendFrameIndexVB, 0, InstanceCount * sizeof(uint32), RLM_WriteOnly);
}

void FAllegroInstanceBuffer::UnlockBuffers()
{
	check(IsLocked());
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();
	RHICmdList.UnlockBuffer(FrameIndexVB);
	RHICmdList.UnlockBuffer(TransformVB);
	RHICmdList.UnlockBuffer(BlendFrameIndexVB);

	MappedTransforms = nullptr;
	MappedFrameIndices = nullptr;
	MappedBlendFrameIndices = nullptr;
}

TSharedPtr<FAllegroInstanceBuffer> FAllegroInstanceBuffer::Create(uint32 InstanceCount)
{
	FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();

	FAllegroInstanceBufferPtr Resource = MakeShared<FAllegroInstanceBuffer>();
	Resource->InstanceCount = InstanceCount;
	Resource->CreationFrameNumber = GFrameNumberRenderThread;

	{
		FRHIResourceCreateInfo CreateInfo(TEXT("InstanceTransform"));
		Resource->TransformVB = RHICmdList.CreateVertexBuffer(InstanceCount * sizeof(AllegroShaderMatrixT), (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
		Resource->TransformSRV = RHICmdList.CreateShaderResourceView(Resource->TransformVB, sizeof(float[4]), PF_A32B32G32R32F);
	}
	{
		FRHIResourceCreateInfo CreateInfo(TEXT("InstanceAnimationFrameIndex"));
		Resource->FrameIndexVB = RHICmdList.CreateVertexBuffer(InstanceCount * sizeof(uint32), (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
		Resource->FrameIndexSRV = RHICmdList.CreateShaderResourceView(Resource->FrameIndexVB, sizeof(uint32), PF_R32_UINT);
	}

	{
		FRHIResourceCreateInfo CreateInfo(TEXT("InstanceBlendFrameIndex"));
		Resource->BlendFrameIndexVB = RHICmdList.CreateVertexBuffer(InstanceCount * sizeof(uint32), (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
		Resource->BlendFrameIndexmSRV = RHICmdList.CreateShaderResourceView(Resource->BlendFrameIndexVB, sizeof(uint32), PF_R32_UINT);
	}

	return Resource;
}




void Allegro_PreRenderFrame(class FRDGBuilder&)
{
	//#Note r.DoInitViewsLightingAfterPrepass removed from UE5
	//static IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DoInitViewsLightingAfterPrepass"));
	//bool bDoInitViewAftersPrepass = !!CV->GetInt();
	//checkf(!bDoInitViewAftersPrepass, TEXT("r.DoInitViewsLightingAfterPrepass must be zero because engine has not exposed required delegates."));
	
	//GAllegroInstanceBufferPool.Commit();
	//GAllegroCIDBufferAllocatorForInitViews.Commit();
}

void Allegro_PostRenderFrame(class FRDGBuilder&)
{
	GAllegroInstanceBufferPool.EndOfFrame();
	GAllegroCIDBufferPool.EndOfFrame();
	GAllegroElementIndexBufferPool.EndOfFrame();
	GAllegroBlendFrameBufferPool.EndOfFrame();
}

void FAllegroCIDBuffer::LockBuffers()
{
	check(MappedData == nullptr);
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();
	MappedData = (float*)RHICmdList.LockBuffer(CustomDataBuffer, 0, NumberOfFloat * sizeof(float), RLM_WriteOnly);
}

void FAllegroCIDBuffer::UnlockBuffers()
{
	check(IsLocked());
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();
	RHICmdList.UnlockBuffer(CustomDataBuffer);
	MappedData = nullptr;
}

TSharedPtr<FAllegroCIDBuffer> FAllegroCIDBuffer::Create(uint32 InNumberOfFloat)
{
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();

	FAllegroCIDBufferPtr Resource = MakeShared<FAllegroCIDBuffer>();
	Resource->NumberOfFloat = InNumberOfFloat;
	Resource->CreationFrameNumber = GFrameNumberRenderThread;

	FRHIResourceCreateInfo CreateInfo(TEXT("CustomData"));
	Resource->CustomDataBuffer = RHICmdList.CreateVertexBuffer(InNumberOfFloat * sizeof(float), (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
	Resource->CustomDataSRV = RHICmdList.CreateShaderResourceView(Resource->CustomDataBuffer, sizeof(float), PF_R32_FLOAT);

	return Resource;
}

TSharedPtr<FAllegroBlendFrameBuffer> FAllegroBlendFrameBuffer::Create(uint32 NumOfFloat)
{
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();

	FAllegroBlendFrameBufferPtr Resource = MakeShared<FAllegroBlendFrameBuffer>();
	FRHIResourceCreateInfo CreateInfo(TEXT("BlendFrameBuffer"));

	Resource->NumberOfFloat = NumOfFloat;
	Resource->BlendFrameDataBuffer = RHICmdList.CreateVertexBuffer(NumOfFloat * sizeof(float), (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
	Resource->BlendFrameDataSRV = RHICmdList.CreateShaderResourceView(Resource->BlendFrameDataBuffer, sizeof(float), PF_R32_FLOAT);

	return Resource;
}


void FAllegroBlendFrameBuffer::LockBuffers()
{
	check(MappedData == nullptr);
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();
	MappedData = (float*)RHICmdList.LockBuffer(BlendFrameDataBuffer, 0, NumberOfFloat * sizeof(float), RLM_WriteOnly);
}

void FAllegroBlendFrameBuffer::UnlockBuffers()
{
	check(IsLocked());
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();
	RHICmdList.UnlockBuffer(BlendFrameDataBuffer);
	MappedData = nullptr;
}


FAllegroInstanceBufferAllocator GAllegroInstanceBufferPool;
FAllegroCIDBufferAllocator GAllegroCIDBufferAllocatorForInitViews;
FAllegroCIDBufferAllocator GAllegroCIDBufferPool;
FAllegroBlendFrameBufferAllocator GAllegroBlendFrameBufferPool;


FAllegroProxyOFR::~FAllegroProxyOFR()
{
	//if(this->CDIShadowBuffer)
	//	GAllegroCIDBufferPool.Release(this->CDIShadowBuffer);
	//
	//if(this->InstanceShadowBuffer)
	//	GAllegroShadowInstanceBufferPool.Release(this->InstanceShadowBuffer);
	//
	check(true);
}

void FAllegroElementIndexBuffer::LockBuffers()
{
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();
	check(MappedData == nullptr);
	MappedData = RHICmdList.LockBuffer(ElementIndexBuffer, 0, SizeInBytes, RLM_WriteOnly);
}

void FAllegroElementIndexBuffer::UnlockBuffers()
{
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();
	check(IsLocked());
	RHICmdList.UnlockBuffer(ElementIndexBuffer);
	MappedData = nullptr;
}

TSharedPtr<FAllegroElementIndexBuffer> FAllegroElementIndexBuffer::Create(uint32 InSizeInBytes)
{
	FRHICommandListBase& RHICmdList = FRHICommandListImmediate::Get();

	FAllegroElementIndexBufferPtr Resource = MakeShared<FAllegroElementIndexBuffer>();
	Resource->SizeInBytes = InSizeInBytes;
	Resource->CreationFrameNumber = GFrameNumberRenderThread;

	FRHIResourceCreateInfo CreateInfo(TEXT("ElementIndices"));
	Resource->ElementIndexBuffer = RHICmdList.CreateVertexBuffer(InSizeInBytes, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
	//create two SRV, we use uint16 when possible
	Resource->ElementIndexUIN16SRV = RHICmdList.CreateShaderResourceView(Resource->ElementIndexBuffer, sizeof(uint16), PF_R16_UINT);
	Resource->ElementIndexUIN32SRV = RHICmdList.CreateShaderResourceView(Resource->ElementIndexBuffer, sizeof(uint32), PF_R32_UINT);

	return Resource;
}

FAllegroElementIndexBufferAllocator GAllegroElementIndexBufferPool;

void FAllegroBoneIndexVertexBuffer::Serialize(FArchive& Ar)
{
	Ar << bIs16BitBoneIndex << MaxBoneInfluences << NumVertices;
	if (Ar.IsLoading())
	{
		ResizeBuffer();
	}
	
	BoneData->Serialize(Ar);
}

void FAllegroBoneIndexVertexBuffer::InitRHI(FRHICommandListBase& RHICmdList)
{
	check(BoneData);
	uint32 size = BoneData->Num();
	FRHIResourceCreateInfo info(TEXT("FAllegroBoneIndexVertexBuffer"), BoneData->GetResourceArray());
	VertexBufferRHI = RHICmdList.CreateVertexBuffer(size, BUF_Static, info);
	delete BoneData;
	BoneData = nullptr;
}

void FAllegroBoneIndexVertexBuffer::ReleaseRHI()
{
	VertexBufferRHI.SafeRelease();
}

