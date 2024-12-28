// Copyright 2024 Lazy Marmot Games. All Rights Reserved.


#pragma once

#include "Commandlets/Commandlet.h"

#include "AllegroCommandlet.generated.h"

UCLASS()
class UAllegroCommandlet : public UCommandlet
{
	GENERATED_BODY()
private:

	int32 Main(const FString& Params);
};