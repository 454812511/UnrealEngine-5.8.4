// Copyright Epic Games, Inc. All Rights Reserved.

// =============================================================================
// PCG Metadata Attribute Interpolation Tests
//
// Tests for the weighted-sum interpolation path of FPCGMetadataAttribute<T> and spline sampling.
//
// Test summary:
//
// SetWeightedValue (unit):
//  - IntegerEndpointsPreserved:  same value on both endpoints survives any alpha (int32/int64)
//  - IntegerRoundsToNearest:     interior interpolation rounds to nearest instead of truncating
//  - MultiKeyWeightedSum:        N-key convex combinations behave like a single rounded sum
//  - FloatingPointLerp:          float/double/FVector interpolation still matches the lerp
//
// Spline metadata path (end-to-end):
//  - SplineWriteMetadataToEntry: int32 control-point attribute is exact on control points and
//                                drift-free on interior input keys
// =============================================================================

#include "PCGTestsCommon.h"

#include "PCGParamData.h"
#include "Data/PCGPointArrayData.h"
#include "Data/PCGSplineData.h"
#include "Metadata/PCGMetadataAttributeTpl.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "TestHarness.h"

namespace PCGAttributeInterpolationTests
{
	static const FName AttributeName = "InterpolatedAttr";
}

/**
 * A value present on both interpolation endpoints must be preserved exactly for any alpha.
 * The old per-step integer conversion drifted by +/-1 for most non-trivial alphas.
 */
TEST_CASE_METHOD(PCGTests::FPCGBaseTestWithBasicContext, "PCG::AttributeInterpolation::IntegerEndpointsPreserved", "[PCG][AttributeInterpolation]")
{
	using namespace PCGAttributeInterpolationTests;

	auto TypedTester = [this]<typename T>()
	{
		UPCGParamData* ParamData = CreateData<UPCGParamData>();
		FPCGMetadataDomain* Domain = ParamData->MutableMetadata()->GetDefaultMetadataDomain();
		REQUIRE_NOT_EQUAL(Domain, nullptr);

		FPCGMetadataAttribute<T>* Attribute = Domain->CreateAttribute<T>(AttributeName, T{}, /*bAllowsInterpolation=*/true, /*bOverrideParent=*/false);
		REQUIRE_NOT_EQUAL(Attribute, nullptr);

		const T TestValues[] = {1, 5, 100, -5, -100, 9999991, -9999991, 16777215, -16777215};

		for (const T Value : TestValues)
		{
			const PCGMetadataEntryKey KeyA = Domain->AddEntry();
			const PCGMetadataEntryKey KeyB = Domain->AddEntry();
			Attribute->SetValue(KeyA, Value);
			Attribute->SetValue(KeyB, Value);

			for (int32 Step = 0; Step <= 100; ++Step)
			{
				const float Alpha = static_cast<float>(Step) / 100.0f;
				const PCGMetadataEntryKey OutKey = Domain->AddEntry();

				// Same coefficient shape as UPCGSplineData::WriteMetadataToEntry.
				const TPair<PCGMetadataEntryKey, float> Coefficients[] = {{KeyA, 1.0f - Alpha}, {KeyB, Alpha}};
				Attribute->SetWeightedValue(OutKey, Attribute, Coefficients);

				REQUIRE_EQUAL(Attribute->GetValueFromItemKey(OutKey), Value);
			}
		}
	};

	SECTION("int32") { TypedTester.template operator()<int32>(); }
	SECTION("int64") { TypedTester.template operator()<int64>(); }
}

/**
 * Interior interpolation between two different integer values rounds to nearest
 * (halves toward positive infinity, matching FMath::RoundToDouble) instead of truncating.
 */
