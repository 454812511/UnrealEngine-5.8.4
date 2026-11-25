// Copyright Epic Games, Inc. All Rights Reserved.

#include "Transcoder/TmvMediaPlayerFrameProducerResource.h"

#include "Async/Async.h"
#include "MediaDelegates.h"
#include "MediaTexture.h"
#include "PixelShaderUtils.h"
#include "RenderGraphUtils.h"
#include "SampleConverter/TmvMediaFrameMipMemoryBuffer.h"
#include "TextureResource.h"
#include "TmvMediaFrameRender.h"
#include "TmvMediaLog.h"
#include "Transcoder/TmvMediaFrameConverter.h"
#include "Transcoder/TmvMediaFrameEncoder.h"
#include "Transcoder/TmvMediaFrameMips.h"
#include "Transcoder/TmvMediaTranscodeJob.h"
#include "Utils/TmvMediaFrameUtils.h"
#include "Utils/TmvMediaGpuReadback.h"
#include "Utils/TmvMediaTimeUtils.h"

DECLARE_GPU_STAT_NAMED(TmvMediaFrameProducer_ConvertFrame, TEXT("TmvMediaFrameProducer.ConvertFrame"));
DECLARE_GPU_STAT_NAMED(TmvMediaFrameProducer_ReadbackFrame, TEXT("TmvMediaFrameProducer.ReadbackFrame"));

static float GTmvTranscodeMediaPlayerMemoryBudgetFraction = 0.6f;
static FAutoConsoleVariableRef CVarTmvTranscodeMediaPlayerMemoryBudgetFraction(
	TEXT("TmvMedia.Transcode.MediaPlayer.MemoryBudgetFraction"),
	GTmvTranscodeMediaPlayerMemoryBudgetFraction,
	TEXT("Lower this when a transcode starves the rest of the machine or the job fails for memory, or ")
	TEXT("raise it to trade memory headroom for throughput on a machine dedicated to transcoding.\n")
	TEXT("Fraction (0.05..1.0) of each currently-available memory pool - physical and GPU-shared alike - ")
	TEXT("that in-flight readback frames may occupy; the rest is left for the decoder, encoder queue and ")
	TEXT("the rest of the app. A share of what is already free, so it compounds with whatever headroom ")
	TEXT("the machine has. Default 0.6."),
	ECVF_RenderThreadSafe);

static float GTmvTranscodeMediaPlayerHostBytesPerFrameScale = 3.0f;
static FAutoConsoleVariableRef CVarTmvTranscodeMediaPlayerHostBytesPerFrameScale(
	TEXT("TmvMedia.Transcode.MediaPlayer.HostBytesPerFrameScale"),
	GTmvTranscodeMediaPlayerHostBytesPerFrameScale,
	TEXT("Change this only when the per-frame estimate is wrong for your content: compare the machine's ")
	TEXT("peak working set against the MB/frame the concurrency log reports. This is a cost model, not ")
	TEXT("a tuning knob - use MemoryBudgetFraction to trade memory for throughput.\n")
	TEXT("Multiplier on the measured readback staging size covering the readback heap, the CPU frame ")
	TEXT("copy handed to the encoder, and decoder read-ahead slack. Applies to the physical-memory ")
	TEXT("budget only; the GPU-shared pool is charged the unscaled staging size, since that is all of ")
	TEXT("the frame that lives there. Default 3.0."),
	ECVF_RenderThreadSafe);

namespace UE::TmvMedia
{
	FFrameMemoryBudget ComputeReadbackFrameBudget(uint64 InStagingBytesPerFrame)
	{
		FFrameMemoryCost Cost;

		// The staging heap is the only part of a frame that occupies the GPU-shared pool, so that pool
		// is charged the measured size and nothing more. The host scale on top covers the CPU FrameMips
		// copy handed to the encoder and the decoder's read-ahead, which are ordinary allocations -
		// billing them to the GPU-shared budget as well would understate what fits there by the whole
		// multiplier, and could refuse a frame the pool has ample room for.
		Cost.GpuSharedBytes = InStagingBytesPerFrame;
		Cost.HostBytes = (uint64)(InStagingBytesPerFrame * (double)FMath::Max(1.0f, GTmvTranscodeMediaPlayerHostBytesPerFrameScale));

		return ComputeFrameMemoryBudget(Cost, GTmvTranscodeMediaPlayerMemoryBudgetFraction);
	}

