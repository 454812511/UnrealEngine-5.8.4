// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Async/Mutex.h"
#include "Containers/ContainersFwd.h"
#include "Containers/Map.h"
#include "Decoder/ITmvMediaDemuxer.h"
#include "IImgMediaReader.h"
#include "Readers/TmvOverflowHysteresis.h"
#include "Templates/SharedPointer.h"

class FImgMediaLoader;
class FTmvMediaFrameMipBufferPool;
class ITmvMediaDecoderAccessUnit;
class ITmvMediaDecoderFactory;
class ITmvMediaDemuxer;
class ITmvMediaDemuxerFactory;

namespace UE::ImgMedia
{
	class FTmvParserPool;
	class FTmvDecoderPool;
}

/**
 * Abstract base class for TMV image media readers.
 * Provides the common decode pipeline (parse, decode, compose).
 * Subclasses provide the access unit for each frame.
 */
class FTmvImgMediaReader : public IImgMediaReader
{
public:
	FTmvImgMediaReader(const TSharedRef<FImgMediaLoader>& InLoader, const TSharedRef<ITmvMediaDecoderFactory>& InDecoderFactory);
	virtual ~FTmvImgMediaReader() override;

	/** Get the list of supported image file extensions for TMV decoders. */
	static TArray<FString> GetSupportedImageFileExtensions();
	
	/** Get the list of supported container file extensions for TMV demuxers. */
	static TArray<FString> GetSupportedContainerFileExtensions();

	/**
	 * Returns true if the given image file extension is supported by a Tmv Reader. 
	 */
	static bool IsImageFileExtensionSupported(const FString& InExtension);

	/**
	 * Returns true if the given container file extension is supported by a Tmv Reader. 
	 */
	static bool IsContainerFileExtensionSupported(const FString& InExtension);

	/** 
	 * Creates a reader of the appropriate type depending on the context (input file, settings, etc).
	 * @param InLoader Parent Img Loader
	 * @param InMediaFilePath Full path of the media file, either the first image of the sequence or the container.
	 */
	static TSharedPtr<FTmvImgMediaReader> CreateReader(const TSharedRef<FImgMediaLoader>& InLoader, const FString& InMediaFilePath);
	
	/**
	 * Returns the video track info if present, nullptr otherwise.
	 */
	virtual const FTmvMediaDemuxerTrackInfo* GetVideoTrackInfo() const
	{
		return nullptr;
	}

public:
	//~ Begin IImgMediaReader
	virtual bool GetFrameInfo(const FString& InImagePath, FImgMediaFrameInfo& OutInfo) override;
	virtual bool ReadFrame(int32 InFrameId, const TMap<int32, FMediaTileSelection>& InMipTiles, TSharedPtr<FImgMediaFrame, ESPMode::ThreadSafe> OutFrame) override;
	virtual void CancelFrame(int32 InFrameNumber) override;
	virtual void UncancelFrame(int32 InFrameNumber) override;
	virtual TSharedPtr<FImgMediaTextureSample, ESPMode::ThreadSafe> CreateSample() override;
	//~ End IImgMediaReader

protected:
	/**
	 * Create an access unit for the given frame.
	 * @param InFrameId Frame index.
	 * @return A valid access unit, or null on failure.
	 */
	virtual TUniquePtr<ITmvMediaDecoderAccessUnit> CreateAccessUnit(int32 InFrameId) = 0;

	/** Parent loader. */
	TWeakPtr<FImgMediaLoader> LoaderWeak;

	/** Decoder factory to create decoders on demand. */
	TSharedRef<ITmvMediaDecoderFactory> DecoderFactory;

	/** Parser Pool for this image reader. We need a parser per worker thread. */
	TSharedPtr<UE::ImgMedia::FTmvParserPool> ParserPool;

	/** Decoder Pool for this image reader. We need a decoder per worker thread. */
	TSharedPtr<UE::ImgMedia::FTmvDecoderPool> DecoderPool;

	/** Texture Sample Buffer Pool for this image reader. */
	TSharedPtr<FTmvMediaFrameMipBufferPool> FrameMipBufferPool;

	/** Residency-overflow upscale-fallback hysteresis (see FTmvOverflowHysteresis). */
	FTmvOverflowHysteresis OverflowHysteresis;

