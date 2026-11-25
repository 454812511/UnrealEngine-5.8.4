// Copyright Epic Games, Inc. All Rights Reserved.
 
#include "Nodes/PVSubgraphSettings.h"

#include "ProceduralVegetation.h"
#include "ProceduralVegetationModule.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGModule.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "Data/Registry/PCGDataTypeRegistry.h"
#include "Helpers/PCGSettingsHelpers.h"
#include "PVCommon.h"

#define LOCTEXT_NAMESPACE "PVSubgraph"

UPCGGraphInterface* UPVSubgraphSettings::GetSubgraphInterface() const
{
	// Must be the graph instance, not the graph it wraps: the base class uses this as the
	// OnGraphChangedDelegate binding target, as the identity compared in OnSubgraphChanged, and as
	// the source of user parameter values. Only the instance carries the parameter overrides.
	return SubgraphInstance ? SubgraphInstance->GraphInstance : nullptr;
}

bool FPVSubgraphElement::ExecuteInternal(FPCGContext* InContext) const
{
	if (const UPVSubgraphSettings* Settings = InContext->GetInputSettings<UPVSubgraphSettings>())
	{
		if (const UPCGGraph* Subgraph = Settings->GetSubgraph())
		{
			const UPCGGraph* CurrentGraph = nullptr;
			if (const FPCGStack* Stack = InContext->GetStack())
			{
				CurrentGraph = Stack->GetGraphForCurrentFrame();
			}
			else if (InContext->ExecutionSource.Get())
			{
				CurrentGraph = InContext->ExecutionSource->GetExecutionState().GetGraph();
			}

			if (CurrentGraph && (Subgraph == CurrentGraph || Subgraph->Contains(CurrentGraph)))
			{
				PCGE_LOG_C(Warning, GraphAndLog, InContext,
					FText::Format(LOCTEXT("PVSubgraphCyclic", "Subgraph '{0}' forms a cyclic reference back to the current graph."),
						FText::FromString(Subgraph->GetName())));
			}
		}
	}

	return FPCGSubgraphElement::ExecuteInternal(InContext);
}

void FPVSubgraphElement::PostExecuteInternal(FPCGContext* InContext) const
{
	FPCGSubgraphElement::PostExecuteInternal(InContext);

#if WITH_EDITORONLY_DATA
	const UPVSubgraphSettings* Settings = InContext->GetInputSettings<UPVSubgraphSettings>();
	check(Settings);

	for (FPCGTaggedData& OutputData : InContext->OutputData.TaggedData)
	{
		if (OutputData.Data && OutputData.Data.IsA<UPVData>())
		{
			if (const UPVData* ProceduralVegetationData = Cast<UPVData>(OutputData.Data))
			{
				FPVDebugSettings DebugSettings = ProceduralVegetationData->GetDebugSettings();
				DebugSettings.VisualizationSettings = Settings->GetDebugVisualizationSettings().VisualizationSettings;
				ProceduralVegetationData->SetDebugSettings(MoveTemp(DebugSettings));
			}
		}
	}
#endif
}

#if WITH_EDITOR
FLinearColor UPVSubgraphSettings::GetNodeTitleColor() const
{
	return PV::NodeColors::Subgraph;
}