	/**
	 * Only works for channel that have the same number of bits per channel for all channels.
	 * Some exceptions: PF_R5G6B5_UNORM, PF_FloatR11G11B10.
	 */
	uint8 CalcNumBitsPerChannel(const FPixelFormatInfo& InPixelFormatInfo)
	{
		if (InPixelFormatInfo.NumComponents == 0)
		{
			return 0;
		}
		
		const int32 PixelsPerBlock = InPixelFormatInfo.BlockSizeX * InPixelFormatInfo.BlockSizeY * InPixelFormatInfo.BlockSizeZ;
		if (PixelsPerBlock == 0)
		{
			return 0;
		}

		// todo: warn about exceptions?
		return 8 * InPixelFormatInfo.BlockBytes / (PixelsPerBlock * InPixelFormatInfo.NumComponents);
	}
	
	void PopulateMipInfoFromTexture(int32 InWidth, int32 InHeight, EPixelFormat InPixelFormat, int32 InMipLevel, FTmvMediaFrameMipInfo& OutMipInfo)
	{
		OutMipInfo.MipLevel = InMipLevel;
		OutMipInfo.Width = InWidth;
		OutMipInfo.Height = InHeight;
		const FPixelFormatInfo& PixelFormatInfo = GPixelFormats[InPixelFormat];
		OutMipInfo.NumComponents = PixelFormatInfo.NumComponents;
		OutMipInfo.ColorModel = ETmvMediaFrameColorModel::RGB;
		// By default, textures are considered in the "working" color space, which we represent as "None" in this case.
		OutMipInfo.ColorInfo.ColorSpace = Color::EColorSpace::None;
		OutMipInfo.ColorInfo.Encoding = IsFloatFormat(InPixelFormat) ?	Color::EEncoding::Linear : Color::EEncoding::sRGB;
		OutMipInfo.ColorInfo.YuvMatrix = ETmvMediaFrameColorMatrix::Identity;
		OutMipInfo.ColorInfo.YuvMatrixRange = ETmvMediaFrameColorMatrixRange::Full;
		OutMipInfo.TileWidth = OutMipInfo.Width;
		OutMipInfo.TileHeight = OutMipInfo.Height;
		OutMipInfo.NumTiles = FIntPoint(1,1);

		OutMipInfo.Planes.SetNumUninitialized(1);

		FTmvMediaFramePlaneInfo& PlaneInfo = OutMipInfo.Planes[0];

		PlaneInfo.NumComponents = PixelFormatInfo.NumComponents;
		PlaneInfo.BitDepth = CalcNumBitsPerChannel(PixelFormatInfo);
		PlaneInfo.Type = IsFloatFormat(InPixelFormat) ? ETmvMediaFrameComponentType::Float : ETmvMediaFrameComponentType::Int;
		PlaneInfo.Width = InWidth;
		PlaneInfo.Height = InHeight;
		PlaneInfo.Stride = PixelFormatInfo.Get2DImageSizeInBytes(InWidth, 1);
		PlaneInfo.NumLines = InHeight;
		PlaneInfo.ComponentLayout = ETmvMediaFrameComponentLayout::Packed;
		PlaneInfo.WidthRatio = PlaneInfo.HeightRatio = 1;
	}
	
	void PopulateMipInfoFromTexture(const FRHITexture* InTexture, int32 InMipLevel, FTmvMediaFrameMipInfo& OutMipInfo)
	{
		PopulateMipInfoFromTexture(InTexture->GetSizeX(), InTexture->GetSizeY(), InTexture->GetFormat(), InMipLevel, OutMipInfo);
	}

	void PopulateMipInfoFromTexture(const FRDGTexture* InTexture, int32 InMipLevel, FTmvMediaFrameMipInfo& OutMipInfo)
	{
		PopulateMipInfoFromTexture(InTexture->Desc.GetSize().X, InTexture->Desc.GetSize().Y, InTexture->Desc.Format, InMipLevel, OutMipInfo);
	}

