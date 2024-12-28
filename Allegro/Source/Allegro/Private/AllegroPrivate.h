// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#pragma once

#include "Allegro.h"

DECLARE_STATS_GROUP(TEXT("Allegro"), STATGROUP_ALLEGRO, STATCAT_Advanced);




DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("ViewNumCulledInstance"), STAT_ALLEGRO_ViewNumCulled, STATGROUP_ALLEGRO, ALLEGRO_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("ViewNumVisibleInstance"), STAT_ALLEGRO_ViewNumVisible, STATGROUP_ALLEGRO, ALLEGRO_API);

DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("ShadowNumCulledInstance"), STAT_ALLEGRO_ShadowNumCulled, STATGROUP_ALLEGRO, ALLEGRO_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("ShadowNumVisibleInstance"), STAT_ALLEGRO_ShadowNumVisible, STATGROUP_ALLEGRO, ALLEGRO_API);


DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("NumTransitionPoseGenerated"), STAT_ALLEGRO_NumTransitionPoseGenerated, STATGROUP_ALLEGRO, ALLEGRO_API);


#define ALLEGRO_SCOPE_CYCLE_COUNTER(CounterName) DECLARE_SCOPE_CYCLE_COUNTER(TEXT(#CounterName), STAT_ALLEGRO_##CounterName, STATGROUP_ALLEGRO)

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

#define ALLEGRO_AUTO_CVAR_DEBUG(Type, Name, DefaultValue, Help, Flags) \
Type GAllegro_##Name = DefaultValue; \
FAutoConsoleVariableRef CVarAllegro_##Name(TEXT("allegro."#Name), GAllegro_##Name, TEXT(Help), Flags); \

#else

#define ALLEGRO_AUTO_CVAR_DEBUG(Type, Name, DefaultValue, Help, Flags) constexpr Type GAllegro_##Name = DefaultValue;



#endif