	/**
	 * Number of times the loader has canceled this frame.
	 *
	 * ReadFrame snapshots it on entry and treats itself as canceled once it has moved on, so a
	 * cancel only reaches reads that were already running when it was issued. A plain per-frame
	 * flag would also cancel the read the loader issues to replace the abandoned one, and would be
	 * cleared by that read's UncancelFrame while the abandoned read is still in flight.
	 */
	uint32 GetCancelGeneration(int32 InFrameId) const;

private:
	/** Serializes the reads of one FImgMediaFrame. */
	struct FFrameReadLock
	{
		UE::FMutex Mutex;

		/** Reads holding or waiting on Mutex. Guarded by FrameReadLocksMutex. */
		int32 NumUsers = 0;
	};

	/**
	 * Blocks until no other read of InFrame is in progress, then returns the lock this read holds
	 * until it calls ReleaseFrameReadLock.
	 *
	 * The loader can issue a second read of a frame while an earlier one is still running - it
	 * drops frames it stops wanting and re-requests them later, and a read already past its
	 * cancellation checks keeps going. When the frame is cached, both reads receive the same
	 * FImgMediaFrame, and with it the same sample converter, converter parameters and per-mip tile
	 * residency, none of which are safe to mutate from two reads at once.
	 */
	TSharedRef<FFrameReadLock> AcquireFrameReadLock(const FImgMediaFrame* InFrame, int32 InFrameId);

	void ReleaseFrameReadLock(const FImgMediaFrame* InFrame, const TSharedRef<FFrameReadLock>& InLock);

	/**
	 * Frame number -> times canceled. Written from the loader's thread, read by decode workers.
	 * Entries are never removed, deliberately: a read in flight compares against its frame's count,
	 * so resetting one would revive it. It holds at most one entry per frame of the sequence.
	 */
	TMap<int32, uint32> CancelGenerations;

	mutable UE::FMutex CancelGenerationsMutex;

	/** Read lock per frame, present only while some read of that frame holds or waits on it. */
	TMap<const FImgMediaFrame*, TSharedRef<FFrameReadLock>> FrameReadLocks;

	UE::FMutex FrameReadLocksMutex;
};

/**
 * TMV reader for file sequences: one file per frame.
 */
class FTmvFileSequenceImgMediaReader final : public FTmvImgMediaReader
{
public:
	FTmvFileSequenceImgMediaReader(const TSharedRef<FImgMediaLoader>& InLoader, const TSharedRef<ITmvMediaDecoderFactory>& InDecoderFactory);

protected:
	//~ Begin FTmvImgMediaReader
	virtual TUniquePtr<ITmvMediaDecoderAccessUnit> CreateAccessUnit(int32 InFrameId) override;
	//~ End FTmvImgMediaReader
};

/**
 * TMV reader for container files: reads frames from a .tmv container via ITmvMediaDemuxer.
 * Uses ReadSampleInfo (metadata only) + direct file reads (no intermediate memory buffer).
 */
class FTmvContainerImgMediaReader final : public FTmvImgMediaReader
{
public:
	FTmvContainerImgMediaReader(
		const TSharedRef<FImgMediaLoader>& InLoader,
		const TSharedRef<ITmvMediaDecoderFactory>& InDecoderFactory,
		const TSharedPtr<ITmvMediaDemuxer, ESPMode::ThreadSafe>& InDemuxer,
		int32 InTrackIndex,
		const FString& InContainerFilePath);

	virtual ~FTmvContainerImgMediaReader() override;

	//~ Begin FTmvImgMediaReader
	virtual const FTmvMediaDemuxerTrackInfo* GetVideoTrackInfo() const override
	{
		return (VideoTrackInfo.TrackType == ETmvMediaTrackType::Video) ? &VideoTrackInfo : nullptr;
	}
	//~ End FTmvImgMediaReader

	//~ Begin IImgMediaReader
	virtual FVariant GetMediaInfo(FName InfoName) const override;
	//~ End IImgMediaReader

protected:
	//~ Begin FTmvImgMediaReader
	virtual TUniquePtr<ITmvMediaDecoderAccessUnit> CreateAccessUnit(int32 InFrameId) override;
	//~ End FTmvImgMediaReader

private:
	/** Demuxer for reading sample info from the container. */
	TSharedPtr<ITmvMediaDemuxer, ESPMode::ThreadSafe> Demuxer;

	/** Serializes SeekToSample + ReadSampleInfo on the demuxer. */
	FCriticalSection DemuxerCS;

	/** Video track index in the demuxer. */
	int32 DemuxerTrackIndex = 0;

	/** Cached video track info from the demuxer. */
	FTmvMediaDemuxerTrackInfo VideoTrackInfo;

	/** Path to the container file (for creating per-thread file access units). */
	FString ContainerFilePath;
};