	/**
	 * Keeps information and resources (staging buffer) for reading back a gpu texture to cpu buffer.
	 * 
	 * For reading the media texture render target, we need to keep the readback requests around
	 * and poll it to know when it has been executed and is ready to be read.
	 */
	struct FFrameReadbackRequest
	{
		void Init(const FRDGTexture* InReadbackRdg, const FTmvMediaFrameTimeInfo& InTimeInfo)
		{
			// Setup all frame info.
			Format = InReadbackRdg->Desc.Format;
			TimeInfo = InTimeInfo;

			// When receiving the pixels from the media texture, we assume it is in the default working colorspace and linear encoding.
			PopulateMipInfoFromTexture(InReadbackRdg, 0, MipInfo);

			// Setup readbacks if necessary. In case of reuse, we just make sure the number of mips is the same (it should).
			const int32 NumMips = InReadbackRdg->Desc.NumMips;
			if (Readbacks.Num() != NumMips)
			{
				Readbacks.Reset();
				Readbacks.Reserve(NumMips);
				for (int32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
				{
					Readbacks.Add(MakeUnique<FTmvMediaGpuTextureReadback>(TEXT("MediaTextureReadback")));
				}
			}
		}

		bool IsReady() const
		{
			// Note that, we would probably only need a fence on the last request.
			for (const TUniquePtr<FTmvMediaGpuTextureReadback>& Readback : Readbacks)
			{
				if (!Readback->IsReady())
				{
					return false;
				}
			}
			return true;
		}

		virtual ~FFrameReadbackRequest() = default;

		/** "Readback" helpers (manage the staging buffer) for each mips. */
		TArray<TUniquePtr<FTmvMediaGpuTextureReadback>> Readbacks;

		/** Keep track of the original pixel format. */
		EPixelFormat Format = EPixelFormat::PF_Unknown;

		/** MipInfo populated from the readback texture */ 
		FTmvMediaFrameMipInfo MipInfo;

		/** Frame time for this readback request */
		FTmvMediaFrameTimeInfo TimeInfo;
	};

