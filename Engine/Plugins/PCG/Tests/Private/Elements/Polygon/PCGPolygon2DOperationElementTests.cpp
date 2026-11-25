// Copyright Epic Games, Inc. All Rights Reserved.

// =============================================================================
// PCG Polygon2D Operation Tests
//
// Tests for UPCGPolygon2DOperationSettings
//
// Focuses on metadata forwarding: outputs must carry the Data-domain attribute
// values and tags of the first contributing input (Merge node convention), and
// Elements-domain (vertex) attributes must keep interpolated values. Union and
// InnerIntersection are regression sentinels: they used to emit outputs that
// were never initialized from any input, so Data-domain attributes existed but
// read back their default values.
//
// Test summary:
//
// Data domain forwarding:
//  - UnionForwardsFirstInputDataDomainValuesAndTags:  regression sentinel, single merged output
//  - UnionForwardsFirstInputToAllOutputs:             disjoint polygons, several outputs
//  - InnerIntersectionForwardsFirstInputDataDomainValues: regression sentinel
//  - DifferenceForwardsInputDataDomainValues:         baseline, per-input outputs
//
// Modes:
//  - UnionMetadataModeNoneDoesNotInheritAttributes:   'None' must not inherit anything
// =============================================================================

#include "TestHarness.h"

#include "PCGCommon.h"
#include "PCGContext.h"
#include "PCGData.h"
#include "PCGTestsCommon.h"
#include "Data/PCGPolygon2DData.h"
#include "Elements/Polygon/PCGPolygon2DOperation.h"
#include "Elements/Polygon/PCGPolygon2DUtils.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "Metadata/PCGMetadataCommon.h"
#include "Metadata/PCGMetadataDomain.h"

#include "Curve/GeneralPolygon2.h"

namespace PCGPolygon2DOperationTests
{
	static const FName DataAttrName = TEXT("MyDataAttr");
	static const FName VertexAttrName = TEXT("MyVertexAttr");

	// Counter-clockwise unit squares, overlapping on (50,50)-(100,100).
	static const TArray<FVector2d> SquareA = { FVector2d(0.0, 0.0), FVector2d(100.0, 0.0), FVector2d(100.0, 100.0), FVector2d(0.0, 100.0) };
	static const TArray<FVector2d> SquareB = { FVector2d(50.0, 50.0), FVector2d(150.0, 50.0), FVector2d(150.0, 150.0), FVector2d(50.0, 150.0) };
	static const TArray<FVector2d> SquareFarAway = { FVector2d(300.0, 0.0), FVector2d(400.0, 0.0), FVector2d(400.0, 100.0), FVector2d(300.0, 100.0) };

	static const TArray<double> VertexValuesA = { 1.0, 2.0, 3.0, 4.0 };
	static const TArray<double> VertexValuesB = { 10.0, 20.0, 30.0, 40.0 };
}

class FPCGPolygon2DOperationTest : public PCGTests::FPCGSingleElementBaseTest<UPCGPolygon2DOperationSettings>
{
public:
	UPCGPolygon2DData* MakePolygon(const TArray<FVector2d>& OuterVertices, int32 DataAttrValue, const TArray<double>& VertexAttrValues)
	{
		using namespace PCGPolygon2DOperationTests;
		check(OuterVertices.Num() == VertexAttrValues.Num());

		UPCGPolygon2DData* PolyData = NewObject<UPCGPolygon2DData>();
		UPCGMetadata* Metadata = PolyData->MutableMetadata();

		FPCGMetadataDomain* DataDomain = Metadata->GetMetadataDomain(PCGMetadataDomainID::Data);
		check(DataDomain);
		const PCGMetadataEntryKey DataEntryKey = DataDomain->AddEntry();
		FPCGMetadataAttribute<int32>* DataAttr = DataDomain->CreateAttribute<int32>(DataAttrName, 0, /*bAllowsInterpolation=*/false, /*bOverrideParent=*/false);
		check(DataAttr);
		DataAttr->SetValue(DataEntryKey, DataAttrValue);

		// Default domain for polygon data is Elements (one entry per vertex).
		FPCGMetadataAttribute<double>* VertexAttr = Metadata->CreateAttribute<double>(VertexAttrName, 0.0, /*bAllowsInterpolation=*/true, /*bOverrideParent=*/false);
		check(VertexAttr);

		TArray<PCGMetadataEntryKey> EntryKeys;
		EntryKeys.Reserve(VertexAttrValues.Num());
		for (double VertexAttrValue : VertexAttrValues)
		{
			const PCGMetadataEntryKey EntryKey = Metadata->AddEntry();
			VertexAttr->SetValue(EntryKey, VertexAttrValue);
			EntryKeys.Add(EntryKey);
		}

		TConstArrayView<PCGMetadataEntryKey> EntryKeysView = MakeConstArrayView(EntryKeys);
		PolyData->SetPolygon(UE::Geometry::FGeneralPolygon2d(UE::Geometry::FPolygon2d(OuterVertices)), &EntryKeysView);
		PolyData->SetTransform(FTransform::Identity);

		return PolyData;
	}

