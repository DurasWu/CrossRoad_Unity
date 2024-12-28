// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroUtility.h"
#include "KismetTraceUtils.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimMontage.h"


#define LOCTEXT_NAMESPACE "Allegro"


int UAllegroBPUtility::LineTraceInstancesSingle(UAllegroComponent* Comp, const FVector& Start, const FVector& End, float Thickness, float DebugDrawTime, double& OutTime, FVector& OutPosition, FVector& OutNormal, int& OutBoneIndex)
{
	OutNormal = OutPosition = FVector::Zero();
	OutTime = 0;
	OutBoneIndex = -1;

	int HitInstanceIndex = -1;
	for (int i = 0; i < Comp->GetInstanceCount(); i++)
	{
		if(Comp->IsInstanceAlive(i))
		{
			double TOI;
			FVector HitPos, HitNorm;
			int HitBone = Comp->LineTraceInstanceSingle(i, Start, End, Thickness, TOI, HitPos, HitNorm);
			if (HitBone != -1)
			{
				if (HitInstanceIndex == -1 || TOI < OutTime)
				{
					HitInstanceIndex = i;
					OutTime = TOI;
					OutPosition = HitPos;
					OutNormal = HitNorm;
					OutBoneIndex = HitBone;
				}
			}
		}
	}
#if ENABLE_DRAW_DEBUG 
	if(DebugDrawTime >= 0)
	{
		if (HitInstanceIndex == -1)
		{

			DrawDebugSweptSphere(Comp->GetWorld(), Start, End, Thickness, FColor::Red, false, DebugDrawTime, 0);
		}
		else
		{
			// Red up to the blocking hit, green thereafter
			::DrawDebugSweptSphere(Comp->GetWorld(), Start, OutPosition, Thickness, FColor::Red, false, DebugDrawTime);
			::DrawDebugSweptSphere(Comp->GetWorld(), OutPosition, End, Thickness, FColor::Green, false, DebugDrawTime);

			::DrawDebugPoint(Comp->GetWorld(), OutPosition, 8, FColor::Red, false, DebugDrawTime);//hit point
			::DrawDebugDirectionalArrow(Comp->GetWorld(), OutPosition, OutPosition + OutNormal * 32, 1, FColor::Yellow, false, DebugDrawTime); //hit normal

			FTransform3f BoneT = Comp->GetInstanceBoneTransform(HitInstanceIndex, OutBoneIndex, true);
			FString BoneStr = Comp->GetBoneName(OutBoneIndex).ToString();
			::DrawDebugString(Comp->GetWorld(), FVector(BoneT.GetLocation()), BoneStr, nullptr, FColor::Yellow, DebugDrawTime);

			FBoxCenterExtentFloat BoundWS = Comp->CalculateInstanceBound(HitInstanceIndex);
			DrawDebugBox(Comp->GetWorld(), FVector(BoundWS.Center), FVector(BoundWS.Extent), FColor::Green, false, DebugDrawTime);
		}
	}
#endif

	return HitInstanceIndex;
}

USkeletalMeshComponent* UAllegroBPUtility::ConstructSkeletalMeshComponentsFromInstance(const UAllegroComponent* Component, int InstanceIndex, UObject* Outer, bool bSetCustomPrimitiveDataFloat)
{
	if (!IsValid(Component) || !Component->IsInstanceValid(InstanceIndex))
		return nullptr;

	Outer = Outer ? Outer : (UObject*)Component;

	USkeletalMeshComponent* RootComp = nullptr;
	Component->ForEachSubmeshOfInstance(InstanceIndex, [&](uint8 SubMeshIndex) {

		USkeletalMesh* SKMesh = Component->Submeshes[SubMeshIndex].SkeletalMesh;
		if (!IsValid(SKMesh))
			return;

		USkeletalMeshComponent* MeshComp = NewObject<USkeletalMeshComponent>(Outer);
		MeshComp->SetSkeletalMesh(SKMesh);
		

		//copy materials
		const int BaseMI = Component->GetSubmeshBaseMaterialIndex(SubMeshIndex);
		for (int MI = 0; MI < SKMesh->GetNumMaterials(); MI++)
		{
			MeshComp->SetMaterial(MI, Component->GetMaterial(BaseMI + MI));
		}

		if (!RootComp)
		{
			RootComp = MeshComp;
			MeshComp->SetWorldTransform(FTransform(Component->GetInstanceTransform(InstanceIndex)));
			AActor* OuterAsActor = Cast<AActor>(Outer);
			USceneComponent* AttachParentComp = OuterAsActor && OuterAsActor->GetRootComponent() ? OuterAsActor->GetRootComponent() : Cast<USceneComponent>(Outer);
		}
		else
		{
			MeshComp->SetupAttachment(RootComp);
			MeshComp->SetLeaderPoseComponent(RootComp);
		}

		if (bSetCustomPrimitiveDataFloat)
		{
			for (int FloatIndex = 0; FloatIndex < Component->NumCustomDataFloats; FloatIndex++)
				MeshComp->SetCustomPrimitiveDataFloat(FloatIndex, Component->GetInstanceCustomData(InstanceIndex, FloatIndex));
		}

		
		MeshComp->RegisterComponent();

		const FAllegroInstanceAnimState& AS = Component->InstancesData.AnimationStates[InstanceIndex];
		if (AS.IsValid())
		{
			MeshComp->PlayAnimation(Component->GetInstanceCurrentAnimSequence(InstanceIndex), Component->IsInstanceAnimationLooped(InstanceIndex));
			MeshComp->SetPosition(AS.Time, false);
			MeshComp->SetPlayRate(Component->AnimationPlayRate * AS.PlayScale);
		}


	});

	return RootComp;
}

