// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

using UnrealBuildTool;

public class Allegro : ModuleRules
{
	public Allegro(ReadOnlyTargetRules Target) : base(Target)
	{
		//PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		//bUseUnity = false;

		PCHUsage = ModuleRules.PCHUsageMode.NoPCHs;

        PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",  "RenderCore", "Engine", "StructUtils"
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject", "Projects", "Slate", "SlateCore", "Chaos", "PhysicsCore", "Landscape", "RHI", "DeveloperSettings", 
				// ... add private dependencies that you statically link with here ...	
			}
			);

        if (Target.bBuildEditor == true)
        {
            PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "EditorStyle", "ContentBrowser", "DerivedDataCache" });
        }

        DynamicallyLoadedModuleNames.AddRange(new string[]
			{
				// ... add any modules that your module loads dynamically here ...
		});


    }
}
