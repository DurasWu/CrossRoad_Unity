// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroEd.h"

#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
#include "PropertyEditorModule.h"

#include "Animation/AnimationPoseData.h"
#include "AllegroCustomization.h"
#include "PropertyEditorDelegates.h"
#include "AllegroComponentDetails.h"

#define LOCTEXT_NAMESPACE "FAllegroEdModule"

// void FAllegroModuleEd::StartupModule()
// {
// 	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
// }
// 
// void FAllegroModuleEd::ShutdownModule()
// {
// 	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
// 	// we call this function before unloading the module.
// }

static FName Name_PropertyEditor("PropertyEditor");
void FAllegroEdModule::StartupModule()
{
	
	FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>(Name_PropertyEditor);
	//PropertyModule.RegisterCustomClassLayout(TEXT("AllegroAnimSet"), FOnGetDetailCustomizationInstance::CreateStatic(&FAllegroAnimSetDetails::MakeInstance));


	PropertyModule.RegisterCustomPropertyTypeLayout(TEXT("AllegroSequenceDef"), FOnGetPropertyTypeCustomizationInstance::CreateLambda([](){ return MakeShared<FAllegroSeqDefCustomization>(); }));
	PropertyModule.RegisterCustomPropertyTypeLayout(TEXT("AllegroMeshDef"), FOnGetPropertyTypeCustomizationInstance::CreateLambda([](){ return MakeShared<FAllegroMeshDefCustomization>(); }));
	//PropertyModule.RegisterCustomPropertyTypeLayout(TEXT("AllegroBlendDef"), FOnGetPropertyTypeCustomizationInstance::CreateLambda([]() { return MakeShared<FAllegroBlendDefCustomization>(); }));

	PropertyModule.RegisterCustomClassLayout(TEXT("AllegroComponent"), FOnGetDetailCustomizationInstance::CreateLambda([](){ return MakeShared<FAllegroComponentDetails>(); }));

	PropertyModule.NotifyCustomizationModuleChanged();
}

void FAllegroEdModule::ShutdownModule()
{
	FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>(Name_PropertyEditor);
	//PropertyModule.UnregisterCustomClassLayout(TEXT("AllegroAnimSet"));

	PropertyModule.UnregisterCustomPropertyTypeLayout(TEXT("AllegroSequenceDef"));
	PropertyModule.UnregisterCustomPropertyTypeLayout(TEXT("AllegroMeshDef"));
	//PropertyModule.UnregisterCustomPropertyTypeLayout(TEXT("AllegroBlendDef"));

	PropertyModule.UnregisterCustomClassLayout(TEXT("AllegroComponent"));
}




#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FAllegroEdModule, Allegro)
