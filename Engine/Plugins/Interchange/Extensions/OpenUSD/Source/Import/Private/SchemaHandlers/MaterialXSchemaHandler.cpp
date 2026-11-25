// Copyright Epic Games, Inc. All Rights Reserved.

#if USE_USD_SDK

#include "SchemaHandlers/MaterialXSchemaHandler.h"

#include "InterchangeUsdContext.h"
#include "InterchangeUsdTranslator.h"
#include "InterchangeUsdTraversalInfo.h"
#include "SchemaHandlers/SchemaHandlerUtils.h"
#include "Usd/InterchangeUsdDefinitions.h"
#include "USDShadeConversion.h"
#include "UsdWrappers/UsdPrim.h"
#include "UsdWrappers/UsdStage.h"

#include "MaterialX/InterchangeMaterialXTranslator.h"
#include "MaterialX/MaterialXUtils/MaterialXManager.h"
#include "USDMaterialXShaderGraph.h"

#include "InterchangeManager.h"
#include "InterchangeMaterialInstanceNode.h"
#include "InterchangeMaterialReferenceNode.h"
#include "InterchangeSceneNode.h"
#include "InterchangeShaderGraphNode.h"
#include "InterchangeTextureNode.h"
#include "Nodes/InterchangeBaseNodeContainer.h"

#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#include "UObject/ObjectKey.h"

namespace UE::MaterialXSchemaHandler::Private
{
	using namespace UE::Interchange::USD;

	// Prefix for the asset-node UIDs we create for MaterialX shader graphs. This normally lives
	// alongside the other UID prefixes in InterchangeUsdDefinitions.h, but it's defined here
	// instead: adding it to that shared public header would be an ABI-breaking change for a patch
	// release, and it is only ever used in this file.
	const FString ShadersPrefix = TEXT("\\Shaders\\");

	// Per-translation map of "MaterialX material prim name -> the UID we rebound that material's
	// translated shader graph node to". This used to be a TMap member on FMaterialXSchemaHandler,
	// but a patch release can't add a data member without changing the class layout (an ABI
	// break), so the state lives here instead.
	//
	// It's keyed by the UInterchangeUsdContext, whose lifetime matches a single translation (one
	// context, and one freshly-spawned handler, per Translate call), so this has exactly the same
	// per-translation scope the member had. Using FObjectKey means a destroyed context can't be
	// confused with a later one that happens to reuse its address, and we drop dead entries on
	// write so the table can't grow without bound. The lock guards concurrent translations, which
	// each run on their own thread and would otherwise race on the shared table (the member was
	// implicitly per-instance, so this preserves that safety).
	FCriticalSection GMaterialXRemapLock;
	TMap<FObjectKey, TMap<FString, FString>> GMaterialXRemapByContext;

	// Returns the remapped UID previously recorded for MaterialPrimName in this translation, or an
	// empty string if there isn't one (recorded UIDs are never empty, so empty means "not found").
	FString FindRemappedMaterialUid(const UInterchangeUsdContext& UsdContext, const FString& MaterialPrimName)
	{
		FScopeLock Lock(&GMaterialXRemapLock);
		if (const TMap<FString, FString>* ContextMap = GMaterialXRemapByContext.Find(FObjectKey(&UsdContext)))
		{
			if (const FString* RemappedUid = ContextMap->Find(MaterialPrimName))
			{
				return *RemappedUid;
			}
		}
		return {};
	}

	void AddRemappedMaterialUid(const UInterchangeUsdContext& UsdContext, const FString& MaterialPrimName, const FString& RemappedUid)
	{
		FScopeLock Lock(&GMaterialXRemapLock);

		// Drop entries whose context has since been garbage collected, so the table doesn't grow
		// unbounded across a long editor session.
		for (auto It = GMaterialXRemapByContext.CreateIterator(); It; ++It)
		{
			if (It.Key().ResolveObjectPtr() == nullptr)
			{
				It.RemoveCurrent();
			}
		}

		GMaterialXRemapByContext.FindOrAdd(FObjectKey(&UsdContext)).Add(MaterialPrimName, RemappedUid);
	}

