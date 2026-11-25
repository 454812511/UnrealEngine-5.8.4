// Copyright Epic Games, Inc. All Rights Reserved.

// All SQLite-heavy implementation for SODB/PSDB read and write operations.
// This includes the full FSqlSODBFile implementation and MergePSDBs.

#include "ASDSqlUtils.h"
#include "ASDD3D12Utils.h"

#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformAtomics.h"
#include "HAL/PlatformTime.h"
#include "Misc/SecureHash.h"

#include "IncludeSQLite.h"

namespace ASDTool
{

//--------------------------------------------------------------------------------------------------
// Helpers
//--------------------------------------------------------------------------------------------------

static void HashToKey(const uint8* Data, int64 Size, uint8 OutKey[16])
{
	FMD5 MD5;
	MD5.Update(Data, Size);
	MD5.Final(OutKey);
}

/** Hash a plain POD struct to a 16-byte key. */
template<typename T>
static void HashStructToKey(const T& Value, uint8 OutKey[16])
{
	HashToKey(reinterpret_cast<const uint8*>(&Value), sizeof(T), OutKey);
}

/** Insert a shader bytecode row and return the 16-byte MD5 key. Returns false if DXBC is empty. */
static bool InsertShaderBytecode(sqlite3_stmt* Stmt, const TArray<uint8>& DXBCData, const char* TypeStr, uint8 OutKey[16])
{
	if (DXBCData.IsEmpty())
	{
		return false;
	}
	HashToKey(DXBCData.GetData(), DXBCData.Num(), OutKey);
	sqlite3_bind_blob(Stmt, 1, OutKey, 16, SQLITE_STATIC);
	sqlite3_bind_text(Stmt, 2, TypeStr, -1, SQLITE_STATIC);
	sqlite3_bind_blob(Stmt, 3, DXBCData.GetData(), (int)DXBCData.Num(), SQLITE_STATIC);
	const int32 StepResult = sqlite3_step(Stmt);
	sqlite3_reset(Stmt);
	// INSERT OR IGNORE -- SQLITE_DONE = inserted, SQLITE_CONSTRAINT = duplicate (both fine)
	if (StepResult != SQLITE_DONE && StepResult != SQLITE_CONSTRAINT)
	{
		UE_LOG(LogASDTool, Warning, TEXT("InsertShaderBytecode: unexpected sqlite3_step result %d for type %s"), StepResult, UTF8_TO_TCHAR(TypeStr));
	}
	return true;
}

/** Execute an INSERT stmt, return true if SQLITE_DONE or SQLITE_CONSTRAINT (dedup). */
static bool ExecInsert(sqlite3_stmt* Stmt, const char* ContextName)
{
	const int32 Result = sqlite3_step(Stmt);
	sqlite3_reset(Stmt);
	if (Result != SQLITE_DONE && Result != SQLITE_CONSTRAINT)
	{
		UE_LOG(LogASDTool, Warning, TEXT("SQLite INSERT failed (%d) for %s"), Result, UTF8_TO_TCHAR(ContextName));
		return false;
	}
	return true;
}

//--------------------------------------------------------------------------------------------------
// InsertGraphicsState
// Inserts all graphics PSO state into the normalized sub-tables.
// Returns 16-byte MD5 keys for each FK that pipeline_states needs.
//--------------------------------------------------------------------------------------------------

FSqlSODBFile::FGraphicsStateKeys FSqlSODBFile::InsertGraphicsState(
	const FD3D12GraphicsPSOState& State,
	const TArray<D3D12_INPUT_ELEMENT_DESC>& InputElements)
{
	FGraphicsStateKeys Keys;

	// ---- depth_stencil_op_descs (FrontFace + BackFace) ----
	// Row layout: (Key, StencilFailOp, StencilDepthFailOp, StencilPassOp, StencilFunc, StencilReadMask, StencilWriteMask)
	// Key = MD5 of the 6 integer fields packed as a struct
	struct FStencilOpRow
	{
		int32 StencilFailOp;
		int32 StencilDepthFailOp;
		int32 StencilPassOp;
		int32 StencilFunc;
		int32 StencilReadMask;
		int32 StencilWriteMask;
	};

	auto InsertStencilOp = [&](const D3D12_DEPTH_STENCILOP_DESC& Op, uint8 OutKey[16])
	{
		FStencilOpRow Row;
		Row.StencilFailOp      = (int32)Op.StencilFailOp;
		Row.StencilDepthFailOp = (int32)Op.StencilDepthFailOp;
		Row.StencilPassOp      = (int32)Op.StencilPassOp;
		Row.StencilFunc        = (int32)Op.StencilFunc;
		Row.StencilReadMask    = (int32)State.DepthStencilDesc.StencilReadMask;
		Row.StencilWriteMask   = (int32)State.DepthStencilDesc.StencilWriteMask;
		HashStructToKey(Row, OutKey);

		sqlite3_bind_blob(StmtDepthStencilOp, 1, OutKey, 16, SQLITE_STATIC);
		sqlite3_bind_int(StmtDepthStencilOp,  2, Row.StencilFailOp);
		sqlite3_bind_int(StmtDepthStencilOp,  3, Row.StencilDepthFailOp);
		sqlite3_bind_int(StmtDepthStencilOp,  4, Row.StencilPassOp);
		sqlite3_bind_int(StmtDepthStencilOp,  5, Row.StencilFunc);
		sqlite3_bind_int(StmtDepthStencilOp,  6, Row.StencilReadMask);
		sqlite3_bind_int(StmtDepthStencilOp,  7, Row.StencilWriteMask);
		ExecInsert(StmtDepthStencilOp, "depth_stencil_op_descs");
	};

	uint8 FrontFaceKey[16] = {};
	uint8 BackFaceKey[16]  = {};
	InsertStencilOp(State.DepthStencilDesc.FrontFace, FrontFaceKey);
	InsertStencilOp(State.DepthStencilDesc.BackFace,  BackFaceKey);

	// ---- depth_stencil_descs ----
	// Key = MD5 of {DepthEnable, DepthWriteMask, DepthFunc, StencilEnable, FrontFaceKey, BackFaceKey, DepthBoundsTestEnable}
	{
		struct FDSRow
		{
			int32 DepthEnable;
			int32 DepthWriteMask;
			int32 DepthFunc;
			int32 StencilEnable;
			uint8 FrontFaceKey[16];
			uint8 BackFaceKey[16];
			int32 DepthBoundsTestEnable;
		};
		FDSRow Row;
		Row.DepthEnable            = State.DepthStencilDesc.DepthEnable        ? 1 : 0;
		Row.DepthWriteMask         = (int32)State.DepthStencilDesc.DepthWriteMask;
		Row.DepthFunc              = (int32)State.DepthStencilDesc.DepthFunc;
		Row.StencilEnable          = State.DepthStencilDesc.StencilEnable       ? 1 : 0;
		FMemory::Memcpy(Row.FrontFaceKey, FrontFaceKey, 16);
		FMemory::Memcpy(Row.BackFaceKey,  BackFaceKey,  16);
		Row.DepthBoundsTestEnable  = State.DepthStencilDesc.DepthBoundsTestEnable ? 1 : 0;
		HashStructToKey(Row, Keys.DepthStencilKey);

		sqlite3_bind_blob(StmtDepthStencil, 1, Keys.DepthStencilKey, 16, SQLITE_STATIC);
		sqlite3_bind_int(StmtDepthStencil,  2, Row.DepthEnable);
		sqlite3_bind_int(StmtDepthStencil,  3, Row.DepthWriteMask);
		sqlite3_bind_int(StmtDepthStencil,  4, Row.DepthFunc);
		sqlite3_bind_int(StmtDepthStencil,  5, Row.StencilEnable);
		sqlite3_bind_blob(StmtDepthStencil, 6, FrontFaceKey, 16, SQLITE_STATIC);
		sqlite3_bind_blob(StmtDepthStencil, 7, BackFaceKey,  16, SQLITE_STATIC);
		sqlite3_bind_int(StmtDepthStencil,  8, Row.DepthBoundsTestEnable);
		ExecInsert(StmtDepthStencil, "depth_stencil_descs");
	}

	// ---- render_target_formats ----
	// Key = MD5 of {RTFormat0..7, NumRenderTargets}
	{
		struct FRTFRow
		{
			int32 RTFormat[8];
			int32 NumRenderTargets;
		};
		FRTFRow Row;
		FMemory::Memzero(Row);
		Row.NumRenderTargets = (int32)State.RTFormats.NumRenderTargets;
		for (uint32 i = 0; i < 8; ++i)
		{
			Row.RTFormat[i] = (int32)State.RTFormats.RTFormats[i];
		}
		HashStructToKey(Row, Keys.RTFormatsKey);

		sqlite3_bind_blob(StmtRTFormats, 1, Keys.RTFormatsKey, 16, SQLITE_STATIC);
		for (int32 i = 0; i < 8; ++i)
		{
			sqlite3_bind_int(StmtRTFormats, 2 + i, Row.RTFormat[i]);
		}
		sqlite3_bind_int(StmtRTFormats, 10, Row.NumRenderTargets);
		ExecInsert(StmtRTFormats, "render_target_formats");
	}

	// ---- render_target_blend_descs (one row per RT slot) ----
	// Key = MD5 of the 10 integer fields
	uint8 RTBlendKeys[8][16] = {};
	{
		struct FRTBlendRow
		{
			int32 BlendEnable;
			int32 LogicOpEnable;
			int32 SrcBlend;
			int32 DestBlend;
			int32 BlendOp;
			int32 SrcBlendAlpha;
			int32 DestBlendAlpha;
			int32 BlendOpAlpha;
			int32 LogicOp;
			int32 RenderTargetWriteMask;
		};

		for (int32 RTIdx = 0; RTIdx < 8; ++RTIdx)
		{
			const D3D12_RENDER_TARGET_BLEND_DESC& RT = State.BlendDesc.RenderTarget[RTIdx];
			FRTBlendRow Row;
			Row.BlendEnable           = RT.BlendEnable          ? 1 : 0;
			Row.LogicOpEnable         = RT.LogicOpEnable         ? 1 : 0;
			Row.SrcBlend              = (int32)RT.SrcBlend;
			Row.DestBlend             = (int32)RT.DestBlend;
			Row.BlendOp               = (int32)RT.BlendOp;
			Row.SrcBlendAlpha         = (int32)RT.SrcBlendAlpha;
			Row.DestBlendAlpha        = (int32)RT.DestBlendAlpha;
			Row.BlendOpAlpha          = (int32)RT.BlendOpAlpha;
			Row.LogicOp               = (int32)RT.LogicOp;
			Row.RenderTargetWriteMask = (int32)RT.RenderTargetWriteMask;
			HashStructToKey(Row, RTBlendKeys[RTIdx]);

			sqlite3_bind_blob(StmtRTBlend, 1,  RTBlendKeys[RTIdx], 16, SQLITE_STATIC);
			sqlite3_bind_int(StmtRTBlend,  2,  Row.BlendEnable);
			sqlite3_bind_int(StmtRTBlend,  3,  Row.LogicOpEnable);
			sqlite3_bind_int(StmtRTBlend,  4,  Row.SrcBlend);
			sqlite3_bind_int(StmtRTBlend,  5,  Row.DestBlend);
			sqlite3_bind_int(StmtRTBlend,  6,  Row.BlendOp);
			sqlite3_bind_int(StmtRTBlend,  7,  Row.SrcBlendAlpha);
			sqlite3_bind_int(StmtRTBlend,  8,  Row.DestBlendAlpha);
			sqlite3_bind_int(StmtRTBlend,  9,  Row.BlendOpAlpha);
			sqlite3_bind_int(StmtRTBlend,  10, Row.LogicOp);
			sqlite3_bind_int(StmtRTBlend,  11, Row.RenderTargetWriteMask);
			ExecInsert(StmtRTBlend, "render_target_blend_descs");
		}
	}

	// ---- blend_descs ----
	// Key = MD5 of {AlphaToCoverageEnable, IndependentBlendEnable, RTBlendKey0..7}
	{
		struct FBlendRow
		{
			int32 AlphaToCoverageEnable;
			int32 IndependentBlendEnable;
			uint8 RTBlendKey[8][16];
		};
		FBlendRow Row;
		Row.AlphaToCoverageEnable  = State.BlendDesc.AlphaToCoverageEnable  ? 1 : 0;
		Row.IndependentBlendEnable = State.BlendDesc.IndependentBlendEnable ? 1 : 0;
		FMemory::Memcpy(Row.RTBlendKey, RTBlendKeys, sizeof(RTBlendKeys));
		HashStructToKey(Row, Keys.BlendKey);

		sqlite3_bind_blob(StmtBlend, 1,  Keys.BlendKey, 16, SQLITE_STATIC);
		sqlite3_bind_int(StmtBlend,  2,  Row.AlphaToCoverageEnable);
		sqlite3_bind_int(StmtBlend,  3,  Row.IndependentBlendEnable);
		for (int32 i = 0; i < 8; ++i)
		{
			sqlite3_bind_blob(StmtBlend, 4 + i, RTBlendKeys[i], 16, SQLITE_STATIC);
		}
		ExecInsert(StmtBlend, "blend_descs");
	}

	// ---- rasterizer_descs ----
	// DepthBias stored as REAL -- convert back from integer: float = DepthBias / (1<<24)
	// Key = MD5 of the packed float/int fields
	{
		struct FRastRow
		{
			int32 FillMode;
			int32 CullMode;
			int32 FrontCounterClockwise;
			float DepthBias;             // REAL column
			float DepthBiasClamp;        // REAL column (always 0 in UE)
			float SlopeScaledDepthBias;  // REAL column
			int32 DepthClipEnable;
			int32 LineRasterizationMode; // always 0
			int32 ForcedSampleCount;     // always 0
			int32 ConservativeRaster;    // always 0
		};
		FRastRow Row;
		Row.FillMode              = (int32)State.RasterizerDesc.FillMode;
		Row.CullMode              = (int32)State.RasterizerDesc.CullMode;
		Row.FrontCounterClockwise = State.RasterizerDesc.FrontCounterClockwise ? 1 : 0;
		Row.DepthBias             = (float)State.RasterizerDesc.DepthBias / (float)(1 << 24);
		Row.DepthBiasClamp        = State.RasterizerDesc.DepthBiasClamp;
		Row.SlopeScaledDepthBias  = State.RasterizerDesc.SlopeScaledDepthBias;
		Row.DepthClipEnable       = State.RasterizerDesc.DepthClipEnable ? 1 : 0;
		Row.LineRasterizationMode = 0;
		Row.ForcedSampleCount     = 0;
		Row.ConservativeRaster    = 0;
		HashStructToKey(Row, Keys.RasterizerKey);

		sqlite3_bind_blob(StmtRasterizer, 1,  Keys.RasterizerKey, 16, SQLITE_STATIC);
		sqlite3_bind_int(StmtRasterizer,  2,  Row.FillMode);
		sqlite3_bind_int(StmtRasterizer,  3,  Row.CullMode);
		sqlite3_bind_int(StmtRasterizer,  4,  Row.FrontCounterClockwise);
		sqlite3_bind_double(StmtRasterizer, 5, (double)Row.DepthBias);
		sqlite3_bind_double(StmtRasterizer, 6, (double)Row.DepthBiasClamp);
		sqlite3_bind_double(StmtRasterizer, 7, (double)Row.SlopeScaledDepthBias);
		sqlite3_bind_int(StmtRasterizer,  8,  Row.DepthClipEnable);
		sqlite3_bind_int(StmtRasterizer,  9,  Row.LineRasterizationMode);
		sqlite3_bind_int(StmtRasterizer,  10, Row.ForcedSampleCount);
		sqlite3_bind_int(StmtRasterizer,  11, Row.ConservativeRaster);
		ExecInsert(StmtRasterizer, "rasterizer_descs");
	}

	// ---- input_element_descs + input_layout_to_input_element_associations ----
	// Only for VS pipelines (InputElements non-empty)
	if (!InputElements.IsEmpty())
	{
		// Hash each input element row, then hash the concatenated key array to get the layout key
		TArray<uint8> ElementKeys;
		ElementKeys.Reserve(InputElements.Num() * 16);

		for (const D3D12_INPUT_ELEMENT_DESC& Elem : InputElements)
		{
			// Row: SemanticName, SemanticIndex, Format, InputSlot, AlignedByteOffset, InputSlotClass, InstanceDataStepRate
			// Key = MD5 of integer fields (SemanticName is always "ATTRIBUTE" -- not hashed, consistent)
			struct FElemRow
			{
				int32 SemanticIndex;
				int32 Format;
				int32 InputSlot;
				int32 AlignedByteOffset;
				int32 InputSlotClass;
				int32 InstanceDataStepRate;
			};
			FElemRow Row;
			Row.SemanticIndex        = (int32)Elem.SemanticIndex;
			Row.Format               = (int32)Elem.Format;
			Row.InputSlot            = (int32)Elem.InputSlot;
			Row.AlignedByteOffset    = (int32)Elem.AlignedByteOffset;
			Row.InputSlotClass       = (int32)Elem.InputSlotClass;
			Row.InstanceDataStepRate = (int32)Elem.InstanceDataStepRate;

			uint8 ElemKey[16] = {};
			HashStructToKey(Row, ElemKey);

			// Insert element row
			sqlite3_bind_blob(StmtInputElement, 1, ElemKey, 16, SQLITE_STATIC);
			sqlite3_bind_text(StmtInputElement, 2, "ATTRIBUTE", -1, SQLITE_STATIC);
			sqlite3_bind_int(StmtInputElement,  3, Row.SemanticIndex);
			sqlite3_bind_int(StmtInputElement,  4, Row.Format);
			sqlite3_bind_int(StmtInputElement,  5, Row.InputSlot);
			sqlite3_bind_int(StmtInputElement,  6, Row.AlignedByteOffset);
			sqlite3_bind_int(StmtInputElement,  7, Row.InputSlotClass);
			sqlite3_bind_int(StmtInputElement,  8, Row.InstanceDataStepRate);
			ExecInsert(StmtInputElement, "input_element_descs");

			ElementKeys.Append(ElemKey, 16);
		}

		// InputLayoutKey = MD5 of concatenated element keys
		HashToKey(ElementKeys.GetData(), ElementKeys.Num(), Keys.InputLayoutKey);
		Keys.bHasInputLayout = true;

		// Insert one association row per element
		for (int32 ElemIdx = 0; ElemIdx < InputElements.Num(); ++ElemIdx)
		{
			const uint8* ElemKey = ElementKeys.GetData() + ElemIdx * 16;
			sqlite3_bind_blob(StmtInputLayoutAssoc, 1, Keys.InputLayoutKey, 16, SQLITE_STATIC);
			sqlite3_bind_blob(StmtInputLayoutAssoc, 2, ElemKey, 16, SQLITE_STATIC);
			ExecInsert(StmtInputLayoutAssoc, "input_layout_to_input_element_associations");
		}
	}

	return Keys;
}

//--------------------------------------------------------------------------------------------------
// FSqlSODBFile -- method implementations
//--------------------------------------------------------------------------------------------------

FSqlSODBFile::~FSqlSODBFile()
{
	Close();
}

void FSqlSODBFile::ExecSQL(const char* SQL)
{
	char* ErrMsg = nullptr;
	int32 Result = sqlite3_exec(DB, SQL, nullptr, nullptr, &ErrMsg);
	if (Result != SQLITE_OK)
	{
		UE_LOG(LogASDTool, Error, TEXT("SQLite error: %s (SQL: %s)"), UTF8_TO_TCHAR(ErrMsg), UTF8_TO_TCHAR(SQL));
		sqlite3_free(ErrMsg);
	}
}

void FSqlSODBFile::PrepareSQLite(const char* SQL, sqlite3_stmt** OutStmt)
{
	int32 Result = sqlite3_prepare_v2(DB, SQL, -1, OutStmt, nullptr);
	if (Result != SQLITE_OK)
	{
		UE_LOG(LogASDTool, Error, TEXT("Failed to prepare SQL: %s (%s)"), UTF8_TO_TCHAR(SQL), UTF8_TO_TCHAR(sqlite3_errmsg(DB)));
		*OutStmt = nullptr;
	}
}

void FSqlSODBFile::CreateSchema()
{
	// Schema DDL must match D3D12StateObjectDatabase EXACTLY (including whitespace)
	// because D3D12StateObjectCompiler parses sqlite_master DDL literally.
	// Reference: https://microsoft.github.io/DirectX-Specs/d3d/StateObjectDatabase.html (SQL Database Schema section)
	ExecSQL("CREATE TABLE root_signatures (Key BLOB NOT NULL PRIMARY KEY , value BLOB NOT NULL )");
	ExecSQL("CREATE TABLE input_element_descs (Key BLOB NOT NULL PRIMARY KEY , SemanticName TEXT NOT NULL ,SemanticIndex INTEGER NOT NULL ,Format INTEGER NOT NULL ,InputSlot INTEGER NOT NULL ,AlignedByteOffset INTEGER NOT NULL ,InputSlotClass INTEGER NOT NULL ,InstanceDataStepRate INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE state_objects (Key BLOB NOT NULL PRIMARY KEY , Type INTEGER  ,NodeMask INTEGER  ,Flags INTEGER  ,AddToStateObjectParent BLOB  ,FOREIGN KEY(AddToStateObjectParent) REFERENCES state_objects(Key))");
	ExecSQL("CREATE TABLE shader_bytecode (Key BLOB NOT NULL PRIMARY KEY , Type TEXT  ,Bytecode BLOB  )");
	ExecSQL("CREATE TABLE exports (Key BLOB NOT NULL PRIMARY KEY , Name TEXT NOT NULL ,ExportToRename TEXT  ,Flags INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE depth_stencil_op_descs (Key BLOB NOT NULL PRIMARY KEY , StencilFailOp INTEGER NOT NULL ,StencilDepthFailOp INTEGER NOT NULL ,StencilPassOp INTEGER NOT NULL ,StencilFunc INTEGER NOT NULL ,StencilReadMask INTEGER NOT NULL ,StencilWriteMask INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE depth_stencil_descs (Key BLOB NOT NULL PRIMARY KEY , DepthEnable INTEGER NOT NULL ,DepthWriteMask INTEGER NOT NULL ,DepthFunc INTEGER NOT NULL ,StencilEnable INTEGER NOT NULL ,FrontFace BLOB NOT NULL ,BackFace BLOB NOT NULL ,DepthBoundsTestEnable INTEGER NOT NULL ,FOREIGN KEY(FrontFace) REFERENCES depth_stencil_op_descs(Key),FOREIGN KEY(BackFace) REFERENCES depth_stencil_op_descs(Key))");
	ExecSQL("CREATE TABLE render_target_formats (Key BLOB NOT NULL PRIMARY KEY , RTFormat0 INTEGER NOT NULL ,RTFormat1 INTEGER NOT NULL ,RTFormat2 INTEGER NOT NULL ,RTFormat3 INTEGER NOT NULL ,RTFormat4 INTEGER NOT NULL ,RTFormat5 INTEGER NOT NULL ,RTFormat6 INTEGER NOT NULL ,RTFormat7 INTEGER NOT NULL ,NumRenderTargets INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE render_target_blend_descs (Key BLOB NOT NULL PRIMARY KEY , BlendEnable INTEGER NOT NULL ,LogicOpEnable INTEGER NOT NULL ,SrcBlend INTEGER NOT NULL ,DestBlend INTEGER NOT NULL ,BlendOp INTEGER NOT NULL ,SrcBlendAlpha INTEGER NOT NULL ,DestBlendAlpha INTEGER NOT NULL ,BlendOpAlpha INTEGER NOT NULL ,LogicOp INTEGER NOT NULL ,RenderTargetWriteMask INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE blend_descs (Key BLOB NOT NULL PRIMARY KEY , AlphaToCoverageEnable INTEGER NOT NULL ,IndependentBlendEnable INTEGER NOT NULL ,RenderTarget0 BLOB  ,RenderTarget1 BLOB  ,RenderTarget2 BLOB  ,RenderTarget3 BLOB  ,RenderTarget4 BLOB  ,RenderTarget5 BLOB  ,RenderTarget6 BLOB  ,RenderTarget7 BLOB  ,FOREIGN KEY(RenderTarget0) REFERENCES render_target_blend_descs(Key),FOREIGN KEY(RenderTarget1) REFERENCES render_target_blend_descs(Key),FOREIGN KEY(RenderTarget2) REFERENCES render_target_blend_descs(Key),FOREIGN KEY(RenderTarget3) REFERENCES render_target_blend_descs(Key),FOREIGN KEY(RenderTarget4) REFERENCES render_target_blend_descs(Key),FOREIGN KEY(RenderTarget5) REFERENCES render_target_blend_descs(Key),FOREIGN KEY(RenderTarget6) REFERENCES render_target_blend_descs(Key),FOREIGN KEY(RenderTarget7) REFERENCES render_target_blend_descs(Key))");
	ExecSQL("CREATE TABLE rasterizer_descs (Key BLOB NOT NULL PRIMARY KEY , FillMode INTEGER NOT NULL ,CullMode INTEGER NOT NULL ,FrontCounterClockwise INTEGER NOT NULL ,DepthBias REAL NOT NULL ,DepthBiasClamp REAL NOT NULL ,SlopeScaledDepthBias REAL NOT NULL ,DepthClipEnable INTEGER NOT NULL ,LineRasterizationMode INTEGER NOT NULL ,ForcedSampleCount INTEGER NOT NULL ,ConservativeRaster INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE view_instancing_descs (Key BLOB NOT NULL PRIMARY KEY , ViewInstanceCount INTEGER NOT NULL ,RenderFlags INTEGER NOT NULL ,ViewportArrayIndex0 INTEGER  ,RenderTargetArrayIndex0 INTEGER  ,ViewportArrayIndex1 INTEGER  ,RenderTargetArrayIndex1 INTEGER  ,ViewportArrayIndex2 INTEGER  ,RenderTargetArrayIndex2 INTEGER  ,ViewportArrayIndex3 INTEGER  ,RenderTargetArrayIndex3 INTEGER  )");
	ExecSQL("CREATE TABLE so_declarations (Key BLOB NOT NULL PRIMARY KEY , Stream INTEGER NOT NULL ,SemanticName TEXT NOT NULL ,SemanticIndex INTEGER NOT NULL ,StartComponent INTEGER NOT NULL ,ComponentCount INTEGER NOT NULL ,OutputSlot INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE stream_out_descs (Key BLOB NOT NULL PRIMARY KEY , BufferStride0 INTEGER NOT NULL ,BufferStride1 INTEGER NOT NULL ,BufferStride2 INTEGER NOT NULL ,BufferStride3 INTEGER NOT NULL ,NumStrides INTEGER NOT NULL ,RasterizedStream INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE rt_hit_groups (Key BLOB NOT NULL PRIMARY KEY , HitGroupExport TEXT NOT NULL ,Type INTEGER NOT NULL ,AnyHitShaderImport TEXT  ,ClosestHitShaderImport TEXT  ,IntersectionShaderImport TEXT  )");
	ExecSQL("CREATE TABLE rt_shader_config (Key BLOB NOT NULL PRIMARY KEY , MaxPayloadSizeInBytes INTEGER NOT NULL ,MaxAttributeSizeInBytes INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE rt_pipeline_config (Key BLOB NOT NULL PRIMARY KEY , MaxTraceRecursionDepth INTEGER NOT NULL ,Flags INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE dxil_subobject_to_exports_associations (Key BLOB NOT NULL PRIMARY KEY , SubobjectToAssociate TEXT NOT NULL )");
	ExecSQL("CREATE TABLE subobject_to_exports_associations (Key BLOB NOT NULL PRIMARY KEY , SubobjectType INTEGER NOT NULL ,SubobjectKey BLOB NOT NULL )");
	ExecSQL("CREATE TABLE generic_programs (Key BLOB NOT NULL PRIMARY KEY , ProgramName TEXT  ,InputLayout BLOB  ,DepthStencilDesc BLOB  ,RenderTargetFormats BLOB  ,BlendDesc BLOB  ,RasterizerDesc BLOB  ,ViewInstancingDesc BLOB  ,StreamOutDesc BLOB  ,SampleDesc_Count INTEGER  ,SampleDesc_Quality INTEGER  ,SampleMask INTEGER  ,IBStripCutValue INTEGER  ,PrimitiveTopology INTEGER  ,DSVFormat INTEGER  ,NodeMask INTEGER  ,Flags INTEGER  ,FOREIGN KEY(DepthStencilDesc) REFERENCES depth_stencil_descs(Key),FOREIGN KEY(RenderTargetFormats) REFERENCES render_target_formats(Key),FOREIGN KEY(BlendDesc) REFERENCES blend_descs(Key),FOREIGN KEY(RasterizerDesc) REFERENCES rasterizer_descs(Key),FOREIGN KEY(ViewInstancingDesc) REFERENCES view_instancing_descs(Key),FOREIGN KEY(StreamOutDesc) REFERENCES stream_out_descs(Key))");
	ExecSQL("CREATE TABLE pipeline_states (Key BLOB NOT NULL PRIMARY KEY , RootSignature BLOB  ,InputLayout BLOB  ,ByteCode_VS BLOB  ,ByteCode_PS BLOB  ,ByteCode_HS BLOB  ,ByteCode_DS BLOB  ,ByteCode_GS BLOB  ,ByteCode_AS BLOB  ,ByteCode_MS BLOB  ,ByteCode_CS BLOB  ,DepthStencilDesc BLOB  ,RenderTargetFormats BLOB  ,BlendDesc BLOB  ,RasterizerDesc BLOB  ,ViewInstancingDesc BLOB  ,StreamOutDesc BLOB  ,SampleDesc_Count INTEGER  ,SampleDesc_Quality INTEGER  ,SampleMask INTEGER  ,IBStripCutValue INTEGER  ,PrimitiveTopology INTEGER  ,DSVFormat INTEGER  ,NodeMask INTEGER  ,Flags INTEGER  ,FOREIGN KEY(RootSignature) REFERENCES root_signatures(Key),FOREIGN KEY(ByteCode_VS) REFERENCES shader_bytecode(Key),FOREIGN KEY(ByteCode_PS) REFERENCES shader_bytecode(Key),FOREIGN KEY(ByteCode_HS) REFERENCES shader_bytecode(Key),FOREIGN KEY(ByteCode_DS) REFERENCES shader_bytecode(Key),FOREIGN KEY(ByteCode_GS) REFERENCES shader_bytecode(Key),FOREIGN KEY(ByteCode_AS) REFERENCES shader_bytecode(Key),FOREIGN KEY(ByteCode_MS) REFERENCES shader_bytecode(Key),FOREIGN KEY(ByteCode_CS) REFERENCES shader_bytecode(Key),FOREIGN KEY(DepthStencilDesc) REFERENCES depth_stencil_descs(Key),FOREIGN KEY(RenderTargetFormats) REFERENCES render_target_formats(Key),FOREIGN KEY(BlendDesc) REFERENCES blend_descs(Key),FOREIGN KEY(RasterizerDesc) REFERENCES rasterizer_descs(Key),FOREIGN KEY(ViewInstancingDesc) REFERENCES view_instancing_descs(Key),FOREIGN KEY(StreamOutDesc) REFERENCES stream_out_descs(Key))");
	ExecSQL("CREATE TABLE work_graphs (Key BLOB NOT NULL PRIMARY KEY , ProgramName TEXT NOT NULL ,Flags INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE node_ids (Key BLOB NOT NULL PRIMARY KEY , Name TEXT NOT NULL ,ArrayIndex INTEGER NOT NULL )");
	ExecSQL("CREATE TABLE node_output_overrides (Key BLOB NOT NULL PRIMARY KEY , OutputIndex INTEGER NOT NULL ,NewName BLOB  ,AllowSparseNodes INTEGER  ,MaxRecords INTEGER  ,MaxRecordsSharedWithOutputIndex INTEGER  ,FOREIGN KEY(NewName) REFERENCES node_ids(Key))");
	ExecSQL("CREATE TABLE shader_nodes (Key BLOB NOT NULL PRIMARY KEY , ShaderOrProgram TEXT NOT NULL ,NodeType INTEGER NOT NULL ,OverridesType INTEGER NOT NULL ,LocalRootArgumentsTableIndex INTEGER  ,ProgramEntry INTEGER  ,NewName BLOB  ,ShareInputOf BLOB  ,DispatchGridX INTEGER  ,DispatchGridY INTEGER  ,DispatchGridZ INTEGER  ,MaxDispatchGridX INTEGER  ,MaxDispatchGridY INTEGER  ,MaxDispatchGridZ INTEGER  ,MaxInputRecordsPerGraphEntryRecord_RecordCount INTEGER  ,MaxInputRecordsPerGraphEntryRecord_bCountSharedAcrossNodeArray INTEGER  ,FOREIGN KEY(NewName) REFERENCES node_ids(Key),FOREIGN KEY(ShareInputOf) REFERENCES node_ids(Key))");
	ExecSQL("CREATE TABLE input_layout_to_input_element_associations (InputLayoutKey BLOB NOT NULL ,InputElementKey BLOB NOT NULL ,FOREIGN KEY(InputElementKey) REFERENCES input_element_descs(Key),UNIQUE(InputLayoutKey, InputElementKey))");
	ExecSQL("CREATE TABLE stream_output_desc_to_stream_output_decl_associations (StreamOutDescKey BLOB NOT NULL ,StreamOutDeclKey BLOB NOT NULL ,FOREIGN KEY(StreamOutDescKey) REFERENCES stream_out_descs(Key),FOREIGN KEY(StreamOutDeclKey) REFERENCES so_declarations(Key),UNIQUE(StreamOutDescKey, StreamOutDeclKey))");
	ExecSQL("CREATE TABLE so_to_global_rs_associations (StateObjectKey BLOB NOT NULL ,RootSignatureKey BLOB NOT NULL ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(RootSignatureKey) REFERENCES root_signatures(Key),UNIQUE(StateObjectKey, RootSignatureKey))");
	ExecSQL("CREATE TABLE so_to_local_rs_associations (StateObjectKey BLOB NOT NULL ,RootSignatureKey BLOB NOT NULL ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(RootSignatureKey) REFERENCES root_signatures(Key),UNIQUE(StateObjectKey, RootSignatureKey))");
	ExecSQL("CREATE TABLE so_to_dxil_lib_associations (StateObjectKey BLOB NOT NULL ,DxilLibKey BLOB NOT NULL ,ExportKey BLOB  ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(DxilLibKey) REFERENCES shader_bytecode(Key),FOREIGN KEY(ExportKey) REFERENCES exports(Key),UNIQUE(StateObjectKey, DxilLibKey, ExportKey))");
	ExecSQL("CREATE TABLE so_to_existing_so_associations (StateObjectKey BLOB NOT NULL ,ExistingStateObjectKey BLOB NOT NULL ,ExportKey BLOB  ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(ExistingStateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(ExportKey) REFERENCES exports(Key),UNIQUE(StateObjectKey, ExistingStateObjectKey, ExportKey))");
	ExecSQL("CREATE TABLE so_to_hit_group_associations (StateObjectKey BLOB NOT NULL ,HitGroupKey BLOB NOT NULL ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(HitGroupKey) REFERENCES rt_hit_groups(Key),UNIQUE(StateObjectKey, HitGroupKey))");
	ExecSQL("CREATE TABLE so_to_rt_shader_config_associations (StateObjectKey BLOB NOT NULL ,ShaderConfigKey BLOB NOT NULL ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(ShaderConfigKey) REFERENCES rt_shader_config(Key),UNIQUE(StateObjectKey, ShaderConfigKey))");
	ExecSQL("CREATE TABLE so_to_rt_pipeline_config_associations (StateObjectKey BLOB NOT NULL ,PipelineConfigKey BLOB NOT NULL ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(PipelineConfigKey) REFERENCES rt_pipeline_config(Key),UNIQUE(StateObjectKey, PipelineConfigKey))");
	ExecSQL("CREATE TABLE so_to_dxil_subobject_to_exports_associations (StateObjectKey BLOB NOT NULL ,DxilSubobjectToExportsAssociationKey BLOB NOT NULL ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(DxilSubobjectToExportsAssociationKey) REFERENCES dxil_subobject_to_exports_associations(Key),UNIQUE(StateObjectKey, DxilSubobjectToExportsAssociationKey))");
	ExecSQL("CREATE TABLE so_to_subobject_to_exports_associations (StateObjectKey BLOB NOT NULL ,SubobjectToExportsAssociationKey BLOB NOT NULL ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(SubobjectToExportsAssociationKey) REFERENCES subobject_to_exports_associations(Key),UNIQUE(StateObjectKey, SubobjectToExportsAssociationKey))");
	ExecSQL("CREATE TABLE so_to_generic_program_associations (StateObjectKey BLOB NOT NULL ,GenericProgramKey BLOB NOT NULL ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(GenericProgramKey) REFERENCES generic_programs(Key),UNIQUE(StateObjectKey, GenericProgramKey))");
	ExecSQL("CREATE TABLE so_to_work_graph_associations (StateObjectKey BLOB NOT NULL ,WorkGraphKey BLOB NOT NULL ,FOREIGN KEY(StateObjectKey) REFERENCES state_objects(Key),FOREIGN KEY(WorkGraphKey) REFERENCES work_graphs(Key),UNIQUE(StateObjectKey, WorkGraphKey))");
	ExecSQL("CREATE TABLE work_graph_to_entrypoint_node_id_associations (WorkGraphKey BLOB NOT NULL ,NodeIDKey BLOB NOT NULL ,FOREIGN KEY(WorkGraphKey) REFERENCES work_graphs(Key),FOREIGN KEY(NodeIDKey) REFERENCES node_ids(Key),UNIQUE(WorkGraphKey, NodeIDKey))");
	ExecSQL("CREATE TABLE workgraph_node_to_node_output_overrides_associations (WorkGraphNodeKey BLOB NOT NULL ,NodeOutputOverridesKey BLOB NOT NULL ,FOREIGN KEY(WorkGraphNodeKey) REFERENCES shader_nodes(Key),FOREIGN KEY(NodeOutputOverridesKey) REFERENCES node_output_overrides(Key),UNIQUE(WorkGraphNodeKey, NodeOutputOverridesKey))");
	ExecSQL("CREATE TABLE work_graph_to_work_graph_node_associations (WorkGraphKey BLOB NOT NULL ,WorkGraphNodeKey BLOB NOT NULL ,FOREIGN KEY(WorkGraphKey) REFERENCES work_graphs(Key),FOREIGN KEY(WorkGraphNodeKey) REFERENCES shader_nodes(Key),UNIQUE(WorkGraphKey, WorkGraphNodeKey))");
	ExecSQL("CREATE TABLE string_associations (OwningTableKey BLOB  ,Value TEXT NOT NULL ,UNIQUE(OwningTableKey, Value))");
	ExecSQL("CREATE TABLE groups (Key BLOB NOT NULL PRIMARY KEY , Version INTEGER NOT NULL ,PSOKey BLOB  ,SOKey BLOB  ,FOREIGN KEY(PSOKey) REFERENCES pipeline_states(Key),FOREIGN KEY(SOKey) REFERENCES state_objects(Key))");
	ExecSQL("CREATE TABLE app_id (id INTEGER NOT NULL PRIMARY KEY , exe TEXT NOT NULL ,app_name TEXT NOT NULL ,engine_name TEXT  ,app_version INTEGER NOT NULL ,engine_version INTEGER NOT NULL )");
	ExecSQL("CREATE UNIQUE INDEX so_to_dxil_lib_associations_partial_index ON so_to_dxil_lib_associations(StateObjectKey,DxilLibKey) WHERE ExportKey IS NULL");
	ExecSQL("CREATE UNIQUE INDEX so_to_existing_so_associations_partial_index ON so_to_existing_so_associations(StateObjectKey,ExistingStateObjectKey) WHERE ExportKey IS NULL");
	ExecSQL("CREATE UNIQUE INDEX string_associations_partial_index ON string_associations(Value) WHERE OwningTableKey IS NULL");
}

void FSqlSODBFile::Cleanup()
{
	auto FinalizeStmt = [](sqlite3_stmt*& Stmt)
	{
		if (Stmt)
		{
			sqlite3_finalize(Stmt);
			Stmt = nullptr;
		}
	};

	FinalizeStmt(StmtRootSig);
	FinalizeStmt(StmtBytecode);
	FinalizeStmt(StmtPSO);
	FinalizeStmt(StmtPSO_Graphics);
	FinalizeStmt(StmtDepthStencilOp);
	FinalizeStmt(StmtDepthStencil);
	FinalizeStmt(StmtRTBlend);
	FinalizeStmt(StmtBlend);
	FinalizeStmt(StmtRasterizer);
	FinalizeStmt(StmtRTFormats);
	FinalizeStmt(StmtInputElement);
	FinalizeStmt(StmtInputLayoutAssoc);
}

bool FSqlSODBFile::Open(const FString& Path, ESODBOpenMode Mode)
{
	FilePath = Path;
	OpenMode = Mode;

	sqlite3_initialize();

	if (Mode == ESODBOpenMode::Write)
	{
		// Delete existing file if present
		if (FPaths::FileExists(Path))
		{
			IFileManager::Get().Delete(*Path);
		}

		int32 Result = sqlite3_open(TCHAR_TO_UTF8(*Path), &DB);
		if (Result != SQLITE_OK || !DB)
		{
			UE_LOG(LogASDTool, Error, TEXT("Failed to open SQLite database '%s': %s"), *Path, UTF8_TO_TCHAR(sqlite3_errmsg(DB)));
			if (DB)
			{
				sqlite3_close(DB);
				DB = nullptr;
			}
			return false;
		}

		// Set pragmas to match official SODB format
		ExecSQL("PRAGMA user_version = 2");
		ExecSQL("PRAGMA application_id = 222122203"); // 0xD3D50DB
		ExecSQL("PRAGMA synchronous = OFF");

		// Create all tables matching the official SODB schema
		CreateSchema();

		// Begin single transaction for all inserts
		ExecSQL("BEGIN TRANSACTION");

		// Prepare statements for the tables we write to

		// Shared
		PrepareSQLite("INSERT OR IGNORE INTO root_signatures (Key, value) VALUES (?, ?)", &StmtRootSig);
		PrepareSQLite("INSERT OR IGNORE INTO shader_bytecode (Key, Type, Bytecode) VALUES (?, ?, ?)", &StmtBytecode);
		PrepareSQLite("INSERT INTO pipeline_states (Key, RootSignature, ByteCode_CS) VALUES (?, ?, ?)", &StmtPSO);
		PrepareSQLite("INSERT INTO groups (Key, Version, PSOKey) VALUES (?, 0, ?)", &StmtGroup);

		// Graphics PSO main row -- now with full FK references
		PrepareSQLite(
			"INSERT INTO pipeline_states (Key, RootSignature, "
			"ByteCode_VS, ByteCode_PS, ByteCode_GS, ByteCode_MS, ByteCode_AS, "
			"DepthStencilDesc, RenderTargetFormats, BlendDesc, RasterizerDesc, InputLayout, "
			"SampleDesc_Count, SampleDesc_Quality, SampleMask, PrimitiveTopology, DSVFormat, "
			"NodeMask, IBStripCutValue) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
			&StmtPSO_Graphics);

		// Graphics state sub-tables
		PrepareSQLite(
			"INSERT OR IGNORE INTO depth_stencil_op_descs "
			"(Key, StencilFailOp, StencilDepthFailOp, StencilPassOp, StencilFunc, StencilReadMask, StencilWriteMask) "
			"VALUES (?, ?, ?, ?, ?, ?, ?)",
			&StmtDepthStencilOp);
		PrepareSQLite(
			"INSERT OR IGNORE INTO depth_stencil_descs "
			"(Key, DepthEnable, DepthWriteMask, DepthFunc, StencilEnable, FrontFace, BackFace, DepthBoundsTestEnable) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
			&StmtDepthStencil);
		PrepareSQLite(
			"INSERT OR IGNORE INTO render_target_blend_descs "
			"(Key, BlendEnable, LogicOpEnable, SrcBlend, DestBlend, BlendOp, SrcBlendAlpha, DestBlendAlpha, BlendOpAlpha, LogicOp, RenderTargetWriteMask) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
			&StmtRTBlend);
		PrepareSQLite(
			"INSERT OR IGNORE INTO blend_descs "
			"(Key, AlphaToCoverageEnable, IndependentBlendEnable, "
			"RenderTarget0, RenderTarget1, RenderTarget2, RenderTarget3, "
			"RenderTarget4, RenderTarget5, RenderTarget6, RenderTarget7) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
			&StmtBlend);
		PrepareSQLite(
			"INSERT OR IGNORE INTO rasterizer_descs "
			"(Key, FillMode, CullMode, FrontCounterClockwise, DepthBias, DepthBiasClamp, SlopeScaledDepthBias, "
			"DepthClipEnable, LineRasterizationMode, ForcedSampleCount, ConservativeRaster) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
			&StmtRasterizer);
		PrepareSQLite(
			"INSERT OR IGNORE INTO render_target_formats "
			"(Key, RTFormat0, RTFormat1, RTFormat2, RTFormat3, RTFormat4, RTFormat5, RTFormat6, RTFormat7, NumRenderTargets) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
			&StmtRTFormats);
		PrepareSQLite(
			"INSERT OR IGNORE INTO input_element_descs "
			"(Key, SemanticName, SemanticIndex, Format, InputSlot, AlignedByteOffset, InputSlotClass, InstanceDataStepRate) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
			&StmtInputElement);
		PrepareSQLite(
			"INSERT OR IGNORE INTO input_layout_to_input_element_associations (InputLayoutKey, InputElementKey) VALUES (?, ?)",
			&StmtInputLayoutAssoc);

		const bool bAllStmtsOK =
			StmtRootSig && StmtBytecode && StmtPSO && StmtGroup &&
			StmtPSO_Graphics && StmtDepthStencilOp && StmtDepthStencil &&
			StmtRTBlend && StmtBlend && StmtRasterizer && StmtRTFormats &&
			StmtInputElement && StmtInputLayoutAssoc;

		if (!bAllStmtsOK)
		{
			UE_LOG(LogASDTool, Error, TEXT("Failed to prepare one or more SQLite statements"));
			Close();
			return false;
		}
	}
	else
	{
		int32 Result = sqlite3_open_v2(TCHAR_TO_UTF8(*Path), &DB, SQLITE_OPEN_READONLY, nullptr);
		if (Result != SQLITE_OK || !DB)
		{
			UE_LOG(LogASDTool, Error, TEXT("Failed to open SQLite database '%s' for reading: %s"), *Path, UTF8_TO_TCHAR(sqlite3_errmsg(DB)));
			if (DB)
			{
				sqlite3_close(DB);
				DB = nullptr;
			}
			return false;
		}
	}

	return true;
}

void FSqlSODBFile::SetApplicationDesc(const D3D12_APPLICATION_DESC& AppDesc)
{
	// Cache the app desc strings -- the pointers in AppDesc may not remain valid
	CachedAppDesc    = AppDesc;
	CachedExeFilename = AppDesc.pExeFilename ? AppDesc.pExeFilename : TEXT("");
	CachedAppName    = AppDesc.pName         ? AppDesc.pName        : TEXT("");
	CachedEngineName = AppDesc.pEngineName   ? AppDesc.pEngineName  : TEXT("");
}

bool FSqlSODBFile::WriteEntries(const TArray<FSODBEntry>& Entries)
{
	int64 StartTime = FPlatformTime::Cycles64();

	for (const FSODBEntry& Entry : Entries)
	{
		// Insert root signature (OR IGNORE deduplicates)
		uint8 RootSigKey[16] = {};
		if (Entry.RootSigBlob)
		{
			HashToKey((const uint8*)Entry.RootSigBlob->GetBufferPointer(), Entry.RootSigBlob->GetBufferSize(), RootSigKey);
			sqlite3_bind_blob(StmtRootSig, 1, RootSigKey, 16, SQLITE_STATIC);
			sqlite3_bind_blob(StmtRootSig, 2, Entry.RootSigBlob->GetBufferPointer(), (int)Entry.RootSigBlob->GetBufferSize(), SQLITE_STATIC);
			const int32 RootSigResult = sqlite3_step(StmtRootSig);
			sqlite3_reset(StmtRootSig);
			// INSERT OR IGNORE -- SQLITE_DONE = inserted, SQLITE_CONSTRAINT = duplicate (both fine)
			if (RootSigResult != SQLITE_DONE && RootSigResult != SQLITE_CONSTRAINT)
			{
				UE_LOG(LogASDTool, Warning, TEXT("Failed to insert root signature (sqlite3_step: %d)"), RootSigResult);
			}
		}

		// PSO key: 16 bytes, zero-padded from Entry.Key (8-byte FXxHash64).
		uint8 FinalPSOKey[16] = {};
		FMemory::Memcpy(FinalPSOKey, Entry.Key.GetData(), FMath::Min(Entry.Key.Num(), 16));
		const void* RootSigKeyPtr  = Entry.RootSigBlob ? RootSigKey : nullptr;
		const int   RootSigKeySize = Entry.RootSigBlob ? 16 : 0;

		int32 StepResult = SQLITE_DONE;

		if (Entry.bIsGraphics)
		{
			// Insert per-stage shader bytecode rows
			uint8 KeyVS[16] = {}, KeyPS[16] = {}, KeyGS[16] = {}, KeyMS[16] = {}, KeyAS[16] = {};
			bool bHasVS = InsertShaderBytecode(StmtBytecode, Entry.StageDXBC[SF_Vertex],        "VS", KeyVS);
			bool bHasPS = InsertShaderBytecode(StmtBytecode, Entry.StageDXBC[SF_Pixel],         "PS", KeyPS);
			bool bHasGS = InsertShaderBytecode(StmtBytecode, Entry.StageDXBC[SF_Geometry],      "GS", KeyGS);
			bool bHasMS = InsertShaderBytecode(StmtBytecode, Entry.StageDXBC[SF_Mesh],          "MS", KeyMS);
			bool bHasAS = InsertShaderBytecode(StmtBytecode, Entry.StageDXBC[SF_Amplification], "AS", KeyAS);

			// Extract FD3D12GraphicsPSOState from the stream we built above.
			// ReadBackGraphicsPSOState logs a warning if any expected subobjects are missing --
			// since we built this stream ourselves, a warning here indicates a bug in BuildGraphicsPipelineStateStream.
			FD3D12GraphicsPSOState State;
			ReadBackGraphicsPSOState(Entry.StreamData, State);

			FGraphicsStateKeys StateKeys = InsertGraphicsState(State, Entry.InputElements);

			// Insert graphics pipeline state row with full FK references
			sqlite3_bind_blob(StmtPSO_Graphics,  1, FinalPSOKey,                            16, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics,  2, RootSigKeyPtr,                          RootSigKeySize, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics,  3, bHasVS ? KeyVS : nullptr,               bHasVS ? 16 : 0, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics,  4, bHasPS ? KeyPS : nullptr,               bHasPS ? 16 : 0, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics,  5, bHasGS ? KeyGS : nullptr,               bHasGS ? 16 : 0, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics,  6, bHasMS ? KeyMS : nullptr,               bHasMS ? 16 : 0, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics,  7, bHasAS ? KeyAS : nullptr,               bHasAS ? 16 : 0, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics,  8, StateKeys.DepthStencilKey,              16, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics,  9, StateKeys.RTFormatsKey,                 16, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics, 10, StateKeys.BlendKey,                     16, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics, 11, StateKeys.RasterizerKey,                16, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO_Graphics, 12, StateKeys.bHasInputLayout ? StateKeys.InputLayoutKey : nullptr,
			                                         StateKeys.bHasInputLayout ? 16 : 0,       SQLITE_STATIC);
			sqlite3_bind_int(StmtPSO_Graphics,  13, (int32)State.SampleDesc.Count);
			sqlite3_bind_int(StmtPSO_Graphics,  14, (int32)State.SampleDesc.Quality);
			sqlite3_bind_int(StmtPSO_Graphics,  15, (int32)State.SampleMask);
			sqlite3_bind_int(StmtPSO_Graphics,  16, (int32)State.PrimitiveTopologyType);
			sqlite3_bind_int(StmtPSO_Graphics,  17, (int32)State.DSVFormat);
			sqlite3_bind_int(StmtPSO_Graphics,  18, 1); // NodeMask -- always 1 for single-GPU
			// IBStripCutValue: 0 for VS pipelines, NULL for MS pipelines (no index buffer)
			if (bHasMS)
			{
				sqlite3_bind_null(StmtPSO_Graphics, 19);
			}
			else
			{
				sqlite3_bind_int(StmtPSO_Graphics,  19, 0);
			}
			StepResult = sqlite3_step(StmtPSO_Graphics);
			sqlite3_reset(StmtPSO_Graphics);
		}
		else
		{
			// Compute path -- insert single CS bytecode row
			uint8 BytecodeKey[16];
			HashToKey(Entry.StageDXBC[SF_Compute].GetData(), Entry.StageDXBC[SF_Compute].Num(), BytecodeKey);
			sqlite3_bind_blob(StmtBytecode, 1, BytecodeKey, 16, SQLITE_STATIC);
			sqlite3_bind_text(StmtBytecode, 2, "CS", -1, SQLITE_STATIC);
			sqlite3_bind_blob(StmtBytecode, 3, Entry.StageDXBC[SF_Compute].GetData(), (int)Entry.StageDXBC[SF_Compute].Num(), SQLITE_STATIC);
			int32 BytecodeResult = sqlite3_step(StmtBytecode);
			sqlite3_reset(StmtBytecode);
			if (BytecodeResult != SQLITE_DONE)
			{
				UE_LOG(LogASDTool, Warning, TEXT("Failed to insert shader bytecode"));
			}

			sqlite3_bind_blob(StmtPSO, 1, FinalPSOKey,    16, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO, 2, RootSigKeyPtr,  RootSigKeySize, SQLITE_STATIC);
			sqlite3_bind_blob(StmtPSO, 3, BytecodeKey,    16, SQLITE_STATIC);
			StepResult = sqlite3_step(StmtPSO);
			sqlite3_reset(StmtPSO);
		}

		if (StepResult != SQLITE_DONE)
		{
			UE_LOG(LogASDTool, Warning, TEXT("Failed to insert pipeline state"));
		}

		// Insert group entry -- Key is 8-byte FXxHash64 (routing), PSOKey FK is 16-byte
		sqlite3_bind_blob(StmtGroup, 1, Entry.Key.GetData(), Entry.Key.Num(), SQLITE_STATIC);
		sqlite3_bind_blob(StmtGroup, 2, FinalPSOKey, 16, SQLITE_STATIC);
		StepResult = sqlite3_step(StmtGroup);
		sqlite3_reset(StmtGroup);
		if (StepResult != SQLITE_DONE)
		{
			UE_LOG(LogASDTool, Warning, TEXT("Failed to insert group entry"));
		}
	}

	FPlatformAtomics::InterlockedAdd(&WriteCycles, FPlatformTime::Cycles64() - StartTime);
	return true;
}

bool FSqlSODBFile::ReadEntries(TArray<FSODBEntry>& OutEntries)
{
	// -------------------------------------------------------------------------
	// Compute PSOs -- ByteCode_CS IS NOT NULL
	// -------------------------------------------------------------------------
	{
		sqlite3_stmt* Stmt = nullptr;
		PrepareSQLite(
			"SELECT g.Key, g.Version, ps.ByteCode_CS, sb.Bytecode, ps.RootSignature, rs.value "
			"FROM groups g "
			"LEFT JOIN pipeline_states ps ON ps.Key = g.PSOKey "
			"LEFT JOIN shader_bytecode sb ON sb.Key = ps.ByteCode_CS "
			"LEFT JOIN root_signatures rs ON rs.Key = ps.RootSignature "
			"WHERE ps.ByteCode_CS IS NOT NULL",
			&Stmt);

		if (!Stmt)
		{
			return false;
		}

		while (sqlite3_step(Stmt) == SQLITE_ROW)
		{
			FSODBEntry& Entry = OutEntries.Emplace_GetRef();

			int32 KeySize      = sqlite3_column_bytes(Stmt, 0);
			const void* KeyData = sqlite3_column_blob(Stmt, 0);
			if (KeyData && KeySize > 0)
			{
				Entry.Key.SetNumUninitialized(KeySize);
				FMemory::Memcpy(Entry.Key.GetData(), KeyData, KeySize);
			}

			Entry.Version = (uint32)sqlite3_column_int(Stmt, 1);

			int32 DXBCSize   = sqlite3_column_bytes(Stmt, 3);
			const void* DXBCPtr = sqlite3_column_blob(Stmt, 3);
			if (DXBCPtr && DXBCSize > 0)
			{
				Entry.StageDXBC[SF_Compute].SetNumUninitialized(DXBCSize);
				FMemory::Memcpy(Entry.StageDXBC[SF_Compute].GetData(), DXBCPtr, DXBCSize);
			}

			int32 RootSigSize   = sqlite3_column_bytes(Stmt, 5);
			const void* RootSigPtr = sqlite3_column_blob(Stmt, 5);
			if (RootSigPtr && RootSigSize > 0)
			{
				Entry.RootSigData.SetNumUninitialized(RootSigSize);
				FMemory::Memcpy(Entry.RootSigData.GetData(), RootSigPtr, RootSigSize);
			}

			// Build StreamData for the compute entry so CompileOnThread doesn't need to build it.
			// Stream layout: [SerializedRootSig][CS]
			if (Entry.StageDXBC[SF_Compute].Num() > 0)
			{
				if (Entry.RootSigData.Num() > 0)
				{
					CD3DX12_PIPELINE_STATE_STREAM_SERIALIZED_ROOT_SIGNATURE RootSigSub;
					RootSigSub = CD3DX12_SERIALIZED_ROOT_SIGNATURE_DESC(Entry.RootSigData.GetData(), (SIZE_T)Entry.RootSigData.Num());
					AppendSubobject(Entry.StreamData, RootSigSub);
				}

				CD3DX12_PIPELINE_STATE_STREAM_CS CSSub;
				CSSub = D3D12_SHADER_BYTECODE{ Entry.StageDXBC[SF_Compute].GetData(), (SIZE_T)Entry.StageDXBC[SF_Compute].Num() };
				AppendSubobject(Entry.StreamData, CSSub);
			}
		}

		sqlite3_finalize(Stmt);
	}

	// -------------------------------------------------------------------------
	// Graphics PSOs -- ByteCode_CS IS NULL, VS or MS present
	// Big JOIN across all state sub-tables to reconstruct FD3D12GraphicsPSOState
	// Columns:
	//   0  = g.Key
	//   1  = g.Version
	//   2  = rs.value (root sig)
	//   3  = sbVS.Bytecode
	//   4  = sbPS.Bytecode
	//   5  = sbGS.Bytecode
	//   6  = sbMS.Bytecode
	//   7  = sbAS.Bytecode
	//   8  = bd.AlphaToCoverageEnable
	//   9  = bd.IndependentBlendEnable
	//   10..17 = rtb0..7 per-RT blend (each expands to 10 fields -- we do 8 separate JOINs)
	//   ... see query below for exact column mapping
	// -------------------------------------------------------------------------
	{
		// To keep the query manageable, we fetch blend per-RT as sub-selects via aliases.
		// Each render_target_blend_descs join adds: BlendEnable,SrcBlend,DestBlend,BlendOp,
		//   SrcBlendAlpha,DestBlendAlpha,BlendOpAlpha,LogicOp,RenderTargetWriteMask,LogicOpEnable (10 cols)
		// Column layout after g.Key,g.Version,rs.value,sbVS..sbAS (8 cols = 0..7):
		//   8  = bd.AlphaToCoverageEnable
		//   9  = bd.IndependentBlendEnable
		//   10..19 = rtb0 (10 fields)
		//   20..29 = rtb1
		//   30..39 = rtb2
		//   40..49 = rtb3
		//   50..59 = rtb4
		//   60..69 = rtb5
		//   70..79 = rtb6
		//   80..89 = rtb7
		//   90 = rd.FillMode
		//   91 = rd.CullMode
		//   92 = rd.FrontCounterClockwise
		//   93 = rd.DepthBias (REAL)
		//   94 = rd.SlopeScaledDepthBias (REAL)
		//   95 = rd.DepthClipEnable
		//   96 = ds.DepthEnable
		//   97 = ds.DepthWriteMask
		//   98 = ds.DepthFunc
		//   99 = ds.StencilEnable
		//   100 = ds.DepthBoundsTestEnable
		//   101 = dso_front.StencilFailOp
		//   102 = dso_front.StencilDepthFailOp
		//   103 = dso_front.StencilPassOp
		//   104 = dso_front.StencilFunc
		//   105 = dso_front.StencilReadMask
		//   106 = dso_front.StencilWriteMask
		//   107 = dso_back.StencilFailOp
		//   108 = dso_back.StencilDepthFailOp
		//   109 = dso_back.StencilPassOp
		//   110 = dso_back.StencilFunc
		//   111 = dso_back.StencilReadMask
		//   112 = dso_back.StencilWriteMask
		//   113 = rtf.RTFormat0
		//   114 = rtf.RTFormat1
		//   115 = rtf.RTFormat2
		//   116 = rtf.RTFormat3
		//   117 = rtf.RTFormat4
		//   118 = rtf.RTFormat5
		//   119 = rtf.RTFormat6
		//   120 = rtf.RTFormat7
		//   121 = rtf.NumRenderTargets
		//   122 = ps.DSVFormat
		//   123 = ps.SampleDesc_Count
		//   124 = ps.SampleDesc_Quality
		//   125 = ps.SampleMask
		//   126 = ps.PrimitiveTopology
		//   127 = ps.InputLayout (FK blob -- used for second input element query)

		sqlite3_stmt* Stmt = nullptr;
		PrepareSQLite(
			"SELECT g.Key, g.Version, rs.value, "
			"sbVS.Bytecode, sbPS.Bytecode, sbGS.Bytecode, sbMS.Bytecode, sbAS.Bytecode, "
			"bd.AlphaToCoverageEnable, bd.IndependentBlendEnable, "
			// RT blend 0
			"rtb0.BlendEnable, rtb0.SrcBlend, rtb0.DestBlend, rtb0.BlendOp, rtb0.SrcBlendAlpha, rtb0.DestBlendAlpha, rtb0.BlendOpAlpha, rtb0.LogicOp, rtb0.RenderTargetWriteMask, rtb0.LogicOpEnable, "
			// RT blend 1
			"rtb1.BlendEnable, rtb1.SrcBlend, rtb1.DestBlend, rtb1.BlendOp, rtb1.SrcBlendAlpha, rtb1.DestBlendAlpha, rtb1.BlendOpAlpha, rtb1.LogicOp, rtb1.RenderTargetWriteMask, rtb1.LogicOpEnable, "
			// RT blend 2
			"rtb2.BlendEnable, rtb2.SrcBlend, rtb2.DestBlend, rtb2.BlendOp, rtb2.SrcBlendAlpha, rtb2.DestBlendAlpha, rtb2.BlendOpAlpha, rtb2.LogicOp, rtb2.RenderTargetWriteMask, rtb2.LogicOpEnable, "
			// RT blend 3
			"rtb3.BlendEnable, rtb3.SrcBlend, rtb3.DestBlend, rtb3.BlendOp, rtb3.SrcBlendAlpha, rtb3.DestBlendAlpha, rtb3.BlendOpAlpha, rtb3.LogicOp, rtb3.RenderTargetWriteMask, rtb3.LogicOpEnable, "
			// RT blend 4
			"rtb4.BlendEnable, rtb4.SrcBlend, rtb4.DestBlend, rtb4.BlendOp, rtb4.SrcBlendAlpha, rtb4.DestBlendAlpha, rtb4.BlendOpAlpha, rtb4.LogicOp, rtb4.RenderTargetWriteMask, rtb4.LogicOpEnable, "
			// RT blend 5
			"rtb5.BlendEnable, rtb5.SrcBlend, rtb5.DestBlend, rtb5.BlendOp, rtb5.SrcBlendAlpha, rtb5.DestBlendAlpha, rtb5.BlendOpAlpha, rtb5.LogicOp, rtb5.RenderTargetWriteMask, rtb5.LogicOpEnable, "
			// RT blend 6
			"rtb6.BlendEnable, rtb6.SrcBlend, rtb6.DestBlend, rtb6.BlendOp, rtb6.SrcBlendAlpha, rtb6.DestBlendAlpha, rtb6.BlendOpAlpha, rtb6.LogicOp, rtb6.RenderTargetWriteMask, rtb6.LogicOpEnable, "
			// RT blend 7
			"rtb7.BlendEnable, rtb7.SrcBlend, rtb7.DestBlend, rtb7.BlendOp, rtb7.SrcBlendAlpha, rtb7.DestBlendAlpha, rtb7.BlendOpAlpha, rtb7.LogicOp, rtb7.RenderTargetWriteMask, rtb7.LogicOpEnable, "
			// Rasterizer
			"rd.FillMode, rd.CullMode, rd.FrontCounterClockwise, rd.DepthBias, rd.SlopeScaledDepthBias, rd.DepthClipEnable, "
			// Depth stencil
			"ds.DepthEnable, ds.DepthWriteMask, ds.DepthFunc, ds.StencilEnable, ds.DepthBoundsTestEnable, "
			"dso_front.StencilFailOp, dso_front.StencilDepthFailOp, dso_front.StencilPassOp, dso_front.StencilFunc, dso_front.StencilReadMask, dso_front.StencilWriteMask, "
			"dso_back.StencilFailOp, dso_back.StencilDepthFailOp, dso_back.StencilPassOp, dso_back.StencilFunc, dso_back.StencilReadMask, dso_back.StencilWriteMask, "
			// RT formats
			"rtf.RTFormat0, rtf.RTFormat1, rtf.RTFormat2, rtf.RTFormat3, rtf.RTFormat4, rtf.RTFormat5, rtf.RTFormat6, rtf.RTFormat7, rtf.NumRenderTargets, "
			// DSV, sample, topology, input layout key
			"ps.DSVFormat, ps.SampleDesc_Count, ps.SampleDesc_Quality, ps.SampleMask, ps.PrimitiveTopology, ps.InputLayout "
			"FROM groups g "
			"LEFT JOIN pipeline_states ps ON ps.Key = g.PSOKey "
			"LEFT JOIN root_signatures rs ON rs.Key = ps.RootSignature "
			"LEFT JOIN shader_bytecode sbVS ON sbVS.Key = ps.ByteCode_VS "
			"LEFT JOIN shader_bytecode sbPS ON sbPS.Key = ps.ByteCode_PS "
			"LEFT JOIN shader_bytecode sbGS ON sbGS.Key = ps.ByteCode_GS "
			"LEFT JOIN shader_bytecode sbMS ON sbMS.Key = ps.ByteCode_MS "
			"LEFT JOIN shader_bytecode sbAS ON sbAS.Key = ps.ByteCode_AS "
			"LEFT JOIN blend_descs bd ON bd.Key = ps.BlendDesc "
			"LEFT JOIN render_target_blend_descs rtb0 ON rtb0.Key = bd.RenderTarget0 "
			"LEFT JOIN render_target_blend_descs rtb1 ON rtb1.Key = bd.RenderTarget1 "
			"LEFT JOIN render_target_blend_descs rtb2 ON rtb2.Key = bd.RenderTarget2 "
			"LEFT JOIN render_target_blend_descs rtb3 ON rtb3.Key = bd.RenderTarget3 "
			"LEFT JOIN render_target_blend_descs rtb4 ON rtb4.Key = bd.RenderTarget4 "
			"LEFT JOIN render_target_blend_descs rtb5 ON rtb5.Key = bd.RenderTarget5 "
			"LEFT JOIN render_target_blend_descs rtb6 ON rtb6.Key = bd.RenderTarget6 "
			"LEFT JOIN render_target_blend_descs rtb7 ON rtb7.Key = bd.RenderTarget7 "
			"LEFT JOIN rasterizer_descs rd ON rd.Key = ps.RasterizerDesc "
			"LEFT JOIN depth_stencil_descs ds ON ds.Key = ps.DepthStencilDesc "
			"LEFT JOIN depth_stencil_op_descs dso_front ON dso_front.Key = ds.FrontFace "
			"LEFT JOIN depth_stencil_op_descs dso_back  ON dso_back.Key  = ds.BackFace "
			"LEFT JOIN render_target_formats rtf ON rtf.Key = ps.RenderTargetFormats "
			"WHERE ps.ByteCode_CS IS NULL AND (ps.ByteCode_VS IS NOT NULL OR ps.ByteCode_MS IS NOT NULL)",
			&Stmt);

		if (Stmt)
		{
			while (sqlite3_step(Stmt) == SQLITE_ROW)
			{
				FSODBEntry& Entry = OutEntries.Emplace_GetRef();
				Entry.bIsGraphics = true;

				// Key + version
				int32 KeySize      = sqlite3_column_bytes(Stmt, 0);
				const void* KeyData = sqlite3_column_blob(Stmt, 0);
				if (KeyData && KeySize > 0)
				{
					Entry.Key.SetNumUninitialized(KeySize);
					FMemory::Memcpy(Entry.Key.GetData(), KeyData, KeySize);
				}
				Entry.Version = (uint32)sqlite3_column_int(Stmt, 1);

				// Root signature
				int32 RootSigSize   = sqlite3_column_bytes(Stmt, 2);
				const void* RootSigPtr = sqlite3_column_blob(Stmt, 2);
				if (RootSigPtr && RootSigSize > 0)
				{
					Entry.RootSigData.SetNumUninitialized(RootSigSize);
					FMemory::Memcpy(Entry.RootSigData.GetData(), RootSigPtr, RootSigSize);
				}

				// Per-stage DXBC (cols 3-7 = VS, PS, GS, MS, AS)
				const struct { EShaderFrequency Freq; int Col; } ShaderCols[] = {
					{ SF_Vertex,        3 },
					{ SF_Pixel,         4 },
					{ SF_Geometry,      5 },
					{ SF_Mesh,          6 },
					{ SF_Amplification, 7 },
				};
				for (const auto& SC : ShaderCols)
				{
					int32 Size = sqlite3_column_bytes(Stmt, SC.Col);
					const void* Ptr = sqlite3_column_blob(Stmt, SC.Col);
					if (Ptr && Size > 0)
					{
						Entry.StageDXBC[SC.Freq].SetNumUninitialized(Size);
						FMemory::Memcpy(Entry.StageDXBC[SC.Freq].GetData(), Ptr, Size);
					}
				}

				// Reconstruct FD3D12GraphicsPSOState from SQL columns
				FD3D12GraphicsPSOState State;

				// Blend (cols 8-89)
				State.BlendDesc.AlphaToCoverageEnable  = sqlite3_column_int(Stmt, 8)  != 0;
				State.BlendDesc.IndependentBlendEnable = sqlite3_column_int(Stmt, 9)  != 0;

				for (int32 RTIdx = 0; RTIdx < 8; ++RTIdx)
				{
					const int BaseCol = 10 + RTIdx * 10;
					D3D12_RENDER_TARGET_BLEND_DESC& RT = State.BlendDesc.RenderTarget[RTIdx];
					RT.BlendEnable          = sqlite3_column_int(Stmt, BaseCol + 0) != 0;
					RT.SrcBlend             = (D3D12_BLEND)sqlite3_column_int(Stmt, BaseCol + 1);
					RT.DestBlend            = (D3D12_BLEND)sqlite3_column_int(Stmt, BaseCol + 2);
					RT.BlendOp              = (D3D12_BLEND_OP)sqlite3_column_int(Stmt, BaseCol + 3);
					RT.SrcBlendAlpha        = (D3D12_BLEND)sqlite3_column_int(Stmt, BaseCol + 4);
					RT.DestBlendAlpha       = (D3D12_BLEND)sqlite3_column_int(Stmt, BaseCol + 5);
					RT.BlendOpAlpha         = (D3D12_BLEND_OP)sqlite3_column_int(Stmt, BaseCol + 6);
					RT.LogicOp              = (D3D12_LOGIC_OP)sqlite3_column_int(Stmt, BaseCol + 7);
					RT.RenderTargetWriteMask = (UINT8)sqlite3_column_int(Stmt, BaseCol + 8);
					RT.LogicOpEnable        = sqlite3_column_int(Stmt, BaseCol + 9) != 0;
				}

				// Rasterizer (cols 90-95)
				State.RasterizerDesc.FillMode              = (D3D12_FILL_MODE)sqlite3_column_int(Stmt, 90);
				State.RasterizerDesc.CullMode              = (D3D12_CULL_MODE)sqlite3_column_int(Stmt, 91);
				State.RasterizerDesc.FrontCounterClockwise = sqlite3_column_int(Stmt, 92) != 0;
				State.RasterizerDesc.DepthBias             = FMath::FloorToInt((float)sqlite3_column_double(Stmt, 93) * (float)(1 << 24));
				State.RasterizerDesc.SlopeScaledDepthBias  = (float)sqlite3_column_double(Stmt, 94);
				State.RasterizerDesc.DepthClipEnable       = sqlite3_column_int(Stmt, 95) != 0;

				// Depth stencil (cols 96-112)
				State.DepthStencilDesc.DepthEnable           = sqlite3_column_int(Stmt, 96) != 0;
				State.DepthStencilDesc.DepthWriteMask        = (D3D12_DEPTH_WRITE_MASK)sqlite3_column_int(Stmt, 97);
				State.DepthStencilDesc.DepthFunc             = (D3D12_COMPARISON_FUNC)sqlite3_column_int(Stmt, 98);
				State.DepthStencilDesc.StencilEnable         = sqlite3_column_int(Stmt, 99) != 0;
				State.DepthStencilDesc.DepthBoundsTestEnable = sqlite3_column_int(Stmt, 100) != 0;
				State.DepthStencilDesc.StencilReadMask       = (UINT8)sqlite3_column_int(Stmt, 105);
				State.DepthStencilDesc.StencilWriteMask      = (UINT8)sqlite3_column_int(Stmt, 106);

				State.DepthStencilDesc.FrontFace.StencilFailOp      = (D3D12_STENCIL_OP)sqlite3_column_int(Stmt, 101);
				State.DepthStencilDesc.FrontFace.StencilDepthFailOp = (D3D12_STENCIL_OP)sqlite3_column_int(Stmt, 102);
				State.DepthStencilDesc.FrontFace.StencilPassOp      = (D3D12_STENCIL_OP)sqlite3_column_int(Stmt, 103);
				State.DepthStencilDesc.FrontFace.StencilFunc        = (D3D12_COMPARISON_FUNC)sqlite3_column_int(Stmt, 104);

				State.DepthStencilDesc.BackFace.StencilFailOp      = (D3D12_STENCIL_OP)sqlite3_column_int(Stmt, 107);
				State.DepthStencilDesc.BackFace.StencilDepthFailOp = (D3D12_STENCIL_OP)sqlite3_column_int(Stmt, 108);
				State.DepthStencilDesc.BackFace.StencilPassOp      = (D3D12_STENCIL_OP)sqlite3_column_int(Stmt, 109);
				State.DepthStencilDesc.BackFace.StencilFunc        = (D3D12_COMPARISON_FUNC)sqlite3_column_int(Stmt, 110);

				// RT formats (cols 113-121)
				State.RTFormats.NumRenderTargets = (UINT)sqlite3_column_int(Stmt, 121);
				for (int32 i = 0; i < 8; ++i)
				{
					State.RTFormats.RTFormats[i] = (DXGI_FORMAT)sqlite3_column_int(Stmt, 113 + i);
				}

				// DSV, sample, topology (cols 122-126)
				State.DSVFormat            = (DXGI_FORMAT)sqlite3_column_int(Stmt, 122);
				State.SampleDesc.Count     = (UINT)sqlite3_column_int(Stmt, 123);
				State.SampleDesc.Quality   = (UINT)sqlite3_column_int(Stmt, 124);
				State.SampleMask           = (UINT)sqlite3_column_int(Stmt, 125);
				State.PrimitiveTopologyType = (D3D12_PRIMITIVE_TOPOLOGY_TYPE)sqlite3_column_int(Stmt, 126);

				// Input layout key (col 127) -- fetch elements via second query
				TArray<D3D12_INPUT_ELEMENT_DESC> InputElements;
				int32 InputLayoutKeySize   = sqlite3_column_bytes(Stmt, 127);
				const void* InputLayoutKey = sqlite3_column_blob(Stmt, 127);

				if (InputLayoutKey && InputLayoutKeySize > 0)
				{
					// Fetch all input elements associated with this layout key
					sqlite3_stmt* ElemStmt = nullptr;
					PrepareSQLite(
						"SELECT ie.SemanticIndex, ie.Format, ie.InputSlot, ie.AlignedByteOffset, ie.InputSlotClass, ie.InstanceDataStepRate "
						"FROM input_layout_to_input_element_associations assoc "
						"JOIN input_element_descs ie ON ie.Key = assoc.InputElementKey "
						"WHERE assoc.InputLayoutKey = ? "
						"ORDER BY ie.InputSlot, ie.AlignedByteOffset, ie.SemanticIndex",
						&ElemStmt);

					if (ElemStmt)
					{
						sqlite3_bind_blob(ElemStmt, 1, InputLayoutKey, InputLayoutKeySize, SQLITE_STATIC);
						while (sqlite3_step(ElemStmt) == SQLITE_ROW)
						{
							D3D12_INPUT_ELEMENT_DESC& Elem = InputElements.AddDefaulted_GetRef();
							Elem.SemanticName         = "ATTRIBUTE";
							Elem.SemanticIndex        = (UINT)sqlite3_column_int(ElemStmt, 0);
							Elem.Format               = (DXGI_FORMAT)sqlite3_column_int(ElemStmt, 1);
							Elem.InputSlot            = (UINT)sqlite3_column_int(ElemStmt, 2);
							Elem.AlignedByteOffset    = (UINT)sqlite3_column_int(ElemStmt, 3);
							Elem.InputSlotClass       = (D3D12_INPUT_CLASSIFICATION)sqlite3_column_int(ElemStmt, 4);
							Elem.InstanceDataStepRate = (UINT)sqlite3_column_int(ElemStmt, 5);
						}
						sqlite3_finalize(ElemStmt);
					}
				}

				// Build the full pipeline state stream from the reconstructed state
				if (BuildGraphicsPipelineStateStream(State,
					Entry.RootSigData.GetData(), Entry.RootSigData.Num(),
					Entry.StageDXBC,
					Entry.StreamData))
				{
					// Move InputElements into the entry, then append INPUT_LAYOUT pointing at the stable location
					Entry.InputElements = MoveTemp(InputElements);
					AppendInputLayoutSubobject(Entry.StreamData, Entry.InputElements);
				}
			}

			sqlite3_finalize(Stmt);
		}
	}

	int32 NumCompute  = 0;
	int32 NumGraphics = 0;
	for (const FSODBEntry& E : OutEntries)
	{
		if (E.bIsGraphics) { ++NumGraphics; }
		else               { ++NumCompute; }
	}
	UE_LOG(LogASDTool, Display, TEXT("  Read %d PSO entries from SODB (SQLite): %d compute, %d graphics -- %s"),
		OutEntries.Num(), NumCompute, NumGraphics, *FPaths::GetCleanFilename(FilePath));
	return true;
}

void FSqlSODBFile::Close()
{
	if (!DB)
	{
		return;
	}

	if (OpenMode == ESODBOpenMode::Write)
	{
		// Finalize write statements before app_id insert
		Cleanup();
		if (StmtGroup)
		{
			sqlite3_finalize(StmtGroup);
			StmtGroup = nullptr;
		}

		// Write app_id using cached desc
		const D3D12_APPLICATION_DESC& AppDesc = CachedAppDesc;
		uint64 AppVersion =
			((uint64)AppDesc.Version.VersionParts[0] << 48) |
			((uint64)AppDesc.Version.VersionParts[1] << 32) |
			((uint64)AppDesc.Version.VersionParts[2] << 16) |
			 (uint64)AppDesc.Version.VersionParts[3];
		uint64 EngineVersion =
			((uint64)AppDesc.EngineVersion.VersionParts[0] << 48) |
			((uint64)AppDesc.EngineVersion.VersionParts[1] << 32) |
			((uint64)AppDesc.EngineVersion.VersionParts[2] << 16) |
			 (uint64)AppDesc.EngineVersion.VersionParts[3];

		sqlite3_stmt* AppStmt = nullptr;
		PrepareSQLite("INSERT INTO app_id (id, exe, app_name, engine_name, app_version, engine_version) VALUES (0, ?, ?, ?, ?, ?)", &AppStmt);
		if (AppStmt)
		{
			sqlite3_bind_text(AppStmt, 1, TCHAR_TO_UTF8(*CachedExeFilename), -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(AppStmt, 2, TCHAR_TO_UTF8(*CachedAppName),     -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(AppStmt, 3, TCHAR_TO_UTF8(*CachedEngineName),  -1, SQLITE_TRANSIENT);
			sqlite3_bind_int64(AppStmt, 4, (int64)AppVersion);
			sqlite3_bind_int64(AppStmt, 5, (int64)EngineVersion);
			const int32 AppIdResult = sqlite3_step(AppStmt);
			if (AppIdResult != SQLITE_DONE)
			{
				UE_LOG(LogASDTool, Warning, TEXT("Failed to insert app_id (sqlite3_step: %d)"), AppIdResult);
			}
			sqlite3_finalize(AppStmt);
		}

		ExecSQL("COMMIT");
	}

	sqlite3_close(DB);
	DB = nullptr;
}

double FSqlSODBFile::GetWriteTimeSeconds() const
{
	return FPlatformTime::ToSeconds64(WriteCycles);
}

//--------------------------------------------------------------------------------------------------
// PSDB merging
//--------------------------------------------------------------------------------------------------

static FString GetPSDBSchemaDDL(const FString& Path)
{
	sqlite3* SchemDB = nullptr;
	sqlite3_open_v2(TCHAR_TO_UTF8(*Path), &SchemDB, SQLITE_OPEN_READONLY, nullptr);
	if (!SchemDB)
	{
		return FString();
	}

	sqlite3_stmt* Stmt = nullptr;
	sqlite3_prepare_v2(SchemDB,
		"SELECT group_concat(sql, ';') FROM sqlite_master WHERE type IN ('table','index') ORDER BY name",
		-1, &Stmt, nullptr);

	FString DDL;
	if (sqlite3_step(Stmt) == SQLITE_ROW)
	{
		const char* Text = (const char*)sqlite3_column_text(Stmt, 0);
		if (Text)
		{
			DDL = UTF8_TO_TCHAR(Text);
		}
	}
	sqlite3_finalize(Stmt);
	sqlite3_close(SchemDB);
	return DDL;
}

static bool ValidatePSDBSchemas(const TArray<FString>& SourcePaths)
{
	if (SourcePaths.Num() < 2)
	{
		return true;
	}

	FString RefDDL = GetPSDBSchemaDDL(SourcePaths[0]);
	if (RefDDL.IsEmpty())
	{
		UE_LOG(LogASDTool, Error, TEXT("Schema validation: failed to read schema from '%s'"), *FPaths::GetCleanFilename(SourcePaths[0]));
		return false;
	}

	bool bAllMatch = true;
	for (int32 i = 1; i < SourcePaths.Num(); ++i)
	{
		FString DDL = GetPSDBSchemaDDL(SourcePaths[i]);
		if (DDL.IsEmpty() || DDL != RefDDL)
		{
			UE_LOG(LogASDTool, Error, TEXT("Schema mismatch in PSDB '%s'"), *FPaths::GetCleanFilename(SourcePaths[i]));
			bAllMatch = false;
		}
	}
	return bAllMatch;
}

bool MergePSDBs(const TArray<FString>& SourcePaths, const FString& DestPath, double& OutMergeSeconds)
{
	OutMergeSeconds = 0.0;

	if (SourcePaths.IsEmpty())
	{
		UE_LOG(LogASDTool, Error, TEXT("MergePSDBs: no source PSDBs to merge."));
		return false;
	}

	if (FPaths::FileExists(DestPath)) { IFileManager::Get().Delete(*DestPath); }
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(DestPath), true);

	double StartTime = FPlatformTime::Seconds();

	sqlite3* DestDB = nullptr;
	if (sqlite3_open(TCHAR_TO_UTF8(*DestPath), &DestDB) != SQLITE_OK || !DestDB)
	{
		UE_LOG(LogASDTool, Error, TEXT("MergePSDBs: failed to create dest DB '%s'"), *DestPath);
		return false;
	}

	auto ExecSQL = [&](const char* SQL) -> bool
	{
		char* ErrMsg = nullptr;
		int32 Rc = sqlite3_exec(DestDB, SQL, nullptr, nullptr, &ErrMsg);
		if (Rc != SQLITE_OK)
		{
			UE_LOG(LogASDTool, Error, TEXT("MergePSDBs SQL error (rc=%d): %s | SQL: %s"),
				Rc, UTF8_TO_TCHAR(ErrMsg ? ErrMsg : "(null)"), UTF8_TO_TCHAR(SQL));
			sqlite3_free(ErrMsg);
			return false;
		}
		return true;
	};

	if (!ValidatePSDBSchemas(SourcePaths))
	{
		sqlite3_close(DestDB);
		IFileManager::Get().Delete(*DestPath);
		return false;
	}

	// Copy schema from source[0] directly -- avoids hardcoding, stays in sync with compiler output
	ExecSQL("PRAGMA synchronous = OFF");
	{
		FString EscapedPath = SourcePaths[0].Replace(TEXT("'"), TEXT("''"));
		if (!ExecSQL(TCHAR_TO_UTF8(*FString::Printf(TEXT("ATTACH DATABASE '%s' AS schema_src"), *EscapedPath))))
		{
			sqlite3_close(DestDB);
			IFileManager::Get().Delete(*DestPath);
			return false;
		}

		sqlite3_stmt* SchemaStmt = nullptr;
		sqlite3_prepare_v2(DestDB,
			"SELECT sql FROM schema_src.sqlite_master WHERE type IN ('table','index') AND sql IS NOT NULL ORDER BY type DESC, name",
			-1, &SchemaStmt, nullptr);
		while (sqlite3_step(SchemaStmt) == SQLITE_ROW)
		{
			const char* DDL = (const char*)sqlite3_column_text(SchemaStmt, 0);
			if (DDL) { ExecSQL(DDL); }
		}
		sqlite3_finalize(SchemaStmt);

		// Read user_version and application_id from source[0]
		int64 UserVersion = 0;
		int64 AppId       = 0;
		{
			sqlite3* SrcDB = nullptr;
			if (sqlite3_open_v2(TCHAR_TO_UTF8(*SourcePaths[0]), &SrcDB, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK && SrcDB)
			{
				auto ReadPragmaInt = [&](const char* SQL) -> int64
				{
					sqlite3_stmt* Stmt = nullptr;
					sqlite3_prepare_v2(SrcDB, SQL, -1, &Stmt, nullptr);
					int64 Val = 0;
					if (sqlite3_step(Stmt) == SQLITE_ROW) { Val = sqlite3_column_int64(Stmt, 0); }
					sqlite3_finalize(Stmt);
					return Val;
				};
				UserVersion = ReadPragmaInt("PRAGMA user_version");
				AppId       = ReadPragmaInt("PRAGMA application_id");
				sqlite3_close(SrcDB);
			}
		}

		ExecSQL(TCHAR_TO_UTF8(*FString::Printf(TEXT("PRAGMA user_version = %lld"),  UserVersion)));
		ExecSQL(TCHAR_TO_UTF8(*FString::Printf(TEXT("PRAGMA application_id = %lld"), AppId)));
		ExecSQL("DETACH DATABASE schema_src");
	}

	bool bSuccess        = true;
	bool bMetadataWritten = false;

	for (int32 i = 0; i < SourcePaths.Num() && bSuccess; ++i)
	{
		FString EscapedPath = SourcePaths[i].Replace(TEXT("'"), TEXT("''"));
		if (!ExecSQL(TCHAR_TO_UTF8(*FString::Printf(TEXT("ATTACH DATABASE '%s' AS src"), *EscapedPath))))
		{
			bSuccess = false;
			break;
		}

		ExecSQL("BEGIN TRANSACTION");

		bSuccess = bSuccess && ExecSQL("INSERT OR IGNORE INTO keys (key, last_accessed) SELECT key, last_accessed FROM src.keys");
		bSuccess = bSuccess && ExecSQL(
			"INSERT OR IGNORE INTO vals (keyid, type, value) "
			"SELECT dk.keyid, sv.type, sv.value "
			"FROM src.vals sv "
			"JOIN src.keys sk ON sk.keyid = sv.keyid "
			"JOIN keys dk ON dk.key = sk.key");
		bSuccess = bSuccess && ExecSQL("INSERT OR IGNORE INTO groupkeys (key, version) SELECT key, version FROM src.groupkeys");
		bSuccess = bSuccess && ExecSQL(
			"INSERT OR IGNORE INTO groups (groupkeyid, valuekeyid) "
			"SELECT dgk.keyid, dk.keyid "
			"FROM src.groups sg "
			"JOIN src.groupkeys sgk ON sgk.keyid = sg.groupkeyid "
			"JOIN groupkeys dgk ON dgk.key = sgk.key "
			"JOIN src.keys sk ON sk.keyid = sg.valuekeyid "
			"JOIN keys dk ON dk.key = sk.key");

		if (!bMetadataWritten)
		{
			bSuccess = bSuccess && ExecSQL("INSERT OR IGNORE INTO compiler_id (id, abi, version, family) SELECT id, abi, version, family FROM src.compiler_id");
			bSuccess = bSuccess && ExecSQL("INSERT OR IGNORE INTO app_id (id, exe_path, app_name, engine_name, app_version, engine_version, app_profile_version) SELECT id, exe_path, app_name, engine_name, app_version, engine_version, app_profile_version FROM src.app_id");
			bMetadataWritten = true;
		}

		ExecSQL(bSuccess ? "COMMIT" : "ROLLBACK");
		ExecSQL("DETACH DATABASE src");
	}

	if (bSuccess)
	{
		auto QueryInt = [&](const char* SQL) -> int32
		{
			sqlite3_stmt* Stmt = nullptr;
			sqlite3_prepare_v2(DestDB, SQL, -1, &Stmt, nullptr);
			int32 Val = 0;
			if (sqlite3_step(Stmt) == SQLITE_ROW) { Val = sqlite3_column_int(Stmt, 0); }
			sqlite3_finalize(Stmt);
			return Val;
		};
		OutMergeSeconds = FPlatformTime::Seconds() - StartTime;
		UE_LOG(LogASDTool, Display, TEXT("  Merged %d PSDBs -> %d compiled shaders, %d PSO groups in %.2fs: %s"),
			SourcePaths.Num(),
			QueryInt("SELECT COUNT(*) FROM keys"),
			QueryInt("SELECT COUNT(*) FROM groups"),
			OutMergeSeconds, *FPaths::GetCleanFilename(DestPath));
	}
	else
	{
		UE_LOG(LogASDTool, Error, TEXT("MergePSDBs failed -- partial data may exist in dest."));
	}

	sqlite3_close(DestDB);
	return bSuccess;
}

} // namespace ASDTool
