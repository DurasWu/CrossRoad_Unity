// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "Allegro.h"

#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
#include "Engine/Engine.h"
#include "Misc/CoreDelegates.h"

//#include "Windows/WindowsPlatformMisc.h"

DEFINE_LOG_CATEGORY(LogAllegro);


#define LOCTEXT_NAMESPACE "FAllegroModule"

void FAllegroModule::StartupModule()
{

	//FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, TEXT("OK"), TEXT("OKK"));

	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module

	AddShaderSourceDirectoryMapping(TEXT("/Plugin/Allegro"), FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("Allegro"))->GetBaseDir(), TEXT("Shaders")));

	//GEngine is not ready right now because of	"LoadingPhase": "PostConfigInit"
	FCoreDelegates::OnPostEngineInit.AddLambda([]() {
		extern void Allegro_PreRenderFrame(class FRDGBuilder&);
		extern void Allegro_PostRenderFrame(class FRDGBuilder&);

		check(GEngine);
		GEngine->GetPreRenderDelegateEx().AddStatic(&Allegro_PreRenderFrame);
		GEngine->GetPostRenderDelegateEx().AddStatic(&Allegro_PostRenderFrame);
	}); 

	FCoreDelegates::OnBeginFrame.AddStatic(&FAllegroModule::OnBeginFrame);
	FCoreDelegates::OnEndFrame.AddStatic(&FAllegroModule::OnEndFrame);
}

void FAllegroModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

void FAllegroModule::OnBeginFrame()
{
	extern int32 GAllegro_NumTransitionGeneratedThisFrame;
	GAllegro_NumTransitionGeneratedThisFrame = 0;
}

void FAllegroModule::OnEndFrame()
{

}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FAllegroModule, Allegro)



