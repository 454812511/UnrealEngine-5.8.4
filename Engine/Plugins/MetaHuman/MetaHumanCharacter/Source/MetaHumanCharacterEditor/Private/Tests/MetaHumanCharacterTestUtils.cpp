// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetaHumanCharacterTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "Cloud/MetaHumanCloudServicesSettings.h"

namespace UE::MetaHuman::TestUtils
{
	TUniquePtr<UE::MetaHuman::TDSUtils::FScopedExchangeCodeHandler> MakeTDSScopedExchangeCodeHandler()
	{
		FString TemplateHost;

		if (FParse::Value(FCommandLine::Get(), TEXT("-TdsTemplateHost="), TemplateHost))
		{
			FString TemplateName;

			if (FParse::Value(FCommandLine::Get(), TEXT("-TdsTemplateName="), TemplateName))
			{
				const UMetaHumanCloudServicesSettings* Settings = GetDefault<UMetaHumanCloudServicesSettings>();

				return MakeUnique<UE::MetaHuman::TDSUtils::FScopedExchangeCodeHandler>(
					TemplateHost,
					TemplateName,
					Settings->ProdEosConstants.ClientCredentialsId
				);
			}
		}

		return {};
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
