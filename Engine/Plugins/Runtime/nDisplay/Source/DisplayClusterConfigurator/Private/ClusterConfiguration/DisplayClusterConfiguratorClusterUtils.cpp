// Copyright Epic Games, Inc. All Rights Reserved.

#include "ClusterConfiguration/DisplayClusterConfiguratorClusterUtils.h"

#include "DisplayClusterConfiguratorBlueprintEditor.h"
#include "DisplayClusterConfigurationTypes.h"
#include "DisplayClusterConfiguratorLog.h"
#include "DisplayClusterConfiguratorPropertyUtils.h"
#include "DisplayClusterConfiguratorStyle.h"
#include "DisplayClusterConfiguratorUtils.h"

#include "Factories.h"
#include "ObjectTools.h"
#include "Editor/Transactor.h"

#define LOCTEXT_NAMESPACE "FDisplayClusterConfiguratorClusterUtils"

namespace UE::DisplayClusterConfiguratorClusterUtils
{
	/**
	 * Gets a unique name for something given a list of existing names.
	 * @param InitialName - The initial name the object wants
	 * @param UsedNames - The list of names that have already been used
	 * @param Class - The class to generate a name for
	 * @param Parent - The parent to check for in-memory objects that collide with the name
	 * @param bAddZero - Whether to add an "_0" to the initial name if it is unique
	 * @returns A unique name for the object
	 */
	FString GetUniqueName(const FString& InitialName, const TArray<FString>& UsedNames, const UClass* Class, UObject* Parent, bool bAddZero = false)
	{
		FString NewName;
		if (!bAddZero && !UsedNames.Contains(InitialName))
		{
			// Name doesn't need to be modified
			NewName = InitialName;
		}
		else
		{
			int32 Counter = bAddZero ? 0 : 1;

			// Find the start of the existing numeric suffix
			int32 Index = InitialName.Len();
			while (Index > 0 && InitialName[Index-1] >= '0' && InitialName[Index-1] <= '9')
			{
				--Index;
			}

			FString BaseName = InitialName;
			if (Index < BaseName.Len())
			{
				// Strip away the suffix and store the value in the counter so we can count up from there
				FString NumericSuffix = BaseName.RightChop(Index);
				Counter = FCString::Atoi(*NumericSuffix);
				NumericSuffix = FString::FromInt(Counter); // Restringify the counter to account for leading 0s that we don't want to remove
				BaseName.RemoveAt(BaseName.Len() - NumericSuffix.Len(), NumericSuffix.Len(), EAllowShrinking::No);
			}
			else
			{
				// No existing suffix, so add our underscore separator
				BaseName += "_";
			}

			do
			{
				NewName = FString::Printf(TEXT("%s%d"), *BaseName, Counter);
				++Counter;
			}
			while (UsedNames.Contains(NewName));
		}
	
		// If there is already an in-memory object connected to the parent cluster with our generated name, we need to use a globally unique object name.
		if (StaticFindObject(nullptr, Parent, *NewName, EFindObjectFlags::ExactClass))
		{
			NewName = MakeUniqueObjectName(Parent, Class, *NewName).ToString();
		}

		return NewName;
	}
	
	const FString DefaultNewHostName = TEXT("Host");
}

using namespace UE::DisplayClusterConfiguratorPropertyUtils;

UDisplayClusterConfigurationHostDisplayData* UE::DisplayClusterConfiguratorClusterUtils::FindOrCreateHostDisplayData(UDisplayClusterConfigurationCluster* Cluster, FString HostIPAddress)
{
	// [UE-386933] The cluster can be null or pending kill after an undo, and this function is on the
	// recovery path for exactly that state. Fail the lookup rather than dereferencing it; callers
	// already handle a null return.
	if (!IsValid(Cluster))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot find or create host display data for '%ls': the cluster is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.", *HostIPAddress);

		return nullptr;
	}

	// In some cases, existing host display data may be pending kill, such as if the user recently performed an undo to a state
	// prior to the data's existence. In this case, simply remove existing host data that is pending kill and create a new one to use.
	TArray<FString> PendingKillHostData;
	for (TPair<FString, TObjectPtr<UDisplayClusterConfigurationHostDisplayData>>& HostPair : Cluster->HostDisplayData)
	{
		if (!IsValid(HostPair.Value))
		{
			PendingKillHostData.Add(HostPair.Key);
		}
	}

	for (const FString& Host : PendingKillHostData)
	{
		Cluster->HostDisplayData.Remove(Host);
	}

	if (!Cluster->HostDisplayData.Contains(HostIPAddress))
	{
		const FString HostName = GetUniqueNameForHost(DefaultNewHostName, Cluster, true);
		UDisplayClusterConfigurationHostDisplayData* NewData = NewObject<UDisplayClusterConfigurationHostDisplayData>(Cluster, NAME_None, RF_Transactional);
		NewData->HostName = FText::FromString(HostName);
		NewData->Color = FDisplayClusterConfiguratorStyle::Get().GetDefaultColor(Cluster->HostDisplayData.Num()).CopyWithNewOpacity(1.0f);

		Cluster->HostDisplayData.Add(HostIPAddress, NewData);
	}

	return Cluster->HostDisplayData[HostIPAddress];
}

