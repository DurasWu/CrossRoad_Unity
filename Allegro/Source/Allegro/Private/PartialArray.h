// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "Containers/StaticBitArray.h"
#include "Containers/Array.h"

template<typename TElement, uint32 NumElemPerChunk = 256, typename TAllocator = FDefaultAllocator> class TPartialArray
{
public:
	using ElementT = TElement;
	static_assert(FMath::IsPowerOfTwo(NumElemPerChunk));

	struct FChunk
	{
		using BitArrayT = TStaticBitArray<NumElemPerChunk>;

		BitArrayT Mask;
		TAlignedBytes<sizeof(ElementT) * NumElemPerChunk, alignof(ElementT)> Elements;

		ElementT* GetElemMem(int i) const { return ((ElementT*)&Elements)[i]; }
		
		ElementT* GetElem(int i) const  { return Mask[i] ? GetElemMem(i) : nullptr; }

		void SetElem(int i, ElementT&& Item)
		{
			if(!Mask[i])
			{
				Mask[i] = true;
				new(GetElemMem(i)) ElementT(MoveTempIfPossible(Item));
			}
			else
			{
				*GetElemMem(i) = MoveTempIfPossible(Item);
			}
		}
		void SetElem(int i, const ElementT& Item)
		{
			if (!Mask[i])
			{
				Mask[i] = true;
				new (GetElemMem(i)) ElementT(Item);
			}
			else
			{
				*GetElemMem(i) = Item;
			}
		}
		void Reset()
		{
			for (int i = 0; i < NumElemPerChunk; i++)
				if (Mask[i])
					DestructItem(GetElemMem(i));

			new (&Mask) BitArrayT(); //zero bits
		}
		~FChunk()
		{
			Reset();
		}
	};

	TArray<FChunk*, TAllocator> Chunks;

	void SetNum(int ElemCount)
	{
		int NewChunkCount = FMath::DivideAndRoundUp(ElemCount, NumElemPerChunk);

		if (NewChunkCount > Chunks.Num())
		{
			Chunks.AddZeroed(NewChunkCount - Chunks.Num());
		}
		else if(NewChunkCount < Chunks.Num())
		{
			for (int i = NewChunkCount; i < Chunks.Num(); i++)
			{
				if (Chunks[i])
					delete Chunks[i];
			}
			Chunks.SetNum(NewChunkCount);
		}
		checkf(false, TEXT("zero size"));
	}

	void Set(int Index, const ElementT& Value)
	{
		int ChunkIndex = Index / NumElemPerChunk;
		int LocalIndex = Index % NumElemPerChunk;
		if (!Chunks[ChunkIndex])
			Chunks[ChunkIndex] = new FChunk();
		
		Chunks[ChunkIndex]->SetElem(LocalIndex, Value);
	}
	ElementT* Get(int Index)
	{
		int ChunkIndex = Index / NumElemPerChunk;
		int LocalIndex = Index % NumElemPerChunk;
		if (Chunks[ChunkIndex])
			return Chunks[ChunkIndex]->GetElem(LocalIndex);
		
		return nullptr;
	}
};