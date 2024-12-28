#pragma once

#include "IDetailCustomization.h"
#include "DetailLayoutBuilder.h"
#include "AllegroComponent.h"

#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Images/SImage.h"

#define LOCTEXT_NAMESPACE "Allegro"

class SSubmeshList : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SSubmeshList)
    {}

    SLATE_ARGUMENT(TWeakObjectPtr<UAllegroComponent>, AllegroComp)

    SLATE_END_ARGS()


    void Construct(const FArguments& InArgs)
    {
        AllegroComp = InArgs._AllegroComp;

        ChildSlot[
            SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()[
                    SAssignNew(ThumbnailBox, SWrapBox)
                ]
                + SVerticalBox::Slot().AutoHeight()[
                    SNew(STextBlock)
                ]   
        ];

        for (int i = 0; i < 10; i++)
        {
            ThumbnailBox->AddSlot().Padding(4).AttachWidget(SNew(SImage));
        }
    }

    
    TSharedPtr<SWrapBox> ThumbnailBox;
    TWeakObjectPtr<UAllegroComponent> AllegroComp;
    TArray<int> SelectedSubmeshes;
};

class FAllegroComponentDetails : public IDetailCustomization
{
public:
    
    void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
    {
        return;
        //#TODO
        IDetailCategoryBuilder& MyCategory = DetailBuilder.EditCategory(TEXT("Allegro"));
        if(DetailBuilder.GetSelectedObjects().Num() == 1)
		{
            UAllegroComponent* EditingComp = static_cast<UAllegroComponent*>(DetailBuilder.GetSelectedObjects()[0].Get());
			// You can get properties using the DetailBuilder:
			//MyProperty= DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(MyClass, MyClassPropertyName));

			MyCategory.AddCustomRow(LOCTEXT("Extra info", "Row header name"))
				.WholeRowContent()
				[
					SNew(SSubmeshList).AllegroComp(EditingComp)
						//SNew(STextBlock)
						//    .Text(LOCTEXT("Extra info", "Custom row header name"))
						//    .Font(IDetailLayoutBuilder::GetDetailFont())
				];

		}
    }
};

#undef LOCTEXT_NAMESPACE