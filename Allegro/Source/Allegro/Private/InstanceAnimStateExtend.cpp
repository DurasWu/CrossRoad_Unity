#include "InstanceAnimStateExtend.h"
#include "BlendSpaceInstance.h"
#include "AnimMontageInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimMontage.h"
#include "AllegroAnimCollection.h"
#include "Allegro.h"

static const FName DefaultSlotName = "DefaultSlot";

FInstanceAnimStateExtend::FInstanceAnimStateExtend()
{

}

FInstanceAnimStateExtend::~FInstanceAnimStateExtend()
{
	Reset();
}

void FInstanceAnimStateExtend::Reset()
{
	if (MontageInstanceProxy)
	{
		delete MontageInstanceProxy;
		MontageInstanceProxy = nullptr;
	}

	if (BlendSpaceInstance)
	{
		delete BlendSpaceInstance;
		BlendSpaceInstance = nullptr;
	}
}

bool FInstanceAnimStateExtend::IsPlaying()
{
	if (MontageInstanceProxy)
	{
		return MontageInstanceProxy->IsPlaying();
	}
	return true;
}

void FInstanceAnimStateExtend::Update(float Delta)
{
	if (MontageInstanceProxy)
	{
		MontageInstanceProxy->UpdateMontage(Delta);
	}
	else if (BlendSpaceInstance)
	{
		BlendSpaceInstance->Update(Delta);
	}
}

float FInstanceAnimStateExtend::SetMontage(UAnimMontage* Asset, FAllegroAnimPlayParams& Params, UAllegroComponent* Owner)
{
	Reset();

	if (MontageInstanceProxy != nullptr)
	{
		delete MontageInstanceProxy;
		MontageInstanceProxy = nullptr;
	}

	MontageInstanceProxy = new FAllegroAnimMontageInstanceProxy();
	float Length = MontageInstanceProxy->MontagePlay(Asset, 1.0f, EMontagePlayReturnType::MontageLength, Params.StartAt);

	this->CurrentSequence = -1;
	this->Time = 0;

	FAnimTrack const* const AnimTrack = Asset->GetAnimationData(DefaultSlotName);
	const float ClampedTime = FMath::Clamp(Params.StartAt, 0.0f, AnimTrack->GetLength());
	if (const FAnimSegment* const AnimSegment = AnimTrack->GetSegmentAtTime(ClampedTime))
	{
		if (AnimSegment->IsValid())
		{
			float PositionInAnim = 0.0f;
			if (UAnimSequenceBase* AnimRef = AnimSegment->GetAnimationData(ClampedTime, PositionInAnim))
			{
				int32* idx = Owner->AnimCollection->SequenceIndexMap.Find(AnimRef); 
				if (idx && *idx >= 0)
				{
					this->CurrentSequence = *idx;
					this->Time = PositionInAnim;
				}
			}
		}
	}

	if (Params.bLoop)
	{
		MontageInstanceProxy->SetLoop();
	}

	return Length;
}

float FInstanceAnimStateExtend::SetBlendSpace(UBlendSpace* Asset, FAllegroAnimPlayParams& Params, UAllegroComponent* Owner)
{
	Reset();

	if (!BlendSpaceInstance)
	{
		BlendSpaceInstance = new FAllegroBlendSpaceInstance();
	}

	BlendSpaceInstance->SetPosition(BlendSpacePositionX, BlendSpacePositionY);
	BlendSpaceInstance->SetBlendSpace(Asset);

	BlendSpaceInstance->Update(0.0f);

	this->CurrentSequence = -1;
	this->Time = BlendSpaceInstance->CurrentTime;

	float Length = 1.0f;
	const TArray<struct FBlendSampleData>& BlendSamples = BlendSpaceInstance->BlendSampleDataCache;
	TArray<OrderAnimSequenceWeight> Temp;
	Temp.Reset(BlendSamples.Num());

	for (int i = 0; i < BlendSamples.Num(); ++i)
	{
		int32* idx = Owner->AnimCollection->SequenceIndexMap.Find(BlendSamples[i].Animation.Get()); // FindSequenceDef(SampleDatas[i].Animation.Get());   
		if (idx && *idx >= 0)
		{
			auto& Ref = Temp.AddDefaulted_GetRef();
			Ref.SequenceIndex = *idx;
			Ref.Weight = BlendSamples[i].GetClampedWeight();
		}
	}

	Temp.Sort([](const OrderAnimSequenceWeight& LHS, const OrderAnimSequenceWeight& RHS)
		{
			return LHS.Weight > RHS.Weight;
		}
	);

	if (Temp.Num() > 0)
	{
		this->CurrentSequence = Temp[0].SequenceIndex;
		const FAllegroSequenceDef& ActiveSequenceStruct = Owner->AnimCollection->Sequences[this->CurrentSequence];
		Length = ActiveSequenceStruct.GetSequenceLength();
	}

	return Length;
}

void FInstanceAnimStateExtend::SetBlendSpacePosition(float InX, float InY)
{
	if (BlendSpaceInstance)
	{
		BlendSpaceInstance->SetPosition(InX, InY);
	}
}

bool FInstanceAnimStateExtend::MontageJumpToSectionName(const FString& SectionName, bool bEndOfSection)
{
	if (MontageInstanceProxy)
	{
		return MontageInstanceProxy->MontageJumpToSectionName(SectionName, bEndOfSection);
	}
	return false;
}

