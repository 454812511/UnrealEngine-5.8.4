// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCGMeshPartitionInteropEditorModule.h"

#include "PCGMeshPartitionInteropEditorSettings.h"
#include "Data/PCGMeshTerrainSectionData.h"
#include "Visualizations/PCGMeshTerrainSectionDataVisualization.h"

#include "PCGModule.h"
#include "Data/Registry/PCGDataTypeRegistry.h"
#include "PCGDataVisualizationRegistry.h"

#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FPCGMegaMeshInteropEditorModule, PCGMeshPartitionInteropEditor);

DEFINE_LOG_CATEGORY(LogPCGMegaMeshInteropEditor);

void FPCGMegaMeshInteropEditorModule::StartupModule()
{
	RegisterPinColors();
	RegisterDataVisualizations();

	// Register onto the PreExit, because we need the class to be still valid to remove them from the mapping
	FCoreDelegates::OnPreExit.AddRaw(this, &FPCGMegaMeshInteropEditorModule::PreExit);
}

void FPCGMegaMeshInteropEditorModule::ShutdownModule()
{
	// Make sure to do the cleanup in PreExit if the module is shutdown while the engine still runs (like in the UnitTests for example)
	if (!IsEngineExitRequested())
	{
		PreExit();
	}

	FCoreDelegates::OnPreExit.RemoveAll(this);
}

void FPCGMegaMeshInteropEditorModule::PreExit()
{
	UnregisterDataVisualizations();
	UnregisterPinColors();
}

void FPCGMegaMeshInteropEditorModule::RegisterPinColors()
{
	FPCGDataTypeRegistry& Registry = FPCGModule::GetMutableDataTypeRegistry();
	Registry.RegisterPinColorFunction(UE::MeshPartition::FPCGDataTypeInfoMeshTerrainSection::AsId(), [](const FPCGDataTypeIdentifier&)
	{
		return GetDefault<UPCGMeshPartitionInteropEditorSettings>()->MeshTerrainSectionDataPinColor;
	});
}

void FPCGMegaMeshInteropEditorModule::UnregisterPinColors()
{
	FPCGDataTypeRegistry& Registry = FPCGModule::GetMutableDataTypeRegistry();
	Registry.UnregisterPinColorFunction(UE::MeshPartition::FPCGDataTypeInfoMeshTerrainSection::AsId());
}

void FPCGMegaMeshInteropEditorModule::RegisterDataVisualizations()
{
	FPCGDataVisualizationRegistry& DataVisRegistry = FPCGModule::GetMutablePCGDataVisualizationRegistry();
	DataVisRegistry.RegisterPCGDataVisualization(UE::MeshPartition::UPCGMeshTerrainSectionData::StaticClass(), MakeUnique<const FPCGMeshTerrainSectionDataVisualization>());
}

void FPCGMegaMeshInteropEditorModule::UnregisterDataVisualizations()
{
	if (FPCGModule::IsPCGModuleLoaded())
	{
		FPCGDataVisualizationRegistry& DataVisRegistry = FPCGModule::GetMutablePCGDataVisualizationRegistry();
		DataVisRegistry.UnregisterPCGDataVisualization(UE::MeshPartition::UPCGMeshTerrainSectionData::StaticClass());
	}
}