TEST_CASE_METHOD(PCGTests::FPCGBaseTestWithBasicContext, "PCG::AttributeInterpolation::IntegerRoundsToNearest", "[PCG][AttributeInterpolation]")
{
	using namespace PCGAttributeInterpolationTests;

	auto [ValueA, ValueB, Alpha, Expected] = GENERATE(Catch::Generators::table<int32, int32, float, int32>(
		{
			// Exact endpoints.
			{10, 20, 0.0f, 10},
			{10, 20, 1.0f, 20},
			// 2.5 rounds to 3; a per-step truncating accumulation yields 2.
			{0, 10, 0.25f, 3},
			{0, 10, 0.5f, 5},
			// 7.5 rounds to 8.
			{0, 10, 0.75f, 8},
			// Negative halves round toward positive infinity: -2.5 -> -2, -7.5 -> -7.
			{0, -10, 0.25f, -2},
			{0, -10, 0.75f, -7},
			// Non-exact float alpha: 10 * (1 - 0.3f) + 20 * 0.3f = 13.0000001; per-step truncation yields 12.
			{10, 20, 0.3f, 13},
		}));

	DYNAMIC_SECTION("Interpolate " << ValueA << " to " << ValueB << " at alpha " << Alpha)
	{
		UPCGParamData* ParamData = CreateData<UPCGParamData>();
		FPCGMetadataDomain* Domain = ParamData->MutableMetadata()->GetDefaultMetadataDomain();
		REQUIRE_NOT_EQUAL(Domain, nullptr);

		FPCGMetadataAttribute<int32>* Attribute = Domain->CreateAttribute<int32>(AttributeName, 0, /*bAllowsInterpolation=*/true, /*bOverrideParent=*/false);
		REQUIRE_NOT_EQUAL(Attribute, nullptr);

		const PCGMetadataEntryKey KeyA = Domain->AddEntry();
		const PCGMetadataEntryKey KeyB = Domain->AddEntry();
		Attribute->SetValue(KeyA, ValueA);
		Attribute->SetValue(KeyB, ValueB);

		const PCGMetadataEntryKey OutKey = Domain->AddEntry();
		const TPair<PCGMetadataEntryKey, float> Coefficients[] = {{KeyA, 1.0f - Alpha}, {KeyB, Alpha}};
		Attribute->SetWeightedValue(OutKey, Attribute, Coefficients);

		REQUIRE_EQUAL(Attribute->GetValueFromItemKey(OutKey), Expected);
	}
}

/**
 * Weighted sums over more than two keys accumulate without intermediate truncation.
 */
TEST_CASE_METHOD(PCGTests::FPCGBaseTestWithBasicContext, "PCG::AttributeInterpolation::MultiKeyWeightedSum", "[PCG][AttributeInterpolation]")
{
	using namespace PCGAttributeInterpolationTests;

	UPCGParamData* ParamData = CreateData<UPCGParamData>();
	FPCGMetadataDomain* Domain = ParamData->MutableMetadata()->GetDefaultMetadataDomain();
	REQUIRE_NOT_EQUAL(Domain, nullptr);

	FPCGMetadataAttribute<int32>* Attribute = Domain->CreateAttribute<int32>(AttributeName, 0, /*bAllowsInterpolation=*/true, /*bOverrideParent=*/false);
	REQUIRE_NOT_EQUAL(Attribute, nullptr);

	SECTION("Four equal keys with quarter weights")
	{
		TArray<TPair<PCGMetadataEntryKey, float>> Coefficients;
		for (int32 i = 0; i < 4; ++i)
		{
			const PCGMetadataEntryKey Key = Domain->AddEntry();
			Attribute->SetValue(Key, 5);
			Coefficients.Emplace(Key, 0.25f);
		}

		const PCGMetadataEntryKey OutKey = Domain->AddEntry();
		Attribute->SetWeightedValue(OutKey, Attribute, Coefficients);
		REQUIRE_EQUAL(Attribute->GetValueFromItemKey(OutKey), 5);
	}

	SECTION("Three equal keys with third weights")
	{
		TArray<TPair<PCGMetadataEntryKey, float>> Coefficients;
		for (int32 i = 0; i < 3; ++i)
		{
			const PCGMetadataEntryKey Key = Domain->AddEntry();
			Attribute->SetValue(Key, 7);
			Coefficients.Emplace(Key, 1.0f / 3.0f);
		}

		const PCGMetadataEntryKey OutKey = Domain->AddEntry();
		Attribute->SetWeightedValue(OutKey, Attribute, Coefficients);
		REQUIRE_EQUAL(Attribute->GetValueFromItemKey(OutKey), 7);
	}

	SECTION("Mixed values round once at the end")
	{
		// 0 * 0.5 + 10 * 0.25 + 20 * 0.25 = 7.5, rounds to 8.
		const int32 Values[] = {0, 10, 20};
		const float Weights[] = {0.5f, 0.25f, 0.25f};

		TArray<TPair<PCGMetadataEntryKey, float>> Coefficients;
		for (int32 i = 0; i < 3; ++i)
		{
			const PCGMetadataEntryKey Key = Domain->AddEntry();
			Attribute->SetValue(Key, Values[i]);
			Coefficients.Emplace(Key, Weights[i]);
		}

		const PCGMetadataEntryKey OutKey = Domain->AddEntry();
		Attribute->SetWeightedValue(OutKey, Attribute, Coefficients);
		REQUIRE_EQUAL(Attribute->GetValueFromItemKey(OutKey), 8);
	}
}

