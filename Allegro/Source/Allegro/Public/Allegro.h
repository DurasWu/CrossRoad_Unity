// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Stats/Stats.h"
#include "Logging/StructuredLog.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAllegro, Log, All);

class FAllegroModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static void OnBeginFrame();
	static void OnEndFrame();
};

static constexpr int ALLEGRO_MAX_LOD = 8;
static constexpr int ALLEGRO_MAX_SUBMESH = 255;


#define ALLEGRO_UE_VERSION 5.3	//the version of engine this plugin is for


extern ALLEGRO_API int	GAllegro_ForceLOD;
extern ALLEGRO_API int	GAllegro_ShadowForceLOD;
extern ALLEGRO_API float GAllegro_DistanceScale;


#define ALLEGRO_BLEND_FRAME_NUM_MAX  4  

#define ALLEGRO_USE_LOD_SCREEN_SIZE 1

#define ALLEGRO_USE_STENCIL 0

#define ALLEGRO_USE_GPU_SCENE 0

#define ALLEGRO_DEBUG 0

#define ALLEGRO_LOD_PRE_SUBMESH 1
#define ALLEGRO_LOD_PRE_SUBMESH_FACTOR 1

#define ALLEGRO_GPU_TRANSITION 1

#define ALLEGRO_ANIMTION_TICK_LOD 0