	void SubmitReadbackRequest(
		FRDGBuilder& InBuilder, 
		FRDGTexture* InReadbackRdg,
		const FTmvMediaFrameTimeInfo& InTimeInfo,
		const TSharedPtr<FFrameReadbackRequest>& InReadbackRequest)
	{
		const int32 NumMips = InReadbackRdg->Desc.NumMips;

		// Init/recycle setup
		InReadbackRequest->Init(InReadbackRdg, InTimeInfo);

		for (int32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
		{
			TmvMedia::AddEnqueueCopyPass(InBuilder, InReadbackRequest->Readbacks[MipIndex].Get(), InReadbackRdg, MipIndex);
		}
	}

	TUniquePtr<FTmvMediaFrameMips> ProcessReadbackRequest(const TSharedPtr<FFrameReadbackRequest>& InReadbackRequest)
	{
		TUniquePtr<FTmvMediaFrameMips> FrameMips = MakeUnique<FTmvMediaFrameMips>();
		FrameMips->TimeInfo = InReadbackRequest->TimeInfo;

		// Second pass to read the readbacks.
		int32 MipWidth = InReadbackRequest->MipInfo.Width;
		int32 MipHeight = InReadbackRequest->MipInfo.Height;

		TRACE_CPUPROFILER_EVENT_SCOPE(ReadGpuTextureMipsV2::CopyMips);
		for (int32 MipIndex = 0; MipIndex < InReadbackRequest->Readbacks.Num(); ++MipIndex)
		{
			FTmvMediaGpuTextureReadback* Readback = InReadbackRequest->Readbacks[MipIndex].Get();
			if (!Readback)
			{
				return nullptr;
			}

			int32 RowPitchInPixels;
			int32 BufferHeight;
			const uint8* LockedData = (const uint8*)Readback->Lock(RowPitchInPixels, &BufferHeight);

			if (!LockedData)
			{
				UE_LOGF(LogTmvMedia, Error, "Readback failed and returned null locked data");
				return nullptr;
			}

			if (RowPitchInPixels == 0 || BufferHeight == 0)
			{
				UE_LOGF(LogTmvMedia, Error, "Readback failed and returned data with zero pitch/buffer height");
				return nullptr;
			}

			// todo: here buffers could (and should) be pooled and recycled.
			TSharedPtr<FTmvMediaFrameMipMemoryBuffer> MipBuffer = MakeShared<FTmvMediaFrameMipMemoryBuffer>();
			FTmvMediaFrameMipInfo MipBufferInfo = InReadbackRequest->MipInfo;

			// Adjust sizes for the mip level
			MipBufferInfo.MipLevel = MipIndex;
			MipBufferInfo.Width = MipBufferInfo.TileWidth = MipWidth;
			MipBufferInfo.Height = MipBufferInfo.TileHeight = MipHeight;
			MipBufferInfo.Planes[0].Width = MipWidth;
			MipBufferInfo.Planes[0].Stride = GPixelFormats[InReadbackRequest->Format].Get2DImageSizeInBytes(MipWidth, 1);
			MipBufferInfo.Planes[0].Height = MipBufferInfo.Planes[0].NumLines = MipHeight;

			if (MipBuffer->RequestAllocation(MipBufferInfo))
			{
				const SIZE_T ImageSizeInBytes = GPixelFormats[InReadbackRequest->Format].Get2DImageSizeInBytes(MipWidth, MipHeight);

				if (RowPitchInPixels == MipWidth && BufferHeight >= MipHeight)
				{
					FMemory::Memcpy(MipBuffer->GetPlaneBufferForComponent(0), LockedData, ImageSizeInBytes);
				}
				else
				{
					const SIZE_T SrcPitch = GPixelFormats[InReadbackRequest->Format].Get2DImageSizeInBytes(RowPitchInPixels, 1);
					const SIZE_T DstPitch = MipBufferInfo.Planes[0].Stride;
					const int32 HeightToCopy = FMath::Min(BufferHeight, MipHeight);
					const int32 RowSizeToCopy = FMath::Min(SrcPitch, DstPitch);
					for (int32 RowIndex = 0; RowIndex < HeightToCopy; ++RowIndex)
					{
						const uint8* SrcRow = LockedData + RowIndex * SrcPitch;
						uint8* DstRow = static_cast<uint8*>(MipBuffer->GetPlaneBufferForComponent(0)) + RowIndex * DstPitch;
						FMemory::Memcpy(DstRow, SrcRow, RowSizeToCopy);
					}
				}
				FrameMips->MipBuffers.Add(MipBuffer);
			}
			else
			{
				UE_LOGF(LogTmvMedia, Error, "Failed to allocate mip buffer %d (%dx%d)", MipIndex, MipWidth, MipHeight);
			}

			Readback->Unlock();

			MipWidth = FMath::Max(MipWidth/2, 1);
			MipHeight = FMath::Max(MipHeight/2, 1);
		}

		return FrameMips;
	}
}

FTmvMediaPlayerFrameProducerResource::FTmvMediaPlayerFrameProducerResource(UTmvMediaTranscodeJob* InParentJob, UMediaTexture* InMediaTexture)
	: ParentJob_RenderThread(InParentJob)
	, MediaTexture_RenderThread(InMediaTexture)
	, MediaFrameRate_RenderThread(0, 0) // invalid
{
	LastRenderedSampleTime = FirstRenderedSampleTime = FTimespan::MinValue();	// Indicates nothing is rendered yet.
}

FTmvMediaPlayerFrameProducerResource::~FTmvMediaPlayerFrameProducerResource() = default;

void FTmvMediaPlayerFrameProducerResource::InitRHI(FRHICommandListImmediate& InRHICmdList)
{
	FMediaDelegates::OnSampleRendered_RenderThread.AddSP(this, &FTmvMediaPlayerFrameProducerResource::OnMediaTextureRendered_RenderThread);
}

void FTmvMediaPlayerFrameProducerResource::ReleaseRHI()
{
	FMediaDelegates::OnSampleRendered_RenderThread.RemoveAll(this);
	RenderTargetRhi.SafeRelease();
}

void FTmvMediaPlayerFrameProducerResource::ReleaseReadbackRequest(TSharedPtr<UE::TmvMedia::FFrameReadbackRequest>&& InReadbackRequest)
{
	FScopeLock lock(&AvailableRequestsLock);

	// Recycle the request for next frame.
	if (AvailableRequests.Num() < 32) // Runaway Protection
	{
		AvailableRequests.Add(MoveTemp(InReadbackRequest));
	}
	else
	{
		UE_LOGF(LogTmvMedia, Warning, "More than 32 requests where created, discarding excess. (Request throttling has failed)");
	}
}

TSharedPtr<UE::TmvMedia::FFrameReadbackRequest> FTmvMediaPlayerFrameProducerResource::AcquireReadbackRequest()
{
	FScopeLock lock(&AvailableRequestsLock);
	return AvailableRequests.IsEmpty() ? MakeShared<UE::TmvMedia::FFrameReadbackRequest>() : AvailableRequests.Pop(EAllowShrinking::No);
}

void FTmvMediaPlayerFrameProducerResource::ProcessPendingReadbackRequests(FRHICommandListImmediate& InRHICmdList, const FTmvMediaTranscodeJobTime& InTime)
{
	using namespace UE::TmvMedia;
	for (TSharedPtr<FFrameReadbackRequest>& PendingRequest : PendingRequests)
	{
		if (PendingRequest->IsReady())
		{
			ProcessAndDispatch(MoveTemp(PendingRequest));
		}
	}

	PendingRequests.RemoveAll([](const TSharedPtr<FFrameReadbackRequest>& InRequest) { return !InRequest.IsValid(); });
}

const FTextureResource* FTmvMediaPlayerFrameProducerResource::GetMediaTextureResource() const
{
	if (UMediaTexture* MediaTexture = MediaTexture_RenderThread.Get())
	{
		return MediaTexture->GetResource();
	}
	return nullptr;
}


void FTmvMediaPlayerFrameProducerResource::CreateRenderTarget_RenderThread(FRHICommandListImmediate& InRHICmdList, const FRHITextureDesc& InRenderTargetDesc)
{
	const static FLazyName ClassName(TEXT("UTmvMediaPlayerFrameProducer"));

	const FRHITextureCreateDesc Desc =
	FRHITextureCreateDesc::Create2D(TEXT("MediaPlayerFrameProducerOutput"),
		InRenderTargetDesc.GetSize().X,
		InRenderTargetDesc.GetSize().Y,
		InRenderTargetDesc.Format)
	.SetNumMips(InRenderTargetDesc.NumMips)
	.SetClearValue(InRenderTargetDesc.ClearValue)
	.SetClassName(ClassName)
	.SetFlags(InRenderTargetDesc.Flags);

	RenderTargetRhi = InRHICmdList.CreateTexture(Desc);
}

void FTmvMediaPlayerFrameProducerResource::OnMediaTextureRendered_RenderThread(const FTextureResource* InMediaTextureResource, TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe> InMediaTextureSample)
{
	if (InMediaTextureResource == nullptr || !InMediaTextureSample.IsValid() || InMediaTextureResource != GetMediaTextureResource())
	{
		return;
	}

	// Driven by the media texture rather than the producer's state machine, so this keeps firing
	// while the job winds down; further conversion work would only add memory pressure.
	if (bInsufficientMemoryForFrame.load(std::memory_order_relaxed))
	{
		return;
	}

	FRHITexture* MediaTextureRhi = InMediaTextureResource->GetTexture2DRHI();
	if (!MediaTextureRhi)
	{
		UE_LOGF(LogTmvMedia, Error, "Transcode Frame Producer: Media Texture Resource has invalid texture rhi.");
		return;
	}

	UE_LOGF(LogTmvMedia, Verbose,
		"Transcode Frame Producer: Render callback Time=%ls Duration=%ls (PrevLast=%ls).",
		*InMediaTextureSample->GetTime().Time.ToString(),
		*InMediaTextureSample->GetDuration().ToString(),
		*LastRenderedSampleTime.load().ToString());

	// Avoid re-encoding already rendered samples: only accept a strictly newer time. The player can
	// re-deliver the same sample at end-of-stream (block-on-range settling on the last frame), so
	// rejecting equal times too - not just earlier ones - stops the same frame being read back and
	// encoded twice. LastRenderedSampleTime starts at MinValue, so the first frame (even at time 0)
	// still passes. Could use a Set instead.
	if (InMediaTextureSample->GetTime().Time <= LastRenderedSampleTime)
	{
		UE_LOGF(LogTmvMedia, Verbose,
			"Transcode Frame Producer: Sample Time %ls not newer than last rendered time %ls; skipping to avoid a duplicate.",
			*InMediaTextureSample->GetTime().Time.ToString(),
			*LastRenderedSampleTime.load().ToString());
		return;
	}

	// Keep track of the first frame to be able to calculate the frame index.
	if (FirstRenderedSampleTime == FTimespan::MinValue())
	{
		FirstRenderedSampleTime = InMediaTextureSample->GetTime().Time;
	}
	
	FTmvMediaFrameTimeInfo TimeInfo;
	TimeInfo.FrameIndex = INDEX_NONE;	// That can be calculated from frame time and duration for fixed rate videos.
	LastRenderedSampleTime = TimeInfo.FrameTime = InMediaTextureSample->GetTime().Time;
	LastRenderedSampleDuration = TimeInfo.FrameDuration = InMediaTextureSample->GetDuration();

	const FFrameRate FrameRate = UE::TmvMedia::TimeUtils::GetOrComputeFrameRate(MediaFrameRate_RenderThread, TimeInfo.FrameDuration);
	
	if (FrameRate.IsValid())
	{
		const FTimespan RelativeFrameTime = TimeInfo.FrameTime - FirstRenderedSampleTime;
		TimeInfo.FrameIndex = UE::TmvMedia::TimeUtils::RoundToFrameIndex(FrameRate, RelativeFrameTime.GetTotalSeconds());
		TimeInfo.FrameIndexNoOffset = TimeInfo.FrameIndex;	// This should already be zero based.
	}

	FRHICommandListImmediate& RhiCmdList = FRHICommandListImmediate::Get();

	if (!RenderTargetRhi)
	{
		// Fetch the desired render target format from the encoder requirements.
		if (UTmvMediaTranscodeJob* ParentJob = ParentJob_RenderThread.Get())
		{
			if (UTmvMediaFrameEncoder* FrameEncoder = ParentJob->GetStage<UTmvMediaFrameEncoder>())
			{
				FTmvMediaFrameMipInfo MediaTextureMipInfo;
				UE::TmvMedia::PopulateMipInfoFromTexture(MediaTextureRhi, 0, MediaTextureMipInfo);

				TargetMipInfo.Reset();

				if (!FrameEncoder->RequestMipInfos(ParentJob, TimeInfo, MediaTextureMipInfo, TargetMipInfo))
				{
					return;
				}
			}
		}

		FRHITextureDesc RenderTargetDesc = MediaTextureRhi->GetDesc();
		
		UE_LOGF(LogTmvMedia, Verbose, "Transcode Frame Producer: Media Texture Format: %d x %d, %d mips, %ls.",
			RenderTargetDesc.Extent.X, RenderTargetDesc.Extent.Y, RenderTargetDesc.NumMips, GPixelFormats[RenderTargetDesc.Format].Name);
		
		if (TargetMipInfo.IsValidIndex(0))
		{
			// Note: The destination render target number of mips matches the media texture, not TargetMipInfo.

			// Make it packed, all components in the same plane, and align with supported formats.
			TargetMipInfo[0].Planes[0].NumComponents = (TargetMipInfo[0].NumComponents == 1) ? 1 : 4;	// simplify to only support 1 or 4 components.
			RenderTargetDesc.Format = UE::TmvMedia::FrameUtils::GetPlanePixelFormat(TargetMipInfo[0].Planes[0], /*bNormalized*/ true);
			RenderTargetDesc.Flags &= ~ETextureCreateFlags::SRGB;	// Remove the SRGB because we will be encoding manually to the desired TF.
			RenderTargetDesc.Flags |= ETextureCreateFlags::RenderTargetable;	// Make sure the texture has a render target view.

			UE_LOGF(LogTmvMedia, Verbose, "Transcode Frame Producer: Conversion Format: %d x %d, %d mips, %ls.",
				RenderTargetDesc.Extent.X, RenderTargetDesc.Extent.Y, RenderTargetDesc.NumMips, GPixelFormats[RenderTargetDesc.Format].Name);
		}

		if (RenderTargetDesc.Format != PF_Unknown)
		{
			CreateRenderTarget_RenderThread(RhiCmdList, RenderTargetDesc);
		}
	}

	bool bGpuConverted = false;
	FRDGBuilder GraphBuilder(RhiCmdList);
	FRDGTextureRef MediaTextureRdg = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(MediaTextureRhi, TEXT("MediaTexture")));

