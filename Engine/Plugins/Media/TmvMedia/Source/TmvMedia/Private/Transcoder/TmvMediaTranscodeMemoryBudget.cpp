// Copyright Epic Games, Inc. All Rights Reserved.

#include "Transcoder/TmvMediaTranscodeMemoryBudget.h"

#include "DynamicRHI.h"
#include "HAL/PlatformMemory.h"
#include "RHIStats.h"
#include "TmvMediaLog.h"

#include <atomic>

static int32 GTmvTranscodeMemoryAwareConcurrency = 1;
static FAutoConsoleVariableRef CVarTmvTranscodeMemoryAwareConcurrency(
	TEXT("TmvMedia.Transcode.MemoryAwareConcurrency"),
	GTmvTranscodeMemoryAwareConcurrency,
	TEXT("Set to 0 only to rule the memory clamp itself in or out when diagnosing a transcode problem; ")
	TEXT("a job that runs out of memory wants a lower MemoryBudgetFraction instead.\n")
	TEXT("At 1 (default) transcode stages cap the frames they keep in flight to what memory can hold, ")
	TEXT("measured at the first frame, and refuse work that cannot fit at all - this is what stops a ")
	TEXT("high-resolution transcode exhausting memory and hanging the GPU. At 0, the legacy fixed count ")
	TEXT("is used instead (NumProducerThreads, or 8 when unset)."),
	ECVF_RenderThreadSafe);

namespace UE::TmvMedia
{
	bool IsMemoryAwareConcurrencyEnabled()
	{
		return GTmvTranscodeMemoryAwareConcurrency != 0;
	}

	/**
	 * The GPU-shared pool is the one that overflows first on a high-resolution transcode, so losing it
	 * leaves the clamp measurably weaker. Say so rather than degrade silently - but only once, since
	 * this is reached from the render thread on every frame-size change.
	 *
	 * Not expected to fire for editor-driven transcodes: only Shipping drops the D3D12 stats, and the
	 * editor has no Shipping configuration. This covers an RHI that does not implement the query, or a
	 * transcode driven from a packaged game.
	 */
	static void WarnGpuSharedStatsUnavailableOnce()
	{
		static std::atomic<bool> bWarned = false;
		if (bWarned.exchange(true, std::memory_order_relaxed))
		{
			return;
		}

		UE_LOGF(LogTmvMedia, Warning,
			"Transcode memory budget: the RHI reports no GPU-shared memory stats, so in-flight frames "
			"are budgeted against physical memory alone. Either this RHI does not implement the query, "
			"or this is a Shipping build - the D3D12 stats need STATS or CSV_PROFILER_STATS. Readback "
			"staging lives in that pool, so the concurrency clamp is less protective here: lower "
			"TmvMedia.Transcode.MediaPlayer.MemoryBudgetFraction if a high-resolution transcode proves "
			"unstable.");
	}

	/** Frames that fit in a pool's budgeted share. Unbounded when the frame costs that pool nothing. */
	static uint64 FramesAllowedIn(uint64 InPoolBudget, uint64 InBytesPerFrame)
	{
		return (InBytesPerFrame > 0) ? (InPoolBudget / InBytesPerFrame) : MAX_uint64;
	}

