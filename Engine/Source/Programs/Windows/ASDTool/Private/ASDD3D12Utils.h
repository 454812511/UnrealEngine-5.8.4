// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIDefinitions.h"
#include "PipelineFileCache.h"

// initguid.h must come before d3d12.h to instantiate CLSIDs (e.g. CLSID_D3D12StateObjectDatabaseFactory).
// Including it here ensures correct GUID instantiation for all translation units that include ASDD3D12Utils.h.
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <initguid.h>
#include <combaseapi.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <dxgi.h>
#include <d3dshadercacheregistration.h>
#include <d3d12compiler.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"

namespace ASDTool
{

//--------------------------------------------------------------------------------------------------
// FD3D12GraphicsPSOState
// Intermediate struct holding all translated D3D12 graphics PSO state.
// Bridges the write path (translated from FPipelineCacheFileFormatPSO::GraphicsDescriptor)
// and the read path (reconstructed directly from SQLite integer/real columns).
//--------------------------------------------------------------------------------------------------

struct FD3D12GraphicsPSOState
{
	FD3D12GraphicsPSOState() { FMemory::Memzero(*this); SampleMask = 0xFFFFFFFF; }

	D3D12_BLEND_DESC              BlendDesc;
	D3D12_RASTERIZER_DESC         RasterizerDesc;
	D3D12_DEPTH_STENCIL_DESC1     DepthStencilDesc;
	D3D12_RT_FORMAT_ARRAY         RTFormats;
	DXGI_FORMAT                   DSVFormat;
	DXGI_SAMPLE_DESC              SampleDesc;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType;
	uint32                        SampleMask;