	// Prepare for readback.
	FRDGTextureRef ReadbackRdg = MediaTextureRdg;

	// Convert if needed.
	if (RenderTargetRhi)
	{
		RHI_BREADCRUMB_EVENT_STAT_F(RhiCmdList, TmvMediaFrameProducer_ConvertFrame, "TmvMediaFrameProducer.ConvertFrame", "TmvMediaFrameProducer.ConvertFrame %d", TimeInfo.FrameIndex);
		FRDGTextureRef RenderTargetRdg = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(RenderTargetRhi, TEXT("RenderTarget")));
		if (TargetMipInfo.IsValidIndex(0))
		{
			UE::TmvMedia::AddConvertTexturePass(GraphBuilder, MediaTextureRdg, RenderTargetRdg, TargetMipInfo[0]);
			bGpuConverted = true;
		}
		else
		{
			UE::TmvMedia::AddCopyTexturePass(GraphBuilder, MediaTextureRdg, RenderTargetRdg);
		}
		ReadbackRdg = RenderTargetRdg;
	}

	// Publish the per-frame readback footprint so the producer can cap concurrency to what host
	// memory can hold. Uses the RDG desc directly (deterministic and available now; the staging
	// texture itself isn't allocated until GraphBuilder.Execute).
	const uint64 FrameStagingBytes = ReadbackRdg->Desc.CalcMemorySizeEstimate();
	const uint64 PreviousStagingBytes = EstimatedFrameStagingBytes.exchange(FrameStagingBytes, std::memory_order_relaxed);