	void AddInput(UPCGData* Data, const TSet<FString>& Tags = {}, FName Pin = PCGPinConstants::DefaultInputLabel)
	{
		FPCGTaggedData& TaggedData = InputData.TaggedData.Emplace_GetRef();
		TaggedData.Data = Data;
		TaggedData.Pin = Pin;
		TaggedData.Tags = Tags;
	}

	const UPCGPolygon2DData* RequireOutputPolygon(int32 OutputIndex) const
	{
		REQUIRE(Context->OutputData.TaggedData.IsValidIndex(OutputIndex));
		const UPCGPolygon2DData* OutPolyData = Cast<UPCGPolygon2DData>(Context->OutputData.TaggedData[OutputIndex].Data);
		REQUIRE(OutPolyData);
		return OutPolyData;
	}

	void CheckDataDomainValue(const UPCGPolygon2DData* OutPolyData, int32 ExpectedValue) const
	{
		using namespace PCGPolygon2DOperationTests;
		const FPCGMetadataDomain* DataDomain = OutPolyData->ConstMetadata()->GetConstMetadataDomain(PCGMetadataDomainID::Data);
		REQUIRE(DataDomain);
		const FPCGMetadataAttribute<int32>* DataAttr = DataDomain->GetConstTypedAttribute<int32>(DataAttrName);
		REQUIRE(DataAttr);
		CHECK(DataAttr->GetValueFromItemKey(PCGFirstEntryKey) == ExpectedValue);
	}

	void CheckVertexValuesInRange(const UPCGPolygon2DData* OutPolyData, double MinValue, double MaxValue) const
	{
		using namespace PCGPolygon2DOperationTests;
		const FPCGMetadataAttribute<double>* VertexAttr = OutPolyData->ConstMetadata()->GetConstTypedAttribute<double>(VertexAttrName);
		REQUIRE(VertexAttr);

		TConstArrayView<PCGMetadataEntryKey> VertexEntryKeys = OutPolyData->GetConstVerticesEntryKeys();
		CHECK(!VertexEntryKeys.IsEmpty());
		for (const PCGMetadataEntryKey VertexEntryKey : VertexEntryKeys)
		{
			CHECK(VertexEntryKey != PCGInvalidEntryKey);
			const double Value = VertexAttr->GetValueFromItemKey(VertexEntryKey);
			CHECK(Value >= MinValue);
			CHECK(Value <= MaxValue);
		}
	}
};

TEST_CASE_METHOD(FPCGPolygon2DOperationTest, "PCG::Polygon2DOperation::UnionForwardsFirstInputDataDomainValuesAndTags", "[PCG][Polygon2DOperation]")
{
	using namespace PCGPolygon2DOperationTests;

	TypedSettings->Operation = EPCGPolygonOperation::Union;
	AddInput(MakePolygon(SquareA, 42, VertexValuesA), { TEXT("TagA") });
	AddInput(MakePolygon(SquareB, 77, VertexValuesB), { TEXT("TagB") });
	ExecuteElement();

	CHECK(NumErrors == 0);
	CHECK(NumWarnings == 0);
	REQUIRE(Context->OutputData.TaggedData.Num() == 1);

	const UPCGPolygon2DData* OutPolyData = RequireOutputPolygon(0);

	// Data domain values and tags come from the first contributing input.
	CheckDataDomainValue(OutPolyData, 42);
	CHECK(Context->OutputData.TaggedData[0].Tags.Contains(TEXT("TagA")));
	CHECK_FALSE(Context->OutputData.TaggedData[0].Tags.Contains(TEXT("TagB")));

	// Vertex values are exact or interpolated from either input, never defaulted.
	CheckVertexValuesInRange(OutPolyData, 1.0, 40.0);
}

