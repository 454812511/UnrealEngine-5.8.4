// Copyright Epic Games, Inc. All Rights Reserved.

#include "ASDD3D12Utils.h"
#include "ASDToolCommands.h"

#include "RHIDefinitions.h"
#include "PixelFormat.h"
#include "D3D12ShaderUtils.h"

#include "DXGIUtilities.h"

// dxgi1_6.h needed for IDXGIFactory6 and DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE (PSO debug device)
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <dxgi1_6.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"

namespace ASDTool
{

static D3D12_PRIMITIVE_TOPOLOGY_TYPE TranslatePrimitiveType(EPrimitiveType PrimitiveType)
{
	switch (PrimitiveType)
	{
	case PT_TriangleList:
	case PT_TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	case PT_LineList:      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	case PT_PointList:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
	default:               return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	}
}

//--------------------------------------------------------------------------------------------------
// TranslateGraphicsDescToD3D12
//--------------------------------------------------------------------------------------------------

bool TranslateGraphicsDescToD3D12(
	const FPipelineCacheFileFormatPSO::GraphicsDescriptor& Desc,
	FD3D12GraphicsPSOState& OutState,
	TArray<D3D12_INPUT_ELEMENT_DESC>& OutInputElements)
{
	OutInputElements.Reset();

	const bool bHasVS = (Desc.VertexShader != FShaderHash());
	const bool bHasMS = (Desc.MeshShader   != FShaderHash());

	if (!bHasVS && !bHasMS)
	{
		return false;
	}

	// ---- Blend state -- shared translation function from D3D12ShaderUtils ----
	TranslateBlendState(Desc.BlendState, Desc.RenderTargetsActive, OutState.BlendDesc);

	// ---- Rasterizer state ----
	TranslateRasterizerState(FRasterizerStateInitializerRHI(Desc.RasterizerState), OutState.RasterizerDesc);

	// ---- Depth/stencil state ----
	// Note: no GSupportsDepthBoundsTest check in tool context -- use Desc.bDepthBounds directly
	TranslateDepthStencilState(Desc.DepthStencilState, Desc.bDepthBounds, OutState.DepthStencilDesc);

	// ---- Render target formats ----
	{
		D3D12_RT_FORMAT_ARRAY& RTArray = OutState.RTFormats;
		FMemory::Memzero(RTArray);
		RTArray.NumRenderTargets = Desc.RenderTargetsActive;
		for (uint32 RTIdx = 0; RTIdx < Desc.RenderTargetsActive; ++RTIdx)
		{
			const DXGI_FORMAT PlatformFormat = (DXGI_FORMAT)GPixelFormats[Desc.RenderTargetFormats[RTIdx]].PlatformFormat;
			const ETextureCreateFlags Flags  = FPipelineCacheFileFormatPSO::GraphicsDescriptor::ReduceRTFlags(Desc.RenderTargetFlags[RTIdx]);
			RTArray.RTFormats[RTIdx] = UE::DXGIUtilities::FindShaderResourceFormat(
				UE::DXGIUtilities::GetPlatformTextureResourceFormat(PlatformFormat, Flags),
				EnumHasAnyFlags(Flags, ETextureCreateFlags::SRGB));
		}
	}

	// ---- DSV format ----
	{
		const DXGI_FORMAT PlatformFormat = (DXGI_FORMAT)GPixelFormats[Desc.DepthStencilFormat].PlatformFormat;
		const ETextureCreateFlags Flags  = FPipelineCacheFileFormatPSO::GraphicsDescriptor::ReduceDSFlags(Desc.DepthStencilFlags);
		OutState.DSVFormat = UE::DXGIUtilities::FindDepthStencilFormat(
			UE::DXGIUtilities::GetPlatformTextureResourceFormat(PlatformFormat, Flags));
	}

	// ---- Sample desc ----
	OutState.SampleDesc.Count   = Desc.MSAASamples > 0 ? Desc.MSAASamples : 1;
	OutState.SampleDesc.Quality = 0;

	// ---- Sample mask ----
	OutState.SampleMask = 0xFFFFFFFF;

	// ---- Primitive topology ----
	OutState.PrimitiveTopologyType = TranslatePrimitiveType(Desc.PrimitiveType);

	// ---- Input layout (VS pipelines only) -- shared translation function from D3D12ShaderUtils ----
	if (bHasVS && !bHasMS)
	{
		TranslateVertexDeclaration(Desc.VertexDescriptor, OutInputElements);
	}

	return true;
}

//--------------------------------------------------------------------------------------------------
// BuildGraphicsPipelineStateStream
//--------------------------------------------------------------------------------------------------

bool BuildGraphicsPipelineStateStream(
	const FD3D12GraphicsPSOState& State,
	const void* SerializedRootSigData,
	SIZE_T SerializedRootSigSize,
	const TArray<uint8> StageDXBC[SF_NumGraphicsFrequencies],
	TArray<uint8>& OutStreamBuffer)
{
	OutStreamBuffer.Reset();

	const bool bHasVS = !StageDXBC[SF_Vertex].IsEmpty();
	const bool bHasMS = !StageDXBC[SF_Mesh].IsEmpty();

	if (!bHasVS && !bHasMS)
	{
		return false;
	}

	// ---- Serialized root signature (type 29) ----
	if (SerializedRootSigData && SerializedRootSigSize > 0)
	{
		CD3DX12_PIPELINE_STATE_STREAM_SERIALIZED_ROOT_SIGNATURE Sub;
		Sub = CD3DX12_SERIALIZED_ROOT_SIGNATURE_DESC(SerializedRootSigData, SerializedRootSigSize);
		AppendSubobject(OutStreamBuffer, Sub);
	}

	// ---- Shader bytecodes (only non-empty stages) ----
	auto AppendShaderStage = [&]<typename SubobjectType>(const TArray<uint8>& DXBC)
	{
		if (!DXBC.IsEmpty())
		{
			SubobjectType Sub = D3D12_SHADER_BYTECODE{ DXBC.GetData(), (SIZE_T)DXBC.Num() };
			AppendSubobject(OutStreamBuffer, Sub);
		}
	};
	AppendShaderStage.operator()<CD3DX12_PIPELINE_STATE_STREAM_VS>(StageDXBC[SF_Vertex]);
	AppendShaderStage.operator()<CD3DX12_PIPELINE_STATE_STREAM_PS>(StageDXBC[SF_Pixel]);
	AppendShaderStage.operator()<CD3DX12_PIPELINE_STATE_STREAM_GS>(StageDXBC[SF_Geometry]);
	AppendShaderStage.operator()<CD3DX12_PIPELINE_STATE_STREAM_MS>(StageDXBC[SF_Mesh]);
	AppendShaderStage.operator()<CD3DX12_PIPELINE_STATE_STREAM_AS>(StageDXBC[SF_Amplification]);

	// ---- Fixed state: appended individually to guarantee correct per-token alignment ----
	// Note: CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC, _RASTERIZER, and _DEPTH_STENCIL1 wrap
	// CD3DX12_BLEND_DESC / CD3DX12_RASTERIZER_DESC / CD3DX12_DEPTH_STENCIL_DESC1 as their inner
	// type (not the raw D3D12_* structs). 
	auto AppendFixed = [&]<typename SubobjectType>(const auto& InnerValue)
	{
		SubobjectType Sub = InnerValue;
		AppendSubobject(OutStreamBuffer, Sub);
	};
	AppendFixed.operator()<CD3DX12_PIPELINE_STATE_STREAM_NODE_MASK>        (1u);
	AppendFixed.operator()<CD3DX12_PIPELINE_STATE_STREAM_IB_STRIP_CUT_VALUE>(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED);
	AppendFixed.operator()<CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC>       (CD3DX12_BLEND_DESC(State.BlendDesc));
	AppendFixed.operator()<CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER>       (CD3DX12_RASTERIZER_DESC(State.RasterizerDesc));
	AppendFixed.operator()<CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL1>   (CD3DX12_DEPTH_STENCIL_DESC1(State.DepthStencilDesc));
	AppendFixed.operator()<CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS>(State.RTFormats);
	AppendFixed.operator()<CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL_FORMAT> (State.DSVFormat);
	AppendFixed.operator()<CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_DESC>      (State.SampleDesc);
	AppendFixed.operator()<CD3DX12_PIPELINE_STATE_STREAM_SAMPLE_MASK>      (State.SampleMask);
	AppendFixed.operator()<CD3DX12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY>(State.PrimitiveTopologyType);

	// NOTE: INPUT_LAYOUT is NOT appended here -- callers must use AppendInputLayoutSubobject
	// after InputElements is in stable storage.

	return true;
}

//--------------------------------------------------------------------------------------------------
// ForEachPSOSubobject
//--------------------------------------------------------------------------------------------------

namespace
{

/**
 * Bridges ID3DX12PipelineParserCallbacks to our TFunctionRef visitor.
 * Each overridden Cb() passes the payload address and size to the visitor.
 * Sets bStopped=true if the visitor returns false (early exit).
 * Ignores unknown/error callbacks silently (they indicate types we don't care about).
 */
struct FPSOParserAdapter : public ID3DX12PipelineParserCallbacks
{
	TFunctionRef<bool(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE, const void*, SIZE_T)> Visitor;
	bool bStopped = false;