	// Returns the UID of the material translated node that was generated from the MaterialX translation of a particular
	// material prim.
	//
	// This works because when parsing MaterialX files we generate shader graph nodes with UIDs that match the original
	// material prim name in the USD file (e.g. on USD we have a binding relationship to </MaterialX/Materials/Marble_3D>,
	// and we end up generating a shader graph node with Uid \\Shaders\\Marble_3D).
	FString GetMaterialXMaterialUid(const FString& PrimName, UInterchangeBaseNodeContainer& NodeContainer)
	{
		FString Result;

		NodeContainer.BreakableIterateNodesOfType<UInterchangeShaderGraphNode>(
			[&NodeContainer, &PrimName, &Result](const FString&, UInterchangeShaderGraphNode* ShaderGraphNode)
			{
				FString NodeUid = ShaderGraphNode->GetUniqueID();
				// Because USD allows having the same name between a parent and its child (unlike MaterialX)
				// When building the shader graph we may end up with a name not totally equal to the USD one (MaterialX will append an index to the material name)
				// e.g: prim -> mtlx_volume
				//      mtlx -> mtlx_volume2
				// see the egg_jade file from https://github.com/stehrani3d/MaterialEggs
				FString BaseName = FPaths::GetBaseFilename(NodeUid);
				if (BaseName == PrimName)
				{
					Result = NodeUid;
					return true;
				}
				else
				{
					if (BaseName.StartsWith(PrimName, ESearchCase::CaseSensitive))
					{
						if (FString Suffix = BaseName.Mid(PrimName.Len()); Suffix.IsNumeric())
						{
							Result = NodeUid;
							return true;
						}
					}
					return false;
				}
			}
		);

		return Result;
	}

	// Uses a couple of tricks to produce a node uid that works for the cloned textures.
	//
	// We have to go through UsdContext.MakeAssetNodeUid() somehow to let pregen determine
	// the uid. However, we have no "texture prim", so we must use the material prim instead.
	// This leads to a problem: This new uid will *contain* that material prim path itself,
	// so if we have multiple materials (in the same pregen target) using the same texture
	// we'll end up with different uids, and so creating extra unnecessary copies of each 
	// texture: one per material that is using it...
	//
	// The "solution" here is to perform some surgery on the provided uids: We manually
	// search for and remove the material prim path from the uid (if present) so that our
	// texture uid doesn't depend on a particular material, and then graft in our original
	// texture uid so that we still retain uids unique per-texture
	//
	// This is really not ideal and indirectly hard-codes a lot of assumptions about the
	// format of the uids, but it should hopefully be ok for now
	FString CreateCloneTextureShaderUid(const UInterchangeUsdContext& UsdContext, const UE::FUsdPrim& MaterialPrim, const FString& OriginalUid)
	{
		// OriginalUid comes in as e.g. "\Texture\51AC1CF6246D6E79878C54B79C5A02619B4C43C7\king_white_base_color.jpg"

		// e.g. "\Texture\<King_29a14e47d62d>/King/Materials/M_King_W_656803929"
		// Note that this is the "remapped path", so it's the path *within the asset's own layer*
		FString NewUid = UsdContext.MakeAssetNodeUid(MaterialPrim, TexturePrefix);

		// e.g. "/ChessSet/White/King/Materials/M_King_W"
		const FString MaterialPrimPathStr = MaterialPrim.GetPrimPath().GetString();

		// e.g. ["ChessSet", "White", "King", "Materials", "M_King_W"]
		TArray<FString> Segments;
		MaterialPrimPathStr.ParseIntoArray(Segments, TEXT("/"));

		// Floor at two segments. A single-segment "/M_King_W" string match risks hitting
		// unrelated occurrences (e.g. a textually similar substring in another node UID),
		// but likely will never happen with MaterialX anyway as the generated prims are
		// usually encapsulated by other parent prims
		if (Segments.Num() < 2)
		{
			return NewUid;
		}

		// e.g. "Materials/M_King_W"
		const FString Anchor = TEXT("/") + Segments[Segments.Num() - 2] + TEXT("/") + Segments.Last();
		const int32 AnchorIdxInUid = NewUid.Find(Anchor, ESearchCase::CaseSensitive);
		if (AnchorIdxInUid == INDEX_NONE)
		{
			return NewUid;
		}

		// Extend the match leftwards a character at a time: as long as NewUid agrees with
		// MaterialPrimPathStr just before the anchor, the match keeps growing. Stops at the
		// first mismatch (or when either string runs out). The matched substring runs from
		// MatchStart up to (but not including) MatchEnd
		const int32 AnchorIdxInPath = MaterialPrimPathStr.Len() - Anchor.Len();
		int32 UidCursor = AnchorIdxInUid - 1;
		int32 PathCursor = AnchorIdxInPath - 1;
		while (UidCursor >= 0 && PathCursor >= 0 && NewUid[UidCursor] == MaterialPrimPathStr[PathCursor])
		{
			--UidCursor;
			--PathCursor;
		}

		// Graft in the OriginalUid in place of the material prim path.
		// e.g. "\Texture\<King_29a14e47d62d>\Texture\51AC1CF6246D6E79878C54B79C5A02619B4C43C7\king_white_base_color.jpg_656803929"
		const int32 MatchStart = UidCursor + 1;
		const int32 MatchEnd = AnchorIdxInUid + Anchor.Len();
		NewUid = NewUid.Mid(0, MatchStart) + OriginalUid + NewUid.Mid(MatchEnd);

		// Strip and re-do the trailing hash (e.g. "_656803929") to ensure it covers case
		// sensitivity
		int32 LastUnderscore = INDEX_NONE;
		if (NewUid.FindLastChar(TEXT('_'), LastUnderscore))
		{
			NewUid.LeftInline(LastUnderscore);
		}
		return MakeNodeUid(NewUid);
	}

