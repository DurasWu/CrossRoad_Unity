// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Layout/Visibility.h"
#include "Widgets/SWidget.h"
#include "AllegroComponent.h"
#include "AllegroAnimCollection.h"
#include "IDetailCustomization.h"
#include "IPropertyTypeCustomization.h"
#include "Modules/ModuleManager.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SComboButton.h"
#include "EditorStyleSet.h"
#include "Editor.h"
#include "EditorCategoryUtils.h"
#include "DetailLayoutBuilder.h"
#include "IDetailPropertyRow.h"
#include "DetailCategoryBuilder.h"
#include "PropertyCustomizationHelpers.h"
#include "ClassViewerModule.h"
#include "ClassViewerFilter.h"
#include "Engine/Selection.h"
#include "Widgets/Images/SImage.h"
#include "IDetailChildrenBuilder.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimComposite.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimSequence.h"

#define LOCTEXT_NAMESPACE "Allegro"

struct FAssetData;
class IDetailLayoutBuilder;
class IPropertyHandle;
class SComboButton;

inline FText GetObjectNameSuffix(UObject* Obj)
{
	return Obj ? FText::Format(INVTEXT("#{0}"), FText::FromString(Obj->GetName())) : INVTEXT("#null");
}






class FAllegroSharedCustomization : public IPropertyTypeCustomization
{
public:
	void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override
	{
		OuterObjects.Empty();
		StructPropertyHandle->GetOuterObjects(OuterObjects);

		ValidSkeleton = GetValidSkeleton(StructPropertyHandle);
		SelectedSkeletonName = FString();
		if (ValidSkeleton)
		{
			SelectedSkeletonName = FString::Printf(TEXT("%s'%s'"), *ValidSkeleton->GetClass()->GetPathName(), *ValidSkeleton->GetPathName());
		}
	}
	void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle, IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override
	{

	}
	bool OnShouldFilterSkeleton(const FAssetData& AssetData)
	{
		const FString SkeletonName = AssetData.GetTagValueRef<FString>(TEXT("Skeleton"));
		return ValidSkeleton == nullptr || SkeletonName != SelectedSkeletonName;
	}
	USkeleton* GetValidSkeleton(TSharedRef<IPropertyHandle> InStructPropertyHandle) const
	{
		USkeleton* Skeleton = nullptr;
		TArray<UObject*> Objects;
		InStructPropertyHandle->GetOuterObjects(Objects);

		for (UObject* ObjectIter : Objects)
		{
			UAllegroAnimCollection* AnimSet = Cast<UAllegroAnimCollection>(ObjectIter);
			if (!AnimSet || !AnimSet->Skeleton)
			{
				continue;
			}

			// If we've not come across a valid skeleton yet, store this one.
			if (!Skeleton)
			{
				Skeleton = AnimSet->Skeleton;
				continue;
			}

			// We've encountered a valid skeleton before.
			// If this skeleton is not the same one, that means there are multiple
			// skeletons selected, so we don't want to take any action.
			if (AnimSet->Skeleton != Skeleton)
			{
				return nullptr;
			}
		}

		return Skeleton;
	}

	TArray<UObject*> OuterObjects;
	USkeleton* ValidSkeleton;
	FString SelectedSkeletonName;
};

