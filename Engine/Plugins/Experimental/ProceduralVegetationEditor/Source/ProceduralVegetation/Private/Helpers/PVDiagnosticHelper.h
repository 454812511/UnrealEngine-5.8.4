// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UPCGNode;

namespace PV::Diagnostic
{
	/**
	 * Builds a single-line, human-readable summary of the nodes upstream of InNode in data-flow order
	 * (the most distant sources first, InNode last and marked ">>...<<"). Reroute and named-reroute
	 * plumbing nodes are traversed but omitted, and each node is rendered as "Title[SettingsClass]".
	 * Intended for diagnostic and crash logging. Returns an empty string when InNode is null.
	 */
	FString GetUpstreamNodeChain(const UPCGNode* InNode, const TCHAR* Separator = TEXT(" -> "));
}
