// Copyright Epic Games, Inc. All Rights Reserved.

#include "MediaIOCoreDisplayedFieldRegistry.h"

#include "HAL/IConsoleManager.h"
#include "IMediaTextureSample.h"
#include "MediaDelegates.h"
#include "MediaIOCoreTextureSampleBase.h"
#include "Misc/ScopeLock.h"
#include "RenderingThread.h"

namespace UE::MediaIOCore
{
	namespace InterlaceFieldSync
	{
		static TAutoConsoleVariable<bool> CVarEnabled(
			TEXT("MediaIO.InterlaceFieldSync.Enabled"),
			false,
			TEXT("Top-level toggle for interlaced displayed-field sync. When true, any interlaced output follows the ")
			TEXT("field actually displayed for the synced interlaced input. When false the feature is inert ")
			TEXT("(legacy frame-parity behavior)."),
			ECVF_Default);

		static TAutoConsoleVariable<FString> CVarInputUrlFilter(
			TEXT("MediaIO.InterlaceFieldSync.InputUrlFilter"),
			TEXT(""),
			TEXT("Optional substring an interlaced input's source URL must contain for it to be the synced input. ")
			TEXT("Empty (default) matches any interlaced input, which is correct for the common single-input case. ")
			TEXT("Set this only to disambiguate when more than one interlaced input is open."),
			ECVF_Default);

		// Fixed join key between the tagging input and the following output. The hotfix exposes a single
		// input-to-output pairing, so the key never needs to be user-visible.
		static const FName SyncTag(TEXT("MediaIOInterlaceFieldSync"));

		bool IsEnabled()
		{
			return CVarEnabled.GetValueOnAnyThread();
		}

		FName GetSyncName()
		{
			if (!IsEnabled())
			{
				return NAME_None;
			}
			return SyncTag;
		}

		FName GetInputSyncName(const FString& InSourceUrl)
		{
			if (!IsEnabled())
			{
				return NAME_None;
			}
			const FString InputUrlFilter = CVarInputUrlFilter.GetValueOnAnyThread();
			if (!InputUrlFilter.IsEmpty() && !InSourceUrl.Contains(InputUrlFilter))
			{
				return NAME_None;
			}
			return GetSyncName();
		}
	}

	FDisplayedFieldRegistry& FDisplayedFieldRegistry::Get()
	{
		static FDisplayedFieldRegistry Instance;
		return Instance;
	}

	void FDisplayedFieldRegistry::Initialize()
	{
		FDisplayedFieldRegistry& Self = Get();
		if (!Self.SampleRenderedHandle.IsValid())
		{
			Self.SampleRenderedHandle = FMediaDelegates::OnSampleRendered_RenderThread.AddStatic(&FDisplayedFieldRegistry::OnSampleRendered_RenderThread);
		}
	}

	void FDisplayedFieldRegistry::Shutdown()
	{
		FDisplayedFieldRegistry& Self = Get();
		if (Self.SampleRenderedHandle.IsValid())
		{
			FMediaDelegates::OnSampleRendered_RenderThread.Remove(Self.SampleRenderedHandle);
			Self.SampleRenderedHandle.Reset();
		}

		FScopeLock Lock(&Self.CriticalSection);
		Self.PendingTags.Reset();
		Self.RecordsByTag.Reset();
	}

	void FDisplayedFieldRegistry::TagSample(const FMediaIOCoreTextureSampleBase* InSample, FName InSyncTag, EDisplayedFieldIdentity InField)
	{
		if (!InSample || InSyncTag.IsNone())
		{
			return;
		}

		// Key on the canonical interface pointer - the same one the rendered-sample delegate hands back.
		const IMediaTextureSample* Key = static_cast<const IMediaTextureSample*>(InSample);

		FScopeLock Lock(&CriticalSection);
		if (PendingTags.Num() >= MaxPendingTags)
		{
			// Only reached on sustained sample drops; a missing tag safely falls back to the parity path.
			PendingTags.Reset();
		}
		PendingTags.Add(Key, FPendingTag{ InSyncTag, InField, Key->GetTime().Time.GetTicks() });
	}

	void FDisplayedFieldRegistry::PropagateTag(const FMediaIOCoreTextureSampleBase* InSource, const FMediaIOCoreTextureSampleBase* InProxy)
	{
		if (!InSource || !InProxy)
		{
			return;
		}

		const IMediaTextureSample* SourceKey = static_cast<const IMediaTextureSample*>(InSource);
		const IMediaTextureSample* ProxyKey = static_cast<const IMediaTextureSample*>(InProxy);

		FScopeLock Lock(&CriticalSection);
		if (const FPendingTag* Found = PendingTags.Find(SourceKey))
		{
			// Re-stamp with the proxy's time (it is what renders) so the render-thread validation matches.
			FPendingTag Copy = *Found;
			Copy.ValidationTimeTicks = ProxyKey->GetTime().Time.GetTicks();
			PendingTags.Add(ProxyKey, Copy);
			PendingTags.Remove(SourceKey);
		}
	}

	bool FDisplayedFieldRegistry::Lookup(FName InSyncTag, uint64 InRenderFrame, EDisplayedFieldIdentity& OutField) const
	{
		FScopeLock Lock(&CriticalSection);
		if (const TArray<FRecord>* Records = RecordsByTag.Find(InSyncTag))
		{
			// Most recent first: the output usually asks for a frame at or near the newest record.
			for (int32 Index = Records->Num() - 1; Index >= 0; --Index)
			{
				if ((*Records)[Index].RenderFrame == InRenderFrame)
				{
					OutField = (*Records)[Index].Field;
					return true;
				}
			}
		}
		return false;
	}

	void FDisplayedFieldRegistry::Record(FName InSyncTag, uint64 InRenderFrame, EDisplayedFieldIdentity InField)
	{
		FScopeLock Lock(&CriticalSection);
		TArray<FRecord>& Records = RecordsByTag.FindOrAdd(InSyncTag);
		Records.Add(FRecord{ InRenderFrame, InField });
		if (Records.Num() > MaxRecordsPerTag)
		{
			Records.RemoveAt(0, Records.Num() - MaxRecordsPerTag);
		}
	}

	void FDisplayedFieldRegistry::OnSampleRendered_RenderThread(const FTextureResource* /*InTextureResource*/, TSharedPtr<IMediaTextureSample, ESPMode::ThreadSafe> InSample)
	{
		if (!InSample)
		{
			return;
		}

		FDisplayedFieldRegistry& Self = Get();
		const IMediaTextureSample* Key = InSample.Get();

		FPendingTag Tag;
		bool bFound = false;
		{
			FScopeLock Lock(&Self.CriticalSection);
			if (const FPendingTag* Found = Self.PendingTags.Find(Key))
			{
				// Reject a pooled pointer that was reused for a different sample before this one rendered.
				if (Found->ValidationTimeTicks == InSample->GetTime().Time.GetTicks())
				{
					Tag = *Found;
					bFound = true;
				}
				Self.PendingTags.Remove(Key);
			}
		}

		if (bFound && !Tag.SyncTag.IsNone())
		{
			Self.Record(Tag.SyncTag, GFrameCounterRenderThread, Tag.Field);
		}
	}
}