	// Deep-clones the shader graph rooted at RootOldUid so that the cloned nodes have UIDs 
	// made by UsdContext.MakeAssetNodeUid(), which is what pregen needs to manage the assets.
	// Returns the new UID, or CurrentOldUid as a fallback
	FString DeepCloneShaderGraphNode(
		const FString& CurrentOldUid,
		const FString& RootOldUid,
		UInterchangeBaseNodeContainer& NodeContainer,
		const UInterchangeUsdContext& UsdContext,
		const UE::FUsdPrim& MaterialPrim,
		const FString& PrimPathString
	)
	{
		using namespace UE::Interchange::USD;

		UInterchangeBaseNode* OldNode = const_cast<UInterchangeBaseNode*>(NodeContainer.GetNode(CurrentOldUid));
		if (!OldNode)
		{
			return CurrentOldUid;
		}

		const bool bIsRootMaterial = (CurrentOldUid == RootOldUid);
		const bool bIsTextureNode = OldNode->IsA<UInterchangeTextureNode>();

		FString NewUid;
		if (bIsRootMaterial)
		{
			// Root material gets the canonical "material for this prim" UID so other handlers'
			// lookups resolve to the clone
			NewUid = UsdContext.MakeAssetNodeUid(MaterialPrim, ShadersPrefix);
		}
		else if (bIsTextureNode)
		{
			// Annoyingly we need to do some workaround for the texture node clone uids, mainly due
			// to how there's no "prim" for them that we just call UsdContext.MakeAssetNodeUid() on
			NewUid = CreateCloneTextureShaderUid(UsdContext, MaterialPrim, CurrentOldUid);
		}
		else
		{
			// Non-texture descendants embed the original UID as a suffix purely to stay 
			// unique within the material's namespace.
			NewUid = UsdContext.MakeAssetNodeUid(MaterialPrim, ShadersPrefix, TEXT("/") + CurrentOldUid);
		}

		if (NodeContainer.GetNode(NewUid))
		{
			return NewUid;
		}

		// Need to use bSkipPostLoad here so that we can perform this duplication on a background thread,
		// in case we're running from a pregen worker process, where the translation defaults to being on
		// an async thread
		FObjectDuplicationParameters DuplicateParams = InitStaticDuplicateObjectParams(OldNode, &NodeContainer);
		DuplicateParams.bSkipPostLoad = true;
		UInterchangeBaseNode* NewNode = Cast<UInterchangeBaseNode>(StaticDuplicateObjectEx(DuplicateParams));
		if (!NewNode)
		{
			return CurrentOldUid;
		}

		NodeContainer.SetupNode(NewNode, NewUid, OldNode->GetDisplayLabel(), OldNode->GetNodeContainerType());

		// We only really need the prim path on nodes that "become assets", as the main purpose of
		// doing this is to be able to look at an asset after import and determine which prim it came from
		if (bIsRootMaterial || NewNode->IsA<UInterchangeTextureNode>())
		{
			SetPrimPath(*NewNode, PrimPathString);
		}

		// Need to now update the clone's connections to also point at newly cloned shader nodes
		TArray<FString> InputNames;
		UInterchangeShaderPortsAPI::GatherInputs(NewNode, InputNames);
		for (const FString& InputName : InputNames)
		{
			// Shader-graph connections (Inputs:Name:Connect -> UID[:OutputName]). The material's
			// BaseColor (etc.) reaches a TextureSample shader node via this, and chained math
			// nodes link together via this.
			FString ConnectedOldUid;
			FString OutputName;
			if (UInterchangeShaderPortsAPI::GetInputConnection(NewNode, InputName, ConnectedOldUid, OutputName) && !ConnectedOldUid.IsEmpty())
			{
				const FString ConnectedNewUid = DeepCloneShaderGraphNode(
					ConnectedOldUid,
					RootOldUid,
					NodeContainer,
					UsdContext,
					MaterialPrim,
					PrimPathString
				);

				if (ConnectedNewUid != ConnectedOldUid)
				{
					UInterchangeShaderPortsAPI::ConnectOuputToInputByName(NewNode, InputName, ConnectedNewUid, OutputName);
				}
			}

			// String-valued inputs (Inputs:Name:Value or :Parameter). MaterialX TextureSample
			// stashes the texture's UID here rather than as a connection, so a pure connection
			// walk would miss every MaterialX-bound texture. Only rewrite if the string actually
			// names a node in the container (other strings are filenames, untyped data, etc.).
			const bool bIsAParameter = UInterchangeShaderPortsAPI::HasParameter(NewNode, FName{*InputName});
			const FString InputKey = bIsAParameter
				? UInterchangeShaderPortsAPI::MakeInputParameterKey(InputName)
				: UInterchangeShaderPortsAPI::MakeInputValueKey(InputName);

			FString CandidateOldUid;
			if (NewNode->GetStringAttribute(InputKey, CandidateOldUid) && !CandidateOldUid.IsEmpty() && NodeContainer.GetNode(CandidateOldUid))
			{
				const FString CandidateNewUid = DeepCloneShaderGraphNode(
					CandidateOldUid,
					RootOldUid,
					NodeContainer,
					UsdContext,
					MaterialPrim,
					PrimPathString
				);

				if (CandidateNewUid != CandidateOldUid)
				{
					NewNode->AddStringAttribute(InputKey, CandidateNewUid);
				}
			}
		}

		return NewUid;
	}