void FInstanceAnimStateExtend::PostUpdate(int32 InstanceIndex, UAllegroComponent* Owner)
{

	UAllegroAnimCollection* AnimCollection = Owner->AnimCollection;
	FAllegroInstancesData& InstancesData = Owner->InstancesData;

	if (MontageInstanceProxy)
	{
		MontageInstanceProxy->PostUpdate([&](const TArray<FMontageEvaluationState>& MontageEvaluationData, TArray<FAnimNotifyEventReference>& NotitfyRefs, float NotifyWeight)
			{
				for (const FMontageEvaluationState& EvalState : MontageEvaluationData)
				{
					if (!EvalState.Montage.IsValid())
					{
						return;
					}
					const UAnimMontage* const Montage = EvalState.Montage.Get();

					FAnimTrack const* const AnimTrack = Montage->GetAnimationData(DefaultSlotName);
					const float ClampedTime = FMath::Clamp(EvalState.MontagePosition, 0.0f, AnimTrack->GetLength());
					if (const FAnimSegment* const AnimSegment = AnimTrack->GetSegmentAtTime(ClampedTime))
					{
						if (AnimSegment->IsValid())
						{
							float PositionInAnim = 0.0f;
							if (UAnimSequenceBase* AnimRef = AnimSegment->GetAnimationData(ClampedTime, PositionInAnim))
							{
								int32* idx = Owner->AnimCollection->SequenceIndexMap.Find(AnimRef); // FindSequenceDef(AnimRef); 
								 if (idx && *idx>=0)
								 {
									 this->CurrentSequence = *idx;
									 this->Time = PositionInAnim;
								 }
							}
						}
					}
				}

				for (auto& Notify : NotitfyRefs)
				{
					Owner->AddEvent(InstanceIndex, FAllegroAnimNotifyEvent{ InstanceIndex,MontageInstanceProxy->GetMonageAsset(), Notify.GetNotify()->NotifyName,Notify.GetNotify()->Notify});
				}

			}
		);
	}
	else if (BlendSpaceInstance)
	{
		BlendSpaceInstance->PostUpdate([&](const TArray<FBlendSampleData>& SampleDatas)
			{
				int32 DataIndex = InstancesData.BlendFrameInfoIndex[InstanceIndex];
				if (DataIndex <= 0)
					return;
	
				FInstanceBlendFrameInfo& BlendInfo = InstancesData.BlendFrameInfo[DataIndex];
				
				BlendInfo.Weight[0] = 1.0f;
				for (int i = 1; i < ALLEGRO_BLEND_FRAME_NUM_MAX; ++i)
				{
					BlendInfo.Weight[i] = 0.0f;
				}

				int TempNum = ALLEGRO_BLEND_FRAME_NUM_MAX * 2;   
				TArray<OrderAnimSequenceWeight> Temp;
				Temp.Reset(TempNum);

				for (int i = 0; (i < SampleDatas.Num() && i < TempNum) ; ++i)
				{	
					int32* idx = Owner->AnimCollection->SequenceIndexMap.Find(SampleDatas[i].Animation.Get()); // FindSequenceDef(SampleDatas[i].Animation.Get());   
					if (idx && *idx >= 0)
					{
						auto& Ref = Temp.AddDefaulted_GetRef();
						Ref.SequenceIndex = *idx;
						Ref.Weight = SampleDatas[i].GetClampedWeight();
					}
				}

				Temp.Sort([](const OrderAnimSequenceWeight& LHS, const OrderAnimSequenceWeight& RHS)
					{
						return LHS.Weight > RHS.Weight;
					}
				);

				int BlendNumMax = ALLEGRO_BLEND_FRAME_NUM_MAX;

#if ALLEGRO_ANIMTION_TICK_LOD
				int AnimLOD = InstancesData.AnimationStates[InstanceIndex].AnimtioneLOD;
				if (AnimLOD > 2)
				{
					BlendNumMax = ALLEGRO_BLEND_FRAME_NUM_MAX - 3;
				}
				else if (AnimLOD > 1)
				{
					BlendNumMax = ALLEGRO_BLEND_FRAME_NUM_MAX - 2;
				}
				else if (AnimLOD > 0)
				{
					BlendNumMax = ALLEGRO_BLEND_FRAME_NUM_MAX - 1;
				}
				if (BlendNumMax < 1) 
					BlendNumMax = 1;
#endif

				float WeightTotal = 0.0f;
				for (int i = 0; i < BlendNumMax && i < Temp.Num(); ++i)
				{
					WeightTotal += Temp[i].Weight;
				}

				for (int i = 0; i < BlendNumMax && i < Temp.Num(); ++i)
				{
					const FAllegroSequenceDef& ActiveSequenceStruct = AnimCollection->Sequences[Temp[i].SequenceIndex];
					int LocalFrameIndex = BlendSpaceInstance->NormalizedCurrentTime * ActiveSequenceStruct.AnimationFrameCount;
					if (i == 0)
					{
						this->CurrentSequence = Temp[i].SequenceIndex;
						this->Time = BlendSpaceInstance->NormalizedCurrentTime * ActiveSequenceStruct.SequenceLength;
					}
					else
					{
						int GlobalFrameIndex = ActiveSequenceStruct.AnimationFrameIndex + LocalFrameIndex;
						BlendInfo.FrameIndex[i - 1] = GlobalFrameIndex;
					}
					BlendInfo.Weight[i] = Temp[i].Weight / WeightTotal;
				}

			});
	}
}