template<typename TLambda> void ForEachInstanceConditional(UAllegroComponent* Component, int32 FlagsToTest, bool bAllFlags, bool bInvert, TLambda Proc)
{
	EAllegroInstanceFlags RightOp = static_cast<EAllegroInstanceFlags>(FlagsToTest << InstaceUserFlagStart);

	for (int i = 0; i < Component->GetInstanceCount(); i++)
	{
		if (Component->IsInstanceAlive(i))
		{
			if (bAllFlags)
			{
				if (EnumHasAllFlags(Component->InstancesData.Flags[i], RightOp) != bInvert)
					Proc(i);
			}
			else
			{
				if (EnumHasAnyFlags(Component->InstancesData.Flags[i], RightOp) != bInvert)
					Proc(i);
			}
		}
	}
}

void UAllegroBPUtility::MoveAllInstancesConditional( UAllegroComponent* Component, FVector Offset, int32 FlagsToTest, bool bAllFlags, bool bInvert)
{
	if(IsValid(Component))
	{
		ForEachInstanceConditional(Component, FlagsToTest, bAllFlags, bInvert, [&](int InstanceIndex) {
			Component->AddInstanceLocation(InstanceIndex, FVector3f(Offset));
		});
	}
}

void UAllegroBPUtility::QueryLocationOverlappingBoxAdvanced(UAllegroComponent* Component, int32 FlagsToTest, bool bAllFlags, bool bInvert, const FBox& Box, TArray<int>& InstanceIndices)
{
	if (IsValid(Component))
	{
		FBox3f BoxFloat(Box);
		ForEachInstanceConditional(Component, FlagsToTest, bAllFlags, bInvert, [&](int InstanceIndex) {
			if (BoxFloat.IsInside(Component->GetInstanceLocation(InstanceIndex)))
				InstanceIndices.Add(InstanceIndex);
		});
	}
}


void UAllegroBPUtility::QueryLocationOverlappingSphereAdvanced(UAllegroComponent* Component, int32 FlagsToTest, bool bAllFlags, bool bInvert, const FVector& Center, float Radius, TArray<int>& InstanceIndices)
{
	if (IsValid(Component))
	{
		ForEachInstanceConditional(Component, FlagsToTest, bAllFlags, bInvert, [&](int InstanceIndex) {
			if(FVector3f::DistSquared(FVector3f(Center), Component->GetInstanceLocation(InstanceIndex)) <= FMath::Square(Radius))
				InstanceIndices.Add(InstanceIndex);
		});
	}
}

void UAllegroBPUtility::QueryLocationOverlappingComponentAdvanced(UAllegroComponent* Component, UPrimitiveComponent* ComponentToTest, int32 FlagsToTest, bool bAllFlags, bool bInvert, TArray<int>& InstanceIndices)
{
	if (IsValid(Component) && IsValid(Component))
	{
		ForEachInstanceConditional(Component, FlagsToTest, bAllFlags, bInvert, [&](int InstanceIndex) {
			float Dist;
			FVector ClosestPoint;
			if (ComponentToTest->GetSquaredDistanceToCollision(FVector(Component->GetInstanceLocation(InstanceIndex)), Dist, ClosestPoint))
			{
				if (Dist == 0)
				{
					InstanceIndices.Add(InstanceIndex);
				}
			}
		});
	}
}

#undef LOCTEXT_NAMESPACE