	explicit FPSOParserAdapter(TFunctionRef<bool(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE, const void*, SIZE_T)> InVisitor)
		: Visitor(InVisitor)
	{
	}

	// Dispatch helper -- calls visitor with typed pointer and sizeof the payload struct.
	// Returns early (sets bStopped) if visitor returns false.
	template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE SubType, typename PayloadType>
	void Dispatch(const PayloadType& Payload)
	{
		if (!bStopped)
		{
			if (!Visitor(SubType, &Payload, sizeof(PayloadType)))
			{
				bStopped = true;
			}
		}
	}

	void FlagsCb(D3D12_PIPELINE_STATE_FLAGS Flags) override                                      { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS>(Flags); }
	void NodeMaskCb(UINT NodeMask) override                                                       { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK>(NodeMask); }
	void RootSignatureCb(ID3D12RootSignature* pRootSignature) override                            { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE>(pRootSignature); }
	void InputLayoutCb(const D3D12_INPUT_LAYOUT_DESC& InputLayout) override                      { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT>(InputLayout); }
	void IBStripCutValueCb(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue) override          { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE>(IBStripCutValue); }
	void PrimitiveTopologyTypeCb(D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimTopology) override            { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY>(PrimTopology); }
	void VSCb(const D3D12_SHADER_BYTECODE& VS) override                                          { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS>(VS); }
	void GSCb(const D3D12_SHADER_BYTECODE& GS) override                                          { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS>(GS); }
	void StreamOutputCb(const D3D12_STREAM_OUTPUT_DESC& StreamOutput) override                   { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT>(StreamOutput); }
	void HSCb(const D3D12_SHADER_BYTECODE& HS) override                                          { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS>(HS); }
	void DSCb(const D3D12_SHADER_BYTECODE& DS) override                                          { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS>(DS); }
	void PSCb(const D3D12_SHADER_BYTECODE& PS) override                                          { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS>(PS); }
	void CSCb(const D3D12_SHADER_BYTECODE& CS) override                                          { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS>(CS); }
	void ASCb(const D3D12_SHADER_BYTECODE& AS) override                                          { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS>(AS); }
	void MSCb(const D3D12_SHADER_BYTECODE& MS) override                                          { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS>(MS); }
	void BlendStateCb(const D3D12_BLEND_DESC& BlendState) override                               { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND>(BlendState); }
	void DepthStencilStateCb(const D3D12_DEPTH_STENCIL_DESC& DepthStencilState) override         { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL>(DepthStencilState); }
	void DepthStencilState1Cb(const D3D12_DEPTH_STENCIL_DESC1& DepthStencilState) override       { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1>(DepthStencilState); }
	void DSVFormatCb(DXGI_FORMAT DSVFormat) override                                             { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT>(DSVFormat); }
	void RasterizerStateCb(const D3D12_RASTERIZER_DESC& RasterizerState) override                { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER>(RasterizerState); }
	void RTVFormatsCb(const D3D12_RT_FORMAT_ARRAY& RTVFormats) override                          { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS>(RTVFormats); }
	void SampleDescCb(const DXGI_SAMPLE_DESC& SampleDesc) override                              { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC>(SampleDesc); }
	void SampleMaskCb(UINT SampleMask) override                                                  { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK>(SampleMask); }
	void ViewInstancingCb(const D3D12_VIEW_INSTANCING_DESC& ViewInstancing) override             { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING>(ViewInstancing); }
	void CachedPSOCb(const D3D12_CACHED_PIPELINE_STATE& CachedPSO) override                     { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO>(CachedPSO); }
#if defined(D3D12_SDK_VERSION) && (D3D12_SDK_VERSION >= 618)
	void SerializedRootSignatureCb(const D3D12_SERIALIZED_ROOT_SIGNATURE_DESC& SerializedRootSig) override { Dispatch<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE>(SerializedRootSig); }
#endif

