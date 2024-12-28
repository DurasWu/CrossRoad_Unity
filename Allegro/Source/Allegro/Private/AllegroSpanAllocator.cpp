// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroSpanAllocator.h"

int FAllegroSpanAllocator::Alloc_Internal(int InSize)
{
	check(InSize > 0 && InSize != -1);

	const int TotalAvail = BufferSize - AllocSize;
	if (InSize > TotalAvail)
		return -1;

	int BlockIndex = FirstFree;
	int PreBlockIndex = -1;
	while (BlockIndex != -1)
	{
		FFreeBlock& Block = FreeBlocks[BlockIndex];
		const int TmpOffset = Block.Offset;
		if (Block.Size == InSize)	//exact match ?
		{
			if (PreBlockIndex != -1)
				FreeBlocks[PreBlockIndex].Next = Block.Next;
			else
				FirstFree = Block.Next;

			FreeBlocks.RemoveAt(BlockIndex);
			return TmpOffset;
		}
		else if (Block.Size > InSize)	//block is big enough so we take from its beginning
		{
			Block.Size -= InSize;
			Block.Offset += InSize;
			return TmpOffset;
		}

		PreBlockIndex = BlockIndex;
		BlockIndex = Block.Next;
	}

	return -1;
}

int FAllegroSpanAllocator::Alloc(int Size)
{
	int Offset = Alloc_Internal(Size);
	if (Offset != -1)
	{
		AllocCounter++;
		AllocSize += Size;
		check(AllocSize <= BufferSize);
	}
	return Offset;
}

void FAllegroSpanAllocator::Free(int InOffset, int InSize)
{
	check(InOffset >= 0 && InSize <= AllocSize && AllocCounter > 0 && InSize > 0);
	AllocCounter--;
	AllocSize -= InSize;

	if (FirstFree == -1)
	{
		FirstFree = (int)FreeBlocks.Add(FFreeBlock{ InOffset, InSize, -1 });
		return;
	}

	const int BlockEnd = InOffset + InSize;

	int PreBlockIndex = -1;
	int CurBlockIndex = FirstFree;
	do
	{
		FFreeBlock& Iter = FreeBlocks[CurBlockIndex];

		check(InOffset != Iter.Offset && BlockEnd != (Iter.Offset + Iter.Size));

		if (InOffset < Iter.Offset)
		{
			bool bMerged = false;
			if (BlockEnd == Iter.Offset) //there is a free block after ?
			{
				bMerged = true;
				Iter.Offset -= InSize;
				Iter.Size += InSize;
			}
			if (PreBlockIndex != -1 && FreeBlocks[PreBlockIndex].GetEnd() == InOffset)	//there is a free block before ?
			{
				if (bMerged) //already merged ?
				{
					FreeBlocks[PreBlockIndex].Size += Iter.Size;
					//remove block on the right side
					FreeBlocks[PreBlockIndex].Next = FreeBlocks[CurBlockIndex].Next;
					FreeBlocks.RemoveAt(CurBlockIndex);
				}
				else
				{
					bMerged = true;
					FreeBlocks[PreBlockIndex].Size += InSize;
				}
			}

			if (!bMerged)
			{
				int NewIdx = FreeBlocks.Add(FFreeBlock{ InOffset, InSize, CurBlockIndex });
				if (PreBlockIndex != -1)
					FreeBlocks[PreBlockIndex].Next = NewIdx;
				else
					FirstFree = NewIdx;
			}

			return;
		}


		PreBlockIndex = CurBlockIndex;
		CurBlockIndex = Iter.Next;

	} while (CurBlockIndex != -1);

	// reaching here means block address was higher so we need to add to the end of linked list
	//PreBlockIndex is last block now

	if (FreeBlocks[PreBlockIndex].GetEnd() == InOffset)
	{
		FreeBlocks[PreBlockIndex].Size += InSize;
	}
	else
	{
		//new block must be the last one in Linked List
		int NewIdx = (int)FreeBlocks.Add(FFreeBlock{ InOffset, InSize, -1 });
		FreeBlocks[PreBlockIndex].Next = NewIdx;
	}
}

void FAllegroSpanAllocator::DebugPrint(FString& StrVisualize, FString& StrInfo) const
{
	StrVisualize.Reserve(BufferSize);
	for (int i = 0; i < BufferSize; i++)
		StrVisualize.AppendChar(TEXT('*'));

	int PreSize = 0;
	int Iter = FirstFree;
	TCHAR chr = TEXT('A');

	while (Iter != -1)
	{
		const FFreeBlock& B = FreeBlocks[Iter];

		StrInfo += FString::Printf(TEXT("[%d_%d]"), B.Offset, B.Size);

		for (int i = 0; i < B.Size; i++)
			StrVisualize[static_cast<int>(B.Offset) + i] = chr;

		Iter = B.Next;

		chr++;
		if (chr > TEXT('W'))
			chr = TEXT('A');
	}
}

void FAllegroSpanAllocator::CheckValidity() const
{
	int TotalFreeSize = 0;
	int PreIter = -1;
	int Iter = FirstFree;
	while (Iter != -1)
	{
		const FFreeBlock& Block = FreeBlocks[Iter];
		TotalFreeSize += Block.Size;
		if (PreIter != -1)
		{
			check(FreeBlocks[PreIter].Offset < Block.Offset);
		}

		PreIter = Iter;
		Iter = Block.Next;
	}

	check(TotalFreeSize <= BufferSize);
	check(TotalFreeSize == (BufferSize - AllocSize))
}
