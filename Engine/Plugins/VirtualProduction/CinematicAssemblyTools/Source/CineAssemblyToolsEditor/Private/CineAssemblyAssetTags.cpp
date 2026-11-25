// Copyright Epic Games, Inc. All Rights Reserved.

#include "CineAssemblyAssetTags.h"

#include "Algo/Find.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CineAssembly.h"
#include "CineAssemblySchema.h"
#include "Containers/Set.h"
#include "HAL/CriticalSection.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogCineAssemblyAssetTags, Log, All)

namespace UE::CineAssemblyTools::Private
{
	void LogMissingSchemaThumbnailTagOnce(FName SchemaPackageName)
	{
		const FAssetRegistryModule& AssetRegistryModule = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");
		if (AssetRegistryModule.Get().IsLoadingAssets())
		{
			return;
		}

		static FCriticalSection Mutex;
		static TSet<FName> LoggedSchemas;

		FScopeLock Lock(&Mutex);
		if (!LoggedSchemas.Contains(SchemaPackageName))
		{
			LoggedSchemas.Add(SchemaPackageName);
			UE_LOGF(LogCineAssemblyAssetTags, Log, "Schema '%ls' needs to be resaved so that its thumbnail texture can be read without loading the asset.", *SchemaPackageName.ToString());
		}
	}

	FGuid GetSchemaID(const FAssetData& AssetData, FName TagName)
	{
		FString SchemaIDTagValue;
		AssetData.GetTagValue(TagName, SchemaIDTagValue);

		FGuid SchemaID;
		FGuid::Parse(SchemaIDTagValue, SchemaID);
		return SchemaID;
	}

	FSoftObjectPath GetSchemaThumbnailPath(const FAssetData& SchemaAssetData)
	{
		FString ThumbnailTagValue;
		const bool bTagPresent = SchemaAssetData.GetTagValue(UCineAssemblySchema::AssetRegistryTag_ThumbnailTexture, ThumbnailTagValue);
		if (!bTagPresent)
		{
			LogMissingSchemaThumbnailTagOnce(SchemaAssetData.PackageName);
		}
		return FSoftObjectPath(ThumbnailTagValue);
	}

	FAssetData GetSchemaAssetData(const FAssetData& AssemblyAssetData)
	{
		const FGuid SchemaID = GetSchemaID(AssemblyAssetData, UCineAssembly::AssetRegistryTag_SchemaGuid);

		FString SchemaName;
		AssemblyAssetData.GetTagValue(UCineAssembly::AssetRegistryTag_AssemblyType, SchemaName);

		// Without either tag there is nothing to match a schema against, so skip the asset registry query entirely
		if (!SchemaID.IsValid() && SchemaName.IsEmpty())
		{
			return FAssetData();
		}

		// Gather the schemas by class rather than by tag value. A filter containing only tags and values is not accelerated for
		// in-memory assets, so it iterates every UObject in memory and calls GetAssetRegistryTags on every loaded asset.
		const FAssetRegistryModule& AssetRegistryModule = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");

		TArray<FAssetData> AllSchemas;
		AssetRegistryModule.Get().GetAssetsByClass(UCineAssemblySchema::StaticClass()->GetClassPathName(), AllSchemas);

		// Find the schema asset with the matching SchemaID
		if (SchemaID.IsValid())
		{
			const FString SchemaIDString = SchemaID.ToString();

			for (const FAssetData& SchemaAssetData : AllSchemas)
			{
				if (SchemaAssetData.TagsAndValues.FindTag(UCineAssemblySchema::SchemaGuidPropertyName).Equals(SchemaIDString))
				{
					return SchemaAssetData;
				}
			}
		}

		// Fallback to checking the schema asset names to find the matching one
		if (!SchemaName.IsEmpty())
		{
			if (const FAssetData* FoundSchema = Algo::FindBy(AllSchemas, FName(*SchemaName), &FAssetData::AssetName))
			{
				return *FoundSchema;
			}
		}

		return FAssetData();
	}
}