	// Error callbacks -- log and stop
	void ErrorUnknownSubobject(UINT UnknownTypeValue) override
	{
		UE_LOG(LogASDTool, Warning, TEXT("ForEachPSOSubobject: unknown subobject type %u -- stopping."), UnknownTypeValue);
		bStopped = true;
	}
};

} // anonymous namespace

void ForEachPSOSubobject(
	const TArray<uint8>& StreamBuffer,
	TFunctionRef<bool(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, const void* Payload, SIZE_T PayloadSize)> Visitor)
{
	if (StreamBuffer.IsEmpty())
	{
		return;
	}

	const D3D12_PIPELINE_STATE_STREAM_DESC Desc
	{
		(SIZE_T)StreamBuffer.Num(),
		(void*)StreamBuffer.GetData()
	};

	FPSOParserAdapter Adapter(Visitor);
	D3DX12ParsePipelineStream(Desc, &Adapter);
}

bool ReadBackGraphicsPSOState(const TArray<uint8>& StreamBuffer, FD3D12GraphicsPSOState& OutState)
{
	bool bFoundBlend        = false;
	bool bFoundRasterizer   = false;
	bool bFoundDepthStencil = false;
	bool bFoundRTFormats    = false;
	bool bFoundDSVFormat    = false;
	bool bFoundSampleDesc   = false;
	bool bFoundSampleMask   = false;
	bool bFoundPrimTopology = false;

	ForEachPSOSubobject(StreamBuffer, [&](D3D12_PIPELINE_STATE_SUBOBJECT_TYPE SubType, const void* Payload, SIZE_T PayloadSize) -> bool
	{
		switch (SubType)
		{
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
			FMemory::Memcpy(&OutState.BlendDesc, Payload, sizeof(D3D12_BLEND_DESC));
			bFoundBlend = true;
			break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
			FMemory::Memcpy(&OutState.RasterizerDesc, Payload, sizeof(D3D12_RASTERIZER_DESC));
			bFoundRasterizer = true;
			break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
			FMemory::Memcpy(&OutState.DepthStencilDesc, Payload, sizeof(D3D12_DEPTH_STENCIL_DESC1));
			bFoundDepthStencil = true;
			break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
			FMemory::Memcpy(&OutState.RTFormats, Payload, sizeof(D3D12_RT_FORMAT_ARRAY));
			bFoundRTFormats = true;
			break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
			FMemory::Memcpy(&OutState.DSVFormat, Payload, sizeof(DXGI_FORMAT));
			bFoundDSVFormat = true;
			break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
			FMemory::Memcpy(&OutState.SampleDesc, Payload, sizeof(DXGI_SAMPLE_DESC));
			bFoundSampleDesc = true;
			break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
			FMemory::Memcpy(&OutState.SampleMask, Payload, sizeof(UINT));
			bFoundSampleMask = true;
			break;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
			FMemory::Memcpy(&OutState.PrimitiveTopologyType, Payload, sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE));
			bFoundPrimTopology = true;
			break;
		default:
			break;
		}
		return true; // continue
	});