bool UE::DisplayClusterConfiguratorClusterUtils::RemoveUnusedHostDisplayData(UDisplayClusterConfigurationCluster* Cluster)
{
	// [UE-386933] The cluster can be null or pending kill after an undo. Report that nothing was
	// removed rather than dereferencing it.
	if (!IsValid(Cluster))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot remove unused host display data: the cluster is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.");

		return false;
	}

	bool bHostDataRemoved = false;

	TArray<FString> UnusedHosts;
	Cluster->HostDisplayData.GetKeys(UnusedHosts);

	// [UE-386933] Set when a node could not be read, so the set of hosts still in use is known to be
	// incomplete.
	bool bConfigDataIncomplete = false;

	for (const TPair<FString, TObjectPtr<UDisplayClusterConfigurationClusterNode>>& NodePair : Cluster->Nodes)
	{
		// [UE-386933] Node value can be nullptr after an undo, guard.
		if (!IsValid(NodePair.Value) || NodePair.Key.IsEmpty())
		{
			bConfigDataIncomplete = true;
			continue;
		}

		UnusedHosts.Remove(NodePair.Value->Host);
	}

	// [UE-386933] Only prune when every node could be read. An unresolvable node's Host cannot be
	// determined, so the host it referenced looks unused here and would be deleted along with the user's
	// host name and colour, even though the node's key is deliberately preserved for a later fix to
	// re-resolve. Removing nothing is recoverable; removing the host data is not.
	if (bConfigDataIncomplete)
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Skipped removing unused host display data: one or more cluster nodes could not be "
			"resolved, so it is not known which hosts are still in use. "
			"Recompile and save the nDisplay config to clear this.");

		return false;
	}

	if (UnusedHosts.Num() > 0)
	{
		Cluster->Modify();

		for (const FString& UnusedHost : UnusedHosts)
		{
			// [UE-386933] Host data value can be nullptr after an undo, guard. Remove the key either way.
			if (UDisplayClusterConfigurationHostDisplayData* HostData = Cluster->HostDisplayData.FindRef(UnusedHost))
			{
				HostData->MarkAsGarbage();
			}

			Cluster->HostDisplayData.Remove(UnusedHost);
		}

		bHostDataRemoved = true;
	}

	return bHostDataRemoved;
}

FString UE::DisplayClusterConfiguratorClusterUtils::GetUniqueNameForHost(FString InitialName, UDisplayClusterConfigurationCluster* ParentCluster, bool bAddZero)
{
	// [UE-386933] The cluster can be null or pending kill after an undo. Without it there is nothing to
	// check the name against, so hand back the initial name rather than dereferencing it.
	if (!IsValid(ParentCluster))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot make the host name '%ls' unique: the parent cluster is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.", *InitialName);

		return InitialName;
	}

	TArray<FString> UsedNames;
	for (TPair<FString, TObjectPtr<UDisplayClusterConfigurationHostDisplayData>>& HostPair : ParentCluster->HostDisplayData)
	{
		// [UE-386933] Host data value can be nullptr after an undo, guard. Such an entry contributes
		// no name to collide with, so skipping it is the correct result and not just the safe one.
		if (!HostPair.Value || HostPair.Key.IsEmpty())
		{
			continue;
		}

		UsedNames.Add(HostPair.Value->HostName.ToString());
	}

	return GetUniqueName(InitialName, UsedNames, UDisplayClusterConfigurationClusterNode::StaticClass(), ParentCluster, bAddZero);
}

