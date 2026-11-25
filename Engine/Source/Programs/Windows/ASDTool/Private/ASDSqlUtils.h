// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SODBFile.h"

// Forward declarations for SQLite -- all SQLite code lives in ASDSqlUtils.cpp
struct sqlite3;
struct sqlite3_stmt;

namespace ASDTool
{

struct FD3D12GraphicsPSOState;

//--------------------------------------------------------------------------------------------------
// FSqlSODBFile  -- Writes/reads SODB as a SQLite database directly (fast, single transaction).
// All method implementations live in ASDSqlUtils.cpp.
//--------------------------------------------------------------------------------------------------

class FSqlSODBFile : public ISODBFile
{
public:
	~FSqlSODBFile();

	virtual bool Open(const FString& Path, ESODBOpenMode Mode) override;
	virtual void SetApplicationDesc(const D3D12_APPLICATION_DESC& AppDesc) override;
	virtual bool WriteEntries(const TArray<FSODBEntry>& Entries) override;
	virtual bool ReadEntries(TArray<FSODBEntry>& OutEntries) override;
	virtual void Close() override;
	virtual double GetWriteTimeSeconds() const override;

private:
	sqlite3*       DB          = nullptr;
	volatile int64 WriteCycles = 0;
	ESODBOpenMode  OpenMode    = ESODBOpenMode::Write;
	FString        FilePath;
	D3D12_APPLICATION_DESC CachedAppDesc = {};
	FString CachedExeFilename;
	FString CachedAppName;
	FString CachedEngineName;

	// Compute + shared statements
	sqlite3_stmt* StmtRootSig           = nullptr;
	sqlite3_stmt* StmtBytecode          = nullptr;
	sqlite3_stmt* StmtPSO               = nullptr;
	sqlite3_stmt* StmtGroup             = nullptr;

	// Graphics state sub-table statements
	sqlite3_stmt* StmtPSO_Graphics      = nullptr;
	sqlite3_stmt* StmtDepthStencilOp    = nullptr;  // depth_stencil_op_descs
	sqlite3_stmt* StmtDepthStencil      = nullptr;  // depth_stencil_descs
	sqlite3_stmt* StmtRTBlend           = nullptr;  // render_target_blend_descs
	sqlite3_stmt* StmtBlend             = nullptr;  // blend_descs
	sqlite3_stmt* StmtRasterizer        = nullptr;  // rasterizer_descs
	sqlite3_stmt* StmtRTFormats         = nullptr;  // render_target_formats
	sqlite3_stmt* StmtInputElement      = nullptr;  // input_element_descs
	sqlite3_stmt* StmtInputLayoutAssoc  = nullptr;  // input_layout_to_input_element_associations

	void Cleanup();
	void ExecSQL(const char* SQL);
	void PrepareSQLite(const char* SQL, sqlite3_stmt** OutStmt);
	void CreateSchema();

	// Helper: insert all graphics state sub-table rows and return their FK keys.
	// Private member so it can access all statement members directly.
	struct FGraphicsStateKeys
	{
		uint8 DepthStencilKey[16]  = {};
		uint8 RTFormatsKey[16]     = {};
		uint8 BlendKey[16]         = {};
		uint8 RasterizerKey[16]    = {};
		uint8 InputLayoutKey[16]   = {};
		bool  bHasInputLayout      = false;
	};
	FGraphicsStateKeys InsertGraphicsState(
		const FD3D12GraphicsPSOState& State,
		const TArray<D3D12_INPUT_ELEMENT_DESC>& InputElements);
};

//--------------------------------------------------------------------------------------------------
// Helpers
//--------------------------------------------------------------------------------------------------

/** Merge N per-thread PSDBs into a single destination PSDB via SQLite ATTACH.
 *  All inserts are content-addressed (INSERT OR IGNORE), keyids remapped through key content. */
bool MergePSDBs(const TArray<FString>& SourcePaths, const FString& DestPath, double& OutMergeSeconds);

} // namespace ASDTool