	// Only re-budget when the footprint actually changes. In practice that is the first frame and no
	// other: RenderTargetRhi is created once and released only at teardown, so from the frame it exists
	// ReadbackRdg is that fixed-desc target and this size is constant for the rest of the job. The
	// comparison is kept rather than replaced with a "first frame" flag because it is the honest
	// condition - but note that the producer's in-flight ceiling is applied once too, so anything that
	// ever makes the render target recreatable mid-job has to revise that ceiling as well, not just
	// re-run the single-frame check below.
	//
	// Two reasons not to re-check on a timer:
	//
	// FPlatformMemory::GetStats is a syscall triple that the platform layer warns against calling
	// often, and it slows down under paging - exactly the condition this guard exists to detect.
	//
	// More importantly, available memory is not an independent signal once the job is running. The
	// in-flight frames this budget authorised are themselves subtracted from it, so re-measuring later
	// reads the job's own footprint back as if it were external pressure. A periodic re-check was tried
	// and had to be removed: it ratcheted the ceiling down every interval and, on a machine with little
	// headroom to start with, could drive free memory under the single-frame threshold and fail a job
	// that was running fine. Re-introducing it requires adding the in-flight cost back first.
	if (PreviousStagingBytes != FrameStagingBytes && UE::TmvMedia::IsMemoryAwareConcurrencyEnabled())
	{
		const UE::TmvMedia::FFrameMemoryBudget Budget = UE::TmvMedia::ComputeReadbackFrameBudget(FrameStagingBytes);
		if (!Budget.bFitsSingleFrame)
		{
			// Refuse here rather than on the game thread: this callback is what enqueues the copy, so
			// by the time the producer could react the unaffordable work is already in flight.
			// Submitting anyway lets the residency manager evict the staging buffer mid-copy, which
			// faults the GPU (DXGI_ERROR_DEVICE_HUNG) instead of failing the job.
			//
			// Publish the deciding budget before the flag, with a release store: the game thread reads
			// it back to report the failure, so both messages quote the same figures rather than a
			// second reading taken however many milliseconds later.
			RefusedFrameBudget = Budget;
			bInsufficientMemoryForFrame.store(true, std::memory_order_release);

			UE_LOGF(LogTmvMedia, Error,
				"Transcode Frame Producer: Refusing frame readback: one frame needs ~%llu MB but only ~%llu MB "
				"of the ~%llu MB available memory can be budgeted (%ls, fraction %.2f). Failing the job instead "
				"of risking a GPU fault.",
				Budget.BytesPerFrame >> 20, Budget.BudgetedBytes >> 20, Budget.Available >> 20,
				*UE::TmvMedia::DescribeBudgetPools(Budget), Budget.Fraction);

			GraphBuilder.Execute();
			return;
		}
	}