	// Walks the shader graph rooted at RootMaterial via the same connection-tree traversal
	// and stamps PrimPathString on every descendant texture node it finds
	void StampPrimPathOnDescendantTextures(
		UInterchangeBaseNodeContainer& NodeContainer,
		const UInterchangeShaderNode& RootMaterial,
		const FString& PrimPathString
	)
	{
		using namespace UE::Interchange::USD;

		TSet<FString> Visited;
		TArray<const UInterchangeShaderNode*> ToProcess;

		Visited.Add(RootMaterial.GetUniqueID());
		ToProcess.Add(&RootMaterial);

		while (!ToProcess.IsEmpty())
		{
			const UInterchangeShaderNode* Current = ToProcess.Pop();

			TArray<FString> InputNames;
			UInterchangeShaderPortsAPI::GatherInputs(Current, InputNames);
			for (const FString& InputName : InputNames)
			{
				auto VisitCandidate = [&](const FString& CandidateUid)
				{
					if (CandidateUid.IsEmpty())
					{
						return;
					}

					bool bAlreadyVisited = false;
					Visited.Add(CandidateUid, &bAlreadyVisited);
					if (bAlreadyVisited)
					{
						return;
					}

					UInterchangeBaseNode* Candidate = const_cast<UInterchangeBaseNode*>(NodeContainer.GetNode(CandidateUid));
					if (!Candidate)
					{
						return;
					}

					if (Candidate->IsA<UInterchangeTextureNode>())
					{
						SetPrimPath(*Candidate, PrimPathString);
					}

					if (const UInterchangeShaderNode* ChildShader = Cast<UInterchangeShaderNode>(Candidate))
					{
						ToProcess.Add(ChildShader);
					}
				};

				// Proper shader-graph connections.
				FString ConnectedUid;
				FString OutputName;
				if (UInterchangeShaderPortsAPI::GetInputConnection(Current, InputName, ConnectedUid, OutputName))
				{
					VisitCandidate(ConnectedUid);
				}

				// MaterialX TextureSample stores its target texture-node UID as a string-valued
				// input rather than a connection (see MaterialXSurfaceShaderAbstract.cpp ~line
				// 605, written via MakeInputValueKey), so a pure connection walk would skip
				// every MaterialX-bound texture.
				const bool bIsAParameter = UInterchangeShaderPortsAPI::HasParameter(Current, FName{*InputName});
				const FString InputKey = bIsAParameter
					? UInterchangeShaderPortsAPI::MakeInputParameterKey(InputName)
					: UInterchangeShaderPortsAPI::MakeInputValueKey(InputName);

				FString CandidateUid;
				if (Current->GetStringAttribute(InputKey, CandidateUid))
				{
					VisitCandidate(CandidateUid);
				}
			}
		}
	}

