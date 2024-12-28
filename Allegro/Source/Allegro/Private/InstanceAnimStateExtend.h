#pragma once

#include "AllegroComponent.h"



class UAnimMontage;
class UBlendSpace;

struct FAllegroBlendSpaceInstance;
struct FAllegroAnimMontageInstanceProxy;



class FInstanceAnimStateExtend
{

public:

	FInstanceAnimStateExtend();

	~FInstanceAnimStateExtend();

	float SetMontage(UAnimMontage* Asset, FAllegroAnimPlayParams& Params, UAllegroComponent* Owner);

	float SetBlendSpace(UBlendSpace* Asset, FAllegroAnimPlayParams& Params, UAllegroComponent* Owner);

	void Update(float Delta);

	void PostUpdate(int32 InstanceIndex,UAllegroComponent* Owner);

	void SetBlendSpacePosition(float InX, float InY = 0);

	bool MontageJumpToSectionName(const FString& SectionName, bool bEndOfSection = false);

	void Reset();

	bool IsPlaying();

	float						Time = 0;
	int32						CurrentSequence = -1;

private:

	FAllegroAnimMontageInstanceProxy* MontageInstanceProxy = nullptr;
	FAllegroBlendSpaceInstance* BlendSpaceInstance = nullptr;

	float						BlendSpacePositionX = 0.0f;
	float						BlendSpacePositionY = 0.0f;

	struct OrderAnimSequenceWeight
	{
		int32 SequenceIndex;
		float Weight;

		OrderAnimSequenceWeight()
		{
			SequenceIndex = 1;
			Weight = 0.0f;
		}
	};

};


