// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Delegates/IDelegateInstance.h"
#include "HAL/CriticalSection.h"
#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

#define UE_API MEDIAIOCORE_API

class FMediaIOCoreTextureSampleBase;
class FTextureResource;
class IMediaTextureSample;

namespace UE::MediaIOCore
{
	/** Which field of an interlaced source a deinterlaced sample represents. */
	enum class EDisplayedFieldIdentity : uint8
	{
		Unknown,
		FirstField,
		SecondField,
	};

	/**
	 * Runtime configuration seam for the interlaced displayed-field sync feature.
	 *
	 * The feature is driven by console variables. This is the single place that decides which inputs tag
	 * their samples and which outputs follow them; swap the implementation here to change the policy without
	 * touching any of the call sites. A single input-to-output pairing is supported, gated by one bool, with an
	 * optional URL filter to pick the input when more than one interlaced input is open.
	 */
	namespace InterlaceFieldSync
	{
		/**
		 * Top-level runtime gate (MediaIO.InterlaceFieldSync.Enabled). When false the whole feature is inert and
		 * adds no per-sample overhead.
		 */
		UE_API bool IsEnabled();

		/**
		 * The sync name an interlaced output should follow, or NAME_None when disabled. This is a fixed internal
		 * join key, not user-configurable; the hotfix supports the common one-input-to-one-output case.
		 */
		UE_API FName GetSyncName();

		/**
		 * Sync name an interlaced input should tag its samples with: GetSyncName() when InSourceUrl matches the
		 * configured input URL filter (or the filter is empty, matching any input), otherwise NAME_None (the
		 * input is left untagged).
		 */
		UE_API FName GetInputSyncName(const FString& InSourceUrl);
	}

	/**
	 * Process-wide bridge that lets an interlaced output place each engine frame into the output field matching
	 * the field actually displayed for a named input, independent of pipeline latency or genlock phase.
	 *
	 * Held in Internal/ and keyed by sample in a side table (rather than tagging IMediaTextureSample or
	 * FMediaIOCoreTextureSampleBase) so the feature adds no engine vtable or class-layout change for hotfixes.
	 */
	class FDisplayedFieldRegistry
	{
	public:
		/** Returns the process-wide singleton. */
		static UE_API FDisplayedFieldRegistry& Get();

		/** Subscribes the render-thread recorder to the media-sample-rendered delegate. Called once at module startup. */
		static UE_API void Initialize();

		/** Unsubscribes the recorder and drops all state. Called once at module shutdown. */
		static UE_API void Shutdown();

		/**
		 * Associates a not-yet-rendered MediaIO sample with the input it came from and the field it represents.
		 * No-op when InSyncTag is NAME_None. Safe to call from any thread.
		 */
		UE_API void TagSample(const FMediaIOCoreTextureSampleBase* InSample, FName InSyncTag, EDisplayedFieldIdentity InField);

		/**
		 * Carries a pending tag from a source sample to the just-in-time render proxy that is actually rendered.
		 * Must be called wherever a proxy copies configuration from a source sample, or the displayed-field
		 * record is lost for JIT-rendered inputs. No-op if InSource was never tagged.
		 */
		UE_API void PropagateTag(const FMediaIOCoreTextureSampleBase* InSource, const FMediaIOCoreTextureSampleBase* InProxy);

		/**
		 * Looks up the field displayed for the named input at the given render-thread frame.
		 * @return true if a matching record was found.
		 */
		UE_API bool Lookup(FName InSyncTag, uint64 InRenderFrame, EDisplayedFieldIdentity& OutField) const;

	private:
		/** Pending field metadata for a sample that has not yet been rendered. */
		struct FPendingTag
		{
			FName SyncTag;
			EDisplayedFieldIdentity Field = EDisplayedFieldIdentity::Unknown;

			/** Sample time (ticks) captured at tag time; re-checked at render time to reject a reused pooled pointer. */
			int64 ValidationTimeTicks = 0;
		};

		/** A displayed-field record for one render-thread frame. */
		struct FRecord
		{
			uint64 RenderFrame = 0;
			EDisplayedFieldIdentity Field = EDisplayedFieldIdentity::Unknown;
		};

		/** Render-thread handler broadcast by FMediaDelegates::OnSampleRendered_RenderThread. */
		static void OnSampleRendered_RenderThread(const FTextureResource* InTextureResource, TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe> InSample);

		/** Records that the named input displayed the given field at the given render frame. */
		void Record(FName InSyncTag, uint64 InRenderFrame, EDisplayedFieldIdentity InField);

		/**
		 * Pending tags keyed by the canonical IMediaTextureSample pointer (the same pointer the rendered-sample
		 * delegate hands back). Entries are erased on render and the table is bounded to MaxPendingTags.
		 */
		TMap<const IMediaTextureSample*, FPendingTag> PendingTags;

		/** Bounded recent displayed-field history per input sync name. */
		TMap<FName, TArray<FRecord>> RecordsByTag;

		/** Guards both maps for cross-thread access (deinterlace/game vs render vs output any-thread). */
		mutable FCriticalSection CriticalSection;

		/** Handle to the OnSampleRendered_RenderThread subscription. */
		FDelegateHandle SampleRenderedHandle;

		/** A few frames of history per input is plenty; bounds memory and lookup cost. */
		static constexpr int32 MaxRecordsPerTag = 16;

		/** Upper bound on un-rendered tags; protects against samples that are dropped before being rendered. */
		static constexpr int32 MaxPendingTags = 256;
	};
}

#undef UE_API