FString UE::DisplayClusterConfiguratorClusterUtils::GetAddressForHost(UDisplayClusterConfigurationHostDisplayData* HostDisplayData)
{
	// [UE-386933] Host data can be null or pending kill after an undo. Answer "no address" rather than
	// dereferencing it.
	if (!IsValid(HostDisplayData))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot get a host address: the host display data is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.");

		return FString();
	}

	FString HostAddress;
	if (UDisplayClusterConfigurationCluster* HostParent = Cast<UDisplayClusterConfigurationCluster>(HostDisplayData->GetOuter()))
	{
		if (const FString* HostKey = HostParent->HostDisplayData.FindKey(HostDisplayData))
		{
			HostAddress = *HostKey;
		}
	}

	return HostAddress;
}

bool UE::DisplayClusterConfiguratorClusterUtils::RemoveHost(UDisplayClusterConfigurationCluster* Cluster, FString Host)
{
	// [UE-386933] The cluster can be null or pending kill after an undo, and both call sites reach this
	// from a user action on the host node. Report that nothing was removed rather than asserting.
	if (!IsValid(Cluster))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot remove host '%ls': the cluster is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.", *Host);

		return false;
	}

	bool bDataRemoved = false;

	if (Cluster->HostDisplayData.Contains(Host))
	{
		Cluster->Modify();
		
		// [UE-386933] Contains() only proves the key is present, and the value can be nullptr after an
		// undo. Modify the host data if it resolves and remove the key either way, matching the idiom in
		// RemoveUnusedHostDisplayData.
		if (UDisplayClusterConfigurationHostDisplayData* HostData = Cluster->HostDisplayData.FindRef(Host))
		{
			HostData->Modify();
		}

		Cluster->HostDisplayData.Remove(Host);

		bDataRemoved = true;
	}

	// Collect the nodes themselves rather than their keys. RemoveClusterNodeFromCluster() takes the node,
	// so carrying the pointer avoids looking the key back up in a map that the removal loop is mutating,
	// and keeps the validity check on the same value that is later used.
	TArray<UDisplayClusterConfigurationClusterNode*> ClusterNodesToRemove;
	for (const TPair<FString, TObjectPtr<UDisplayClusterConfigurationClusterNode>>& Pair : Cluster->Nodes)
	{
		if (IsValid(Pair.Value) && Pair.Value->Host == Host)
		{
			ClusterNodesToRemove.Add(Pair.Value);
		}
	}

	for (UDisplayClusterConfigurationClusterNode* ClusterNodeToRemove : ClusterNodesToRemove)
	{
		RemoveClusterNodeFromCluster(ClusterNodeToRemove);
	}

	FDisplayClusterConfiguratorUtils::MarkDisplayClusterBlueprintAsModified(Cluster, true);
	
	return bDataRemoved;
}

UDisplayClusterConfigurationHostDisplayData* UE::DisplayClusterConfiguratorClusterUtils::GetHostDisplayDataForClusterNode(UDisplayClusterConfigurationClusterNode* ClusterNode)
{
	// [UE-386933] A node can be null or pending kill after an undo. Answer "none" rather than asserting.
	if (!IsValid(ClusterNode))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot get host display data: the cluster node is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.");

		return nullptr;
	}

	UDisplayClusterConfigurationHostDisplayData* HostDisplayData = nullptr;
	if (UDisplayClusterConfigurationCluster* ClusterNodeParent = Cast<UDisplayClusterConfigurationCluster>(ClusterNode->GetOuter()))
	{
		// [UE-386933] Contains() would only prove the key is present, not that it resolves to host data,
		// so look the value up. An entry that does not resolve is reported as no host data, matching the
		// nullptr this function already returns when there is no entry at all.
		UDisplayClusterConfigurationHostDisplayData* const FoundHostData = ClusterNodeParent->HostDisplayData.FindRef(ClusterNode->Host);
		if (IsValid(FoundHostData))
		{
			HostDisplayData = FoundHostData;
		}
	}

	return HostDisplayData;
}

FString UE::DisplayClusterConfiguratorClusterUtils::GetClusterNodeName(UDisplayClusterConfigurationClusterNode* ClusterNode)
{
	// [UE-386933] A node can be null or pending kill after an undo. Answer "no name" rather than asserting.
	if (!IsValid(ClusterNode))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot get a cluster node name: the cluster node is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.");

		return "";
	}

	if (UDisplayClusterConfigurationCluster* ClusterNodeParent = Cast<UDisplayClusterConfigurationCluster>(ClusterNode->GetOuter()))
	{
		if (const FString* KeyPtr = ClusterNodeParent->Nodes.FindKey(ClusterNode))
		{
			return *KeyPtr;
		}
	}

	return "";
}

