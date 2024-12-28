// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "Containers/SparseArray.h"



struct ALLEGRO_API FAllegroSpanAllocator
{
	struct FFreeBlock
	{
		int Offset;
		int Size;
		int Next; //index of the next free block with higher @Offset

		int GetEnd() const { return Offset + Size; }
	};

	struct FBlockInfo
	{
		int Offset = -1;
		int Size = -1;

		bool IsValid() const { return Offset != -1; }
		int GetLast() const { return Offset + Size - 1; }
	};

	TSparseArray<FFreeBlock> FreeBlocks;	//the array itself is not sorted, we use it as memory pool
	int FirstFree = -1;	//index of the free block with least Offset, linked list is sorted by Block.Offset
	int BufferSize = 0;	//size of buffer
	int AllocCounter = 0;	//number of block allocated
	int AllocSize = 0; //number of byes allocated


	FAllegroSpanAllocator()
	{

	}
	~FAllegroSpanAllocator()
	{
		check(AllocCounter == 0 && AllocSize == 0);
	}
	void Init(int InInBufferSize)
	{
		check(InInBufferSize > 0);
		check(BufferSize == 0);
		BufferSize = InInBufferSize;
		FirstFree = (int)FreeBlocks.Add(FFreeBlock{ 0, InInBufferSize, -1 });
		AllocCounter = AllocSize = 0;
	}
	void Empty()
	{
		FreeBlocks.Empty();
		FirstFree = -1;
		BufferSize = AllocCounter = AllocSize = 0;
	}

	//
	int Alloc_Internal(int InSize);
	//
	int Alloc(int Size);
	//
	void Free(int InOffset, int InSize);
	
	//fills StrVisualize with unique chars each one representing a free block
	void DebugPrint(FString& StrVisualize, FString& StrInfo) const;
	void CheckValidity() const;
};
