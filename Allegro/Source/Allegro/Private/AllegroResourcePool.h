// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "RenderResource.h"
#include "SceneManagement.h"

template<typename TResoruce> struct TBufferAllocator
{
	struct FAllocation
	{
		TResoruce Resource;	//must be TSharedPtr
		uint32 Offset = 0;//not in byte, its index of element

		uint32 Avail() const { return Resource->GetSize() - Offset; }
	};

	struct FPool : FAllocation
	{
		uint32 UnusedCounter = 0;
	};

	TArray<FPool> Pools;
	FPool* Current = nullptr;
	uint32 Counter = 0;


	FAllocation Alloc(uint32 InNumElement)
	{
		check(IsInRenderingThread());

		if (Current && Current->Avail() >= InNumElement)
		{

		}
		else
		{
			if (Current && InNumElement > Current->Avail())	//current buffer is not big enough ?
			{
				Current = nullptr;
				for (FPool& Pool : Pools)	//look for compatible pool
				{
					if (Pool.Avail() >= InNumElement)
					{
						Current = &Pool;
						break;
					}
				}
			}


			if (Current == nullptr)	//nothing exist so should allocate a new one
			{
				Current = &Pools.AddDefaulted_GetRef();
				Current->Resource = TResoruce::ElementType::Create(Align(InNumElement, TResoruce::ElementType::SizeAlign));
			}

		}

		if (!Current->Resource->IsLocked())	//try map if its not already
			Current->Resource->LockBuffers();

		FAllocation Alc;
		Alc.Resource = Current->Resource;
		Alc.Offset = Current->Offset;
		Current->Offset += InNumElement;
		return Alc;
	}

	void Commit()
	{
		check(IsInRenderingThread());

		Current = nullptr;

		Pools.RemoveAllSwap([&](FPool& Pool) {
			if (Pool.Resource->IsLocked())
			{
				Pool.Offset = 0;
				Pool.UnusedCounter = 0;
				Pool.Resource->UnlockBuffers();
				return false;
			}
			else
			{
				constexpr uint32 ReleaseThreshold = 180;
				if (Pool.UnusedCounter++ >= ReleaseThreshold)
				{
					Pool.Resource = nullptr;
					return true;
				}
			}
			return false;
			}, false);

		Current = Pools.Num() ? &Pools[0] : nullptr;
	}
};


template<typename TResource> struct TBufferAllocatorSingle
{
	struct FPool
	{
		TResource Resource;	//must be TSharedPtr
		uint32 ExpireTime;
		uint32 Size;
	};

	TArray<FPool> Pools;
	TArray<TResource> AllocatedResources;
	//TArray<TResource> AllocatedResources[2];
	//uint32 WriteIndex = 0;

	TResource Alloc(uint32 InSize)
	{
		check(IsInRenderingThread());

		int BestChoice = -1;
		uint32 LastWaste = UINT32_MAX;

		for (int i = 0; i < Pools.Num(); i++)
		{
			FPool& P = Pools[i];
			if (P.Size >= InSize)
			{
				uint32 Waste = P.Size - InSize;
				if (Waste < LastWaste)
				{
					LastWaste = Waste;
					BestChoice = i;
				}
			}
		}

		if (BestChoice != -1)
		{
			TResource R = MoveTemp(Pools[BestChoice].Resource);
			AllocatedResources.Add(R);
			Pools.RemoveAtSwap(BestChoice);
			return R;
		}

		TResource R = TResource::ElementType::Create(Align(InSize, TResource::ElementType::SizeAlign));
		AllocatedResources.Add(R);
		return R;
	}

	void EndOfFrame()
	{
		check(IsInRenderingThread());
		const uint32 FC = GFrameCounterRenderThread;
		int NumRemoved = Pools.RemoveAllSwap([&](FPool& P) {
			if (FC >= P.ExpireTime)
			{
				P.Resource = nullptr;
				return true;
			}
			return false;
		}, false);

		//WriteIndex ^= 1;

		for (TResource& R : AllocatedResources)
		{
			FPool& P = Pools.AddDefaulted_GetRef();
			P.Resource = MoveTemp(R);
			P.Size = P.Resource->GetSize();
			P.ExpireTime = FC + 180;
		}
		
		AllocatedResources.Reset();
	}

	void ReleaseAll()
	{
		AllocatedResources.Empty();
		AllocatedResources.Empty();
		Pools.Empty();
	}

};