bool UE::DisplayClusterConfiguratorClusterUtils::IsClusterNodePrimary(UDisplayClusterConfigurationClusterNode* ClusterNode)
{
	// [UE-386933] A node can be null or pending kill after an undo. Answer "not primary" rather than asserting.
	if (!IsValid(ClusterNode))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot determine whether a cluster node is primary: it is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.");

		return false;
	}

	if (const UDisplayClusterConfigurationCluster* Cluster = Cast<UDisplayClusterConfigurationCluster>(ClusterNode->GetOuter()))
	{
		const FString PrimaryNodeId = Cluster->PrimaryNode.Id;
		if (Cluster->Nodes.Contains(PrimaryNodeId) && Cluster->Nodes[PrimaryNodeId] == ClusterNode)
		{
			return true;
		}
	}

	return false;
}

FString UE::DisplayClusterConfiguratorClusterUtils::GetUniqueNameForClusterNode(FString InitialName, UDisplayClusterConfigurationCluster* ParentCluster, bool bAddZero)
{
	InitialName = ObjectTools::SanitizeObjectName(InitialName);

	// [UE-386933] The cluster can be null or pending kill after an undo. Without it there is nothing to
	// check the name against, so hand back the sanitized name rather than asserting.
	if (!IsValid(ParentCluster))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot make the cluster node name '%ls' unique: the parent cluster is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.", *InitialName);

		return InitialName;
	}

	TArray<FString> UsedNames;
	ParentCluster->Nodes.GenerateKeyArray(UsedNames);

	return GetUniqueName(InitialName, UsedNames, UDisplayClusterConfigurationClusterNode::StaticClass(), ParentCluster, bAddZero);
}

UDisplayClusterConfigurationClusterNode* UE::DisplayClusterConfiguratorClusterUtils::AddClusterNodeToCluster(UDisplayClusterConfigurationClusterNode* ClusterNode, UDisplayClusterConfigurationCluster* Cluster, FString NewClusterNodeName)
{
	// [UE-386933] Either argument can be null or pending kill after an undo, so fail the operation
	// rather than asserting on it. Callers already handle a null return.
	if (!IsValid(Cluster) || !IsValid(ClusterNode))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot add cluster node '%ls' because the %ls is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.",
			*NewClusterNodeName, !IsValid(Cluster) ? TEXT("parent cluster") : TEXT("cluster node"));

		return nullptr;
	}

	FString ClusterNodeName = "";

	// First, remove the cluster node from its current parent cluster, if it has one.
	if (UDisplayClusterConfigurationCluster* ClusterNodeParent = Cast<UDisplayClusterConfigurationCluster>(ClusterNode->GetOuter()))
	{
		if (const FString* OldKeyPtr = ClusterNodeParent->Nodes.FindKey(ClusterNode))
		{
			ClusterNodeName = *OldKeyPtr;

			ClusterNodeParent->Modify();

			const FName FieldName = GET_MEMBER_NAME_CHECKED(UDisplayClusterConfigurationCluster, Nodes);
			RemoveKeyFromMap(ClusterNodeParent, FieldName, ClusterNodeName);
		}
	}

	if (!NewClusterNodeName.IsEmpty())
	{
		ClusterNodeName = NewClusterNodeName;
	}

	if (ClusterNodeName.IsEmpty())
	{
		ClusterNodeName = TEXT("ClusterNode");
	}

	ClusterNodeName = GetUniqueNameForClusterNode(ClusterNodeName, Cluster);

	Cluster->Modify();
	ClusterNode->Modify();
	ClusterNode->Rename(*ClusterNodeName, Cluster, REN_DontCreateRedirectors);

	const FName FieldName = GET_MEMBER_NAME_CHECKED(UDisplayClusterConfigurationCluster, Nodes);
	UDisplayClusterConfigurationClusterNode* NewClusterNode = CastChecked<UDisplayClusterConfigurationClusterNode>(AddKeyWithInstancedValueToMap(Cluster, FieldName, ClusterNodeName, ClusterNode));

	check(ClusterNode != NewClusterNode);
	ClusterNode->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
	ClusterNode->SetFlags(RF_Transient);
	
	return NewClusterNode;
}

