// Copyright Netease Games, Inc. All Rights Reserved.

#pragma once


#include "Animation/AnimInstance.h"
#include "Animation/AnimMontageEvaluationState.h"

struct FAnimMontageInstance;
class UAnimInstancedStaticMeshComponent;


struct FAllegroAnimMontageInstanceProxy
{
public:
	FAllegroAnimMontageInstanceProxy();
	~FAllegroAnimMontageInstanceProxy();

	float MontagePlay(UAnimMontage* MontageToPlay, float InPlayRate = 1.0f, EMontagePlayReturnType ReturnValueType = EMontagePlayReturnType::MontageLength, float InTimeToStartMontageAt = 0.0f, bool bStopAllMontages = true);

	bool MontageJumpToSectionName(const FString& SectionName, bool bEndOfSection);

	void SetPlayRate(float InPlayRate);

	void UpdateMontage(float DeltaSeconds);

	void PostUpdate(TFunctionRef<void(const TArray<FMontageEvaluationState>&, TArray<FAnimNotifyEventReference>&,float)> ProxyLambdaFunc);

	bool IsPlaying();

	bool SetLoop();

	TObjectPtr<class UAnimMontage> GetMonageAsset();
protected:

	void UpdateInternal();

	void Montage_UpdateWeight(float DeltaSeconds);

	void Montage_Advance(float DeltaSeconds);

	void Montage_UpdateEvaluateParameters(UAnimInstancedStaticMeshComponent* MeshComponent);

	void UpdateMontageEvaluationData();

	void MontageSetNextSection(FName SectionNameToChange, FName NextSection, const UAnimMontage* Montage = nullptr);

	FAnimMontageInstance* MontageInstance;
	friend struct FAllegroAnimInstanceMontagePrivate;

	TArray<FMontageEvaluationState> MontageEvaluationData;
	TArray<FAnimNotifyEventReference> NotitfyRefs;

}; 
