// Copyright Epic Games, Inc. All Rights Reserved.

// =============================================================================
// PCG Copy Attributes Tests
//
// Focus: the "same data" (Source pin and Target pin reference the same UPCGData)
// early-outs in the copy-all-attributes path. Copying all attributes onto self is
// only a trivial no-op when the resolved domain mapping maps every domain onto
// itself (empty mapping => Default->Default, or Copy All Domains). When an explicit
// mapping moves attributes to a *different* domain, it is a real operation and must
// not be skipped.
//
// Test summary:
//
// Same-data behavior:
//  - CrossDomainMappingCopies:   explicit Data->Elements mapping on the same data must copy
//  - EqualSelectorsStillCopies:  copy-all must not be skipped by the single-attribute self early-out
//  - EmptyMappingIsNoOp:         empty mapping (Default->Default) is a no-op, output keeps its attribute
//  - CopyAllDomainsIsNoOp:       Copy All Domains maps each domain to itself, a no-op on the same data
//
// Control:
//  - DifferentDataCrossDomainCopies: distinct source/target always copies (isolates the early-out)
// =============================================================================

#include "PCGTestsCommon.h"

#include "PCGContext.h"
#include "PCGData.h"
#include "PCGParamData.h"
#include "Elements/PCGCopyAttributes.h"
#include "Elements/PCGCopyPoints.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/PCGMetadataDomain.h"

#include <catch2/catch_test_macros.hpp>

#include "TestHarness.h"

namespace PCGCopyAttributesTests
{
	class FCopyAttributesTestBase : public PCGTests::FPCGSingleElementBaseTest<UPCGCopyAttributesSettings>
	{
	public:
		inline static const FName AttrName = TEXT("MyAttr");

		/** Param data carrying a single int32 attribute (default value 42) on the requested domain only. */
		UPCGParamData* MakeParamWithAttributeOnDomain(const FPCGMetadataDomainID DomainID)
		{
			UPCGParamData* ParamData = NewObject<UPCGParamData>();
			UPCGMetadata* Metadata = ParamData->MutableMetadata();
			check(Metadata);

			FPCGMetadataDomain* Domain = Metadata->GetMetadataDomain(DomainID);
			check(Domain);

			FPCGMetadataAttribute<int32>* Attr = Domain->CreateAttribute<int32>(AttrName, 42, /*bAllowsInterpolation=*/false, /*bOverrideParent=*/false);
			check(Attr);

			return ParamData;
		}

		void AddInput(FName PinLabel, UPCGData* Data)
		{
			FPCGTaggedData& Tagged = InputData.TaggedData.Emplace_GetRef();
			Tagged.Data = Data;
			Tagged.Pin = PinLabel;
		}

		const UPCGData* GetSingleOutput()
		{
			REQUIRE(Context->OutputData.TaggedData.Num() == 1);
			const UPCGData* OutData = Context->OutputData.TaggedData[0].Data.Get();
			REQUIRE(OutData != nullptr);
			return OutData;
		}

		static bool HasAttributeOnDomain(const UPCGData* Data, const FPCGMetadataDomainID DomainID, FName InAttrName)
		{
			const UPCGMetadata* Metadata = Data ? Data->ConstMetadata() : nullptr;
			const FPCGMetadataDomain* Domain = Metadata ? Metadata->GetConstMetadataDomain(DomainID) : nullptr;
			return Domain && Domain->HasAttribute(InAttrName);
		}
	};
}

// Same data, explicit Data->Elements mapping: this is the bug. Copying all attributes onto self
// with a cross-domain mapping is a real transfer and must not be treated as a trivial no-op.
TEST_CASE_METHOD(PCGCopyAttributesTests::FCopyAttributesTestBase, "PCG::CopyAttributes::SameData::CrossDomainMappingCopies", "[PCG][CopyAttributes]")
{
	UPCGParamData* ParamData = MakeParamWithAttributeOnDomain(PCGMetadataDomainID::Data);

	// The attribute starts only on the Data domain.
	REQUIRE(HasAttributeOnDomain(ParamData, PCGMetadataDomainID::Data, AttrName));
	REQUIRE(!HasAttributeOnDomain(ParamData, PCGMetadataDomainID::Elements, AttrName));

	TypedSettings->bCopyAllAttributes = true;
	TypedSettings->bCopyAllDomains = false;
	TypedSettings->MetadataDomainsMapping.Add(PCGDataConstants::DataDomainName, PCGParamDataConstants::ElementsDomainName);

	// Same object on both pins => bIsSameData is true, exercising the early-out under test.
	AddInput(PCGCopyPointsConstants::SourcePointsLabel, ParamData);
	AddInput(PCGCopyPointsConstants::TargetPointsLabel, ParamData);

	ExecuteElement();

	CHECK(NumErrors == 0);

	const UPCGData* OutData = GetSingleOutput();
	CHECK(HasAttributeOnDomain(OutData, PCGMetadataDomainID::Elements, AttrName));
}

