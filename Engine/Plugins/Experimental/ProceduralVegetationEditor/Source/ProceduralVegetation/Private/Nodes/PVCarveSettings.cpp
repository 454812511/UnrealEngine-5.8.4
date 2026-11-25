// Copyright Epic Games, Inc. All Rights Reserved.

#include "PVCarveSettings.h"
#include "DataTypes/PVData.h"
#include "DataTypes/PVGrowthData.h"
#include "Data/PCGBasePointData.h"
#include "GeometryCollection/ManagedArrayCollection.h"
#include "Helpers/PCGSettingsHelpers.h"
#include "PVCommon.h"
#include "Facades/PVBranchFacade.h"
#include "Facades/PVPointFacade.h"
#include "Helpers/PVDiagnosticHelper.h"
#include "ProceduralVegetationModule.h"

#define LOCTEXT_NAMESPACE "PVCarveSettings"

namespace
{
	// Formats a single branch's source-bud diagnostic for logging. Only called on the failure path,
	// after Carve reports a branch whose branchSourceBudNumber either matched none of its own points,
	// or matched a point that is not the branch's first.
	FString DescribeCarveBranch(const FManagedArrayCollection& InCollection, const int32 BranchIndex)
	{
		const PV::Facades::FBranchFacade BranchFacade(InCollection);
		const PV::Facades::FPointFacade PointFacade(InCollection);

		const TArray<int32>& BranchPoints = BranchFacade.GetPoints(BranchIndex);

		FString PointBudNumbers;
		for (const int32 PointIndex : BranchPoints)
		{
			PointBudNumbers += FString::Printf(TEXT("%d "), PointFacade.GetBudNumber(PointIndex));
		}

		return FString::Printf(
			TEXT("branch %d: branchSourceBudNumber=%d vs its %d point bud numbers [ %s]"),
			BranchIndex, BranchFacade.GetBranchSourceBudNumber(BranchIndex), BranchPoints.Num(), *PointBudNumbers);
	}
}

#if WITH_EDITOR
FLinearColor UPVCarveSettings::GetNodeTitleColor() const
{
	return PV::NodeColors::PostGrowthModifiers;
}

FText UPVCarveSettings::GetCategoryOverride() const
{
	return PV::Categories::PostGrowthModifiers;
}


FText UPVCarveSettings::GetDefaultNodeTitle() const
{ 
	return LOCTEXT("NodeTitle", "Carve"); 
}

FText UPVCarveSettings::GetNodeTooltipText() const
{
	return LOCTEXT("NodeTooltip",
		"Trim branches off the plant based on a chosen rule (length, height, world Z, or thickness)."
		"\n\n"
		"Removes branch from the plant structure using one of four reference systems. Useful for cleaning up low-hanging branches, thinning out fine twigs, or shaping the silhouette. Acts as a post-process — does not re-run the growth simulation."
	);
}
#endif

FPCGDataTypeIdentifier UPVCarveSettings::GetInputPinTypeIdentifier() const
{
	return FPCGDataTypeIdentifier{ FPVDataTypeInfoGrowth::AsId() };
}

FPCGDataTypeIdentifier UPVCarveSettings::GetOutputPinTypeIdentifier() const
{
	return FPCGDataTypeIdentifier{ FPVDataTypeInfoGrowth::AsId() };
}

FPCGElementPtr UPVCarveSettings::CreateElement() const
{
	return MakeShared<FPVCarveElement>();
}

bool FPVCarveElement::ExecuteInternal(FPCGContext* InContext) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPVCarveElement::ExecuteInternal);

	check(InContext);

	const UPVCarveSettings* InputSettings = InContext->GetInputSettings<UPVCarveSettings>();
	check(InputSettings);

	for (const FPCGTaggedData& Input : InContext->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel))
	{
		if (const UPVGrowthData* InputData = Cast<UPVGrowthData>(Input.Data))
		{
			FManagedArrayCollection SourceCollection = InputData->GetCollection();

			FManagedArrayCollection OutCollection;
			SourceCollection.CopyTo(&OutCollection);

			UPVGrowthData* OutManagedArrayCollectionData = FPCGContext::NewObject_AnyThread<UPVGrowthData>(InContext);

			auto [CarveBasis, Carve] = InputSettings->CarveSettings;

			// Carve reports the first branch (if any) whose branchSourceBudNumber either matched none of its
			// own points, or matched a point that is not the branch's first. The diagnostic below only pays for
			// the offending-branch formatting and node-chain walk on that rare path, leaving the common case
			// free of extra work.
			int32 InvalidBranchIndex = INDEX_NONE;
			FPVCarve::ApplyCarve(OutCollection, SourceCollection, CarveBasis, Carve, &InvalidBranchIndex);

			if (InvalidBranchIndex != INDEX_NONE)
			{
				UE_LOG(LogProceduralVegetation, Error,
					TEXT("Carve hit a branch with a mismatched source bud number (see ensure above). %s | CarveBasis=%d | upstream nodes: %s"),
					*DescribeCarveBranch(SourceCollection, InvalidBranchIndex), static_cast<int32>(CarveBasis),
					*PV::Diagnostic::GetUpstreamNodeChain(InContext->Node));
			}

			OutManagedArrayCollectionData->Initialize(MoveTemp(OutCollection));
			InContext->OutputData.TaggedData.Emplace(OutManagedArrayCollectionData);
		}
		else
		{
			PCGLog::InputOutput::LogInvalidInputDataError(InContext);
			return true;
		}
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