	if (!bFoundBlend || !bFoundRasterizer || !bFoundDepthStencil || !bFoundRTFormats ||
		!bFoundDSVFormat || !bFoundSampleDesc || !bFoundSampleMask || !bFoundPrimTopology)
	{
		UE_LOG(LogASDTool, Warning,
			TEXT("ReadBackGraphicsPSOState: incomplete -- Blend=%d Raster=%d DS=%d RTF=%d DSV=%d SD=%d SM=%d PT=%d"),
			(int32)bFoundBlend, (int32)bFoundRasterizer, (int32)bFoundDepthStencil, (int32)bFoundRTFormats,
			(int32)bFoundDSVFormat, (int32)bFoundSampleDesc, (int32)bFoundSampleMask, (int32)bFoundPrimTopology);
		return false;
	}
	return true;
}

//--------------------------------------------------------------------------------------------------
// FPSODebugCompiler
//--------------------------------------------------------------------------------------------------

// Function pointer types for dynamic loading of d3d12.dll and dxgi.dll
typedef HRESULT(WINAPI* PFN_CREATE_DXGI_FACTORY2)(UINT Flags, REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_D3D12_GET_DEBUG_INTERFACE)(REFIID riid, void** ppvDebug);
typedef HRESULT(WINAPI* PFN_D3D12_CREATE_DEVICE)(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinFeatureLevel, REFIID riid, void** ppDevice);

