
#include "AnimMontageInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimCompositeBase.h"


struct FAllegroAnimInstanceMontagePrivate
{
	// ------------------------------------------------------
	// Montage Helper
	static void StopAllMontages(FAllegroAnimMontageInstanceProxy& Self)
	{

	}

	static void RestartMontage(FAllegroAnimMontageInstanceProxy& Self, UAnimMontage* Montage, FName FromSection = FName())
	{
		/*if (Montage == Self.CurrentAsset)
		{
			Self.MontagePlay(Montage, 1.0);
		}*/
	}

	static void SetMontageLoop(FAllegroAnimMontageInstanceProxy& Self, UAnimMontage* Montage, bool bIsLooping, FName StartingSection = FName())
	{
		int32 TotalSection = Montage->CompositeSections.Num();
		if (TotalSection > 0)
		{
			if (StartingSection == NAME_None)
			{
				StartingSection = Montage->CompositeSections[0].SectionName;
			}
			FName FirstSection = StartingSection;
			FName LastSection = StartingSection;

			bool bSucceeded = false;
			// Find last section
			int32 CurSection = Montage->GetSectionIndex(FirstSection);

			int32 Count = TotalSection;
			while (Count-- > 0)
			{
				FName NewLastSection = Montage->CompositeSections[CurSection].NextSectionName;
				CurSection = Montage->GetSectionIndex(NewLastSection);

				if (CurSection != INDEX_NONE)
				{
					// Used to rebuild next/prev
					Self.MontageSetNextSection(LastSection, NewLastSection);
					LastSection = NewLastSection;
				}
				else
				{
					bSucceeded = true;
					break;
				}
			}

			if (bSucceeded)
			{
				if (bIsLooping)
				{
					Self.MontageSetNextSection(LastSection, FirstSection);
				}
				else
				{
					Self.MontageSetNextSection(LastSection, NAME_None);
				}
			}
			// else the default is already looping
		}
	}
};

//-----------------------------------------------------------------------------------

FAllegroAnimMontageInstanceProxy::FAllegroAnimMontageInstanceProxy()
	:MontageInstance(nullptr)
{

}

FAllegroAnimMontageInstanceProxy::~FAllegroAnimMontageInstanceProxy()
{
	if (MontageInstance != nullptr)
	{
		delete MontageInstance;
		MontageInstance = nullptr;
	}
}

void FAllegroAnimMontageInstanceProxy::MontageSetNextSection(FName SectionNameToChange, FName NextSection, const UAnimMontage* Montage)
{
	if (MontageInstance)
	{
		MontageInstance->SetNextSectionName(SectionNameToChange, NextSection);
	}
}

void FAllegroAnimMontageInstanceProxy::UpdateMontage(float DeltaSeconds)
{

	float PreviousTrackPos = MontageInstance->GetPosition();

	Montage_UpdateWeight(DeltaSeconds);

	Montage_Advance(DeltaSeconds);

	UAnimMontage* Montage = MontageInstance->Montage;
	if (!Montage) return;

	float CurrentTrackPos = MontageInstance->GetPosition();

	// We already break up AnimMontage update to handle looping, so we guarantee that PreviousPos and CurrentPos are contiguous.
	Montage->GetAnimNotifiesFromDeltaPositions(PreviousTrackPos, CurrentTrackPos, NotitfyRefs);

	// For Montage only, remove notifies marked as 'branching points'. They are not queued and are handled separately.
	//Montage->FilterOutNotifyBranchingPoints(NotitfyRefsForParallel);

	// now trigger notifies for all animations within montage
	// we'll do this for all slots for now
	/*for (auto SlotTrack = Montage->SlotAnimTracks.CreateIterator(); SlotTrack; ++SlotTrack)
	{
		TArray<FAnimNotifyEventReference>& MapNotifies = NotifyMap.FindOrAdd(SlotTrack->SlotName);
		SlotTrack->AnimTrack.GetAnimNotifiesFromTrackPositions(PreviousTrackPos, CurrentTrackPos, MapNotifies);
	}*/

	UpdateMontageEvaluationData();
}