bool UE::DisplayClusterConfiguratorClusterUtils::RemoveClusterNodeFromCluster(UDisplayClusterConfigurationClusterNode* ClusterNode)
{
	// [UE-386933] A node can be null or pending kill after an undo. Report that nothing was removed
	// rather than asserting.
	if (!IsValid(ClusterNode))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot remove a cluster node from its cluster: the node is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.");

		return false;
	}

	if (UDisplayClusterConfigurationCluster* ClusterNodeParent = Cast<UDisplayClusterConfigurationCluster>(ClusterNode->GetOuter()))
	{
		if (const FString* KeyPtr = ClusterNodeParent->Nodes.FindKey(ClusterNode))
		{
			const FString ClusterNodeName = *KeyPtr;

			ClusterNode->Modify();

			ClusterNodeParent->Modify();

			const bool bRemovingPrimaryNode = IsClusterNodePrimary(ClusterNode);
			if (bRemovingPrimaryNode)
			{
				ClusterNodeParent->PrimaryNode.Id.Empty();
			}			
			
			RemoveKeyFromMap(ClusterNodeParent, GET_MEMBER_NAME_CHECKED(UDisplayClusterConfigurationCluster, Nodes), ClusterNodeName);
			ensure (ClusterNode->GetPackage() == GetTransientPackage());
			
			ClusterNode->SetFlags(RF_Transient);

			if (bRemovingPrimaryNode)
			{
				// [UE-386933] A key can outlive its node object and FindArbitraryElement() would happily
				// return one of those. Take the first entry that is actually accepted as primary instead;
				// it is no less arbitrary. Test with IsValid rather than a null check, because
				// SetClusterNodeAsPrimary() rejects pending-kill nodes too, and only stop once one has
				// been accepted, so a dead entry early in the map does not leave the cluster with no
				// primary while valid candidates remain. If none are accepted the primary id stays empty,
				// which is the same state the removal above already leaves it in.
				for (const TTuple<FString, TObjectPtr<UDisplayClusterConfigurationClusterNode>>& NodesKeyVal : ClusterNodeParent->Nodes)
				{
					if (IsValid(NodesKeyVal.Value) && SetClusterNodeAsPrimary(NodesKeyVal.Value))
					{
						break;
					}
				}
			}
			
			FDisplayClusterConfiguratorUtils::MarkDisplayClusterBlueprintAsModified(ClusterNodeParent, true);
			
			return true;
		}
	}

	return false;
}

bool UE::DisplayClusterConfiguratorClusterUtils::RenameClusterNode(UDisplayClusterConfigurationClusterNode* ClusterNode, FString NewClusterNodeName)
{
	// [UE-386933] A node can be null or pending kill after an undo. Report that the rename did not
	// happen rather than asserting.
	if (!IsValid(ClusterNode))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot rename a cluster node to '%ls': the node is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.", *NewClusterNodeName);

		return false;
	}

	if (UDisplayClusterConfigurationCluster* ClusterNodeParent = Cast<UDisplayClusterConfigurationCluster>(ClusterNode->GetOuter()))
	{
		if (const FString* KeyPtr = ClusterNodeParent->Nodes.FindKey(ClusterNode))
		{
			const FString OldClusterNodeName = *KeyPtr;

			if (KeyPtr->Equals(NewClusterNodeName))
			{
				return false;
			}

			const bool bIsPrimary = IsClusterNodePrimary(ClusterNode);

			ClusterNode->Modify();
			ClusterNodeParent->Modify();

			const FString UniqueName = GetUniqueNameForClusterNode(NewClusterNodeName, ClusterNodeParent);
			const FName FieldName = GET_MEMBER_NAME_CHECKED(UDisplayClusterConfigurationCluster, Nodes);
			RemoveKeyFromMap(ClusterNodeParent, FieldName, OldClusterNodeName);

			// Rename after remove, before add.
			ClusterNode->Rename(*UniqueName, ClusterNodeParent, REN_DontCreateRedirectors);
			
			UDisplayClusterConfigurationClusterNode* NewClusterNode = CastChecked<UDisplayClusterConfigurationClusterNode>(AddKeyWithInstancedValueToMap(ClusterNodeParent, FieldName, UniqueName, ClusterNode));

			check(ClusterNode != NewClusterNode);
			ClusterNode->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
			ClusterNode->SetFlags(RF_Transient);
			
			// If the cluster node was a primary node before the rename, we need to update the primary reference in the cluster with the new name
			if (bIsPrimary)
			{
				SetClusterNodeAsPrimary(NewClusterNode);
			}

			return true;
		}
	}

	return false;
}