FPSODebugCompiler::~FPSODebugCompiler()
{
	Shutdown();
}

bool FPSODebugCompiler::Init()
{
	D3D12Module = LoadLibrary(TEXT("d3d12.dll"));
	if (!D3D12Module)
	{
		UE_LOG(LogASDTool, Error, TEXT("FPSODebugCompiler::Init: Failed to load d3d12.dll"));
		return false;
	}

	DXGIModule = LoadLibrary(TEXT("dxgi.dll"));
	if (!DXGIModule)
	{
		UE_LOG(LogASDTool, Error, TEXT("FPSODebugCompiler::Init: Failed to load dxgi.dll"));
		Shutdown();
		return false;
	}

	auto D3D12GetDebugInterfaceFn = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(
		reinterpret_cast<void*>(GetProcAddress(D3D12Module, "D3D12GetDebugInterface")));
	auto D3D12CreateDeviceFn = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
		reinterpret_cast<void*>(GetProcAddress(D3D12Module, "D3D12CreateDevice")));
	auto CreateDXGIFactory2Fn = reinterpret_cast<PFN_CREATE_DXGI_FACTORY2>(
		reinterpret_cast<void*>(GetProcAddress(DXGIModule, "CreateDXGIFactory2")));

	if (!D3D12CreateDeviceFn || !CreateDXGIFactory2Fn)
	{
		UE_LOG(LogASDTool, Error, TEXT("FPSODebugCompiler::Init: Failed to get required function pointers from d3d12.dll/dxgi.dll"));
		Shutdown();
		return false;
	}

	// Enable debug layer
	if (D3D12GetDebugInterfaceFn)
	{
		ID3D12Debug* DebugInterface = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterfaceFn(IID_PPV_ARGS(&DebugInterface))) && DebugInterface)
		{
			DebugInterface->EnableDebugLayer();
			DebugInterface->Release();
			UE_LOG(LogASDTool, Display, TEXT("FPSODebugCompiler: D3D12 debug layer enabled."));
		}
	}

	// Create DXGI factory and find first hardware adapter
	IDXGIFactory6* Factory = nullptr;
	if (FAILED(CreateDXGIFactory2Fn(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&Factory))) || !Factory)
	{
		if (FAILED(CreateDXGIFactory2Fn(0, IID_PPV_ARGS(&Factory))) || !Factory)
		{
			UE_LOG(LogASDTool, Error, TEXT("FPSODebugCompiler::Init: Failed to create DXGI factory."));
			Shutdown();
			return false;
		}
	}

	IDXGIAdapter1* Adapter = nullptr;
	for (UINT AdapterIndex = 0; ; ++AdapterIndex)
	{
		IDXGIAdapter1* TryAdapter = nullptr;
		if (FAILED(Factory->EnumAdapterByGpuPreference(AdapterIndex,
			DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&TryAdapter))))
		{
			break;
		}

		DXGI_ADAPTER_DESC1 Desc;
		TryAdapter->GetDesc1(&Desc);
		if (!(Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
		{
			Adapter = TryAdapter;
			UE_LOG(LogASDTool, Display, TEXT("FPSODebugCompiler: Using adapter: %s"), WCHAR_TO_TCHAR(Desc.Description));
			break;
		}
		TryAdapter->Release();
	}
	Factory->Release();

	if (!Adapter)
	{
		UE_LOG(LogASDTool, Error, TEXT("FPSODebugCompiler::Init: No suitable D3D12 hardware adapter found."));
		Shutdown();
		return false;
	}

	HRESULT hr = D3D12CreateDeviceFn(Adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device));
	Adapter->Release();

	if (FAILED(hr) || !Device)
	{
		UE_LOG(LogASDTool, Error, TEXT("FPSODebugCompiler::Init: D3D12CreateDevice failed: 0x%08X"), hr);
		Shutdown();
		return false;
	}

	// Get info queue for debug messages
	if (SUCCEEDED(Device->QueryInterface(IID_PPV_ARGS(&InfoQueue))) && InfoQueue)
	{
		InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, 0);
		InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, 0);
	}

	UE_LOG(LogASDTool, Display, TEXT("FPSODebugCompiler: Initialized successfully."));
	return true;
}

