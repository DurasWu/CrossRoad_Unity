// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "../Public/AllegroCustomization.h"




#if 0
TSharedRef<IDetailCustomization> FAllegroAnimSetDetails::MakeInstance()
{
	return MakeShared<FAllegroAnimSetDetails>();
}

void FAllegroAnimSetDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	ValidSkeleton = GetValidSkeleton(DetailBuilder);
	SelectedSkeletonName = FString();
	if (ValidSkeleton)
	{
		SelectedSkeletonName = FString::Printf(TEXT("%s'%s'"), *ValidSkeleton->GetClass()->GetName(), *ValidSkeleton->GetPathName());
	}
	

	const FName SequencesFName(GET_MEMBER_NAME_CHECKED(UAllegroAnimCollection, Sequences));
	const FName MeshesFName(GET_MEMBER_NAME_CHECKED(UAllegroAnimCollection, Meshes));

	TSharedPtr<IPropertyHandleArray> SequencesPH = DetailBuilder.GetProperty(SequencesFName)->AsArray();
	check(SequencesPH.IsValid());

	{
		uint32 NumElement = 0;
		SequencesPH->GetNumElements(NumElement);

		for (uint32 i = 0; i < NumElement; i++)
		{
			TSharedRef<IPropertyHandle> ElementPH = SequencesPH->GetElement(i);
			uint32 TotalChildren = 0;
			ElementPH->GetNumChildren(TotalChildren);

			for (uint32 ChildIndex = 0; ChildIndex < TotalChildren; ++ChildIndex)
			{
				TSharedPtr<IPropertyHandle> ChildHandle = ElementPH->GetChildHandle(ChildIndex);

				
			};
		}
	}


	//DetailBuilder.AddPropertyToCategory();

	{
		TSharedRef<IPropertyHandle> MeshesPH = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UAllegroAnimCollection, Meshes));
		TSharedRef<FDetailArrayBuilder> MeshesBuilder = MakeShareable(new FDetailArrayBuilder(MeshesPH));
		MeshesBuilder->OnGenerateArrayElementWidget(FOnGenerateArrayElementWidget::CreateSP(this, &FAllegroAnimSetDetails::GenerateMeshes));
		
		IDetailCategoryBuilder& DefaultCategory = DetailBuilder.EditCategory(MeshesPH->GetDefaultCategoryName());
		DefaultCategory.AddCustomBuilder(MeshesBuilder);

		

	}

}

bool FAllegroAnimSetDetails::OnShouldFilterAnimAsset(const FAssetData& AssetData)
{
	const FString SkeletonName = AssetData.GetTagValueRef<FString>("Skeleton");
	return ValidSkeleton == nullptr || SkeletonName != SelectedSkeletonName;
}

USkeleton* FAllegroAnimSetDetails::GetValidSkeleton(IDetailLayoutBuilder& DetailBuilder) const
{
	USkeleton* Skeleton = nullptr;
	
	for (TWeakObjectPtr<UObject> ObjectIter : DetailBuilder.GetSelectedObjects())
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

void FAllegroAnimSetDetails::GenerateMeshes(TSharedRef<IPropertyHandle> PropertyHandle, int32 ArrayIndex, IDetailChildrenBuilder& ChildrenBuilder)
{
	IDetailPropertyRow& Row = ChildrenBuilder.AddProperty(PropertyHandle);


	TSharedRef<SWidget> PropWidget = SNew(SObjectPropertyEntryBox)
		.ThumbnailPool(ChildrenBuilder.GetParentCategory().GetParentLayout().GetThumbnailPool())
		.PropertyHandle(PropertyHandle)
		.AllowedClass(USkeletalMesh::StaticClass())
		.AllowClear(true)
		.OnShouldFilterAsset(FOnShouldFilterAsset::CreateSP(this, &FAllegroAnimSetDetails::OnShouldFilterAnimAsset));

	Row.CustomWidget(false).NameContent()[
		PropertyHandle->CreatePropertyNameWidget()
	].ValueContent().MaxDesiredWidth(TOptional<float>())[
		PropertyHandle->CreatePropertyValueWidget()
	];
}



TSharedRef<IPropertyTypeCustomization> FAllegroAnimSetCustomization::MakeInstance()
{
	return MakeShared<FAllegroAnimSetCustomization>();
}

void FAllegroAnimSetCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> StructPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils)
{

}

void FAllegroAnimSetCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> StructPropertyHandle, IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils)
{
	uint32 TotalChildren = 0;
	StructPropertyHandle->GetNumChildren(TotalChildren);

	for (uint32 ChildIndex = 0; ChildIndex < TotalChildren; ++ChildIndex)
	{
		TSharedRef<IPropertyHandle> ChildHandle = StructPropertyHandle->GetChildHandle(ChildIndex).ToSharedRef();
		IDetailPropertyRow& Row = StructBuilder.AddProperty(ChildHandle);
	}
}

bool FAllegroAnimSetCustomization::OnShouldFilterAnimAsset(const FAssetData& AssetData)
{
	return true;
}

USkeleton* FAllegroAnimSetCustomization::GetValidSkeleton(TSharedRef<IPropertyHandle> InStructPropertyHandle) const
{
	return ValidSkeleton;
}
#endif