class FAllegroSeqDefCustomization : public FAllegroSharedCustomization
{
public:
	void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override
	{
		FAllegroSharedCustomization::CustomizeHeader(StructPropertyHandle, HeaderRow, StructCustomizationUtils);

		HeaderRow.NameContent()[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[
				StructPropertyHandle->CreatePropertyNameWidget()
			]
			+ SHorizontalBox::Slot().VAlign(VAlign_Center)[
				SNew(STextBlock).Text(this, &FAllegroSeqDefCustomization::GetHeaderExtraText, StructPropertyHandle)
			]
		];
	}
	FText GetHeaderExtraText(TSharedRef<IPropertyHandle> StructPropertyHandle) const
	{
		if (OuterObjects.Num() == 1 && StructPropertyHandle->IsValidHandle())
		{
			FAllegroSequenceDef* StructPtr = (FAllegroSequenceDef*)StructPropertyHandle->GetValueBaseAddress((uint8*)OuterObjects[0]);
			if(StructPtr)
				return GetObjectNameSuffix(StructPtr->Sequence);
		}
		return FText();
	}
	void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle, IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override
	{
		uint32 TotalChildren = 0;
		StructPropertyHandle->GetNumChildren(TotalChildren);

		for (uint32 ChildIndex = 0; ChildIndex < TotalChildren; ++ChildIndex)
		{
			TSharedRef<IPropertyHandle> ChildHandle = StructPropertyHandle->GetChildHandle(ChildIndex).ToSharedRef();
			IDetailPropertyRow& Row = StructBuilder.AddProperty(ChildHandle);

			if (ChildHandle->GetProperty()->GetFName() == GET_MEMBER_NAME_CHECKED(FAllegroSequenceDef, Sequence))
			{
				TSharedRef<SWidget> PropWidget = SNew(SObjectPropertyEntryBox)
					.ThumbnailPool(StructCustomizationUtils.GetThumbnailPool())
					.PropertyHandle(ChildHandle)
					.AllowedClass(UAnimSequenceBase::StaticClass())
					.AllowClear(true)
					.OnShouldFilterAsset(FOnShouldFilterAsset::CreateSP(this, &FAllegroSeqDefCustomization::OnShouldFilterAnim));

				FDetailWidgetRow& WidgetRow = Row.CustomWidget();
				WidgetRow.NameContent().Widget = ChildHandle->CreatePropertyNameWidget();
				WidgetRow.ValueContent().Widget = PropWidget;
				WidgetRow.ValueContent().MinDesiredWidth(300).MaxDesiredWidth(0);
			}
//			else if (ChildHandle->GetProperty()->GetFName() == GET_MEMBER_NAME_CHECKED(FAllegroSequenceDef, Blends))
// 			{
// 				TSharedPtr<IPropertyHandleMap> BlendsPH = ChildHandle->AsMap();
// 				uint32 NumElem = 0;
// 				if (BlendsPH->GetNumElements(NumElem) == FPropertyAccess::Success)
// 				{
// 					for (uint32 ElementIndex = 0; ElementIndex < NumElem; ElementIndex++)
// 					{
// 						TSharedRef<IPropertyHandle> ElementPH = ChildHandle->GetChildHandle(ElementIndex).ToSharedRef();
// 						IDetailPropertyRow& KeyRow = StructBuilder.AddProperty(ElementPH->GetKeyHandle().ToSharedRef());
// 					}
// 				}
// 			
// 				
// 			}

		};


		//FDetailWidgetRow& BlendsRow = StructBuilder.AddCustomRow(LOCTEXT("Blends", "Blends"));
		//BlendsRow.WholeRowContent()[
		//	
		//];
	}
	bool OnShouldFilterAnim(const FAssetData& AssetData)
	{
		for (UObject* OuterObj : OuterObjects)
		{
			if (UAllegroAnimCollection* OuterAnimSet = Cast<UAllegroAnimCollection>(OuterObj))
			{
				const bool bAlreadyTaken = OuterAnimSet->FindSequenceDefByPath(AssetData.ToSoftObjectPath()) != INDEX_NONE;
				if (bAlreadyTaken)
					return true;

				bool bClassIsSupported = AssetData.AssetClassPath == UAnimComposite::StaticClass()->GetClassPathName() || AssetData.AssetClassPath == UAnimSequence::StaticClass()->GetClassPathName();
				if(!bClassIsSupported)
					return true;

			}
		}

		return OnShouldFilterSkeleton(AssetData);
	}
};

class FAllegroMeshDefCustomization : public FAllegroSharedCustomization
{
public:
	void CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override
	{
		FAllegroSharedCustomization::CustomizeHeader(StructPropertyHandle, HeaderRow, StructCustomizationUtils);

		HeaderRow.NameContent()[
			SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()[
					StructPropertyHandle->CreatePropertyNameWidget()
				]
				+ SHorizontalBox::Slot().VAlign(VAlign_Center)[
					SNew(STextBlock).Text(this, &FAllegroMeshDefCustomization::GetHeaderExtraText, StructPropertyHandle)
				]
		];
	}

	FText GetHeaderExtraText(TSharedRef<IPropertyHandle> StructPropertyHandle) const
	{
		if (OuterObjects.Num() == 1 && StructPropertyHandle->IsValidHandle())
		{
			FAllegroMeshDef* StructPtr = (FAllegroMeshDef*)StructPropertyHandle->GetValueBaseAddress((uint8*)OuterObjects[0]);
			if (StructPtr)
				return GetObjectNameSuffix(StructPtr->Mesh);
		}
		return FText();
	}
	void CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle, IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override
	{
		uint32 TotalChildren = 0;
		StructPropertyHandle->GetNumChildren(TotalChildren);

		for (uint32 ChildIndex = 0; ChildIndex < TotalChildren; ++ChildIndex)
		{
			TSharedRef<IPropertyHandle> ChildHandle = StructPropertyHandle->GetChildHandle(ChildIndex).ToSharedRef();
			IDetailPropertyRow& Row = StructBuilder.AddProperty(ChildHandle);

			if (ChildHandle->GetProperty()->GetFName() == GET_MEMBER_NAME_CHECKED(FAllegroMeshDef, Mesh))
			{
				TSharedRef<SWidget> PropWidget = SNew(SObjectPropertyEntryBox)
					.ThumbnailPool(StructCustomizationUtils.GetThumbnailPool())
					.PropertyHandle(ChildHandle)
					.AllowedClass(USkeletalMesh::StaticClass())
					.AllowClear(true)
					.OnShouldFilterAsset(FOnShouldFilterAsset::CreateSP(this, &FAllegroMeshDefCustomization::OnShouldFilterMesh));

				FDetailWidgetRow& WidgetRow = Row.CustomWidget();
				WidgetRow.NameContent().Widget = ChildHandle->CreatePropertyNameWidget();
				WidgetRow.ValueContent().Widget = PropWidget;
				WidgetRow.ValueContent().MinDesiredWidth(300).MaxDesiredWidth(600);
			}
		};
	}
	bool OnShouldFilterMesh(const FAssetData& AssetData)
	{
		for (UObject* OuterObj : OuterObjects)
		{
			if (UAllegroAnimCollection* OuterAnimSet = Cast<UAllegroAnimCollection>(OuterObj))
			{
				const bool bAlreadyTaken = OuterAnimSet->FindMeshDefByPath(AssetData.ToSoftObjectPath()) != INDEX_NONE;
				if(bAlreadyTaken)
					return true;
			}
		}
		
		return OnShouldFilterSkeleton(AssetData);
	}
};




#undef  LOCTEXT_NAMESPACE