bool UE::DisplayClusterConfiguratorClusterUtils::SetClusterNodeAsPrimary(UDisplayClusterConfigurationClusterNode* ClusterNode)
{
	// [UE-386933] A node can be null or pending kill after an undo, and RemoveClusterNodeFromCluster()
	// picks a replacement primary from the map. Report failure rather than asserting.
	if (!IsValid(ClusterNode))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot set a cluster node as primary: the node is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.");

		return false;
	}

	if (UDisplayClusterConfigurationCluster* ClusterNodeParent = Cast<UDisplayClusterConfigurationCluster>(ClusterNode->GetOuter()))
	{
		const FString* NodeIdPtr = ClusterNodeParent->Nodes.FindKey(ClusterNode);

		if (NodeIdPtr != nullptr)
		{
			ClusterNodeParent->Modify();
			ClusterNodeParent->PrimaryNode.Id = *NodeIdPtr;
			return true;
		}
	}

	return false;
}

FString UE::DisplayClusterConfiguratorClusterUtils::GetViewportName(UDisplayClusterConfigurationViewport* Viewport)
{
	// [UE-386933] A viewport can be null or pending kill after an undo. Answer "no name" rather than asserting.
	if (!IsValid(Viewport))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot get a viewport name: the viewport is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.");

		return "";
	}

	if (UDisplayClusterConfigurationClusterNode* ViewportParent = Cast<UDisplayClusterConfigurationClusterNode>(Viewport->GetOuter()))
	{
		if (const FString* KeyPtr = ViewportParent->Viewports.FindKey(Viewport))
		{
			return *KeyPtr;
		}
	}

	return "";
}

FString UE::DisplayClusterConfiguratorClusterUtils::GetUniqueNameForViewport(FString InitialName, UDisplayClusterConfigurationClusterNode* ParentClusterNode, bool bAddZero)
{
	InitialName = ObjectTools::SanitizeObjectName(InitialName);

	// [UE-386933] The parent node can be null or pending kill after an undo. Without it there is nothing
	// to check the name against, so hand back the sanitized name rather than asserting.
	if (!IsValid(ParentClusterNode))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot make the viewport name '%ls' unique: the parent cluster node is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.", *InitialName);

		return InitialName;
	}

	// Viewport names must be unique across the entire cluster, not just within its parent cluster nodes. Gather all of the viewport names
	// in the cluster to check for uniqueness. Add the parent cluster node's viewports first, in case we can't get to the root cluster through
	// the cluster node's Outer (i.e. the cluster node has not been added to the cluster yet)
	TSet<FString> UsedNames;
	for (const TPair<FString, TObjectPtr<UDisplayClusterConfigurationViewport>>& ViewportKeyPair : ParentClusterNode->Viewports)
	{
		UsedNames.Add(ViewportKeyPair.Key);
	}

	if (UDisplayClusterConfigurationCluster* Cluster = Cast<UDisplayClusterConfigurationCluster>(ParentClusterNode->GetOuter()))
	{
		for (const TPair<FString, TObjectPtr<UDisplayClusterConfigurationClusterNode>>& ClusterNodeKeyPair : Cluster->Nodes)
		{
			UDisplayClusterConfigurationClusterNode* ClusterNode = ClusterNodeKeyPair.Value;
			if (ClusterNode && ClusterNode != ParentClusterNode)
			{
				for (const TPair<FString, TObjectPtr<UDisplayClusterConfigurationViewport>>& ViewportKeyPair : ClusterNode->Viewports)
				{
					UsedNames.Add(ViewportKeyPair.Key);
				}
			}
		}
	}

	TArray<FString> UsedNamesArray = UsedNames.Array();
	
	return GetUniqueName(InitialName, UsedNamesArray, UDisplayClusterConfigurationViewport::StaticClass(), ParentClusterNode, bAddZero);
}

