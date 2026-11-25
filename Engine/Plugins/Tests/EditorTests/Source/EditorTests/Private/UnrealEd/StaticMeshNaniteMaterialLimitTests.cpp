// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshDescription.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Verifies a static mesh whose section references a material index at the Nanite limit (64) builds gracefully
 * without Nanite rather than crashing. The trigger is the material index value, not the section count.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshNaniteMaterialLimitTest, "Editor.Meshes.StaticMesh.NaniteMaterialLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace UE::Private::StaticMeshNaniteMaterialLimitTest
{

// Nanite's material limit (NANITE_MAX_CLUSTER_MATERIALS / Nanite::MaxSectionArraySize), inlined to avoid a
// dependency on the NaniteBuilder module.
constexpr int32 NaniteMaterialLimit = 64;

// Builds a Nanite-enabled mesh with one section forced to material index NaniteMaterialLimit (the 65th slot).
UStaticMesh* CreateOverLimitNaniteStaticMesh()
{
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	FMeshNaniteSettings NaniteSettings;
	NaniteSettings.bEnabled = true;
	StaticMesh->SetNaniteSettings(NaniteSettings);

	// Add one more material slot than Nanite supports so that slot index NaniteMaterialLimit is a valid
	// index into the Materials array (the non-Nanite fallback still needs to resolve it).
	UMaterialInterface* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	TArray<FName> SlotNames;
	SlotNames.Reserve(NaniteMaterialLimit + 1);
	for (int32 SlotIndex = 0; SlotIndex <= NaniteMaterialLimit; ++SlotIndex)
	{
		const FName SlotName(*FString::Printf(TEXT("TestMaterialSlot_%d"), SlotIndex));
		SlotNames.Add(SlotName);
		StaticMesh->GetStaticMaterials().Add(FStaticMaterial(DefaultMaterial, SlotName, SlotName));
	}

	// Single cube bound to the over-limit slot (index NaniteMaterialLimit).
	const FName OverLimitSlotName = SlotNames.Last();
	StaticMesh->SetNumSourceModels(1);
	UStaticMeshDescription* StaticMeshDescription = UStaticMesh::CreateStaticMeshDescription();
	const FPolygonGroupID PolygonGroupID = StaticMeshDescription->CreatePolygonGroup();
	StaticMeshDescription->SetPolygonGroupMaterialSlotName(PolygonGroupID, OverLimitSlotName);
	FPolygonID PolygonIDs[6];
	StaticMeshDescription->CreateCube(FVector::ZeroVector, FVector(50.0), PolygonGroupID,
		PolygonIDs[0], PolygonIDs[1], PolygonIDs[2], PolygonIDs[3], PolygonIDs[4], PolygonIDs[5]);

	FMeshDescription* MeshDescription = StaticMesh->CreateMeshDescription(0);
	*MeshDescription = StaticMeshDescription->GetMeshDescription();

	UStaticMesh::FCommitMeshDescriptionParams Params;
	Params.bUseHashAsGuid = true;
	Params.bMarkPackageDirty = false;
	StaticMesh->CommitMeshDescription(0, Params);

	// The Nanite build reads section material indices from the section info map, so pin it to the over-limit slot.
	StaticMesh->GetSectionInfoMap().Set(0, 0, FMeshSectionInfo(NaniteMaterialLimit));

	return StaticMesh;
}

void BuildAndWait(UStaticMesh* StaticMesh)
{
	UStaticMesh::FBuildParameters BuildParameters;
	BuildParameters.bInSilent = true;
	StaticMesh->Build(BuildParameters);

	if (StaticMesh->IsCompiling())
	{
		FStaticMeshCompilingManager::Get().FinishCompilation({ StaticMesh });
	}
}

} // namespace UE::Private::StaticMeshNaniteMaterialLimitTest