/**
 * Floating-point and vector attributes keep their existing interpolation behavior.
 */
TEST_CASE_METHOD(PCGTests::FPCGBaseTestWithBasicContext, "PCG::AttributeInterpolation::FloatingPointLerp", "[PCG][AttributeInterpolation]")
{
	using namespace PCGAttributeInterpolationTests;

	auto TypedTester = [this]<typename T>(const T& ValueA, const T& ValueB, float Alpha, const T& Expected)
	{
		UPCGParamData* ParamData = CreateData<UPCGParamData>();
		FPCGMetadataDomain* Domain = ParamData->MutableMetadata()->GetDefaultMetadataDomain();
		REQUIRE_NOT_EQUAL(Domain, nullptr);

		FPCGMetadataAttribute<T>* Attribute = Domain->CreateAttribute<T>(AttributeName, T{}, /*bAllowsInterpolation=*/true, /*bOverrideParent=*/false);
		REQUIRE_NOT_EQUAL(Attribute, nullptr);

		const PCGMetadataEntryKey KeyA = Domain->AddEntry();
		const PCGMetadataEntryKey KeyB = Domain->AddEntry();
		Attribute->SetValue(KeyA, ValueA);
		Attribute->SetValue(KeyB, ValueB);

		const PCGMetadataEntryKey OutKey = Domain->AddEntry();
		const TPair<PCGMetadataEntryKey, float> Coefficients[] = {{KeyA, 1.0f - Alpha}, {KeyB, Alpha}};
		Attribute->SetWeightedValue(OutKey, Attribute, Coefficients);

		const T Result = Attribute->GetValueFromItemKey(OutKey);
		if constexpr (std::is_same_v<T, FVector>)
		{
			REQUIRE(Result.Equals(Expected, /*Tolerance=*/1.0e-4));
		}
		else
		{
			REQUIRE(FMath::IsNearlyEqual(Result, Expected, static_cast<T>(1.0e-4)));
		}
	};

	SECTION("float endpoints preserved") { TypedTester.template operator()<float>(5.0f, 5.0f, 0.3f, 5.0f); }
	SECTION("float lerp") { TypedTester.template operator()<float>(10.0f, 20.0f, 0.25f, 12.5f); }
	SECTION("double endpoints preserved") { TypedTester.template operator()<double>(5.0, 5.0, 0.3f, 5.0); }
	SECTION("double lerp") { TypedTester.template operator()<double>(10.0, 20.0, 0.25f, 12.5); }
	SECTION("FVector lerp") { TypedTester.template operator()<FVector>(FVector(1.0, 2.0, 3.0), FVector(3.0, 4.0, 5.0), 0.5f, FVector(2.0, 3.0, 4.0)); }
}

/**
 * End-to-end repro of the Spline Sampler drift: an int32 attribute carried on spline control
 * points must be exact on control points and drift-free on interior input keys.
 */