void UPVSubgraphSettings::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	if (PropertyChangedEvent.Property && PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UPVSubgraphSettings, SubgraphInstance))
	{
		// Also rebuild the overrides
		InitializeCachedOverridableParams(/*bReset=*/true);
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

EPCGChangeType UPVSubgraphSettings::GetChangeTypeForProperty(const FName& InPropertyName) const
{
	EPCGChangeType ChangeType = Super::GetChangeTypeForProperty(InPropertyName);

	// Mark SubgraphInstance changes Structural so the base class rebinds OnGraphChangedDelegate when
	// the user retargets, undoes, or redoes the source PV graph reference.
	if ((InPropertyName == NAME_None) || (InPropertyName == GET_MEMBER_NAME_CHECKED(UPVSubgraphSettings, SubgraphInstance)))
	{
		ChangeType |= EPCGChangeType::Structural;
	}

	return ChangeType;
}

UObject* UPVSubgraphSettings::GetJumpTargetForDoubleClick() const
{
	if (SubgraphInstance && SubgraphInstance->GetProceduralVegetationAsset() &&
		SubgraphInstance->GetProceduralVegetationAsset()->GetGraph())
	{
		return SubgraphInstance->GetProceduralVegetationAsset()->GetGraph();
	}
	
	return nullptr;
}

#endif

UPVSubgraphSettings::UPVSubgraphSettings(const FObjectInitializer& InObjectInitializer)
	: Super(InObjectInitializer)
{
	SubgraphInstance = InObjectInitializer.CreateDefaultSubobject<UProceduralVegetationInstance>(this, TEXT("ProceduralVegetationSubgraphInstance"));
}


#if WITH_EDITOR
void UPVSubgraphSettings::SetCurrentRenderType(TArray<EPVRenderType> InRenderTypes)
{
	Visualization.CurrentRenderType = MoveTemp(InRenderTypes);
}
#endif

UPCGNode* UPVSubgraphSettings::CreateNode() const
{
	return NewObject<UPCGSubgraphNode>();
}

FString UPVSubgraphSettings::GetAdditionalTitleInformation() const
{
	if (auto PVSubgraph = SubgraphInstance ? SubgraphInstance->GetProceduralVegetationAsset() : nullptr)
	{
		FString GraphName; 
		
		if (PVSubgraph->GetGraph() && 
			PVSubgraph->GetGraph()->GetGraph() && 
			PVSubgraph->GetGraph()->GetGraph()->IsEmbeddedSubgraph())
		{
			UPCGGraph* SourceGraph = PVSubgraph->GetGraph()->GetGraph();
			GraphName =  SourceGraph->GetDisplayName().ToString();
		}
		else
		{
			GraphName = PVSubgraph->GetName();
		}
		
		// Use the same transformation than in the palette view to add spaces between uppercase characters
		return FName::NameToDisplayString(GraphName, /*bIsBool=*/false);
	}
	
	return LOCTEXT("NodeTitleExtendedInvalidSubgraph", "Missing Subgraph").ToString();
}

FPCGElementPtr UPVSubgraphSettings::CreateElement() const
{
	return MakeShared<FPVSubgraphElement>();
}

TArray<FPCGPinProperties> UPVSubgraphSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Properties = Super::OutputPinProperties();

	const UPCGGraph* Subgraph = GetSubgraph();
	const UPCGNode* OutputNode = Subgraph ? Subgraph->GetOutputNode() : nullptr;
	if (!OutputNode)
	{
		return Properties;
	}

	const UPCGNode* OwningNode = Cast<const UPCGNode>(GetOuter());

	// Hide any output pin that nothing is wired to inside the subgraph - it can never carry data. This matters for
	// the default "Out" pin: a graph's output node is seeded with one, and collapsing a PVE node into an embedded
	// subgraph clones that node's output pin, which is also labelled "Out", so FixPinProperties renames the real
	// pin to "Out1" and leaves the unused default ahead of it. Inspection defaults to the first visible output pin,
	// which would otherwise be the one that never has data.
	for (FPCGPinProperties& PinProperties : Properties)
	{
		const UPCGPin* InnerPin = OutputNode->GetInputPin(PinProperties.Label);
		if (!InnerPin || InnerPin->IsConnected())
		{
			continue;
		}

		// Hiding only suppresses the widget, so a pin something outside is already wired to has to stay visible:
		// otherwise that edge can no longer be seen or broken, and RebuildEdgesFromPins drops it on copy/paste.
		const UPCGPin* OuterPin = OwningNode ? OwningNode->GetOutputPin(PinProperties.Label) : nullptr;
		if (OuterPin && OuterPin->IsConnected())
		{
			continue;
		}

		PinProperties.bInvisiblePin = true;
	}

	return Properties;
}