bool FStaticMeshNaniteMaterialLimitTest::RunTest(const FString& Parameters)
{
	using namespace UE::Private::StaticMeshNaniteMaterialLimitTest;

	UStaticMesh* StaticMesh = CreateOverLimitNaniteStaticMesh();
	StaticMesh->BuildCacheAutomationTestGuid = FGuid::NewGuid();

	// Must complete without crashing in FCluster::BuildMaterialRanges.
	BuildAndWait(StaticMesh);

	UTEST_NOT_NULL(TEXT("RenderData after building an over-limit Nanite mesh"), StaticMesh->GetRenderData());
	UTEST_TRUE(TEXT("Non-Nanite render data was built so the mesh stays renderable"), StaticMesh->GetRenderData()->LODResources.Num() > 0);
	UTEST_FALSE(TEXT("Nanite build was skipped for the over-limit mesh"), StaticMesh->GetRenderData()->HasValidNaniteData());
	UTEST_EQUAL(TEXT("Material slots are preserved after the failed Nanite build"), StaticMesh->GetStaticMaterials().Num(), NaniteMaterialLimit + 1);

	// A mesh within the limit still builds Nanite normally (guards against the fix over-rejecting).
	{
		UStaticMesh* WithinLimitMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
		FMeshNaniteSettings WithinLimitNaniteSettings;
		WithinLimitNaniteSettings.bEnabled = true;
		WithinLimitMesh->SetNaniteSettings(WithinLimitNaniteSettings);
		const FName MaterialSlotName(TEXT("TestMaterialSlot"));
		WithinLimitMesh->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), MaterialSlotName, MaterialSlotName));

		WithinLimitMesh->SetNumSourceModels(1);
		UStaticMeshDescription* Description = UStaticMesh::CreateStaticMeshDescription();
		const FPolygonGroupID PolygonGroupID = Description->CreatePolygonGroup();
		Description->SetPolygonGroupMaterialSlotName(PolygonGroupID, MaterialSlotName);
		FPolygonID PolygonIDs[6];
		Description->CreateCube(FVector::ZeroVector, FVector(50.0), PolygonGroupID,
			PolygonIDs[0], PolygonIDs[1], PolygonIDs[2], PolygonIDs[3], PolygonIDs[4], PolygonIDs[5]);

		FMeshDescription* MeshDescription = WithinLimitMesh->CreateMeshDescription(0);
		*MeshDescription = Description->GetMeshDescription();

		UStaticMesh::FCommitMeshDescriptionParams Params;
		Params.bUseHashAsGuid = true;
		Params.bMarkPackageDirty = false;
		WithinLimitMesh->CommitMeshDescription(0, Params);
		WithinLimitMesh->BuildCacheAutomationTestGuid = FGuid::NewGuid();

		BuildAndWait(WithinLimitMesh);
		UTEST_NOT_NULL(TEXT("RenderData for a within-limit Nanite mesh"), WithinLimitMesh->GetRenderData());
		UTEST_TRUE(TEXT("Within-limit mesh still builds Nanite"), WithinLimitMesh->GetRenderData()->HasValidNaniteData());
	}

	return true;
}