	bool AddMaterialXShaderGraph(const UE::FUsdPrim& Prim, TArray<FUsdMaterialXShaderGraph::FGeomProp>& GeomPropValues, UInterchangeUsdContext& UsdContext)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AddMaterialXShaderGraph)

#if WITH_EDITOR
		TArray<FString> FilePaths = UsdUtils::GetMaterialXFilePaths(Prim);
		for (const FString& File : FilePaths)
		{
			// the file has already been handled no need to do a Translate again
			if (!UsdContext.SubTranslators.Find(File))
			{
				UInterchangeManager& InterchangeManager = UInterchangeManager::GetInterchangeManager();
				UInterchangeSourceData* SourceData = UInterchangeManager::CreateSourceData(File);

				UInterchangeTranslatorBase* Translator = InterchangeManager.GetTranslatorForSourceData(SourceData);
				// check on the Translator, it might return nullptr in case of reimport
				if (Translator)
				{
					Translator->Results = UsdContext.GetResultsContainer();
					Translator->Translate(*UsdContext.GetNodeContainer());
					UsdContext.SubTranslators.Emplace(File, Translator);
				}
			}
		}

		if (!FilePaths.IsEmpty())
		{
			return true;
		}

		// Conditionally enable the shader graph for the time being as it crashes on Linux cause of a probable double free
#if ENABLE_USD_MATERIALX
		FUsdMaterialXShaderGraph ShaderGraph(Prim, *UnrealIdentifiers::MaterialXRenderContext.ToString());
		GeomPropValues = ShaderGraph.GetGeomPropValueNames();
		if (MaterialX::DocumentPtr Document = ShaderGraph.GetDocument())
		{
			return FMaterialXManager::GetInstance().Translate(Document, *UsdContext.GetNodeContainer());
		}
#endif // ENABLE_USD_MATERIALX

#endif	  // WITH_EDITOR
		return false;
	}

}	 // namespace UE::MaterialXSchemaHandler::Private

namespace UE::Interchange::USD
{
	const FString& FMaterialXSchemaHandler::GetHandlerName() const
	{
		const static FString HandlerName = TEXT("MaterialXHandler");
		return HandlerName;
	}

	const TArray<FString>& FMaterialXSchemaHandler::GetDefaultRenderContexts() const
	{
		const static TArray<FString> RenderContexts{UnrealIdentifiers::MaterialXRenderContext.ToString()};
		return RenderContexts;
	}

	bool FMaterialXSchemaHandler::CanHandlePrim(const UE::FUsdPrim& Prim, const UInterchangeUsdContext& UsdContext) const
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FMaterialXSchemaHandler::CanHandlePrim)

#if WITH_EDITOR && ENABLE_USD_MATERIALX
		if (!Prim.IsA(*GetTargetSchemaName()))
		{
			return false;
		}

		const TArray<FString>& RenderContexts = GetDefaultRenderContexts();
		return UsdUtils::HasSurfaceOutput(Prim, RenderContexts)
			   || UsdUtils::HasVolumeOutput(Prim, RenderContexts)
			   || UsdUtils::HasDisplacementOutput(Prim, RenderContexts);
#else	 // #if WITH_EDITOR && ENABLE_USD_MATERIALX
		return false;
