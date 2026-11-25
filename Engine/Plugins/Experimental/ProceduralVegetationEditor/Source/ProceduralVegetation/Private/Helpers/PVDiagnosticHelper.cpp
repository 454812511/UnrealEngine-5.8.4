// Copyright Epic Games, Inc. All Rights Reserved.

#include "Helpers/PVDiagnosticHelper.h"

#include "Algo/Reverse.h"
#include "Elements/PCGReroute.h"
#include "PCGCommon.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"

namespace
{
	// A named-reroute usage node pulls from its declaration through a wireless reference rather than a pin
	// edge, so the declaration node has to be located by matching settings against the owning graph.
	const UPCGNode* ResolveNamedRerouteDeclarationNode(const UPCGNode* UsageNode)
	{
		const UPCGNamedRerouteUsageSettings* UsageSettings = Cast<UPCGNamedRerouteUsageSettings>(UsageNode->GetSettings());
		if (UsageSettings == nullptr || UsageSettings->Declaration == nullptr)
		{
			return nullptr;
		}

		const UPCGGraph* Graph = UsageNode->GetGraph();
		if (Graph == nullptr)
		{
			return nullptr;
		}

		for (const UPCGNode* Node : Graph->GetNodes())
		{
			if (Node != nullptr && Node->GetSettings() == UsageSettings->Declaration)
			{
				return Node;
			}
		}

		return nullptr;
	}

	// Returns the single node feeding InNode. PVE nodes have one input pin allowing one connection
	// (UPVBaseSettings), so the upstream path is a chain. A named-reroute usage pulls from its declaration
	// through a wireless reference rather than an edge, so that link is followed explicitly.
	const UPCGNode* GetUpstreamNode(const UPCGNode* InNode)
	{
		if (InNode->GetSettings() != nullptr && InNode->GetSettings()->IsA<UPCGNamedRerouteUsageSettings>())
		{
			return ResolveNamedRerouteDeclarationNode(InNode);
		}

		for (const UPCGPin* InputPin : InNode->GetInputPins())
		{
			if (InputPin == nullptr)
			{
				continue;
			}

			for (const UPCGEdge* Edge : InputPin->Edges)
			{
				if (Edge != nullptr)
				{
					if (const UPCGPin* UpstreamPin = Edge->GetOtherPin(InputPin))
					{
						if (UpstreamPin->Node != nullptr)
						{
							return UpstreamPin->Node;
						}
					}
				}
			}
		}

		return nullptr;
	}
}

namespace PV::Diagnostic
{
	FString GetUpstreamNodeChain(const UPCGNode* InNode, const TCHAR* Separator)
	{
		// PVE nodes have a single input pin allowing a single connection (UPVBaseSettings), so the upstream
		// path is a linear chain. Walk it, collecting nodes nearest-first (reroute plumbing is traversed but
		// omitted), then reverse for data-flow order. VisitedNodes is a defensive stop against a cycle.
		TArray<FString> NodeLabels;
		TSet<const UPCGNode*> VisitedNodes;

		for (const UPCGNode* Node = InNode; Node != nullptr && !VisitedNodes.Contains(Node); Node = GetUpstreamNode(Node))
		{
			VisitedNodes.Add(Node);

			const UPCGSettings* Settings = Node->GetSettings();
			const bool bIsReroute = Settings != nullptr && Settings->IsA<UPCGRerouteSettings>();
			if (Node != InNode && bIsReroute)
			{
				continue;
			}

			FString Label = Settings != nullptr
				? FString::Printf(TEXT("%s[%s]"), *Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString(), *Settings->GetClass()->GetName())
				: Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString();

			if (Node == InNode)
			{
				Label = FString::Printf(TEXT(">>%s<<"), *Label);
			}

			NodeLabels.Add(MoveTemp(Label));
		}

		Algo::Reverse(NodeLabels);
		return FString::Join(NodeLabels, Separator);
	}
}
