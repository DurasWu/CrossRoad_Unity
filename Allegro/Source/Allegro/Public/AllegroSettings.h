// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "Engine/DeveloperSettingsBackedByCVars.h"

#include "AllegroSettings.generated.h"

UCLASS(Config = Allegro, defaultconfig, meta = (DisplayName = "Allegro"))
class ALLEGRO_API UAllegroDeveloperSettings : public UDeveloperSettingsBackedByCVars
{
	GENERATED_BODY()
public:

	UPROPERTY(Config, EditAnywhere, Category = "Settings")
	int32 MaxTransitionGenerationPerFrame;

	UAllegroDeveloperSettings(const FObjectInitializer& Initializer);
};