	if (PendingRequests.Num() < 32)	// Protect against too many pushes.
	{
		RHI_BREADCRUMB_EVENT_STAT_F(RhiCmdList, TmvMediaFrameProducer_ReadbackFrame, "TmvMediaFrameProducer.ReadbackFrame", "TmvMediaFrameProducer.ReadbackFrame %d", TimeInfo.FrameIndex);
		using namespace UE::TmvMedia;
		if (TSharedPtr<FFrameReadbackRequest> ReadbackRequest = AcquireReadbackRequest())
		{
			SubmitReadbackRequest(GraphBuilder, ReadbackRdg, TimeInfo, ReadbackRequest);

			// If the texture was converted by the GPU, it should have converted the color space, color model and encoded.
			if (bGpuConverted && TargetMipInfo.IsValidIndex(0))
			{
				ReadbackRequest->MipInfo.ColorInfo = TargetMipInfo[0].ColorInfo;
				ReadbackRequest->MipInfo.ColorModel = TargetMipInfo[0].ColorModel;
			}
			PendingRequests.Add(ReadbackRequest);

			// Increment the number of "submitted" frames to the async processing task queue so we can throttle the "pushes" accordingly.
			++SubmittedCount;
		}
		else
		{
			UE_LOGF(LogTmvMedia, Error,
				"Transcode Frame Producer: Failed to make a readback requests for Sample Time %ls.", *InMediaTextureSample->GetTime().Time.ToString());
		}
	}
	else
	{
		UE_LOGF(LogTmvMedia, Error,
			"Transcode Frame Producer: Submit pipeline is overflowing. Skipping sample Time %ls.", *InMediaTextureSample->GetTime().Time.ToString());
	}