	bool operator==(const FD3D12GraphicsPSOState& Other) const
	{
		return FMemory::Memcmp(this, &Other, sizeof(FD3D12GraphicsPSOState)) == 0;
	}
	bool operator!=(const FD3D12GraphicsPSOState& Other) const { return !(*this == Other); }
};

//--------------------------------------------------------------------------------------------------
// Stream helpers
// Append a PSO subobject token to a raw byte buffer.
// CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT overloads operator& to return &pssInner (payload only),
// so we cast through unsigned char& to get the true object address including the type field.
//--------------------------------------------------------------------------------------------------

template<typename SubobjectType>
inline void AppendSubobject(TArray<uint8>& Buffer, const SubobjectType& Subobject)
{
	const int32 Offset = Buffer.AddUninitialized(sizeof(SubobjectType));
	const void* RawAddr = &reinterpret_cast<const unsigned char&>(Subobject);
	FMemory::Memcpy(Buffer.GetData() + Offset, RawAddr, sizeof(SubobjectType));
}

/** Append a CD3DX12_PIPELINE_STATE_STREAM_INPUT_LAYOUT subobject to a stream buffer.
 *  InputElements must be in stable storage (not about to move/reallocate) since the
 *  subobject contains a pointer into the array.
 */
inline void AppendInputLayoutSubobject(TArray<uint8>& StreamBuffer, const TArray<D3D12_INPUT_ELEMENT_DESC>& InputElements)
{
	if (!InputElements.IsEmpty())
	{
		CD3DX12_PIPELINE_STATE_STREAM_INPUT_LAYOUT Sub;
		Sub = D3D12_INPUT_LAYOUT_DESC{ InputElements.GetData(), (UINT)InputElements.Num() };
		AppendSubobject(StreamBuffer, Sub);
	}
}

/**
 * Translate a pipeline cache graphics descriptor to D3D12 state structs and input elements.
 * Pure data mapping -- no RHI initialization required.
 *
 * @param Desc             Graphics PSO descriptor from a .upipelinecache file
 * @param OutState         Receives all translated D3D12 state (blend, rasterizer, DS, RT formats etc.)
 * @param OutInputElements Receives the input element descs for VS pipelines (empty for mesh shader pipelines)
 * @return true on success; false if the descriptor has neither a VS nor an MS (invalid pipeline)
 */
bool TranslateGraphicsDescToD3D12(
	const FPipelineCacheFileFormatPSO::GraphicsDescriptor& Desc,
	FD3D12GraphicsPSOState& OutState,
	TArray<D3D12_INPUT_ELEMENT_DESC>& OutInputElements);

/**
 * Assemble a complete D3D12 pipeline state stream from pre-translated D3D12 state structs.
 * Used by both the write path (after TranslateGraphicsDescToD3D12) and the read path
 * (after reconstructing FD3D12GraphicsPSOState from SQLite columns).
 *
 * Memory ownership:
 *   OutStreamBuffer -- owns the packed stream bytes; keep alive for any D3D12 API call.
 *
 * NOTE: INPUT_LAYOUT is intentionally NOT included in the stream. Callers must append it
 * separately via AppendInputLayoutSubobject after InputElements is in stable storage.
 *
 * @param State                Pre-translated D3D12 graphics PSO state
 * @param SerializedRootSigData Serialized root signature blob
 * @param SerializedRootSigSize Size of the root signature blob in bytes
 * @param StageDXBC            Raw DXBC per shader stage, indexed by EShaderFrequency (SF_NumGraphicsFrequencies entries)
 * @param OutStreamBuffer      Receives the packed pipeline state stream (owns the memory)
 * @return true on success
 */
bool BuildGraphicsPipelineStateStream(
	const FD3D12GraphicsPSOState& State,
	const void* SerializedRootSigData,
	SIZE_T SerializedRootSigSize,
	const TArray<uint8> StageDXBC[SF_NumGraphicsFrequencies],
	TArray<uint8>& OutStreamBuffer);

/**
 * Iterate over all subobjects in a D3D12 pipeline state stream.
 * The visitor receives the type, a pointer to the payload, and the payload size.
 * Return false from the visitor to stop iteration early.
 *
 * Handles all standard PSO subobject types. Stops with a warning on unknown types.
 */
void ForEachPSOSubobject(
	const TArray<uint8>& StreamBuffer,
	TFunctionRef<bool(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, const void* Payload, SIZE_T PayloadSize)> Visitor);

/**
 * Read back FD3D12GraphicsPSOState from a pipeline state stream built by BuildGraphicsPipelineStateStream.
 *
 * @return true if all expected state fields were found in the stream.
 */
bool ReadBackGraphicsPSOState(const TArray<uint8>& StreamBuffer, FD3D12GraphicsPSOState& OutState);

//--------------------------------------------------------------------------------------------------
// FPSODebugCompiler
// Creates a system D3D12 debug device and attempts to compile PSO streams via CreatePipelineState.
// The D3D12 debug layer provides detailed error messages that the IHV compiler DLL does not.
// Serialized root signatures (type 29) are automatically deserialized to live pointers.
//--------------------------------------------------------------------------------------------------

class FPSODebugCompiler
{
public:
	FPSODebugCompiler() = default;
	~FPSODebugCompiler();

	/** Initialize the system D3D12 debug device. Returns true on success. */
	bool Init();

	/** Shut down and release the device. Safe to call even if Init() was not called or failed. */
	void Shutdown();

	/** Returns true if Init() succeeded and the device is ready. */
	bool IsReady() const;

	/**
	 * Attempt to create a PSO from a pipeline state stream.
	 * Logs D3D12 debug layer messages on failure.
	 *
	 * @param StreamBuffer  Raw bytes of the D3D12 pipeline state stream
	 * @param Label         Human-readable label for log output (e.g. PSO key hex string)
	 * @return true if CreatePipelineState succeeded
	 */
	bool Compile(const TArray<uint8>& StreamBuffer, const FString& Label);

private:
	void FlushInfoQueue(const FString& Label);

	HMODULE             D3D12Module = nullptr;
	HMODULE             DXGIModule  = nullptr;
	ID3D12Device2*      Device      = nullptr;
	ID3D12InfoQueue*    InfoQueue   = nullptr;
};

} // namespace ASDTool