	FFrameMemoryBudget ComputeFrameMemoryBudget(const FFrameMemoryCost& InCost, float InBudgetFraction)
	{
		FFrameMemoryBudget Budget;
		Budget.Cost = InCost;
		Budget.Fraction = FMath::Clamp(InBudgetFraction, 0.05f, 1.0f);
		Budget.AvailablePhysical = FPlatformMemory::GetStats().AvailablePhysical;

		if (InCost.GpuSharedBytes > 0)
		{
			// Readback heaps live in D3D shared memory, which runs out well before VRAM does on a
			// high-resolution transcode. Note this is the per-process DXGI non-local budget, not the
			// machine-wide shared segment that Task Manager reports, so it can be much smaller than the
			// latter suggests.
			FRHIMemoryStats RhiStats;
			RHIGetMemoryStats(RhiStats);
			if (RhiStats.IsValid() && RhiStats.BudgetSystem > 0)
			{
				// AvailableSystem is clamped to 0 once the pool goes over budget, with the overage moved
				// to DemotedSystem, so 0 here means "no headroom left" rather than "no data" -
				// bGpuSharedConsulted is what tells the two apart downstream.
				Budget.bGpuSharedConsulted = true;
				Budget.AvailableGpuShared = RhiStats.AvailableSystem;
				Budget.GpuSharedOverBudget = RhiStats.DemotedSystem;
			}
			else
			{
				// Not every RHI/build reports these; see EFrameMemoryPool::GpuShared.
				WarnGpuSharedStatsUnavailableOnce();
			}
		}

		// Charge each pool only for the part of the frame that lives in it, then let whichever pool
		// allows the fewest frames decide. Testing one combined figure against the tighter pool would
		// bill host-only bytes - the CPU frame copy, the decoder read-ahead - to the GPU-shared budget,
		// understating what fits there and, at high resolutions, refusing frames the pool has room for.
		const uint64 HostBudget = (uint64)(Budget.AvailablePhysical * Budget.Fraction);

		Budget.BindingPool = EFrameMemoryPool::Host;
		Budget.BytesPerFrame = InCost.HostBytes;
		Budget.BudgetedBytes = HostBudget;
		Budget.Available = Budget.AvailablePhysical;
		Budget.AllowedFrames = FramesAllowedIn(HostBudget, InCost.HostBytes);
		Budget.bFitsSingleFrame = InCost.HostBytes <= HostBudget;

		if (Budget.bGpuSharedConsulted)
		{
			const uint64 GpuSharedBudget = (uint64)(Budget.AvailableGpuShared * Budget.Fraction);
			const uint64 GpuSharedFrames = FramesAllowedIn(GpuSharedBudget, InCost.GpuSharedBytes);

			if (GpuSharedFrames < Budget.AllowedFrames)
			{
				Budget.BindingPool = EFrameMemoryPool::GpuShared;
				Budget.BytesPerFrame = InCost.GpuSharedBytes;
				Budget.BudgetedBytes = GpuSharedBudget;
				Budget.Available = Budget.AvailableGpuShared;
				Budget.AllowedFrames = GpuSharedFrames;
			}

			// A frame has to fit in every pool it draws on, whichever one ends up binding.
			Budget.bFitsSingleFrame = Budget.bFitsSingleFrame && (InCost.GpuSharedBytes <= GpuSharedBudget);
		}

		return Budget;
	}

	FString DescribeBudgetPools(const FFrameMemoryBudget& InBudget)
	{
		// Report each pool's free memory next to what a frame costs *that* pool: the two differ by the
		// host multiplier, and seeing them side by side is what makes a surprising verdict legible.
		FString Detail = FString::Printf(TEXT("physical ~%llu MB free vs ~%llu MB/frame"),
			InBudget.AvailablePhysical >> 20, InBudget.Cost.HostBytes >> 20);

		if (InBudget.bGpuSharedConsulted)
		{
			Detail += FString::Printf(TEXT("; gpu-shared ~%llu MB free"), InBudget.AvailableGpuShared >> 20);

			if (InBudget.GpuSharedOverBudget > 0)
			{
				Detail += FString::Printf(TEXT(" (over budget by ~%llu MB)"), InBudget.GpuSharedOverBudget >> 20);
			}

			Detail += FString::Printf(TEXT(" vs ~%llu MB/frame"), InBudget.Cost.GpuSharedBytes >> 20);
		}
		else if (InBudget.Cost.GpuSharedBytes > 0)
		{
			Detail += TEXT("; gpu-shared not reported by this RHI/build");
		}

		Detail += FString::Printf(TEXT("; %ls binding"),
			(InBudget.BindingPool == EFrameMemoryPool::GpuShared) ? TEXT("gpu-shared") : TEXT("physical"));

		return Detail;
	}

	int32 ClampConcurrencyToBudget(const FFrameMemoryBudget& InBudget, int32 InRequestedFrames, const TCHAR* InLogContext)
	{
		const int32 RequestedFrames = FMath::Max(1, InRequestedFrames);
		const int32 AllowedFrames = (int32)FMath::Clamp<uint64>(InBudget.AllowedFrames, 1, (uint64)RequestedFrames);

		const FString AvailableDetail = DescribeBudgetPools(InBudget);

		UE_LOGF(LogTmvMedia, Display,
			"%ls: memory-aware concurrency = %d frame(s) (requested up to %d). Binding pool: ~%llu MB "
			"per frame against a ~%llu MB budget of ~%llu MB available (%ls, fraction %.2f).",
			InLogContext, AllowedFrames, RequestedFrames,
			InBudget.BytesPerFrame >> 20, InBudget.BudgetedBytes >> 20, InBudget.Available >> 20,
			*AvailableDetail, InBudget.Fraction);

		return AllowedFrames;
	}
}
