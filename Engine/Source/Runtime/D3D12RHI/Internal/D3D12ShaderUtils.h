// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "D3D12ThirdParty.h"
#include "RHI.h"

/**
 * Build a serialized root signature for a compute shader from its UE shader blob.
 * Uses the same root signature construction logic as RHICreateComputeShader.
 *
 * @param ShaderCode            Full UE shader blob (SRT + native bytecode + optional data)
 * @param ResourceBindingTier   Resource binding tier to use for the root signature layout
 * @param OutRootSigBlob        Receives the serialized root signature blob
 * @param OutNativeBytecodeOffset  Receives the offset to native DXBC/DXIL bytecode within ShaderCode
 * @return true on success
 */
D3D12RHI_API bool BuildComputeShaderRootSignature(
	const TConstArrayView<uint8>& ShaderCode,
	D3D12_RESOURCE_BINDING_TIER ResourceBindingTier,
	TRefCountPtr<ID3DBlob>& OutRootSigBlob,
	int32& OutNativeBytecodeOffset);

/**
 * Build a serialized root signature for a graphics PSO from per-stage UE shader blobs.
 *
 * @param ShaderBlobs           Fixed array of full UE shader blobs indexed by EShaderFrequency
 * @param ResourceBindingTier   Resource binding tier to use for the root signature layout
 * @param bHasInputLayout       Whether the PSO has a vertex input layout
 * @param OutRootSigBlob        Receives the serialized root signature blob
 * @return true on success
 */
D3D12RHI_API bool BuildGraphicsShaderRootSignature(
	const TArray<uint8> ShaderBlobs[SF_NumGraphicsFrequencies],
	D3D12_RESOURCE_BINDING_TIER ResourceBindingTier,
	bool bHasInputLayout,
	TRefCountPtr<ID3DBlob>& OutRootSigBlob
	);

/**
 * Translate FBlendStateInitializerRHI to D3D12_BLEND_DESC.
 *
 * @param Initializer   RHI blend state initializer
 * @param NumActiveRTs  Number of active render targets; slots beyond this are zeroed
 * @param OutDesc       Receives the translated D3D12_BLEND_DESC
 */
D3D12RHI_API void TranslateBlendState(
	const FBlendStateInitializerRHI& Initializer,
	uint32 NumActiveRTs,
	D3D12_BLEND_DESC& OutDesc);

/**
 * Translate FRasterizerStateInitializerRHI to D3D12_RASTERIZER_DESC.
 *
 * @param Initializer   RHI rasterizer state initializer
 * @param OutDesc       Receives the translated D3D12_RASTERIZER_DESC
 */
D3D12RHI_API void TranslateRasterizerState(
	const FRasterizerStateInitializerRHI& Initializer,
	D3D12_RASTERIZER_DESC& OutDesc);

/**
 * Translate FDepthStencilStateInitializerRHI to D3D12_DEPTH_STENCIL_DESC1.
 *
 * @param Initializer   RHI depth/stencil state initializer
 * @param bDepthBounds  Whether depth bounds testing is enabled (caller resolves GSupportsDepthBoundsTest)
 * @param OutDesc       Receives the translated D3D12_DEPTH_STENCIL_DESC1
 */
D3D12RHI_API void TranslateDepthStencilState(
	const FDepthStencilStateInitializerRHI& Initializer,
	bool bDepthBounds,
	D3D12_DEPTH_STENCIL_DESC1& OutDesc);

/**
 * Translate a FVertexDeclarationElementList to a TArray<D3D12_INPUT_ELEMENT_DESC>.
 *
 * @param Elements      UE vertex declaration element list
 * @param OutElements   Receives the translated D3D12 input element descs
 */
D3D12RHI_API void TranslateVertexDeclaration(
	const FVertexDeclarationElementList& Elements,
	TArray<D3D12_INPUT_ELEMENT_DESC>& OutElements);

/**
 * Initialize the GPixelFormats platform format fields for D3D12.
 *
 * @param bUseD24  If true, PF_DepthStencil maps to R24G8_TYPELESS (D24_UNORM_S8_UINT);
 *                 if false (default), it maps to R32G8X24_TYPELESS (D32_FLOAT_S8X24_UINT).
 */
D3D12RHI_API void InitializeD3D12PixelFormats(bool bUseD24);