UDisplayClusterConfigurationViewport* UE::DisplayClusterConfiguratorClusterUtils::AddViewportToClusterNode(UDisplayClusterConfigurationViewport* Viewport, UDisplayClusterConfigurationClusterNode* ClusterNode, FString NewViewportName)
{
	// [UE-386933] Either argument can be null or pending kill after an undo, and ClusterNode is also
	// null when AddClusterNodeToCluster() above has already failed for the same reason. Fail the
	// operation rather than asserting on it. Callers already handle a null return.
	if (!IsValid(Viewport) || !IsValid(ClusterNode))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot add viewport '%ls' because the %ls is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.",
			*NewViewportName, !IsValid(ClusterNode) ? TEXT("parent cluster node") : TEXT("viewport"));

		return nullptr;
	}

	FString ViewportName = "";

	// First, remove the viewport from its current parent node, if it has one.
	if (UDisplayClusterConfigurationClusterNode* ParentNode = Cast<UDisplayClusterConfigurationClusterNode>(Viewport->GetOuter()))
	{
		if (const FString* ViewportNamePtr = ParentNode->Viewports.FindKey(Viewport))
		{
			// Keep the existing name, so re-parenting a viewport preserves its key. Other config data
			// references viewports by name (per-viewport color grading, movie pipeline selections, the
			// JSON export), so renaming on a move would break those references.
			ViewportName = *ViewportNamePtr;

			ParentNode->Modify();

			// Pass the copy: removing the key invalidates ViewportNamePtr, which points into the map.
			RemoveKeyFromMap(ParentNode, GET_MEMBER_NAME_CHECKED(UDisplayClusterConfigurationClusterNode, Viewports), ViewportName);
		}
	}

	if (!NewViewportName.IsEmpty())
	{
		ViewportName = NewViewportName;
	}

	if (ViewportName.IsEmpty())
	{
		ViewportName = TEXT("Viewport");
	}

	ViewportName = GetUniqueNameForViewport(ViewportName, ClusterNode);

	ClusterNode->Modify();
	Viewport->Modify();
	Viewport->Rename(*ViewportName, ClusterNode, REN_DontCreateRedirectors | REN_DoNotDirty | REN_AllowPackageLinkerMismatch);

	const FName FieldName = GET_MEMBER_NAME_CHECKED(UDisplayClusterConfigurationClusterNode, Viewports);
	UDisplayClusterConfigurationViewport* NewViewport = CastChecked<UDisplayClusterConfigurationViewport>(AddKeyWithInstancedValueToMap(ClusterNode, FieldName, ViewportName, Viewport));

	check(Viewport != NewViewport);
	Viewport->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
	Viewport->SetFlags(RF_Transient);
	
	FDisplayClusterConfiguratorUtils::MarkDisplayClusterBlueprintAsModified(NewViewport, true);

	return NewViewport;
}

bool UE::DisplayClusterConfiguratorClusterUtils::RemoveViewportFromClusterNode(UDisplayClusterConfigurationViewport* Viewport)
{
	// [UE-386933] A viewport can be null or pending kill after an undo. Report that nothing was removed
	// rather than asserting.
	if (!IsValid(Viewport))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot remove a viewport from its cluster node: the viewport is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.");

		return false;
	}

	if (UDisplayClusterConfigurationClusterNode* ViewportParent = Cast<UDisplayClusterConfigurationClusterNode>(Viewport->GetOuter()))
	{
		if (const FString* KeyPtr = ViewportParent->Viewports.FindKey(Viewport))
		{
			const FString ViewportName = *KeyPtr;

			Viewport->Modify();
			ViewportParent->Modify();

			RemoveKeyFromMap(ViewportParent, GET_MEMBER_NAME_CHECKED(UDisplayClusterConfigurationClusterNode, Viewports), ViewportName);
			ensure (Viewport->GetPackage() == GetTransientPackage());
			
			Viewport->SetFlags(RF_Transient);
			
			return true;
		}
	}

	return false;
}