void FPSODebugCompiler::Shutdown()
{
	if (InfoQueue)
	{
		InfoQueue->Release();
		InfoQueue = nullptr;
	}
	if (Device)
	{
		Device->Release();
		Device = nullptr;
	}
	if (D3D12Module)
	{
		FreeLibrary(D3D12Module);
		D3D12Module = nullptr;
	}
	if (DXGIModule)
	{
		FreeLibrary(DXGIModule);
		DXGIModule = nullptr;
	}
}

bool FPSODebugCompiler::IsReady() const
{
	return Device != nullptr;
}

void FPSODebugCompiler::FlushInfoQueue(const FString& Label)
{
	if (!InfoQueue)
	{
		return;
	}

	const UINT64 NumMessages = InfoQueue->GetNumStoredMessages();
	for (UINT64 i = 0; i < NumMessages; ++i)
	{
		SIZE_T MessageLength = 0;
		if (FAILED(InfoQueue->GetMessage(i, nullptr, &MessageLength)))
		{
			continue;
		}

		TArray<uint8> MessageBuffer;
		MessageBuffer.SetNumUninitialized((int32)MessageLength);
		D3D12_MESSAGE* Message = reinterpret_cast<D3D12_MESSAGE*>(MessageBuffer.GetData());

		if (FAILED(InfoQueue->GetMessage(i, Message, &MessageLength)))
		{
			continue;
		}

		const TCHAR* SeverityStr = TEXT("UNKNOWN");
		switch (Message->Severity)
		{
		case D3D12_MESSAGE_SEVERITY_CORRUPTION: SeverityStr = TEXT("CORRUPTION"); break;
		case D3D12_MESSAGE_SEVERITY_ERROR:      SeverityStr = TEXT("ERROR");      break;
		case D3D12_MESSAGE_SEVERITY_WARNING:    SeverityStr = TEXT("WARNING");    break;
		case D3D12_MESSAGE_SEVERITY_INFO:       SeverityStr = TEXT("INFO");       break;
		case D3D12_MESSAGE_SEVERITY_MESSAGE:    SeverityStr = TEXT("MESSAGE");    break;
		}

		UE_LOG(LogASDTool, Display, TEXT("FPSODebugCompiler [%s] D3D12 %s (ID=%d): %s"),
			*Label,
			SeverityStr,
			(int32)Message->ID,
			Message->pDescription ? UTF8_TO_TCHAR(Message->pDescription) : TEXT("(no description)"));
	}

	InfoQueue->ClearStoredMessages();
}

