// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Verifies that requesting MikkTSpace tangents for a mesh description with no UV channels falls back to per-vertex
 * tangent averaging instead of asserting, that calling ComputeMikktTangents directly on such a mesh warns
 * and leaves the tangents untouched, and that a mesh with a UV channel still yields a valid tangent basis.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshOperationsTangentsWithoutUVsTest, "Editor.Meshes.MeshDescription.TangentsWithoutUVs",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace UE::Private::StaticMeshOperationsTangentTests
{

// Two triangles sharing an edge in one polygon group, with positions and UV0 set and normals/tangents left zero so the
// tangent computation has to produce them. Without bWithUVChannel the UV attribute stays registered with no channels.
void BuildQuad(FMeshDescription& Mesh, bool bWithUVChannel)
{
	FStaticMeshAttributes Attributes(Mesh);
	Attributes.Register();

	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	UVs.SetNumChannels(1);

	const FVector3f Corners[4] = {
		FVector3f(0.0f, 0.0f, 0.0f),
		FVector3f(10.0f, 0.0f, 0.0f),
		FVector3f(0.0f, 10.0f, 0.0f),
		FVector3f(10.0f, 10.0f, 0.0f)
	};
	FVertexID Vertices[4];
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Vertices[Index] = Mesh.CreateVertex();
		Positions[Vertices[Index]] = Corners[Index];
	}

	const FPolygonGroupID PolygonGroupID = Mesh.CreatePolygonGroup();
	constexpr int32 TriangleCorners[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
	for (const int32 (&Triangle)[3] : TriangleCorners)
	{
		FVertexInstanceID Instances[3];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Instances[Corner] = Mesh.CreateVertexInstance(Vertices[Triangle[Corner]]);
			UVs.Set(Instances[Corner], 0, FVector2f(Corners[Triangle[Corner]].X, Corners[Triangle[Corner]].Y) / 10.0f);
		}
		Mesh.CreateTriangle(PolygonGroupID, Instances);
	}

	if (!bWithUVChannel)
	{
		UVs.SetNumChannels(0);
	}
}

// Every vertex instance must end up with a unit normal, a unit tangent orthogonal to it and a binormal sign of +/-1.
void CheckTangentBasis(FAutomationTestBase& Test, const FMeshDescription& Mesh, const TCHAR* Context)
{
	FStaticMeshConstAttributes Attributes(Mesh);
	const TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	const TVertexInstanceAttributesConstRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	const TVertexInstanceAttributesConstRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();

	for (const FVertexInstanceID VertexInstanceID : Mesh.VertexInstances().GetElementIDs())
	{
		const FVector3f Normal = Normals[VertexInstanceID];
		const FVector3f Tangent = Tangents[VertexInstanceID];
		const int32 Index = VertexInstanceID.GetValue();
		Test.TestNearlyEqual(FString::Printf(TEXT("%s: vertex instance %d normal length"), Context, Index), Normal.Length(), 1.0f);
		Test.TestNearlyEqual(FString::Printf(TEXT("%s: vertex instance %d tangent length"), Context, Index), Tangent.Length(), 1.0f);
		Test.TestNearlyEqual(FString::Printf(TEXT("%s: vertex instance %d normal . tangent"), Context, Index),
			FVector3f::DotProduct(Normal, Tangent), 0.0f, 0.01f);
		Test.TestNearlyEqual(FString::Printf(TEXT("%s: vertex instance %d |binormal sign|"), Context, Index),
			FMath::Abs(BinormalSigns[VertexInstanceID]), 1.0f);
	}
}

} // namespace UE::Private::StaticMeshOperationsTangentTests

bool FStaticMeshOperationsTangentsWithoutUVsTest::RunTest(const FString& Parameters)
{
	using namespace UE::Private::StaticMeshOperationsTangentTests;

	constexpr EComputeNTBsFlags MikkTOptions =
		EComputeNTBsFlags::Normals | EComputeNTBsFlags::Tangents | EComputeNTBsFlags::UseMikkTSpace | EComputeNTBsFlags::BlendOverlappingNormals;

	// The static mesh builder's path: MikkTSpace requested for a mesh without UV channels must not assert and must still produce a full tangent basis through
	// the averaging fallback.
	{
		FMeshDescription Mesh;
		BuildQuad(Mesh, /*bWithUVChannel*/ false);
		UTEST_EQUAL(TEXT("UV attribute has no channels"), FStaticMeshConstAttributes(Mesh).GetVertexInstanceUVs().GetNumChannels(), 0);

		// Expected messages are keyed on the pattern string, so the two entry points need distinct patterns.
		AddExpectedMessage(TEXT("ComputeTangentsAndNormals: mesh description has no UV channels"), ELogVerbosity::Display);
		AddExpectedMessage(TEXT("ComputeMikktTangents: mesh description has no UV channels"), ELogVerbosity::Warning);

		FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Mesh, 0.0f);
		FStaticMeshOperations::ComputeTangentsAndNormals(Mesh, MikkTOptions);
		CheckTangentBasis(*this, Mesh, TEXT("No UV channels, MikkTSpace requested"));

		// Direct callers of ComputeMikktTangents get a warning and unchanged tangents instead of an assert.
		const TArrayView<const FVector3f> Tangents = FStaticMeshConstAttributes(Mesh).GetVertexInstanceTangents().GetRawArray();
		const TArray<FVector3f> TangentsBefore(Tangents.GetData(), Tangents.Num());
		FStaticMeshOperations::ComputeMikktTangents(Mesh, false);
		TestTrue(TEXT("ComputeMikktTangents left tangents unchanged"), TangentsBefore == TArray<FVector3f>(Tangents.GetData(), Tangents.Num()));
	}

	// Control: with a UV channel present the MikkTSpace path is taken (no fallback message, exact count above) and yields a full tangent basis.
	{
		FMeshDescription Mesh;
		BuildQuad(Mesh, /*bWithUVChannel*/ true);
		FStaticMeshOperations::ComputeTriangleTangentsAndNormals(Mesh, 0.0f);
		FStaticMeshOperations::ComputeTangentsAndNormals(Mesh, MikkTOptions);
		CheckTangentBasis(*this, Mesh, TEXT("One UV channel, MikkTSpace requested"));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