#endif
	}

	bool FMaterialXSchemaHandler::OnTranslate(
		const UE::FUsdPrim& Prim,
		FTraversalInfo& TraversalInfo,
		FHandlerAccumulatedInfo& AccumulatedInfo,
		UInterchangeUsdContext& UsdContext
	)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FMaterialXSchemaHandler::OnTranslate)

		using namespace UE::MaterialXSchemaHandler::Private;
		using namespace UE::Interchange::USD;

		UInterchangeBaseNodeContainer* NodeContainer = UsdContext.GetNodeContainer();
		if (!NodeContainer)
		{
			return false;
		}

		// For the material schema handlers, we only touch the asset if no other handler has produced a material node yet.
		// This helps manage the separate material handlers per render context, without having them partially overwrite the
		// node with render-context-specific data over and over
		const static TSet<UClass*> MaterialLikeClasses = {
			UInterchangeMaterialInstanceNode::StaticClass(),
			UInterchangeShaderGraphNode::StaticClass(),
			UInterchangeMaterialReferenceNode::StaticClass()
		};
		for (const UInterchangeBaseNode* AssetNode : AccumulatedInfo.PrimAssetNodes)
		{
			for (const UClass* MaterialClass : MaterialLikeClasses)
			{
				if (AssetNode->IsA(MaterialClass))
				{
					return false;
				}
			}
		}

		// If we already have a shader graph node for our expected ID then just use it
		const FString TargetUid = UsdContext.MakeAssetNodeUid(Prim, ShadersPrefix);
		if (UInterchangeShaderGraphNode* ExistingMaterial = const_cast<UInterchangeShaderGraphNode*>(Cast<UInterchangeShaderGraphNode>(NodeContainer->GetNode(TargetUid))))
		{
			const uint32 Index = 0;
			AccumulatedInfo.PrimAssetNodes.Insert(ExistingMaterial, Index);
			return true;
		}

		// Make sure the MaterialX document has been translated and its shader graph node(s)
		// are in the container. Cheap on subsequent calls (UsdContext.SubTranslators caches
		// the inner translator per file path).
		TArray<FUsdMaterialXShaderGraph::FGeomProp> GeomPropValues;
		const bool bParsedMaterialX = AddMaterialXShaderGraph(Prim, GeomPropValues, UsdContext);
		if (!bParsedMaterialX)
		{
			return false;
		}

		// Find the MaterialX-translated source shader graph node for this material. The remap
		// table is consulted first for two reasons: it's faster than GetMaterialXMaterialUid's
		// container scan, and more importantly a previous OnTranslate call may have
		// already rebound the node to a different UID, in which case the prim-name-based
		// container scan wouldn't find it any more
		const FString MaterialPrimName = Prim.GetName().ToString();
		FString SourceUid = FindRemappedMaterialUid(UsdContext, MaterialPrimName);
		if (SourceUid.IsEmpty())
		{
			SourceUid = GetMaterialXMaterialUid(MaterialPrimName, *NodeContainer);
		}

		UInterchangeShaderGraphNode* SourceMaterial = const_cast<UInterchangeShaderGraphNode*>(Cast<UInterchangeShaderGraphNode>(NodeContainer->GetNode(SourceUid)));
		if (!SourceMaterial)
		{
			return false;
		}

		const FString PrimPathString = Prim.GetPrimPath().GetString();
		FString FinalUid;

		// If there's no prim path assigned to this node it means it hasn't been rebound by
		// a previous run, so we can just rebind it for this prim
		if (GetPrimPath(*SourceMaterial).IsEmpty())
		{
			// We really just need to change its Uid to our TargetUid to make sure that pregen can
			// do it's job whenever we're doing a pregen import (i.e. so that we can find the same
			// node again iff pregen determines that they should correspond to the same asset)
			NodeContainer->RemoveNode(SourceUid);
			NodeContainer->SetupNode(SourceMaterial, TargetUid, SourceMaterial->GetDisplayLabel(), SourceMaterial->GetNodeContainerType());
			SetPrimPath(*SourceMaterial, PrimPathString);

			// We need the correct prim path on the shader graph node but also the leaf texture nodes
			// (if any), so that they ultimately make their way to the produced assets. Pregen also
			// uses this to determine where each asset should go
			StampPrimPathOnDescendantTextures(*NodeContainer, *SourceMaterial, PrimPathString);

			// Note how we only add the original material to the remap table: This ensures all of our
			// duplicates originate from the same subtree of nodes
			AddRemappedMaterialUid(UsdContext, MaterialPrimName, TargetUid);
			FinalUid = TargetUid;
		}
		else
		{
			// The translated source already has a prim path (some earlier consumer remapped it
			// and rebound its UID). Deep-clone the whole subgraph with new ideal (following pregen
			// on pregen imports) UIDs so this consumer ends up with its own per-target material and
			// texture factory nodes. We don't add the clone to the map: the map only tracks the original
			// remapped copy so future deep clones still spin off of it.
			FinalUid = DeepCloneShaderGraphNode(
				SourceUid,
				SourceUid,
				*NodeContainer,
				UsdContext,
				Prim,
				PrimPathString
			);
		}

		if (UInterchangeShaderGraphNode* FinalMaterial = const_cast<UInterchangeShaderGraphNode*>(Cast<UInterchangeShaderGraphNode>(NodeContainer->GetNode(FinalUid))))
		{
			// Insert the main material at the front so we run into it first, as in case we have twosided materials we want to use the
			// twosided version
			const uint32 Index = 0;
			AccumulatedInfo.PrimAssetNodes.Insert(FinalMaterial, Index);
		}

		if (GeomPropValues.Num() > 0 && !FinalUid.IsEmpty())
		{
			UsdContext.MaterialUidToGeomProps.Add(FinalUid, GeomPropValues);
		}

		return true;
	}

	bool FMaterialXSchemaHandler::OnGetTexturePayloadData(
		const FString& PayloadKey,
		TOptional<FString>& AlternateTexturePath,
		UInterchangeUsdContext& UsdContext,
		TOptional<UE::Interchange::FImportImage>& InOutPayloadData
	)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FMaterialXSchemaHandler::OnGetTexturePayloadData)

		// We did not find a suitable Payload in USD Translator, let's find one in one of the Translators (MaterialX for the moment)
		// The best way would be to have a direct association between the payload and the right Translator, but we don't have a suitable way of
		// knowing which Payload belongs to which Translator So let's just loop over them all
		if (!InOutPayloadData.IsSet())
		{
			for (const TPair<FString, TStrongObjectPtr<UInterchangeTranslatorBase>>& Pair : UsdContext.SubTranslators)
			{
				if (IInterchangeTexturePayloadInterface* TexturePayloadInterface = Cast<IInterchangeTexturePayloadInterface>(Pair.Value.Get()))
				{
					InOutPayloadData = TexturePayloadInterface->GetTexturePayloadData(PayloadKey, AlternateTexturePath);
					if (InOutPayloadData.IsSet())
					{
						break;
					}
				}
			}
		}

		// If we couldn't find a texture in either the USD Translator nor the Translators, then it's most likely coming from reading an mtlx in memory
		if (!InOutPayloadData.IsSet())
		{
			InOutPayloadData = UInterchangeMaterialXTranslator::GetTexturePayloadData(
				PayloadKey,
				AlternateTexturePath,
				UsdContext.GetResultsContainer(),
				UsdContext.GetTranslator()->AnalyticsHandler
			);
		}

		return InOutPayloadData.IsSet();
	}

	bool FMaterialXSchemaHandler::OnGetBlockedTexturePayloadData(
		const FString& PayloadKey,
		TOptional<FString>& AlternateTexturePath,
		UInterchangeUsdContext& UsdContext,
		TOptional<UE::Interchange::FImportBlockedImage>& InOutPayloadData
	)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FMaterialXSchemaHandler::OnGetBlockedTexturePayloadData)

		// We did not find a suitable Payload in USD Translator, let's find one in one of the Translators (MaterialX for the moment)
		// The best way would be to have a direct association between the payload and the right Translator, but we don't have a suitable way of
		// knowing which Payload belongs to which Translator So let's just loop over them all
		for (const TPair<FString, TStrongObjectPtr<UInterchangeTranslatorBase>>& Pair : UsdContext.SubTranslators)
		{
			if (IInterchangeBlockedTexturePayloadInterface* TexturePayloadInterface = Cast<IInterchangeBlockedTexturePayloadInterface>(Pair.Value.Get()))
			{
				InOutPayloadData = TexturePayloadInterface->GetBlockedTexturePayloadData(PayloadKey, AlternateTexturePath);
				if (InOutPayloadData.IsSet())
				{
					break;
				}
			}
		}

		// If we couldn't find a texture in either the USD Translator nor the Translators, then it's most likely coming from reading an mtlx in
		// memory
		if (!InOutPayloadData.IsSet())
		{
			InOutPayloadData = UInterchangeMaterialXTranslator::GetBlockedTexturePayloadData(
				PayloadKey,
				AlternateTexturePath,
				UsdContext.GetResultsContainer(),
				UsdContext.GetTranslator()->AnalyticsHandler
			);
		}

		return InOutPayloadData.IsSet();
	}
}	 // namespace UE::Interchange::USD

#endif	  // USE_USD_SDK