FPCGDataTypeIdentifier UPVSubgraphSettings::GetCurrentPinTypesID(const UPCGPin* InPin) const
{
	// Resolving the inner type walks the edges feeding the subgraph's output node, so a nested subgraph node lands back here.
	// Nothing stops a PV graph from referencing itself - FPVSubgraphElement only warns about it at execution - and the scope
	// helper backing the walk memoizes a pin only once its type is resolved, so it cannot break the cycle for us.
	static thread_local TSet<const UPVSubgraphSettings*> SettingsBeingResolved;

	if (InPin && InPin->IsOutputPin() && !SettingsBeingResolved.Contains(this))
	{
		const UPCGGraph* Subgraph = GetSubgraph();
		const UPCGNode* OutputNode = Subgraph ? Subgraph->GetOutputNode() : nullptr;
		const UPCGPin* InnerPin = OutputNode ? OutputNode->GetInputPin(InPin->Properties.Label) : nullptr;

		if (InnerPin)
		{
			// Walk the edges here instead of asking the output node's settings for GetTypeUnionIDOfIncidentEdges: those
			// settings are outered to the graph rather than to the node (see UPCGGraph's constructor), and that helper
			// resolves the pin through Cast<UPCGNode>(GetOuter()), so it reports no type at all for a graph output node.
			SettingsBeingResolved.Add(this);

			// Memoize within this walk so a pin reached by several paths, or shared between this node's output pins,
			// resolves once. Nests safely: an outer scope keeps ownership of the map.
			PCGSettingsHelpers::FPinTypeScopeHelper PinTypeHelper;

			TArray<FPCGDataTypeIdentifier, TInlineAllocator<4>> UpstreamTypes;
			for (const UPCGEdge* Edge : InnerPin->Edges)
			{
				if (const UPCGPin* UpstreamPin = (Edge && Edge->IsValid()) ? Edge->GetOtherPin(InnerPin) : nullptr)
				{
					UpstreamTypes.AddUnique(PinTypeHelper.GetCurrentPinTypeID(UpstreamPin));
				}
			}

			SettingsBeingResolved.Remove(this);

			if (!UpstreamTypes.IsEmpty())
			{
				// Union rather than composition: every other dynamically typed PCG node narrows an output pin with
				// GetTypeUnionIDOfAllIncidentEdges, which resolves to the common ancestor. A composition of the
				// upstream types would instead report them all, and IsCompatible rejects a composition outright
				// when it shares no type with the downstream pin.
				const FPCGDataTypeIdentifier InnerTypes = FPCGModule::GetConstDataTypeRegistry().GetIdentifiersUnion(UpstreamTypes);
				if (InnerTypes.IsValid())
				{
					return InnerTypes;
				}
			}
		}
	}

	return Super::GetCurrentPinTypesID(InPin);
}

void UPVSubgraphSettings::SetSubgraphInternal(UPCGGraphInterface* InGraph)
{
	if (SubgraphInstance && SubgraphInstance->GraphInstance)
	{
		// Resolve through the interface so a graph instance wrapping a PV graph is accepted, not just a raw PV graph.
		UPCGGraph* ResolvedGraph = InGraph ? InGraph->GetGraph() : nullptr;

		if (UProceduralVegetationGraph* ProceduralVegetationGraph = Cast<UProceduralVegetationGraph>(ResolvedGraph))
		{
			if (ProceduralVegetationGraph->IsEmbeddedSubgraph())
			{
				UProceduralVegetation* ProceduralVegetation = NewObject<UProceduralVegetation>(this,
					NAME_None,
					RF_Transactional | RF_Public);

				if (ensure(ProceduralVegetation))
				{
					ProceduralVegetation->SetGraph(ProceduralVegetationGraph);
					SubgraphInstance->SetProceduralVegetationAsset(ProceduralVegetation);
				}
			}
			else
			{
				// Standalone PV graph: reuse the wrapping asset; don't create a per-node wrapper.
				if (UProceduralVegetation* OuterAsset = ProceduralVegetationGraph->GetTypedOuter<UProceduralVegetation>())
				{
					SubgraphInstance->SetProceduralVegetationAsset(OuterAsset);
				}
			}

			SubgraphInstance->GraphInstance->SetGraph(InGraph);
		}
		else
		{
			if (InGraph)
			{
				UE_LOGF(LogProceduralVegetation, Warning,
					"Cannot assign '%ls' to a PV subgraph node: it does not resolve to a UProceduralVegetationGraph. Clearing the subgraph.",
					*InGraph->GetPathName());
			}

			SubgraphInstance->SetProceduralVegetationAsset(nullptr);
			SubgraphInstance->GraphInstance->SetGraph(nullptr);
		}
	}
}

#undef LOCTEXT_NAMESPACE