bool UE::DisplayClusterConfiguratorClusterUtils::RenameViewport(UDisplayClusterConfigurationViewport* Viewport, FString NewViewportName)
{
	// [UE-386933] A viewport can be null or pending kill after an undo, and the clipboard paste path
	// renames viewports taken straight out of a duplicated node's map. Report failure rather than asserting.
	if (!IsValid(Viewport))
	{
		UE_LOGF(DisplayClusterConfiguratorLog, Warning,
			"Cannot rename a viewport to '%ls': the viewport is null or pending kill. "
			"Recompile and save the nDisplay config to clear this.", *NewViewportName);

		return false;
	}

	if (UDisplayClusterConfigurationClusterNode* ViewportParent = Cast<UDisplayClusterConfigurationClusterNode>(Viewport->GetOuter()))
	{
		if (const FString* KeyPtr = ViewportParent->Viewports.FindKey(Viewport))
		{
			const FString OldViewportName = *KeyPtr;

			if (KeyPtr->Equals(NewViewportName))
			{
				return false;
			}

			const FString UniqueName = GetUniqueNameForViewport(NewViewportName, ViewportParent);
			
			Viewport->Modify();
			ViewportParent->Modify();
			
			const FName FieldName = GET_MEMBER_NAME_CHECKED(UDisplayClusterConfigurationClusterNode, Viewports);

			// Remove and re-add. We can't just call RenameKeyInMap because when we rename the viewport after
			// it will lose sync with instances.
			RemoveKeyFromMap(ViewportParent, FieldName, OldViewportName);

			// Rename after removing. If this is done after adding instances will lose sync with the CDO.
			Viewport->Rename(*UniqueName, ViewportParent, REN_DontCreateRedirectors);
			
			UDisplayClusterConfigurationViewport* NewViewport =
				CastChecked<UDisplayClusterConfigurationViewport>(AddKeyWithInstancedValueToMap(ViewportParent, FieldName, UniqueName, Viewport));
			check(Viewport != NewViewport);
			Viewport->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
			Viewport->SetFlags(RF_Transient);
			
			FDisplayClusterConfiguratorUtils::MarkDisplayClusterBlueprintAsModified(NewViewport, true);
			
			return true;
		}
	}

	return false;
}

void UE::DisplayClusterConfiguratorClusterUtils::SortClusterNodesByHost(const TMap<FString, UDisplayClusterConfigurationClusterNode*>& InClusterNodes, TMap<FString, TMap<FString, UDisplayClusterConfigurationClusterNode*>>& OutSortedNodes)
{
	for (const TPair<FString, UDisplayClusterConfigurationClusterNode*>& ClusterNodePair : InClusterNodes)
	{
		// [UE-386933] Node value can be null or pending kill after an undo, and an empty key is not a
		// usable node id. Consumers of OutSortedNodes rely on this being the only filter, so it must
		// reject both.
		if (!IsValid(ClusterNodePair.Value) || ClusterNodePair.Key.IsEmpty())
		{
			UE_LOGF(DisplayClusterConfiguratorLog, Warning,
				"Cluster node '%ls' could not be resolved and will be skipped. "
				"Recompile and save the nDisplay config to clear this.", *ClusterNodePair.Key);

			continue;
		}

		FString Host = ClusterNodePair.Value->Host;
		if (!OutSortedNodes.Contains(Host))
		{
			OutSortedNodes.Add(Host, TMap<FString, UDisplayClusterConfigurationClusterNode*>());
		}

		OutSortedNodes[Host].Add(ClusterNodePair);
	}

	// Sort the hosts by the host address
	OutSortedNodes.KeySort(TLess<FString>());
}

void UE::DisplayClusterConfiguratorClusterUtils::SortClusterNodesByHost(const TMap<FString, TObjectPtr<UDisplayClusterConfigurationClusterNode>>& InClusterNodes, TMap<FString, TMap<FString, UDisplayClusterConfigurationClusterNode*>>& OutSortedNodes)
{
	for (const TPair<FString, TObjectPtr<UDisplayClusterConfigurationClusterNode>>& ClusterNodePair : InClusterNodes)
	{
		// [UE-386933] Node value can be null or pending kill after an undo, and an empty key is not a
		// usable node id. Consumers of OutSortedNodes rely on this being the only filter, so it must
		// reject both.
		if (!IsValid(ClusterNodePair.Value) || ClusterNodePair.Key.IsEmpty())
		{
			UE_LOGF(DisplayClusterConfiguratorLog, Warning,
				"Cluster node '%ls' could not be resolved and will be skipped. "
				"Recompile and save the nDisplay config to clear this.", *ClusterNodePair.Key);

			continue;
		}

		const FString Host = ClusterNodePair.Value->Host;
		if (!OutSortedNodes.Contains(Host))
		{
			OutSortedNodes.Add(Host, TMap<FString, UDisplayClusterConfigurationClusterNode*>());
		}

		OutSortedNodes[Host].Add(ClusterNodePair);
	}

	// Sort the hosts by the host address
	OutSortedNodes.KeySort(TLess<FString>());
}

#undef LOCTEXT_NAMESPACE