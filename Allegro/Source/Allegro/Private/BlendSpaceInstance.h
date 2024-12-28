#pragma once

#include "Animation/AnimationAsset.h"

class UBlendSpace;

struct FAllegroBlendSpaceInstance
{
    FAllegroBlendSpaceInstance();

    void SetBlendSpace(UBlendSpace* InBlendSpace);

    void SetPosition(float InX, float InY = 0);

    FVector GetPosition() const;

    void Update(float DeltaTime);

    void PostUpdate(TFunctionRef<void(const TArray<FBlendSampleData>&)> ProxyLambdaFunc);

    float   NormalizedCurrentTime;
    float   CurrentTime;

    TArray<FBlendSampleData> BlendSampleDataCache;

protected:

    FAnimTickRecord  AssetPlayerToTick;
    FBlendFilter     BlendFilter;
    FDeltaTimeRecord DeltaTimeRecord;
    
   
     
};


