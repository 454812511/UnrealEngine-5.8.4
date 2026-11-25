// Copyright Epic Games, Inc. All Rights Reserved.

#include "PVBaseInteractiveTool.h"

#include "ContextObjectStore.h"
#include "InteractiveToolManager.h"
#include "PCGData.h"
#include "PCGGraphExecutionInspection.h"

#include "AssetEditorMode/Tools/PCGAssetEditorInteractiveTool.h"

#include "Contexts/PVToolContext.h"

#include "DataTypes/PVData.h"

#include "Engine/World.h"

#include "Nodes/PVBaseSettings.h"

#include "Subsystems/IPCGBaseSubsystem.h"

#include "Tools/EdModeInteractiveToolsContext.h"

void UPVBaseInteractiveTool::Setup()
{
	Super::Setup();
	
	if (!ExecutionSource)
	{
		return;
	}
	
	InteractiveToolsContext = Cast<UEditorInteractiveToolsContext>(GetToolManager()->GetOuter());
	check(InteractiveToolsContext);
	TargetWorld = GetToolManager()->GetWorld();

	NodeSettings = Cast<UPVBaseSettings>(NodeToolContext->NodeSettings);

	if (ensureMsgf(TargetWorld, TEXT("Target World should be valid when setting up a PV tool")))
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags = RF_Transient;
		Params.bHideFromSceneOutliner = false;
		PreviewActor = TargetWorld->SpawnActor<AActor>(Params);

		USceneComponent* Root = NewObject<USceneComponent>(PreviewActor, TEXT("PVToolPreviewRoot"), RF_Transient);
		Root->RegisterComponent();
		PreviewActor->SetRootComponent(Root);
	}

	IPCGBaseSubsystem* Subsystem = ExecutionSource->GetExecutionState().GetSubsystem();
	check(Subsystem);
	Subsystem->GetOnPCGSourceGenerationDone().AddUObject(this, &UPVBaseInteractiveTool::OnGraphExecuted);

	const FPCGGraphExecutionInspection& Inspection = ExecutionSource->GetExecutionState().GetInspection();
	const UPVToolContextObject* PVToolContext = InteractiveToolsContext->ContextObjectStore->FindContext<UPVToolContextObject>();
	if (!PVToolContext)
	{
		return;
	}

	Inspection.InspectData(
		PVToolContext->InspectionStack,
		[this](const FPCGDataCollection& Data)
			{
				if (Data.TaggedData.Num())
				{
					CacheData(Data.TaggedData);
				}
			}
	);
}

void UPVBaseInteractiveTool::Shutdown(EToolShutdownType ShutdownType)
{
	if (IPCGBaseSubsystem* Subsystem = ExecutionSource ? ExecutionSource->GetExecutionState().GetSubsystem() : nullptr)
	{
		Subsystem->GetOnPCGSourceGenerationDone().RemoveAll(this);
	}
	
	InputData = nullptr;
	
	if (PreviewActor)
	{
		if (USceneComponent* const RootComponent = PreviewActor->GetRootComponent())
		{
			RootComponent->DestroyComponent();
		}
		PreviewActor->Destroy();
		PreviewActor = nullptr;
	}
	
	if (NodeSettings)
	{
		NodeSettings->OnSettingsChangedDelegate.RemoveAll(this);
	}

	Super::Shutdown(ShutdownType);
}

void UPVBaseInteractiveTool::RequestShutdown(const EToolShutdownType ShutdownType)
{
	GetToolManager()->PostActiveToolShutdownRequest(this, ShutdownType);
}

TObjectPtr<UWorld> UPVBaseInteractiveTool::GetTargetWorld()
{
	return TargetWorld;
}

bool UPVBaseInteractiveTool::HasCollection() const
{
	return InputData != nullptr;
}

const TSharedPtr<FManagedArrayCollection>& UPVBaseInteractiveTool::GetCollection() const
{
	static const TSharedPtr<FManagedArrayCollection> NullData(nullptr);
	return InputData != nullptr ? InputData->GetSharedCollection() : NullData;
}

void UPVBaseInteractiveTool::OnGraphExecuted(
	IPCGBaseSubsystem* InSubsystem,
	IPCGGraphExecutionSource* InExecutionSource,
	EPCGGenerationStatus InGenerationStatus
)
{
	if (InExecutionSource != ExecutionSource || InGenerationStatus != EPCGGenerationStatus::Completed)
	{
		return;
	}

	const FPCGGraphExecutionInspection& Inspection = InExecutionSource->GetExecutionState().GetInspection();

	const UPVToolContextObject* PVToolContext = GetToolManager()->GetContextObjectStore()->FindContext<UPVToolContextObject>();
	if (!PVToolContext)
	{
		InputData = nullptr;
		return;
	}

	const bool bHasData = Inspection.InspectData(
		PVToolContext->InspectionStack,
		[this](const FPCGDataCollection& Data)
			{
				if (Data.TaggedData.Num())
				{
					if (CacheData(Data.TaggedData))
					{
						OnNodeExecuted();
					}
				}
			}
	);
	
	if (!bHasData)
	{
		InputData = nullptr;
	}
}

bool UPVBaseInteractiveTool::CacheData(const TArray<FPCGTaggedData>& InTaggedData)
{
	for (const FPCGTaggedData& TaggedData : InTaggedData)
	{
		const UPVData* PVData = Cast<UPVData>(TaggedData.Data.Get());
		if (PVData != nullptr && InputData != PVData)
		{
			InputData = PVData;
			return true;
		}
	}
	
	return false;
}