	GraphBuilder.Execute();
}

void FTmvMediaPlayerFrameProducerResource::ProcessAndDispatch(TSharedPtr<UE::TmvMedia::FFrameReadbackRequest>&& InReadbackRequest)
{
#if WITH_EDITOR
	EAsyncExecution ExecutionTarget = EAsyncExecution::LargeThreadPool;
#else
	EAsyncExecution ExecutionTarget = EAsyncExecution::ThreadPool;
#endif

	if (FTmvMediaGpuTextureReadback::SupportsAnyThreadReadback())
	{
		TWeakPtr<FTmvMediaPlayerFrameProducerResource> LocalWeakThis = SharedThis(this);

		// Everything from that point will be done in a worker thread.
		Async(ExecutionTarget, [LocalWeakThis, ReadbackRequest = MoveTemp(InReadbackRequest)]()
		{
			if (TSharedPtr<FTmvMediaPlayerFrameProducerResource> LocalThis = LocalWeakThis.Pin())
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(UTmvMediaPlayerFrameProducer::ProcessFrame);
				
				TUniquePtr<FTmvMediaFrameMips> FrameMips = UE::TmvMedia::ProcessReadbackRequest(ReadbackRequest);

				// Return the request to the pool now that we are done with it.
				TSharedPtr<UE::TmvMedia::FFrameReadbackRequest> ReadbackRequestMovable = ReadbackRequest;
				LocalThis->ReleaseReadbackRequest(MoveTemp(ReadbackRequestMovable));

				if (FrameMips)
				{
					UTmvMediaTranscodeJob* ParentJob = LocalThis->ParentJob_RenderThread.Get();
					if (UTmvMediaFrameConverter* FrameConverter = ParentJob ? ParentJob->GetStage<UTmvMediaFrameConverter>() : nullptr)
					{
						FrameConverter->ReceiveMips(ParentJob, MoveTemp(FrameMips));
					}
				}
				--LocalThis->SubmittedCount;
			}
		});
	}
	else
	{
		/**
		 * Todo (eventually...):
		 * If we want to move the readback lock-copy-unlock out of the render thread, and lock/unlock 
		 * is only supported on render thread, we will need to split that into 3 stages, which will have 2 
		 * (render thread) pending states:
		 * - PendingLock: First waiting on the gpu to complete and the readback to be "ready" to be locked.
		 *   When it is locked, it goes in an async task to be processed (copy). Once copy is done, it 
		 *   is submitted back to the pending unlock list.
		 * - PendingUnlock: After being processed (while still locked), it has to go in pending unlock so that
		 *   the render thread unlocks it on the next render tick and finally returns it to the available list.
		 */

		// Perform lock-copy-unlock on the render thread for now. Simplest way.
		TUniquePtr<FTmvMediaFrameMips> FrameMips = UE::TmvMedia::ProcessReadbackRequest(InReadbackRequest);

		// Return the request to the pool now that we are done with it.
		ReleaseReadbackRequest(MoveTemp(InReadbackRequest));

		if (!FrameMips)
		{
			return;
		}

		TWeakPtr<FTmvMediaPlayerFrameProducerResource> LocalWeakThis = SharedThis(this);

		// When the converted render target is ready, we fire the rest of the cpu work on another thread.
		Async(ExecutionTarget, [LocalWeakThis, FrameMipsMovable = MoveTemp(FrameMips)]() mutable
		{
			if (TSharedPtr<FTmvMediaPlayerFrameProducerResource> LocalThis = LocalWeakThis.Pin())
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(UTmvMediaPlayerFrameProducer::ProcessFrame);
				UTmvMediaTranscodeJob* ParentJob = LocalThis->ParentJob_RenderThread.Get();
				if (UTmvMediaFrameConverter* FrameConverter = ParentJob ? ParentJob->GetStage<UTmvMediaFrameConverter>() : nullptr)
				{
					FrameConverter->ReceiveMips(ParentJob, MoveTemp(FrameMipsMovable));
				}
				--LocalThis->SubmittedCount;
			}
		});
	}
}