// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AllegroBase.h"


template <typename TType, typename TSize> void AllegroElementCopy(TType* Dst, const TType* Src, TSize Count)
{
	for (TSize i = 0; i < Count; i++)
		Dst[i] = Src[i];
}

inline bool AllegroTerminatedArrayAddUnique(uint8* Array /*must be 0xFF terminated*/, uint8 MaxLen , uint8 NewItem)
{
	check(NewItem != 0xFF);
	auto End = Array + MaxLen;

	for (auto Iter = Array; Iter != End; Iter++)
	{
		if (Iter[0] == NewItem)	//already exist ?
			return true;

		if (Iter[0] == 0xFF)
		{
			Iter[0] = NewItem;
			Iter[1] = 0xFF;
			return true;
		}
	}

	return false;
}

inline bool AllegroTerminatedArrayRemoveShift(uint8* Array /*must be 0xFF terminated*/, uint8 MaxLen, uint8 ItemToRemove)
{
	check(ItemToRemove != 0xFF)
	auto End = Array + MaxLen;

	for (auto Iter = Array; Iter != End; Iter++)
	{
		if (Iter[0] == ItemToRemove) //found it ?
		{
			do
			{
				Iter[0] = Iter[1];
				Iter++;

			} while (Iter[0] != 0xFF);

			return true;
		}
	}

	return false;
}


template<typename T> void AllegroRemoveDuplicates(T* Elements, uint32 Len)
{
	// use nested for loop to find the duplicate elements in array  
	for (uint32 i = 0; i < Len; i++)
	{
		for (uint32 j = i + 1; j < Len; j++)
		{
			// use if statement to check duplicate element  
			if (Elements[i] == Elements[j])
			{
				// delete the current position of the duplicate element  
				for (uint32 k = j; k < Len - 1; k++)
				{
					Elements[k] = Elements[k + 1];
				}
				// decrease the size of array after removing duplicate element  
				Len--;
				// if the position of the elements is changes, don't increase the index j  
				j--;
			}
		}
	}
}


template<typename T> T AllegroGetEven(T value)
{
	return value & ~T(1);
}


inline void AllegroCalcGridSize(float W, float H, int CellCount, int& OutX, int& OutY)
{
	check(W > 0 && H > 0 && CellCount > 1);
	const bool bHightHigher = H > W;
	const float Ratio = bHightHigher ? (W / H) : (H / W);
	int A = FMath::TruncToInt32(FMath::Sqrt(FMath::Max(1.0f, CellCount * Ratio)));
	check(A > 0);
	int B = CellCount / A;
	check(B > 0);
	check(A * B <= CellCount);
	if (bHightHigher)
	{
		OutX = A;
		OutY = B;
	}
	else
	{
		OutX = B;
		OutY = A;
	}
}