bool FPSODebugCompiler::Compile(const TArray<uint8>& StreamBuffer, const FString& Label)
{
	if (!Device)
	{
		UE_LOG(LogASDTool, Error, TEXT("FPSODebugCompiler::Compile: Not initialized. Call Init() first."));
		return false;
	}

	if (StreamBuffer.IsEmpty())
	{
		UE_LOG(LogASDTool, Warning, TEXT("FPSODebugCompiler::Compile [%s]: Empty stream buffer."), *Label);
		return false;
	}

	// The stream may contain a D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE (type 29)
	// subobject, which is only valid for StorePipelineStateDesc -- not for CreatePipelineState.
	// We need to find it, deserialize it into a live ID3D12RootSignature, and substitute
	// a D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE (type 0) subobject in its place.

	const void* SerializedRootSigData   = nullptr;
	SIZE_T      SerializedRootSigSize   = 0;
	SIZE_T      SerializedRootSigOffset = 0;

	const uint8* Ptr = StreamBuffer.GetData();
	ForEachPSOSubobject(StreamBuffer, [&](D3D12_PIPELINE_STATE_SUBOBJECT_TYPE SubType, const void* Payload, SIZE_T PayloadSize) -> bool
	{
		if (SubType == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SERIALIZED_ROOT_SIGNATURE)
		{
			const D3D12_SERIALIZED_ROOT_SIGNATURE_DESC* SRSDesc =
				reinterpret_cast<const D3D12_SERIALIZED_ROOT_SIGNATURE_DESC*>(Payload);
			SerializedRootSigData   = SRSDesc->pSerializedBlob;
			SerializedRootSigSize   = SRSDesc->SerializedBlobSizeInBytes;
			// Offset of the full token (type field is sizeof(void*) before the payload)
			SerializedRootSigOffset = reinterpret_cast<const uint8*>(Payload) - sizeof(void*) - Ptr;
			return false; // stop
		}
		return true; // continue
	});

	// Deserialize root sig if found
	TRefCountPtr<ID3D12RootSignature> RootSig;
	if (SerializedRootSigData && SerializedRootSigSize > 0)
	{
		HRESULT hr = Device->CreateRootSignature(
			0,
			SerializedRootSigData, SerializedRootSigSize,
			IID_PPV_ARGS(RootSig.GetInitReference()));
		if (FAILED(hr) || !RootSig)
		{
			UE_LOG(LogASDTool, Error, TEXT("FPSODebugCompiler::Compile [%s]: CreateRootSignature failed: 0x%08X"), *Label, hr);
			FlushInfoQueue(Label);
			return false;
		}
	}

	// Build a patched stream replacing the serialized root sig subobject (type 29) with a
	// live root sig pointer subobject (type 0) for CreatePipelineState.
	// Uses CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE + AppendSubobject for correct layout.
	const uint8* End = Ptr + StreamBuffer.Num();
	TArray<uint8> PatchedStream;

	if (RootSig)
	{
		// Calculate the size of the serialized root sig token to skip over it in the original stream.
		// Token layout: alignas(void*) { Type(4) + pad(4) + D3D12_SERIALIZED_ROOT_SIGNATURE_DESC(16) } = 24 bytes
		const SIZE_T SerializedTokenSize = Align(sizeof(void*) + sizeof(D3D12_SERIALIZED_ROOT_SIGNATURE_DESC), sizeof(void*));

		// Copy everything before the serialized root sig token
		PatchedStream.Append(Ptr, (int32)SerializedRootSigOffset);

		// Append the live root sig pointer subobject using SDK type
		CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE RootSigSub;
		RootSigSub = RootSig.GetReference();
		AppendSubobject(PatchedStream, RootSigSub);

		// Copy everything after the serialized root sig token
		const uint8* AfterSerialized = Ptr + SerializedRootSigOffset + SerializedTokenSize;
		if (AfterSerialized < End)
		{
			PatchedStream.Append(AfterSerialized, (int32)(End - AfterSerialized));
		}
	}
	else
	{
		// No root sig at all -- use stream as-is
		PatchedStream = StreamBuffer;
	}

	D3D12_PIPELINE_STATE_STREAM_DESC StreamDesc = {};
	StreamDesc.SizeInBytes                   = (SIZE_T)PatchedStream.Num();
	StreamDesc.pPipelineStateSubobjectStream = PatchedStream.GetData();

	ID3D12PipelineState* PSO = nullptr;
	HRESULT hr = Device->CreatePipelineState(&StreamDesc, IID_PPV_ARGS(&PSO));

	if (SUCCEEDED(hr) && PSO)
	{
		PSO->Release();
		FlushInfoQueue(Label);
		return true;
	}

	UE_LOG(LogASDTool, Error, TEXT("FPSODebugCompiler::Compile [%s]: CreatePipelineState FAILED hr=0x%08X"), *Label, hr);
	FlushInfoQueue(Label);
	return false;
}

} // namespace ASDTool