TEST_CASE_METHOD(PCGTests::FPCGBaseTestWithBasicContext, "PCG::AttributeInterpolation::SplineWriteMetadataToEntry", "[PCG][AttributeInterpolation]")
{
	static const FName ConstantAttrName = "ConstantAttr";
	static const FName VaryingAttrName = "VaryingAttr";

	constexpr int32 NumControlPoints = 3;
	constexpr int32 ConstantValue = 5;
	const int32 VaryingValues[NumControlPoints] = {10, 20, 30};

	UPCGSplineData* SplineData = CreateData<UPCGSplineData>();
	FPCGMetadataDomain* Domain = SplineData->MutableMetadata()->GetMetadataDomain(PCGMetadataDomainID::Elements);
	REQUIRE_NOT_EQUAL(Domain, nullptr);

	FPCGMetadataAttribute<int32>* ConstantAttribute = Domain->CreateAttribute<int32>(ConstantAttrName, 0, /*bAllowsInterpolation=*/true, /*bOverrideParent=*/false);
	FPCGMetadataAttribute<int32>* VaryingAttribute = Domain->CreateAttribute<int32>(VaryingAttrName, 0, /*bAllowsInterpolation=*/true, /*bOverrideParent=*/false);
	REQUIRE_NOT_EQUAL(ConstantAttribute, nullptr);
	REQUIRE_NOT_EQUAL(VaryingAttribute, nullptr);

	TArray<FSplinePoint> SplinePoints;
	TArray<PCGMetadataEntryKey> EntryKeys;
	for (int32 i = 0; i < NumControlPoints; ++i)
	{
		SplinePoints.Emplace_GetRef(static_cast<float>(i), FVector(i * 100.0, 0.0, 0.0)).Type = ESplinePointType::Linear;

		const PCGMetadataEntryKey EntryKey = EntryKeys.Add_GetRef(Domain->AddEntry());
		ConstantAttribute->SetValue(EntryKey, ConstantValue);
		VaryingAttribute->SetValue(EntryKey, VaryingValues[i]);
	}

	SplineData->Initialize(SplinePoints, /*bClosedLoop=*/false, FTransform::Identity, EntryKeys);

	// Mirror UPCGSplineData::CreateBasePointData to get a production-like target metadata.
	UPCGPointArrayData* OutData = CreateData<UPCGPointArrayData>();
	FPCGInitializeFromDataParams InitializeFromDataParams(SplineData);
	InitializeFromDataParams.bInheritSpatialData = false;
	OutData->InitializeFromDataWithParams(InitializeFromDataParams);

	FPCGMetadataDomain* OutDomain = OutData->MutableMetadata()->GetMetadataDomain(PCGMetadataDomainID::Elements);
	REQUIRE_NOT_EQUAL(OutDomain, nullptr);

	FPCGMetadataAttribute<int32>* OutConstantAttribute = OutDomain->GetMutableTypedAttribute<int32>(ConstantAttrName);
	FPCGMetadataAttribute<int32>* OutVaryingAttribute = OutDomain->GetMutableTypedAttribute<int32>(VaryingAttrName);
	REQUIRE_NOT_EQUAL(OutConstantAttribute, nullptr);
	REQUIRE_NOT_EQUAL(OutVaryingAttribute, nullptr);

	auto SampleAt = [SplineData, OutData](float InputKey) -> PCGMetadataEntryKey
	{
		PCGMetadataEntryKey OutKey = PCGInvalidEntryKey;
		SplineData->WriteMetadataToEntry(InputKey, OutKey, OutData->MutableMetadata());
		REQUIRE(OutKey != PCGInvalidEntryKey);
		return OutKey;
	};

	SECTION("Constant attribute is preserved on a dense input key sweep")
	{
		for (int32 Step = 0; Step <= 40; ++Step)
		{
			const float InputKey = static_cast<float>(Step) * 0.05f;
			const PCGMetadataEntryKey OutKey = SampleAt(InputKey);
			REQUIRE_EQUAL(OutConstantAttribute->GetValueFromItemKey(OutKey), ConstantValue);
		}
	}

	SECTION("Control points keep exact varying values")
	{
		for (int32 i = 0; i < NumControlPoints; ++i)
		{
			const PCGMetadataEntryKey OutKey = SampleAt(static_cast<float>(i));
			REQUIRE_EQUAL(OutVaryingAttribute->GetValueFromItemKey(OutKey), VaryingValues[i]);
		}
	}

	SECTION("Interior samples interpolate the varying value with round-to-nearest")
	{
		auto [InputKey, Expected] = GENERATE(Catch::Generators::table<float, int32>(
			{
				// Segment 0: lerp(10, 20). 12.5 rounds to 13.
				{0.25f, 13},
				{0.5f, 15},
				{0.75f, 18},
				// Segment 1: lerp(20, 30). 27.5 rounds to 28.
				{1.25f, 23},
				{1.5f, 25},
				{1.75f, 28},
			}));

		DYNAMIC_SECTION("Sample at input key " << InputKey)
		{
			const PCGMetadataEntryKey OutKey = SampleAt(InputKey);
			REQUIRE_EQUAL(OutVaryingAttribute->GetValueFromItemKey(OutKey), Expected);
		}
	}
}
