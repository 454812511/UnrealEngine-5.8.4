// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ASDToolCommands.h"
#include "ASDD3D12Utils.h"
#include "ShaderCodeArchive.h"

namespace ASDTool
{

//--------------------------------------------------------------------------------------------------
// FSODBEntry  -- A single PSO entry from an SODB, shared between generation and compilation.
//--------------------------------------------------------------------------------------------------

struct FSODBEntry
{
	TArray<uint8> Key;           // Opaque PSO key -- 8 bytes (FShaderHash is FXxHash64, stored big-endian via ToByteArray)
	uint32 Version = 0;
	TArray<uint8> StreamData;    // Owns the pipeline state stream memory

	// Whether this is a graphics PSO (vs compute)
	bool bIsGraphics = false;

	// Per-stage raw DXBC for the direct SQLite read/write path.
	// Both StreamData and StageDXBC can be populated simultaneously (e.g. for compute, StoreComputeShader
	// fills StreamData for the D3D12 API write path and StageDXBC[SF_Compute] for the SQLite write path).
	// Callers should use whichever field is relevant to their write path.
	TArray<uint8> StageDXBC[SF_NumStandardFrequencies];

	TArray<uint8> RootSigData;             // Serialized root signature bytes (populated by SQLite read)
	TRefCountPtr<ID3DBlob> RootSigBlob;    // Serialized root signature as ID3DBlob (populated by GenerateSODB)

	// Owned storage for the D3D12_INPUT_ELEMENT_DESC array that the INPUT_LAYOUT subobject in StreamData
	// points into. Must remain alive alongside StreamData for the duration of any D3D12 API call.
	TArray<D3D12_INPUT_ELEMENT_DESC> InputElements;

	/** Get a D3D12_PIPELINE_STATE_STREAM_DESC pointing into StreamData. */
	D3D12_PIPELINE_STATE_STREAM_DESC GetDesc() const
	{
		D3D12_PIPELINE_STATE_STREAM_DESC Desc = {};
		Desc.SizeInBytes = StreamData.Num();
		Desc.pPipelineStateSubobjectStream = const_cast<uint8*>(StreamData.GetData());
		return Desc;
	}
};

//--------------------------------------------------------------------------------------------------
// FSODBGroup
// One per output .sodb file. Collects FSODBEntry instances during PSO collection phases, then
// writes them all in a single batch when WriteSODBFiles is called.
// WantedHashes (from SHK files) defines which compute shaders belong to this group for routing.
// All public methods are thread-safe.
//--------------------------------------------------------------------------------------------------

struct FSODBGroup
{
	FString Name;
	TSet<FShaderHash> WantedHashes;
	TSet<FShaderHash> WrittenHashes;
	FRWLock WrittenLock;
	int32 NumFailed = 0;
	volatile int64 TotalDXILBytes = 0;

	/** Batch-collected entries -- written by WriteSODBFiles. */
	TArray<FSODBEntry> CollectedEntries;
	FCriticalSection EntriesLock;

	/** Check if this group wants the hash. No lock needed -- WantedHashes is immutable after SetupGroups. */
	bool Contains(const FShaderHash& Hash) const;

	/** Build an FSODBEntry from compute shader data and add to CollectedEntries. Thread-safe. */
	bool StoreComputeShader(const FShaderHash& Hash, const uint8* DXBCData, int64 DXBCSize, ID3DBlob* RootSignatureBlob);

	/** Build an FSODBEntry for a graphics PSO and add to CollectedEntries. Thread-safe. */
	bool StoreGraphicsPSO(
		const FShaderHash& Hash,
		const TArray<uint8> StageDXBC[SF_NumGraphicsFrequencies],
		ID3DBlob* RootSignatureBlob,
		TArray<uint8>&& StreamBuffer,
		TArray<D3D12_INPUT_ELEMENT_DESC>&& InputElements);

private:
	/** Returns true if this is the first time we've seen this hash. Thread-safe. */
	bool DeduplicateAndMark(const FShaderHash& Hash);
};

//--------------------------------------------------------------------------------------------------
// FSODBFactory  -- Loads D3D12Core.dll once, creates ID3D12StateObjectDatabase instances.
// Implementation in SODBFile.cpp.
//--------------------------------------------------------------------------------------------------

class FSODBFactory
{
public:
	~FSODBFactory();

	bool Initialize();
	ID3D12StateObjectDatabase* CreateDatabase(const FString& OutputPath);

private:
	HMODULE D3D12Module = nullptr;
	ID3D12StateObjectDatabaseFactory* Factory = nullptr;
};

//--------------------------------------------------------------------------------------------------
// ESODBOpenMode  -- Whether to open an SODB for reading or writing.
//--------------------------------------------------------------------------------------------------

enum class ESODBOpenMode : uint8
{
	Read,
	Write
};

//--------------------------------------------------------------------------------------------------
// ISODBFile  -- Abstract interface for reading/writing SODB files.
//--------------------------------------------------------------------------------------------------

class ISODBFile
{
public:
	virtual ~ISODBFile() = default;

	/** Open an SODB file for reading or writing. */
	virtual bool Open(const FString& Path, ESODBOpenMode Mode) = 0;

	/** Set application metadata. */
	virtual void SetApplicationDesc(const D3D12_APPLICATION_DESC& AppDesc) = 0;

	/** Write all entries to the SODB. */
	virtual bool WriteEntries(const TArray<FSODBEntry>& Entries) = 0;

	/** Read all entries from the SODB. */
	virtual bool ReadEntries(TArray<FSODBEntry>& OutEntries) = 0;

	/** Close/finalize the database. */
	virtual void Close() = 0;

	/** Get timing stats in seconds. */
	virtual double GetWriteTimeSeconds() const = 0;
};

//--------------------------------------------------------------------------------------------------
// FD3D12SODBFile  -- Writes/reads via ID3D12StateObjectDatabase API (official).
// Writes a key manifest file (.keys) alongside the SODB for enumeration.
//--------------------------------------------------------------------------------------------------

class FD3D12SODBFile : public ISODBFile
{
public:
	~FD3D12SODBFile();

	/** Set the factory to use. Must be called before Open(). */
	void SetFactory(FSODBFactory* InFactory) { Factory = InFactory; }

	virtual bool Open(const FString& Path, ESODBOpenMode Mode) override;
	virtual void SetApplicationDesc(const D3D12_APPLICATION_DESC& AppDesc) override;
	virtual bool WriteEntries(const TArray<FSODBEntry>& Entries) override;
	virtual bool ReadEntries(TArray<FSODBEntry>& OutEntries) override;
	virtual void Close() override;
	virtual double GetWriteTimeSeconds() const override;

private:
	FSODBFactory* Factory = nullptr;
	ID3D12StateObjectDatabase* Database = nullptr;
	FString FilePath;
	volatile int64 WriteCycles = 0;
};

//--------------------------------------------------------------------------------------------------
// Helpers
//--------------------------------------------------------------------------------------------------

/** Read PSO entries from an SODB via direct SQLite. (D3D12 API read path is disabled -- retrieved data is not usable by the compiler.) */
bool ReadSODBEntries(const FString& SODBPath, TArray<FSODBEntry>& OutEntries);


};