/**
 * Test for a single cluster referencing a material beyond the per-cluster limit. NaniteMaterialLimit
 * disconnected triangles fit in one cluster but span material slots 1..NaniteMaterialLimit, so the cluster
 * references material index NaniteMaterialLimit and overflows the per-cluster table. Stays within the section
 * count, so it reaches the cluster builder rather than the section-count guard, and must build without Nanite.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshNaniteSingleClusterMaterialLimitTest, "Editor.Meshes.StaticMesh.NaniteSingleClusterMaterialLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace UE::Private::StaticMeshNaniteMaterialLimitTest
{

// Appends one disconnected triangle bound to PolygonGroupID, offset so it shares no geometry with the others.
void AppendDisconnectedTriangle(FMeshDescription& Mesh, FStaticMeshAttributes& Attributes, FPolygonGroupID PolygonGroupID, int32 TriangleIndex)
{
	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();

	const FVector3f Offset(TriangleIndex * 100.0f, 0.0f, 0.0f);

	const FVertexID V0 = Mesh.CreateVertex();
	const FVertexID V1 = Mesh.CreateVertex();
	const FVertexID V2 = Mesh.CreateVertex();
	Positions[V0] = Offset + FVector3f(0.0f, 0.0f, 0.0f);
	Positions[V1] = Offset + FVector3f(10.0f, 0.0f, 0.0f);
	Positions[V2] = Offset + FVector3f(0.0f, 10.0f, 0.0f);

	const FVertexInstanceID I0 = Mesh.CreateVertexInstance(V0);
	const FVertexInstanceID I1 = Mesh.CreateVertexInstance(V1);
	const FVertexInstanceID I2 = Mesh.CreateVertexInstance(V2);
	const FVertexInstanceID Instances[3] = { I0, I1, I2 };
	const FVector2f TriangleUVs[3] = { FVector2f(0.0f, 0.0f), FVector2f(1.0f, 0.0f), FVector2f(0.0f, 1.0f) };
	for (int32 Corner = 0; Corner < 3; ++Corner)
	{
		Normals[Instances[Corner]] = FVector3f(0.0f, 0.0f, 1.0f);
		UVs.Set(Instances[Corner], 0, TriangleUVs[Corner]);
	}

	Mesh.CreateTriangle(PolygonGroupID, Instances);
}

// Builds a Nanite-enabled mesh of NaniteMaterialLimit disconnected triangles (one section each, so within the
// section-count guard) whose materials span slots 1..NaniteMaterialLimit. All triangles land in a single cluster
// that then references material index NaniteMaterialLimit, overflowing the per-cluster material table.
UStaticMesh* CreateSingleClusterOverLimitNaniteStaticMesh()
{
	UStaticMesh* StaticMesh = NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient);
	FMeshNaniteSettings NaniteSettings;
	NaniteSettings.bEnabled = true;
	StaticMesh->SetNaniteSettings(NaniteSettings);

	UMaterialInterface* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);

	// One extra slot so sections map to slots 1..NaniteMaterialLimit (slot 0 unused), putting the over-limit index last.
	const int32 NumSlots = NaniteMaterialLimit + 1;
	TArray<FName> SlotNames;
	SlotNames.Reserve(NumSlots);
	for (int32 SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
	{
		const FName SlotName(*FString::Printf(TEXT("TestMaterialSlot_%d"), SlotIndex));
		SlotNames.Add(SlotName);
		StaticMesh->GetStaticMaterials().Add(FStaticMaterial(DefaultMaterial, SlotName, SlotName));
	}

	StaticMesh->SetNumSourceModels(1);
	FMeshDescription* MeshDescription = StaticMesh->CreateMeshDescription(0);
	FStaticMeshAttributes Attributes(*MeshDescription);
	Attributes.Register();

	for (int32 SectionIndex = 0; SectionIndex < NaniteMaterialLimit; ++SectionIndex)
	{
		// Section i uses material slot i + 1, so the highest section references slot NaniteMaterialLimit.
		const FPolygonGroupID PolygonGroupID = MeshDescription->CreatePolygonGroup();
		Attributes.GetPolygonGroupMaterialSlotNames().Set(PolygonGroupID, SlotNames[SectionIndex + 1]);
		AppendDisconnectedTriangle(*MeshDescription, Attributes, PolygonGroupID, SectionIndex);
	}

	UStaticMesh::FCommitMeshDescriptionParams Params;
	Params.bUseHashAsGuid = true;
	Params.bMarkPackageDirty = false;
	StaticMesh->CommitMeshDescription(0, Params);

	// The Nanite build reads section material indices from the section info map, so pin them to slots 1..NaniteMaterialLimit.
	for (int32 SectionIndex = 0; SectionIndex < NaniteMaterialLimit; ++SectionIndex)
	{
		StaticMesh->GetSectionInfoMap().Set(0, SectionIndex, FMeshSectionInfo(SectionIndex + 1));
	}

	return StaticMesh;
}

} // namespace UE::Private::StaticMeshNaniteMaterialLimitTest

bool FStaticMeshNaniteSingleClusterMaterialLimitTest::RunTest(const FString& Parameters)
{
	using namespace UE::Private::StaticMeshNaniteMaterialLimitTest;

	UStaticMesh* StaticMesh = CreateSingleClusterOverLimitNaniteStaticMesh();
	StaticMesh->BuildCacheAutomationTestGuid = FGuid::NewGuid();

	// Must complete without crashing in FCluster::BuildMaterialRanges.
	BuildAndWait(StaticMesh);

	UTEST_NOT_NULL(TEXT("RenderData after building a single-cluster over-limit Nanite mesh"), StaticMesh->GetRenderData());
	UTEST_TRUE(TEXT("Non-Nanite render data was built so the mesh stays renderable"), StaticMesh->GetRenderData()->LODResources.Num() > 0);
	UTEST_FALSE(TEXT("Nanite build was skipped for the over-limit mesh"), StaticMesh->GetRenderData()->HasValidNaniteData());
	UTEST_EQUAL(TEXT("Material slots are preserved after the failed Nanite build"), StaticMesh->GetStaticMaterials().Num(), NaniteMaterialLimit + 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