// Same data, copy-all, plus InputSource == OutputTarget. The single-attribute "copy to self"
// early-out must not fire while copying all attributes. Guards the `&& !bCopyAllAttributes` clause.
TEST_CASE_METHOD(PCGCopyAttributesTests::FCopyAttributesTestBase, "PCG::CopyAttributes::SameData::EqualSelectorsStillCopies", "[PCG][CopyAttributes]")
{
	UPCGParamData* ParamData = MakeParamWithAttributeOnDomain(PCGMetadataDomainID::Data);

	TypedSettings->bCopyAllAttributes = true;
	TypedSettings->bCopyAllDomains = false;
	TypedSettings->MetadataDomainsMapping.Add(PCGDataConstants::DataDomainName, PCGParamDataConstants::ElementsDomainName);

	// Make the (otherwise unused in copy-all mode) selectors equal, so the single-attribute
	// self early-out would trigger if it were not gated on !bCopyAllAttributes.
	TypedSettings->InputSource.SetAttributeName(AttrName);
	TypedSettings->OutputTarget.SetAttributeName(AttrName);
	REQUIRE(TypedSettings->InputSource == TypedSettings->OutputTarget);

	AddInput(PCGCopyPointsConstants::SourcePointsLabel, ParamData);
	AddInput(PCGCopyPointsConstants::TargetPointsLabel, ParamData);

	ExecuteElement();

	CHECK(NumErrors == 0);

	const UPCGData* OutData = GetSingleOutput();
	CHECK(HasAttributeOnDomain(OutData, PCGMetadataDomainID::Elements, AttrName));
}

// Same data, empty mapping (resolves to Default->Default): a genuine no-op. The output must still
// carry the original attribute and report no errors.
TEST_CASE_METHOD(PCGCopyAttributesTests::FCopyAttributesTestBase, "PCG::CopyAttributes::SameData::EmptyMappingIsNoOp", "[PCG][CopyAttributes]")
{
	UPCGParamData* ParamData = MakeParamWithAttributeOnDomain(PCGMetadataDomainID::Elements);

	TypedSettings->bCopyAllAttributes = true;
	TypedSettings->bCopyAllDomains = false;
	// MetadataDomainsMapping intentionally left empty.

	AddInput(PCGCopyPointsConstants::SourcePointsLabel, ParamData);
	AddInput(PCGCopyPointsConstants::TargetPointsLabel, ParamData);

	ExecuteElement();

	CHECK(NumErrors == 0);

	const UPCGData* OutData = GetSingleOutput();
	CHECK(HasAttributeOnDomain(OutData, PCGMetadataDomainID::Elements, AttrName));
}

// Same data, Copy All Domains: maps every domain onto itself, so it is a no-op on the same data.
// The original attribute must survive on the output.
TEST_CASE_METHOD(PCGCopyAttributesTests::FCopyAttributesTestBase, "PCG::CopyAttributes::SameData::CopyAllDomainsIsNoOp", "[PCG][CopyAttributes]")
{
	UPCGParamData* ParamData = MakeParamWithAttributeOnDomain(PCGMetadataDomainID::Elements);

	TypedSettings->bCopyAllAttributes = true;
	TypedSettings->bCopyAllDomains = true;

	AddInput(PCGCopyPointsConstants::SourcePointsLabel, ParamData);
	AddInput(PCGCopyPointsConstants::TargetPointsLabel, ParamData);

	ExecuteElement();

	CHECK(NumErrors == 0);

	const UPCGData* OutData = GetSingleOutput();
	CHECK(HasAttributeOnDomain(OutData, PCGMetadataDomainID::Elements, AttrName));
}

// Control: distinct source and target. The same-data early-out never applies, so the cross-domain
// copy always happens. Confirms the copy machinery works, isolating the early-out as the variable.
TEST_CASE_METHOD(PCGCopyAttributesTests::FCopyAttributesTestBase, "PCG::CopyAttributes::DifferentData::CrossDomainMappingCopies", "[PCG][CopyAttributes]")
{
	UPCGParamData* Source = MakeParamWithAttributeOnDomain(PCGMetadataDomainID::Data);
	UPCGParamData* Target = NewObject<UPCGParamData>();

	TypedSettings->bCopyAllAttributes = true;
	TypedSettings->bCopyAllDomains = false;
	TypedSettings->MetadataDomainsMapping.Add(PCGDataConstants::DataDomainName, PCGParamDataConstants::ElementsDomainName);

	AddInput(PCGCopyPointsConstants::SourcePointsLabel, Source);
	AddInput(PCGCopyPointsConstants::TargetPointsLabel, Target);

	ExecuteElement();

	CHECK(NumErrors == 0);

	const UPCGData* OutData = GetSingleOutput();
	CHECK(HasAttributeOnDomain(OutData, PCGMetadataDomainID::Elements, AttrName));
}
