// Copyright 2024 Lazy Marmot Games. All Rights Reserved.

#include "AllegroObjectVersion.h"
#include "Serialization/CustomVersion.h"

const FGuid FAllegroObjectVersion::GUID(0x1C01A2C, 0xF25D4159, 0x84EE0, 0x0CAE6E1F3);
FCustomVersionRegistration GRegisterAllegroCustomVersion(FAllegroObjectVersion::GUID, FAllegroObjectVersion::LatestVersion, TEXT("AllegroVer"));
