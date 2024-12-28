#include "BlendSpaceInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"

FAllegroBlendSpaceInstance::FAllegroBlendSpaceInstance()
{

}

FVector FAllegroBlendSpaceInstance::GetPosition() const 
{ 
    FVector Position;
    Position.X = AssetPlayerToTick.BlendSpace.BlendSpacePositionX;
    Position.Y = AssetPlayerToTick.BlendSpace.BlendSpacePositionY;
    Position.Z = 0;
    return Position; 
}

void FAllegroBlendSpaceInstance::SetPosition(float InX, float InY) 
{ 
    AssetPlayerToTick.BlendSpace.BlendSpacePositionX = InX;
    AssetPlayerToTick.BlendSpace.BlendSpacePositionY = InY;
}

void FAllegroBlendSpaceInstance::SetBlendSpace(UBlendSpace* InBlendSpace) 
{ 
    InBlendSpace->InitializeFilter(&BlendFilter);

    BlendSampleDataCache.Reset();
    NormalizedCurrentTime = 0.0f;
    DeltaTimeRecord = FDeltaTimeRecord();

    AssetPlayerToTick = FAnimTickRecord();
    AssetPlayerToTick.bLooping = true;
    AssetPlayerToTick.SourceAsset = InBlendSpace;
    AssetPlayerToTick.BlendSpace.BlendFilter = &BlendFilter;
    AssetPlayerToTick.TimeAccumulator = &NormalizedCurrentTime;
    AssetPlayerToTick.BlendSpace.BlendSampleDataCache = &BlendSampleDataCache;
    AssetPlayerToTick.DeltaTimeRecord = &DeltaTimeRecord;

    CurrentTime = 0.0f;
}

void FAllegroBlendSpaceInstance::Update(float DeltaTime)
{
    TArray<FName> ValidMarkers;
    FAnimNotifyQueue NotifyQueue;
    FAnimAssetTickContext TickContext(DeltaTime, ERootMotionMode::RootMotionFromEverything, false, ValidMarkers);
    AssetPlayerToTick.SourceAsset->TickAssetPlayer(AssetPlayerToTick, NotifyQueue, TickContext);

    CurrentTime += DeltaTime;
}

void FAllegroBlendSpaceInstance::PostUpdate(TFunctionRef<void(const TArray<FBlendSampleData>&)> ProxyLambdaFunc)
{
    ProxyLambdaFunc(BlendSampleDataCache);
}