void FAllegroAnimMontageInstanceProxy::UpdateMontageEvaluationData()
{
	MontageEvaluationData.Reset();
	//for (FAnimMontageInstance* MontageInstance : MontageInstances)
	{
		if (MontageInstance->Montage && MontageInstance->GetWeight() > ZERO_ANIMWEIGHT_THRESH)
		{
			MontageEvaluationData.Add(FMontageEvaluationState(MontageInstance->Montage, MontageInstance->GetPosition(),MontageInstance->DeltaTimeRecord,
				MontageInstance->bPlaying, MontageInstance->IsActive(), MontageInstance->GetBlend(), MontageInstance->GetActiveBlendProfile(), MontageInstance->GetBlendStartAlpha()));
		}
	}
}

void FAllegroAnimMontageInstanceProxy::Montage_UpdateWeight(float DeltaSeconds)
{
	//for (int32 i = 0; i < MontageInstances.Num(); i++)
	{
		if (MontageInstance)
		{
			MontageInstance->UpdateWeight(DeltaSeconds);
		}
	}
}

void FAllegroAnimMontageInstanceProxy::Montage_Advance(float DeltaSeconds)
{
	if (MontageInstance)
	{
		if (MontageInstance && MontageInstance->IsValid())
		{
			MontageInstance->Advance(DeltaSeconds, nullptr, false);
		}
	}
}

void FAllegroAnimMontageInstanceProxy::SetPlayRate(float InPlayRate)
{
	if (MontageInstance)
	{
		MontageInstance->SetPlayRate(InPlayRate);
	}
}

float FAllegroAnimMontageInstanceProxy::MontagePlay(UAnimMontage* MontageToPlay, float InPlayRate, EMontagePlayReturnType ReturnValueType, float InTimeToStartMontageAt, bool bStopAllMontages)
{
	if (MontageToPlay && (MontageToPlay->GetPlayLength() > 0.0f)
		//&& MontageToPlay->HasValidSlotSetup() //this function has not been exported
		)
	{
		if (MontageInstance != nullptr)
		{
			delete MontageInstance;
			MontageInstance = nullptr;
		}

		{
			FAnimMontageInstance* NewInstance = new FAnimMontageInstance();
			check(NewInstance);
			const float MontageLength = MontageToPlay->GetPlayLength();
			NewInstance->Initialize(MontageToPlay);
			NewInstance->Play(InPlayRate);
			NewInstance->SetPosition(FMath::Clamp(InTimeToStartMontageAt, 0.0f, MontageLength));
			MontageInstance = NewInstance;

			return (ReturnValueType == EMontagePlayReturnType::MontageLength) ? MontageLength : (MontageLength / (InPlayRate * MontageToPlay->RateScale));
		}
	}
	return 0.0f;
}

bool FAllegroAnimMontageInstanceProxy::MontageJumpToSectionName(const FString& SectionName, bool bEndOfSection)
{
	if (MontageInstance)
	{
		return MontageInstance->JumpToSectionName(FName(SectionName), bEndOfSection);
	}
	return false;
}

bool FAllegroAnimMontageInstanceProxy::SetLoop()
{
	if (MontageInstance)
	{
		auto id = MontageInstance->GetCurrentSection();
		return MontageInstance->SetNextSectionName(id, id);
	}
	return false;
}

void FAllegroAnimMontageInstanceProxy::PostUpdate(TFunctionRef<void(const TArray<FMontageEvaluationState>&,  TArray<FAnimNotifyEventReference>&, float)> ProxyLambdaFunc)
{
	float NotifyWeight = MontageInstance->GetWeight();
	ProxyLambdaFunc(MontageEvaluationData, NotitfyRefs, NotifyWeight);
	NotitfyRefs.Empty();
}

bool FAllegroAnimMontageInstanceProxy::IsPlaying()
{
	if (MontageInstance)
		return MontageInstance->IsPlaying();

	return false;
}

TObjectPtr<class UAnimMontage> FAllegroAnimMontageInstanceProxy::GetMonageAsset()
{
	if (MontageInstance)
		return MontageInstance->Montage;

	return nullptr;
}
