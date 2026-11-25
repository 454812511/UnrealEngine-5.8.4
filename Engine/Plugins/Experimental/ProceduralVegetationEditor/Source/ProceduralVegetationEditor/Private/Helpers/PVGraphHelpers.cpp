// Copyright Epic Games, Inc. All Rights Reserved.

#include "PVGraphHelpers.h"

#include "PCGEdge.h"

#include "Nodes/PVBaseSettings.h"
#include "Dataflow/DataflowCollectionSpreadSheetWidget.h"

#include "DataTypes/PVGrowthData.h"
#include "DataTypes/PVMeshData.h"

namespace PV::Graph
{
	UPVBaseSettings* GetNodeSettings(const UPCGNode* InNode)
	{
		return InNode
			? Cast<UPVBaseSettings>(InNode->GetSettings())
			: nullptr;
	}

	IPVRenderSettings* GetRenderSettings(const UPCGNode* InNode)
	{
		if (InNode && InNode->GetSettings())
		{
			if (InNode->GetSettings()->Implements<UPVRenderSettings>())
			{
				return Cast<IPVRenderSettings>(InNode->GetSettings());
			}
		}

		return nullptr;
	}

	UPCGPin* GetInPinFromNode(UPCGNode* InNode)
	{
		return InNode
			? InNode->GetInputPin(PCGPinConstants::DefaultInputLabel)
			: nullptr;
	}

	UPCGPin* GetOutPinFromNode(UPCGNode* InNode)
	{
		if (!InNode)
		{
			return nullptr;
		}

		auto CanCarryData = [](const UPCGPin* InPin)
		{
			return InPin && !InPin->Properties.bInvisiblePin && !InPin->Properties.IsDatalessPin();
		};

		UPCGPin* DefaultPin = InNode->GetOutputPin(PCGPinConstants::DefaultOutputLabel);
		if (CanCarryData(DefaultPin))
		{
			return DefaultPin;
		}

		// A subgraph node mirrors its graph's output node, where a collapsed pin colliding with the default "Out"
		// label is renamed ("Out1", "Out2", ...) and the unused default is hidden, so fall back to the first pin
		// that can actually carry data.
		for (const TObjectPtr<UPCGPin>& Pin : InNode->GetOutputPins())
		{
			if (CanCarryData(Pin))
			{
				return Pin;
			}
		}

		return DefaultPin;
	}

	UPCGPin* GetUpstreamPin(UPCGPin* InPin)
	{
		return (InPin && InPin->Edges.Num())
			? InPin->Edges[0]->InputPin
			: nullptr;
	}

	UPCGPin* GetDownstreamPin(UPCGPin* InPin)
	{
		return (InPin && InPin->Edges.Num())
			? InPin->Edges[0]->OutputPin
			: nullptr;
	}

	TArray<TObjectPtr<UPCGPin>> GetAllPVPinsFromNode(const UPCGNode* InNode)
	{
		static TArray PinTypes = {
			FPCGDataTypeIdentifier{FPVDataTypeInfoGrowth::AsId()},
			FPCGDataTypeIdentifier{FPVDataTypeInfoMesh::AsId()}
		};

		TArray<TObjectPtr<UPCGPin>> AllPins;
		for (TObjectPtr<UPCGPin> OutputPin : InNode->GetOutputPins())
		{
			if (PinTypes.Contains(OutputPin->GetCurrentTypesID()))
			{
				AllPins.Add(OutputPin);
			}
		}
		for (TObjectPtr<UPCGPin> InputPin : InNode->GetInputPins())
		{
			if (PinTypes.Contains(InputPin->GetCurrentTypesID()))
			{
				AllPins.Add(InputPin);
			}
		}

		return AllPins;
	}
}