TEST_CASE_METHOD(FPCGPolygon2DOperationTest, "PCG::Polygon2DOperation::UnionForwardsFirstInputToAllOutputs", "[PCG][Polygon2DOperation]")
{
	using namespace PCGPolygon2DOperationTests;

	TypedSettings->Operation = EPCGPolygonOperation::Union;
	AddInput(MakePolygon(SquareA, 42, VertexValuesA), { TEXT("TagA") });
	AddInput(MakePolygon(SquareFarAway, 77, VertexValuesB));
	ExecuteElement();

	CHECK(NumErrors == 0);
	CHECK(NumWarnings == 0);
	REQUIRE(Context->OutputData.TaggedData.Num() == 2);

	for (int32 OutputIndex = 0; OutputIndex < Context->OutputData.TaggedData.Num(); ++OutputIndex)
	{
		const UPCGPolygon2DData* OutPolyData = RequireOutputPolygon(OutputIndex);
		CheckDataDomainValue(OutPolyData, 42);
		CHECK(Context->OutputData.TaggedData[OutputIndex].Tags.Contains(TEXT("TagA")));
		CheckVertexValuesInRange(OutPolyData, 1.0, 40.0);
	}
}

TEST_CASE_METHOD(FPCGPolygon2DOperationTest, "PCG::Polygon2DOperation::InnerIntersectionForwardsFirstInputDataDomainValues", "[PCG][Polygon2DOperation]")
{
	using namespace PCGPolygon2DOperationTests;

	TypedSettings->Operation = EPCGPolygonOperation::InnerIntersection;
	AddInput(MakePolygon(SquareA, 42, VertexValuesA));
	AddInput(MakePolygon(SquareB, 77, VertexValuesB));
	ExecuteElement();

	CHECK(NumErrors == 0);
	CHECK(NumWarnings == 0);
	REQUIRE(Context->OutputData.TaggedData.Num() == 1);

	const UPCGPolygon2DData* OutPolyData = RequireOutputPolygon(0);
	CheckDataDomainValue(OutPolyData, 42);
	CheckVertexValuesInRange(OutPolyData, 1.0, 40.0);
}

TEST_CASE_METHOD(FPCGPolygon2DOperationTest, "PCG::Polygon2DOperation::DifferenceForwardsInputDataDomainValues", "[PCG][Polygon2DOperation]")
{
	using namespace PCGPolygon2DOperationTests;

	TypedSettings->Operation = EPCGPolygonOperation::Difference;
	AddInput(MakePolygon(SquareA, 42, VertexValuesA), { TEXT("TagA") });
	AddInput(MakePolygon(SquareB, 77, VertexValuesB), { TEXT("TagB") }, PCGPolygon2DUtils::Constants::ClipPolysLabel);
	ExecuteElement();

	CHECK(NumErrors == 0);
	CHECK(NumWarnings == 0);
	REQUIRE(Context->OutputData.TaggedData.Num() == 1);

	const UPCGPolygon2DData* OutPolyData = RequireOutputPolygon(0);
	CheckDataDomainValue(OutPolyData, 42);
	CHECK(Context->OutputData.TaggedData[0].Tags.Contains(TEXT("TagA")));
	CheckVertexValuesInRange(OutPolyData, 1.0, 40.0);
}

TEST_CASE_METHOD(FPCGPolygon2DOperationTest, "PCG::Polygon2DOperation::UnionMetadataModeNoneDoesNotInheritAttributes", "[PCG][Polygon2DOperation]")
{
	using namespace PCGPolygon2DOperationTests;

	TypedSettings->Operation = EPCGPolygonOperation::Union;
	TypedSettings->MetadataMode = EPCGPolygonOperationMetadataMode::None;
	AddInput(MakePolygon(SquareA, 42, VertexValuesA));
	AddInput(MakePolygon(SquareB, 77, VertexValuesB));
	ExecuteElement();

	CHECK(NumErrors == 0);
	CHECK(NumWarnings == 0);
	REQUIRE(Context->OutputData.TaggedData.Num() == 1);

	const UPCGPolygon2DData* OutPolyData = RequireOutputPolygon(0);
	const UPCGMetadata* OutMetadata = OutPolyData->ConstMetadata();
	REQUIRE(OutMetadata);

	// If the data domain has no attribute it might never have been initialized, so it is either null or without the attribute.
	const FPCGMetadataDomain* DataDomain = OutMetadata->GetConstMetadataDomain(PCGMetadataDomainID::Data);
	CHECK((DataDomain == nullptr || !DataDomain->HasAttribute(DataAttrName)));
	CHECK(OutMetadata->GetConstAttribute(VertexAttrName) == nullptr);
}
