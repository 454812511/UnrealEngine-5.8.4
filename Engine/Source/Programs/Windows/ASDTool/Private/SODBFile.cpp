// Copyright Epic Games, Inc. All Rights Reserved.

#include "SODBFile.h"
#include "ASDToolUtil.h"
#include "ASDSqlUtils.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformAtomics.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/SecureHash.h"

namespace ASDTool
{

//--------------------------------------------------------------------------------------------------
// FSODBGroup
//--------------------------------------------------------------------------------------------------

bool FSODBGroup::Contains(const FShaderHash& Hash) const
{
	return WantedHashes.Contains(Hash);
}

bool FSODBGroup::DeduplicateAndMark(const FShaderHash& Hash)
{
	{
		FReadScopeLock ReadLock(WrittenLock);
		if (WrittenHashes.Contains(Hash))
		{
			return false;
		}
	}
	{
		FWriteScopeLock WriteLock(WrittenLock);
		WrittenHashes.Add(Hash);
	}
	return true;
}

bool FSODBGroup::StoreComputeShader(const FShaderHash& Hash, const uint8* DXBCData, int64 DXBCSize, ID3DBlob* RootSignatureBlob)
{
	if (!DeduplicateAndMark(Hash))
	{
		return false;
	}

	FPlatformAtomics::InterlockedAdd(&TotalDXILBytes, DXBCSize);

	// Build PSO stream desc (same layout as the old FD3D12SODBWriter)
	struct FComputePipelineStream
	{
		struct
		{
			D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type;
			D3D12_SHADER_BYTECODE CS;
		} CSSubobject;
	};

	FComputePipelineStream Stream = {};
	// NOTE: pShaderBytecode is set after the DXBC is copied into Entry.StageDXBC[SF_Compute]
	// below, so the stream points at the entry-owned buffer rather than the caller's pointer.
	Stream.CSSubobject.Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;

	FSODBEntry Entry;
	uint8 KeyBytes[sizeof(uint64)];  // FShaderHash is FXxHash64 -- ToByteArray writes exactly 8 bytes
	Hash.ToByteArray(KeyBytes);
	Entry.Key.SetNumUninitialized(sizeof(KeyBytes));
	FMemory::Memcpy(Entry.Key.GetData(), KeyBytes, sizeof(KeyBytes));
	Entry.Version = 0;

	// Store DXBC for the direct SQLite path (compute = SF_Compute index).
	// Also point the stream's pShaderBytecode at this owned copy so StreamData doesn't
	// dangle when the caller's DXBCData pointer goes out of scope.
	Entry.StageDXBC[SF_Compute].SetNumUninitialized(DXBCSize);
	FMemory::Memcpy(Entry.StageDXBC[SF_Compute].GetData(), DXBCData, DXBCSize);
	Stream.CSSubobject.CS.pShaderBytecode = Entry.StageDXBC[SF_Compute].GetData();
	Stream.CSSubobject.CS.BytecodeLength  = (SIZE_T)DXBCSize;

	// Store root signature blob for the direct SQLite path
	if (RootSignatureBlob)
	{
		Entry.RootSigBlob = RootSignatureBlob;
	}

	// Build StreamData for the D3D12 API path
	if (RootSignatureBlob)
	{
		struct FRootSigSubobject
		{
			D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type;
			D3D12_SERIALIZED_ROOT_SIGNATURE_DESC RootSig;
		};

		FRootSigSubobject RootSigSubobject = {};
		RootSigSubobject.Type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE;
		RootSigSubobject.RootSig.pSerializedBlob = RootSignatureBlob->GetBufferPointer();
		RootSigSubobject.RootSig.SerializedBlobSizeInBytes = RootSignatureBlob->GetBufferSize();

		Entry.StreamData.SetNumUninitialized(sizeof(FRootSigSubobject) + sizeof(FComputePipelineStream));
		FMemory::Memcpy(Entry.StreamData.GetData(), &RootSigSubobject, sizeof(FRootSigSubobject));
		FMemory::Memcpy(Entry.StreamData.GetData() + sizeof(FRootSigSubobject), &Stream, sizeof(FComputePipelineStream));
	}
	else
	{
		Entry.StreamData.SetNumUninitialized(sizeof(FComputePipelineStream));
		FMemory::Memcpy(Entry.StreamData.GetData(), &Stream, sizeof(FComputePipelineStream));
	}

	{
		FScopeLock Lock(&EntriesLock);
		CollectedEntries.Add(MoveTemp(Entry));
	}

	return true;
}

bool FSODBGroup::StoreGraphicsPSO(
	const FShaderHash& Hash,
	const TArray<uint8> StageDXBC[SF_NumGraphicsFrequencies],
	ID3DBlob* RootSignatureBlob,
	TArray<uint8>&& StreamBuffer,
	TArray<D3D12_INPUT_ELEMENT_DESC>&& InputElements)
{
	if (!DeduplicateAndMark(Hash))
	{
		return false;
	}

	FSODBEntry Entry;
	Entry.bIsGraphics = true;

	uint8 GraphicsKeyBytes[sizeof(uint64)];  // FShaderHash is FXxHash64 -- ToByteArray writes exactly 8 bytes
	Hash.ToByteArray(GraphicsKeyBytes);
	Entry.Key.SetNumUninitialized(sizeof(GraphicsKeyBytes));
	FMemory::Memcpy(Entry.Key.GetData(), GraphicsKeyBytes, sizeof(GraphicsKeyBytes));
	Entry.Version = 0;

	// Store per-stage DXBC for the direct SQLite read/write path
	for (int32 FreqIdx = 0; FreqIdx < SF_NumGraphicsFrequencies; ++FreqIdx)
	{
		Entry.StageDXBC[FreqIdx] = StageDXBC[FreqIdx];
	}

	// Store root signature blob for the direct SQLite path
	if (RootSignatureBlob)
	{
		Entry.RootSigBlob = RootSignatureBlob;
	}

	// Move the pipeline state stream and input elements into the entry.
	// Append INPUT_LAYOUT after InputElements is in stable storage.
	Entry.StreamData    = MoveTemp(StreamBuffer);
	Entry.InputElements = MoveTemp(InputElements);
	AppendInputLayoutSubobject(Entry.StreamData, Entry.InputElements);

	{
		FScopeLock Lock(&EntriesLock);
		CollectedEntries.Add(MoveTemp(Entry));
	}

	return true;
}

//--------------------------------------------------------------------------------------------------
// FSODBFactory
//--------------------------------------------------------------------------------------------------

FSODBFactory::~FSODBFactory()
{
	if (Factory)
	{
		Factory->Release();
		Factory = nullptr;
	}
	if (D3D12Module)
	{
		FreeLibrary(D3D12Module);
		D3D12Module = nullptr;
	}
}

bool FSODBFactory::Initialize()
{
	const FString D3D12CorePath = FindAgilitySDKFile(TEXT("D3D12Core.dll"));
	if (D3D12CorePath.IsEmpty())
	{
		UE_LOGF(LogASDTool, Error, "D3D12Core.dll not found in AgilitySDK directories. Ensure Agility SDK 1.618+ is available.");
		return false;
	}

	D3D12Module = LoadLibrary(*D3D12CorePath);
	if (!D3D12Module)
	{
		UE_LOGF(LogASDTool, Error, "Failed to load D3D12Core.dll from: %ls", *D3D12CorePath);
		return false;
	}

	auto GetInterfaceFunc = reinterpret_cast<PFN_D3D12_GET_INTERFACE>(reinterpret_cast<void*>(GetProcAddress(D3D12Module, "D3D12GetInterface")));
	if (!GetInterfaceFunc)
	{
		UE_LOGF(LogASDTool, Error, "D3D12GetInterface not found in D3D12Core.dll");
		FreeLibrary(D3D12Module);
		D3D12Module = nullptr;
		return false;
	}

	HRESULT hr = GetInterfaceFunc(CLSID_D3D12StateObjectFactory, IID_PPV_ARGS(&Factory));
	if (FAILED(hr) || !Factory)
	{
		UE_LOGF(LogASDTool, Error, "Failed to create ID3D12StateObjectDatabaseFactory: 0x%08X", hr);
		FreeLibrary(D3D12Module);
		D3D12Module = nullptr;
		return false;
	}

	return true;
}

ID3D12StateObjectDatabase* FSODBFactory::CreateDatabase(const FString& OutputPath)
{
	ID3D12StateObjectDatabase* Database = nullptr;
	HRESULT hr = Factory->CreateStateObjectDatabaseFromFile(
		*OutputPath,
		D3D12_STATE_OBJECT_DATABASE_FLAG_NONE,
		IID_PPV_ARGS(&Database));

	if (FAILED(hr) || !Database)
	{
		UE_LOGF(LogASDTool, Error, "Failed to create SODB file '%ls': 0x%08X", *OutputPath, hr);
		return nullptr;
	}

	return Database;
}

//--------------------------------------------------------------------------------------------------
// FD3D12SODBFile
//--------------------------------------------------------------------------------------------------

FD3D12SODBFile::~FD3D12SODBFile()
{
	Close();
}

bool FD3D12SODBFile::Open(const FString& Path, ESODBOpenMode Mode)
{
	FilePath = Path;

	if (!Factory)
	{
		UE_LOGF(LogASDTool, Error, "FD3D12SODBFile::Open  -- no factory set. Call SetFactory() before Open().");
		return false;
	}

	Database = Factory->CreateDatabase(Path);
	return Database != nullptr;
}

void FD3D12SODBFile::SetApplicationDesc(const D3D12_APPLICATION_DESC& AppDesc)
{
	if (Database)
	{
		Database->SetApplicationDesc(&AppDesc);
	}
}

template<typename T>
static void ManifestAppend(TArray<uint8>& Buffer, const T& Value)
{
	Buffer.AddUninitialized(sizeof(T));
	FMemory::Memcpy(Buffer.GetData() + Buffer.Num() - sizeof(T), &Value, sizeof(T));
}

bool FD3D12SODBFile::WriteEntries(const TArray<FSODBEntry>& Entries)
{
	if (!Database)
	{
		return false;
	}

	bool bAllSucceeded = true;
	for (const FSODBEntry& Entry : Entries)
	{
		D3D12_PIPELINE_STATE_STREAM_DESC StreamDesc = Entry.GetDesc();

		int64 StartTime = FPlatformTime::Cycles64();
		HRESULT hr = Database->StorePipelineStateDesc(
			Entry.Key.GetData(), Entry.Key.Num(),
			Entry.Version,
			&StreamDesc);
		FPlatformAtomics::InterlockedAdd(&WriteCycles, FPlatformTime::Cycles64() - StartTime);

		if (FAILED(hr))
		{
			bAllSucceeded = false;
			UE_LOG(LogASDTool, Warning, TEXT("StorePipelineStateDesc failed (hr=0x%08X) for %s PSO key=%s"),
				hr,
				Entry.bIsGraphics ? TEXT("graphics") : TEXT("compute"),
				*FString::FromHexBlob(Entry.Key.GetData(), Entry.Key.Num()));
		}
	}

	// Write key manifest file (<sodb_path>.keys) containing all keys+versions
	{
		FString KeysPath = FilePath + TEXT(".keys");
		TArray<uint8> ManifestData;

		int32 Count = Entries.Num();
		ManifestAppend(ManifestData, Count);

		for (const FSODBEntry& Entry : Entries)
		{
			int32 KeySize = Entry.Key.Num();
			ManifestAppend(ManifestData, KeySize);
			ManifestData.Append(Entry.Key.GetData(), KeySize);
			ManifestAppend(ManifestData, Entry.Version);
		}

		if (!FFileHelper::SaveArrayToFile(ManifestData, *KeysPath))
		{
			UE_LOGF(LogASDTool, Warning, "Failed to write key manifest: %ls (SODB read will fall back to SQLite)", *KeysPath);
		}
	}

	return bAllSucceeded;
}

template<typename T>
static bool ManifestRead(const uint8*& Ptr, const uint8* End, T& OutValue)
{
	if (Ptr + sizeof(T) > End)
	{
		return false;
	}
	FMemory::Memcpy(&OutValue, Ptr, sizeof(T));
	Ptr += sizeof(T);
	return true;
}

bool FD3D12SODBFile::ReadEntries(TArray<FSODBEntry>& OutEntries)
{
	if (!Database)
	{
		return false;
	}

	// Try to read key manifest first
	FString KeysPath = FilePath + TEXT(".keys");
	TArray<uint8> ManifestData;

	if (FFileHelper::LoadFileToArray(ManifestData, *KeysPath) && ManifestData.Num() >= sizeof(int32))
	{
		const uint8* Ptr = ManifestData.GetData();
		const uint8* End = ManifestData.GetData() + ManifestData.Num();

		int32 Count = 0;
		if (!ManifestRead(Ptr, End, Count))
		{
			return false;
		}

		for (int32 i = 0; i < Count; ++i)
		{
			int32 KeySize = 0;
			if (!ManifestRead(Ptr, End, KeySize))
			{
				break;
			}

			// Validate KeySize before allocating
			const int64 Remaining = End - Ptr;
			if (KeySize < 0 || int64(KeySize) + int64(sizeof(uint32)) > Remaining)
			{
				break;
			}

			FSODBEntry& Entry = OutEntries.Emplace_GetRef();
			Entry.Key.SetNumUninitialized(KeySize);
			FMemory::Memcpy(Entry.Key.GetData(), Ptr, KeySize);
			Ptr += KeySize;

			if (!ManifestRead(Ptr, End, Entry.Version))
			{
				break;
			}

			// Retrieve the stream data from the database
			struct FCallback
			{
				static void __stdcall OnEntry(const void* pKey, UINT KeySz, UINT Ver, const D3D12_PIPELINE_STATE_STREAM_DESC* pDesc, void* pContext)
				{
					FSODBEntry* OutEntry = static_cast<FSODBEntry*>(pContext);
					if (pDesc && pDesc->SizeInBytes > 0 && pDesc->pPipelineStateSubobjectStream)
					{
						OutEntry->StreamData.SetNumUninitialized(pDesc->SizeInBytes);
						FMemory::Memcpy(OutEntry->StreamData.GetData(), pDesc->pPipelineStateSubobjectStream, pDesc->SizeInBytes);
					}
				}
			};

			Database->FindPipelineStateDesc(
				Entry.Key.GetData(), Entry.Key.Num(),
				&FCallback::OnEntry,
				&Entry);
		}

		UE_LOG(LogASDTool, Display, TEXT("  Read %d PSO entries from SODB (via manifest): %s"), OutEntries.Num(), *FPaths::GetCleanFilename(FilePath));
		return true;
	}

	// No manifest file -- treat SODB as empty
	UE_LOG(LogASDTool, Warning, TEXT("No key manifest found for SODB: %s (no .keys file)"), *FPaths::GetCleanFilename(FilePath));
	return true;
}

void FD3D12SODBFile::Close()
{
	if (Database)
	{
		Database->Release();
		Database = nullptr;
	}
}

double FD3D12SODBFile::GetWriteTimeSeconds() const
{
	return FPlatformTime::ToSeconds64(WriteCycles);
}

//--------------------------------------------------------------------------------------------------
// Helpers
//--------------------------------------------------------------------------------------------------

bool ReadSODBEntries(const FString& SODBPath, TArray<FSODBEntry>& OutEntries)
{
	const FString Filename = FPaths::GetCleanFilename(SODBPath);

	// Disable the D3D12 API read path -- the retrieved data cannot be used directly for PSO compilation
	// as noted in the spec: May contain CD3DX12_GLOBAL_SERIALIZED_ROOT_SIGNATURE_SUBOBJECT or
	// CD3DX12_LOCAL_SERIALIZED_ROOT_SIGNATURE_SUBOBJECT which cannot currently be consumed
	// by the compiler or runtime.
	bool bAllowD3D12ReadPath = false;
	FString KeysPath = SODBPath + TEXT(".keys");
	if (bAllowD3D12ReadPath && FPaths::FileExists(KeysPath))
	{
		FSODBFactory Factory;
		if (Factory.Initialize())
		{
			FD3D12SODBFile SODBFile;
			SODBFile.SetFactory(&Factory);
			if (SODBFile.Open(SODBPath, ESODBOpenMode::Read))
			{
				double StartTime = FPlatformTime::Seconds();
				if (SODBFile.ReadEntries(OutEntries))
				{
					UE_LOGF(LogASDTool, Display, "  Read %d entries via D3D12 API in %.3fs: %ls",
						OutEntries.Num(), FPlatformTime::Seconds() - StartTime, *Filename);
					SODBFile.Close();
					return true;
				}
				SODBFile.Close();
			}
		}
		UE_LOGF(LogASDTool, Warning, "D3D12 API read failed for '%ls', falling back to direct SQLite.", *Filename);
		OutEntries.Empty();
	}

	FSqlSODBFile DirectFile;
	if (!DirectFile.Open(SODBPath, ESODBOpenMode::Read))
	{
		UE_LOGF(LogASDTool, Error, "Failed to open SODB '%ls' for reading.", *SODBPath);
		return false;
	}

	double StartTime = FPlatformTime::Seconds();
	if (!DirectFile.ReadEntries(OutEntries))
	{
		UE_LOGF(LogASDTool, Error, "Failed to read entries from SODB '%ls'.", *SODBPath);
		DirectFile.Close();
		return false;
	}

	UE_LOGF(LogASDTool, Display, "  Read %d entries via direct SQLite in %.3fs: %ls",
		OutEntries.Num(), FPlatformTime::Seconds() - StartTime, *Filename);
	DirectFile.Close();
	return true;
}

}; // namespace ASDTool