//copied from RadixSort32 to send distances as a separate array
template< typename ValueType, typename CountType> void AllegroRadixSort32(ValueType* RESTRICT Dst, ValueType* RESTRICT Src, uint32* RESTRICT DstDist, uint32* RESTRICT SrcDist, CountType Num)
{
	CountType Histograms[1024 + 2048 + 2048];
	CountType* RESTRICT Histogram0 = Histograms + 0;
	CountType* RESTRICT Histogram1 = Histogram0 + 1024;
	CountType* RESTRICT Histogram2 = Histogram1 + 2048;

	FMemory::Memzero(Histograms, sizeof(Histograms));

	{
		// Parallel histogram generation pass
		//const ValueType* RESTRICT s = (const ValueType * RESTRICT)Src;
		for (CountType i = 0; i < Num; i++)
		{
			uint32 Key = SrcDist[i];// SortKey(s[i]);
			Histogram0[(Key >> 0) & 1023]++;
			Histogram1[(Key >> 10) & 2047]++;
			Histogram2[(Key >> 21) & 2047]++;
		}
	}
	{
		// Prefix sum
		// Set each histogram entry to the sum of entries preceding it
		CountType Sum0 = 0;
		CountType Sum1 = 0;
		CountType Sum2 = 0;
		for (CountType i = 0; i < 1024; i++)
		{
			CountType t;
			t = Histogram0[i] + Sum0; Histogram0[i] = Sum0 - 1; Sum0 = t;
			t = Histogram1[i] + Sum1; Histogram1[i] = Sum1 - 1; Sum1 = t;
			t = Histogram2[i] + Sum2; Histogram2[i] = Sum2 - 1; Sum2 = t;
		}
		for (CountType i = 1024; i < 2048; i++)
		{
			CountType t;
			t = Histogram1[i] + Sum1; Histogram1[i] = Sum1 - 1; Sum1 = t;
			t = Histogram2[i] + Sum2; Histogram2[i] = Sum2 - 1; Sum2 = t;
		}
	}
	{
		// Sort pass 1
		const ValueType* RESTRICT s = (const ValueType * RESTRICT)Src;
		ValueType* RESTRICT d = Dst;
		const uint32* RESTRICT sd = SrcDist;
		uint32* RESTRICT dd = DstDist;

		for (CountType i = 0; i < Num; i++)
		{
			ValueType Value = s[i];
			uint32 Key = sd[i]; //SortKey(Value);
			CountType Idx = ++Histogram0[((Key >> 0) & 1023)];
			d[Idx] = Value;
			dd[Idx] = Key;
		}
	}
	{
		// Sort pass 2
		const ValueType* RESTRICT s = (const ValueType * RESTRICT)Dst;
		ValueType* RESTRICT d = Src;
		const uint32* RESTRICT sd = DstDist;
		uint32* RESTRICT dd = SrcDist;

		for (CountType i = 0; i < Num; i++)
		{
			ValueType Value = s[i];
			uint32 Key = sd[i];//SortKey(Value);
			CountType Idx = ++Histogram1[((Key >> 10) & 2047)];
			d[Idx] = Value;
			dd[Idx] = Key;
		}
	}
	{
		// Sort pass 3
		const ValueType* RESTRICT s = (const ValueType * RESTRICT)Src;
		ValueType* RESTRICT d = Dst;
		const uint32* RESTRICT sd = SrcDist;
		uint32* RESTRICT dd = DstDist;

		for (CountType i = 0; i < Num; i++)
		{
			ValueType Value = s[i];
			uint32 Key = sd[i];//SortKey(Value);
			CountType Idx = ++Histogram2[((Key >> 21) & 2047)];
			d[Idx] = Value;
			dd[Idx] = Key;
		}
	}
}

inline void AllegroArrayAndSSE(void* Data, int NumPack, int Value)
{
	VectorRegister4Int MaskReg = VectorIntSet1(Value);
	VectorRegister4Int* FlagPacks = (VectorRegister4Int*)Data;
	for (int i = 0; i < NumPack; i++)
		VectorIntStore(VectorIntAnd(VectorIntLoad(FlagPacks + i), MaskReg), FlagPacks + i);
}

inline void AllegroArrayOrSSE(void* Data, int NumPack, int Value)
{
	VectorRegister4Int MaskReg = VectorIntSet1(Value);
	VectorRegister4Int* FlagPacks = (VectorRegister4Int*)Data;
	for (int i = 0; i < NumPack; i++)
		VectorIntStore(VectorIntOr(VectorIntLoad(FlagPacks + i), MaskReg), FlagPacks + i);
}

//because FMatrix3x4.SetMatrixTranspose takes FMatrix
inline void AllegroSetMatrix3x4Transpose(FMatrix3x4& DstMatrix, const FMatrix44f& SrcMatrix)
{
	const float* RESTRICT Src = &(SrcMatrix.M[0][0]);
	float* RESTRICT Dest = &(DstMatrix.M[0][0]);

	Dest[0] = Src[0];   // [0][0]
	Dest[1] = Src[4];   // [1][0]
	Dest[2] = Src[8];   // [2][0]
	Dest[3] = Src[12];  // [3][0]

	Dest[4] = Src[1];   // [0][1]
	Dest[5] = Src[5];   // [1][1]
	Dest[6] = Src[9];   // [2][1]
	Dest[7] = Src[13];  // [3][1]

	Dest[8]  = Src[2];  // [0][2]
	Dest[9]  = Src[6];  // [1][2]
	Dest[10] = Src[10]; // [2][2]
	Dest[11] = Src[14]; // [3][2]
}



