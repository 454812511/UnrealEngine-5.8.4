// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "Templates/UniquePtr.h"

#include "Cloud/MetaHumanTDSUtils.h"

namespace UE::MetaHuman::TestUtils
{
	/** Creates a scoped exchange code handler configured from the TDS template command line arguments, or an empty pointer if they are not present. */
	TUniquePtr<UE::MetaHuman::TDSUtils::FScopedExchangeCodeHandler> MakeTDSScopedExchangeCodeHandler();
}

#endif // WITH_DEV_AUTOMATION_TESTS
