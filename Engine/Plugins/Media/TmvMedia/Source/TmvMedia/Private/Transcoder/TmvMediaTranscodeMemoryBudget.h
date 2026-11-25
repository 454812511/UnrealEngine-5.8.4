// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/UnrealString.h"
#include "CoreTypes.h"

namespace UE::TmvMedia
{
	/** A pool that can bound the frames a transcode stage keeps in flight. */
	enum class EFrameMemoryPool : uint8
	{
		/** OS physical memory: CPU buffers, plus the host side of anything staged through the GPU. */
		Host,

		/**
		 * The D3D shared-memory heaps that GPU readbacks stage through.
		 *
		 * Only consulted when the RHI reports memory stats. RHIGetMemoryStats is a no-op on RHIs that do
		 * not implement it, and the D3D12 RHI collects the stats only under STATS || CSV_PROFILER_STATS
		 * - true in every configuration except Shipping. Transcoding is driven from the editor, which
		 * has no Shipping configuration, so in practice the pool is always available; the fallback below
		 * exists because this module is Runtime-type and so still compiles into a packaged game.
		 *
		 * When the stats are unavailable the budget falls back to physical memory alone and
		 * FFrameMemoryBudget::bGpuSharedConsulted stays false.
		 */
		GpuShared,
	};

	/**
	 * What one in-flight frame costs, per pool. Stated separately because the pools charge for different
	 * things: a readback frame occupies the GPU-shared heap only for its staging, while its host cost
	 * also covers the CPU copy handed to the encoder and the decoder's read-ahead. Collapsing the two
	 * into one figure and testing it against the tighter pool understates what fits in the GPU-shared
	 * pool by the whole host multiplier.
	 */
	struct FFrameMemoryCost
	{
		/** Peak physical-memory footprint of one in-flight frame, including any GPU staging. */
		uint64 HostBytes = 0;

		/** Portion of the frame held in GPU-shared memory. 0 for stages that never stage through the GPU. */
		uint64 GpuSharedBytes = 0;
	};

	/** How much memory one in-flight frame needs, against how much the stage may use. */
	struct FFrameMemoryBudget
	{
		/** The per-pool cost this budget was computed against. */
		FFrameMemoryCost Cost;

		/** Fraction of each pool's free memory the in-flight set may occupy, after clamping. */
		float Fraction = 0.0f;

		/** OS physical memory currently available. */
		uint64 AvailablePhysical = 0;

		/**
		 * GPU-shared memory currently available. Only meaningful when bGpuSharedConsulted is true, and
		 * legitimately 0 there: the RHI clamps it to zero once the pool is over budget.
		 */
		uint64 AvailableGpuShared = 0;

		/**
		 * How far the GPU-shared pool is past its budget, when it is. Non-zero only alongside an
		 * AvailableGpuShared of 0, and separates "exactly no headroom left" from "already well over" -
		 * the first thing worth knowing when a budget collapses.
		 */
		uint64 GpuSharedOverBudget = 0;

		/** Frames that fit, before the caller's own ceiling. MAX_uint64 when a frame costs nothing. */
		uint64 AllowedFrames = 0;

		/** The pool that allowed the fewest frames, and so decided the budget. */
		EFrameMemoryPool BindingPool = EFrameMemoryPool::Host;

		/** BindingPool's per-frame cost, budgeted share and free memory: the numbers that decided. */
		uint64 BytesPerFrame = 0;
		uint64 BudgetedBytes = 0;
		uint64 Available = 0;

		/**
		 * True when the GPU-shared pool was charged for and the RHI reported usable stats. False means
		 * the budget rests on physical memory alone - either the frame has no GPU-shared cost, or the
		 * stats were unavailable in this build/RHI.
		 */
		bool bGpuSharedConsulted = false;

		/** False when a single in-flight frame exceeds the budgeted share of any pool it draws on. */
		bool bFitsSingleFrame = true;
	};

	/** Returns whether transcode stages should size their concurrency to available memory. */
	bool IsMemoryAwareConcurrencyEnabled();

	/**
	 * Budgets one in-flight frame against live memory stats, charging each pool only for the part of the
	 * frame that lives in it. InCost is the caller's own peak estimate, since each stage holds a
	 * different mix of buffers. The GPU-shared pool is consulted only when InCost charges it.
	 */
	FFrameMemoryBudget ComputeFrameMemoryBudget(const FFrameMemoryCost& InCost, float InBudgetFraction);

	/**
	 * Describes the pools behind the budget, naming the one that bound it. For logs and error reporting:
	 * which pool ran out is the question every budget-driven clamp or refusal raises.
	 */
	FString DescribeBudgetPools(const FFrameMemoryBudget& InBudget);

	/**
	 * Converts a budget into a frame count, never exceeding InRequestedFrames and never below 1, and
	 * logs the decision. InLogContext names the calling stage.
	 */
	int32 ClampConcurrencyToBudget(const FFrameMemoryBudget& InBudget, int32 InRequestedFrames, const TCHAR* InLogContext);
}
