// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "AllegroComponent.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "AllegroUtility.generated.h"


UCLASS()
class UAnimNotifyTest : public UAnimNotify
{
	GENERATED_BODY()

public:

	UPROPERTY(EditAnywhere, Category = "Allegro")
	int32 IntParam;

	UPROPERTY(EditAnywhere, Category = "Allegro")
	FString StringParam;
};


UCLASS()
class ALLEGRO_API UAllegroBPUtility : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/*
	* iterate over all the instances and find the closest hit
	*/
	UFUNCTION(BlueprintCallable, Category = "Allegro|Utility")
	static UPARAM(DisplayName="OutInstanceIndex") int LineTraceInstancesSingle(UAllegroComponent* Component, const FVector& Start, const FVector& End, float Thickness, float DebugDrawTime, double& OutTime, FVector& OutPosition, FVector& OutNormal, int& OutBoneIndex);

	/*
	* @param bSetCustomPrimitiveDataFloat	 copy PerInstanceCustomDataFloat to each Component's CustomPrimitiveDataFloat ?
	*/
	UFUNCTION(BlueprintCallable, Category = "Allegro|Utility")
	static USkeletalMeshComponent* ConstructSkeletalMeshComponentsFromInstance(const UAllegroComponent* Component, int InstanceIndex, UObject* Outer = nullptr, bool bSetCustomPrimitiveDataFloat = true);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Utility")
	static void MoveAllInstancesConditional(UAllegroComponent* Component, FVector Offset, UPARAM(meta = (Bitmask, BitmaskEnum = EInstanceUserFlags)) int32 FlagsToTest, bool bAllFlags, bool bInvert);


	UFUNCTION(BlueprintCallable, Category = "Allegro|Utility")
	static void QueryLocationOverlappingBox(UAllegroComponent* Component, const FBox& Box, TArray<int>& InstanceIndices)
	{
		if(IsValid(Component))
			Component->QueryLocationOverlappingBox(FBox3f(Box), InstanceIndices);
	}

	UFUNCTION(BlueprintCallable, Category = "Allegro|Utility")
	static void QueryLocationOverlappingSphere(UAllegroComponent* Component, const FVector& Center, float Radius, TArray<int>& InstanceIndices)
	{
		if(IsValid(Component))
			Component->QueryLocationOverlappingSphere(FVector3f(Center), Radius, InstanceIndices);
	}




	UFUNCTION(BlueprintCallable, Category = "Allegro|Utility")
	static void QueryLocationOverlappingBoxAdvanced(UAllegroComponent* Component, UPARAM(meta = (Bitmask, BitmaskEnum = EInstanceUserFlags)) int32 FlagsToTest, bool bAllFlags, bool bInvert, const FBox& Box, TArray<int>& InstanceIndices);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Utility")
	static void QueryLocationOverlappingSphereAdvanced(UAllegroComponent* Component, UPARAM(meta = (Bitmask, BitmaskEnum = EInstanceUserFlags)) int32 FlagsToTest, bool bAllFlags, bool bInvert, const FVector& Center, float Radius, TArray<int>& InstanceIndices);

	UFUNCTION(BlueprintCallable, Category = "Allegro|Utility")
	static void QueryLocationOverlappingComponentAdvanced(UAllegroComponent* Component, UPrimitiveComponent* ComponentToTest, UPARAM(meta = (Bitmask, BitmaskEnum = EInstanceUserFlags)) int32 FlagsToTest, bool bAllFlags, bool bInvert, TArray<int>& InstanceIndices);
};

