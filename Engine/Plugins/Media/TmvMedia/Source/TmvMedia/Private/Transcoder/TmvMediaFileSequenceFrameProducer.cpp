// Copyright Epic Games, Inc. All Rights Reserved.

#include "Transcoder/TmvMediaFileSequenceFrameProducer.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "ImageUtils.h"
#include "ImageWrapperHelper.h"
#include "Misc/Paths.h"
#include "TmvMediaLog.h"
#include "Transcoder/TmvMediaFrameConverter.h"
#include "Transcoder/TmvMediaFrameMips.h"
#include "Transcoder/TmvMediaTranscodeJob.h"
#include "Transcoder/TmvMediaTranscodeMemoryBudget.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(TmvMediaFileSequenceFrameProducer)

#define LOCTEXT_NAMESPACE "TmvMediaFileSequenceFrameProducer"

static float GTmvTranscodeFileSequenceMemoryBudgetFraction = 0.6f;
static FAutoConsoleVariableRef CVarTmvTranscodeFileSequenceMemoryBudgetFraction(
	TEXT("TmvMedia.Transcode.FileSequence.MemoryBudgetFraction"),
	GTmvTranscodeFileSequenceMemoryBudgetFraction,
	TEXT("Lower this when a file-sequence transcode starves the rest of the machine, or raise it to ")
	TEXT("trade memory headroom for throughput on a machine dedicated to transcoding.\n")
	TEXT("Fraction (0.05..1.0) of currently-available physical memory the in-flight frames may occupy. ")
	TEXT("Default 0.6."),
	ECVF_Default);

static float GTmvTranscodeFileSequenceBytesPerFrameScale = 3.0f;
static FAutoConsoleVariableRef CVarTmvTranscodeFileSequenceBytesPerFrameScale(
	TEXT("TmvMedia.Transcode.FileSequence.BytesPerFrameScale"),
	GTmvTranscodeFileSequenceBytesPerFrameScale,
	TEXT("Change this only when the per-frame estimate is wrong for your content: compare the machine's ")
	TEXT("peak working set against the MB/frame the concurrency log reports. This is a cost model, not ")
	TEXT("a tuning knob - use MemoryBudgetFraction to trade memory for throughput.\n")
	TEXT("Multiplier on the decoded mip-chain size covering the converter's parallel buffer set, the ")
	TEXT("source image still alive during ChangeFormat, and the encoder's output buffer. Default 3.0, ")
	TEXT("measured against peak working set on a 16K sequence."),
	ECVF_Default);

namespace UE::TmvMedia
{
	/**
	 * Caps concurrent frame decoding to what physical memory can hold, from the measured size of a
	 * decoded mip 0. Only ever lowers InRequestedThreads, and returns it unchanged when the size is
	 * not yet known or the clamp is disabled.
	 */
	static int32 ComputeFileSequenceConcurrencyLimit(uint64 InMip0Bytes, bool bInGenerateMips, int32 InRequestedThreads)
	{
		if (!IsMemoryAwareConcurrencyEnabled() || InMip0Bytes == 0)
		{
			return InRequestedThreads;
		}

		// A frame in flight holds more than its mip 0: generated mips add a third, and the scale
		// covers the converter's parallel buffer set, the source image still alive during
		// ChangeFormat, and the encoder's output buffer. This path never stages through the GPU, so
		// only physical memory bounds it.
		const double MipChainFactor = bInGenerateMips ? (4.0 / 3.0) : 1.0;
		const uint64 BytesPerFrame = (uint64)(InMip0Bytes * MipChainFactor
			* (double)FMath::Max(1.0f, GTmvTranscodeFileSequenceBytesPerFrameScale));

		// Host only: this path decodes straight into CPU buffers, so it never charges the GPU-shared pool.
		FFrameMemoryCost Cost;
		Cost.HostBytes = BytesPerFrame;

		const FFrameMemoryBudget Budget = ComputeFrameMemoryBudget(
			Cost, GTmvTranscodeFileSequenceMemoryBudgetFraction);

		return ClampConcurrencyToBudget(Budget, InRequestedThreads, TEXT("FileSequenceFrameProducer"));
	}

	/**
	 * Assuming the filename is something like "filename00000.extension"
	 * We are assuming only one extension, not dot naming.
	 * We will look for a numerical value at the end of the filename and return that.
	 */
	bool GetFrameNumberFromFilename(const FString& InFilename, int32& OutFrameNumber)
	{
		// Exclude the extension in case it has a number in it, that doesn't count.
		FString BaseFilename = FPaths::GetBaseFilename(InFilename);

		bool bFoundNumber = false;

		// Find the first digit starting from the right.
		int32 Index = BaseFilename.Len() - 1;
		for (; Index >= 0; --Index)
		{
			if (FChar::IsDigit(BaseFilename[Index]))
			{
				break;
			}
		}

		// Did we find one?
		if (Index >= 0)
		{
			int32 LastNumberIndex = Index;
			// Find the next non digit...
			for (; Index >= 0; --Index)
			{
				bool bIsDigit = FChar::IsDigit(BaseFilename[Index]);
				if (bIsDigit == false)
				{
					break;
				}
			}

			// Index now points to the next non digit.
			// Extract the number.
			Index++;
			if (Index < BaseFilename.Len())
			{
				bFoundNumber = true;
				FString NumberString = BaseFilename.Mid(Index, LastNumberIndex - Index + 1);
				OutFrameNumber = FCString::Atoi(*NumberString);
			}
		}
		return bFoundNumber;
	}
}

UTmvMediaFileSequenceFrameProducer::UTmvMediaFileSequenceFrameProducer()
{
	bHasAsyncQueue = true;
}

bool UTmvMediaFileSequenceFrameProducer::Start(UTmvMediaTranscodeJob* InParentJob)
{
	Super::Start(InParentJob); // set status to "started".
	
	// This could be either the first image of the sequence, or a directory.
	// Note: must be resolved on the main thread because Zenloader can't flush async load on async thread.
	FString SequencePath = InParentJob->Settings.GetAbsoluteInputPath();

	TWeakObjectPtr<UTmvMediaTranscodeJob> ParentJobWeak = InParentJob;
	TWeakObjectPtr<UTmvMediaFileSequenceFrameProducer> SelfWeak(this);

	Async(EAsyncExecution::Thread, [SelfWeak, ParentJobWeak, SequencePath]()
	{
		TStrongObjectPtr<UTmvMediaTranscodeJob> ParentJob = ParentJobWeak.Pin();
		TStrongObjectPtr<UTmvMediaFileSequenceFrameProducer> Self = SelfWeak.Pin();
		if (ParentJob && Self)
		{
			Self->ProcessAllImages(ParentJob.Get(), SequencePath);
			Self->SetStageStatus(ETmvMediaTranscodeStageStatus::Stopped, ParentJob.Get());
		}
	});
	return true;
}

void UTmvMediaFileSequenceFrameProducer::RequestStop(UTmvMediaTranscodeJob* InParentJob)
{
	if (NumActiveFrames > 0)
	{
		// Stopping is not immediate, need to wait for the processing thread to return.
		SetStageStatus(ETmvMediaTranscodeStageStatus::Stopping, InParentJob);
		return;
	}

	Super::RequestStop(InParentJob);
}

void UTmvMediaFileSequenceFrameProducer::ProcessAllImages(UTmvMediaTranscodeJob* InParentJob, const FString& InSequencePath)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UTmvMediaFileSequenceFrameProducer::ProcessAllImages);

	// This could be either the first image of the sequence, or a directory.
	FString SequencePath = InSequencePath;
	FString FileExtension = TEXT("*");

	if (FPaths::FileExists(SequencePath))
	{
		// If a file with an extension is specified, use it to restrict the file search in the folder.
		FileExtension += FPaths::GetExtension(SequencePath, /*IncludeDot*/ true);
		// Extract the folder path.
		SequencePath = FPaths::GetPath(SequencePath);
	}

	if (!FPaths::DirectoryExists(SequencePath))
	{
		const FText Message = FText::Format(LOCTEXT("DirectoryNotFound", "Directory \"{0}\" does not exist"), FText::FromString(SequencePath));
		UE_LOGF(LogTmvMedia, Error, "FileSequenceFrameProducer: %ls.", *Message.ToString());
		UTmvMediaTranscodeJob::SafeReportError(InParentJob, Message);
		return;
	}

	// Get source files.
	TArray<FString> FoundFiles;
	IFileManager::Get().FindFiles(FoundFiles, *SequencePath, *FileExtension);
	FoundFiles.RemoveAll([](const FString& FileName) { return FileName.StartsWith(TEXT(".")); });
	FoundFiles.Sort();

	if (FoundFiles.Num() == 0)
	{
		const FText Message = FText::Format(LOCTEXT("NoFiles", "No files to import in directory \"{0}\" (Extension: {1})"),
			FText::FromString(SequencePath), FText::FromString(FileExtension));
		UE_LOGF(LogTmvMedia, Error, "FileSequenceFrameProducer: %ls.", *Message.ToString());
		UTmvMediaTranscodeJob::SafeReportError(InParentJob, Message);
		return;
	}

	UE_LOGF(LogTmvMedia, Log, "Found %i image files in %ls to import.", FoundFiles.Num(), *SequencePath);

	// Create image wrapper
	FString Ext = FPaths::GetExtension(FoundFiles[0]);
	EImageFormat ImageFormat = ImageWrapperHelper::GetImageFormat(Ext);

	if (ImageFormat == EImageFormat::Invalid)
	{
		const FText Message = FText::Format(LOCTEXT("InvalidFileFormat", "Invalid file format \"{0}\""), FText::FromString(Ext));
		UE_LOGF(LogTmvMedia, Error, "FileSequenceFrameProducer: %ls.", *Message.ToString());
		UTmvMediaTranscodeJob::SafeReportError(InParentJob, Message);
		return;
	}

	// Update the video track info
	if (InParentJob->Settings.FrameRate.Numerator > 0 && InParentJob->Settings.FrameRate.Denominator > 0)
	{
		FTmvMediaFrameProducerTrackInfo LocalVideoTrackInfo = GetVideoTrackInfo();
		LocalVideoTrackInfo.FrameRate = InParentJob->Settings.FrameRate;
		LocalVideoTrackInfo.Duration = FTimespan::FromSeconds(LocalVideoTrackInfo.FrameRate.AsInterval() * static_cast<double>(FoundFiles.Num()));
		SetVideoTrackInfo(LocalVideoTrackInfo);
	}

	// Get number of threads to use.
	int32 NumThreads = InParentJob->Settings.NumProducerThreads;
	if (NumThreads <= 0)
	{
		NumThreads = 8;
	}

	// Upper bound for this to avoid overloading the system. Todo: Make this configurable?
	NumThreads = FMath::Min(NumThreads, FPlatformMisc::NumberOfCores());

	int32 FirstFrameIndex = 0;
	if (FoundFiles.Num() > 0)
	{
		UE::TmvMedia::GetFrameNumberFromFilename(FoundFiles[0], FirstFrameIndex);
	}
	
	// Loop through all files.
	int NumDone = 0;
	int TotalNum = FoundFiles.Num();
	NumActiveFrames = 0;
	MeasuredMip0Bytes = 0;
	// With the clamp disabled this starts latched, so the loop fans out immediately as it used to.
	bool bConcurrencyClamped = !UE::TmvMedia::IsMemoryAwareConcurrencyEnabled();
	for (const FString& FileName : FoundFiles)
	{
		// Wait for threads to finish if we have too many. Poll finely: the memory clamp can settle on
		// one or two frames for high-resolution sequences, and at that width the loop sits at capacity
		// on every iteration, so a coarse sleep would be added to each frame's turnaround.
		while (NumActiveFrames >= NumThreads)
		{
			FPlatformProcess::Sleep(0.01f);
		}

		++NumDone;

		UTmvMediaTranscodeJob::SafeUpdateProgress(InParentJob, NumDone, TotalNum);

		++NumActiveFrames;

		const FString FullFileName = FPaths::Combine(SequencePath, FileName);

#if WITH_EDITOR
		EAsyncExecution ExecutionTarget = EAsyncExecution::LargeThreadPool;
#else
		EAsyncExecution ExecutionTarget = EAsyncExecution::ThreadPool;
#endif

		TWeakObjectPtr<UTmvMediaTranscodeJob> ParentJobWeak = InParentJob;
		TWeakObjectPtr<UTmvMediaFileSequenceFrameProducer> SelfWeak(this);

		Async(ExecutionTarget, [SelfWeak, FullFileName, ParentJobWeak, FirstFrameIndex]() mutable
		{
			if (TStrongObjectPtr<UTmvMediaFileSequenceFrameProducer> Self = SelfWeak.Pin())
			{
				if (TStrongObjectPtr<UTmvMediaTranscodeJob> ParentJob = ParentJobWeak.Pin())
				{
					Self->ProcessImage(ParentJob.Get(), FullFileName, FirstFrameIndex);
				}
				--Self->NumActiveFrames;
			}
		});

		if (!bConcurrencyClamped)
		{
			// Let the first frame decode on its own before fanning out: the source resolution is only
			// known once an image has been decoded. Waiting on the measurement rather than on the 
			// frame releases the fan-out as soon as the size is known, without also serializing
			// that frame's mip generation, conversion and encode.
			while (MeasuredMip0Bytes.load(std::memory_order_relaxed) == 0
				&& NumActiveFrames > 0
				&& !IsCancelled(InParentJob))
			{
				FPlatformProcess::Sleep(0.01f);
			}

			// Retry on the next frame if this one failed to decode, rather than leaving the budget
			// unset for the rest of the job.
			const uint64 MeasuredBytes = MeasuredMip0Bytes.load(std::memory_order_relaxed);
			if (MeasuredBytes > 0)
			{
				NumThreads = UE::TmvMedia::ComputeFileSequenceConcurrencyLimit(
					MeasuredBytes, InParentJob->Settings.bEnableMipMapping, NumThreads);
				bConcurrencyClamped = true;
			}
		}

		// Do we want to cancel?
		if (IsCancelled(InParentJob))
		{
			break;
		}
	}

	// Wait for all our tasks to finish.
	while (NumActiveFrames > 0) // todo: add timeout? (there isn't one in ProcessExr either)
	{
		FPlatformProcess::Sleep(0.2f);
	}
}

bool UTmvMediaFileSequenceFrameProducer::IsCancelled(const UTmvMediaTranscodeJob* InParentJob) const
{
	return !IsValid(InParentJob) || !InParentJob->IsRunning() || GetStageStatus() != ETmvMediaTranscodeStageStatus::Started;
}

bool UTmvMediaFileSequenceFrameProducer::ProcessImage(UTmvMediaTranscodeJob* InParentJob, const FString& InImagePath, int32 InFirstFrameIndex)
{
	TSharedPtr<FImage> Image = MakeShared<FImage>();

	// Load image into buffer.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UTmvMediaFileSequenceFrameProducer::ProcessImageCustom:LoadImage);
		if (!FImageUtils::LoadImage(*InImagePath, *Image))
		{
			UE_LOGF(LogTmvMedia, Error, "Failed to load %ls", *InImagePath);
			return false;
		}
	}

	// Convert to linear float16 if not already.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UTmvMediaFileSequenceFrameProducer::ProcessImageCustom:ChangeFormat);
		Image->ChangeFormat(ERawImageFormat::RGBA16F, EGammaSpace::Linear);
	}

	// Record the decoded size once, so ProcessAllImages can size the concurrency budget. Stored after
	// the format change because that is the representation carried through the rest of the pipeline.
	if (MeasuredMip0Bytes.load(std::memory_order_relaxed) == 0)
	{
		MeasuredMip0Bytes.store((uint64)Image->GetImageSizeBytes(), std::memory_order_relaxed);
	}

	// Push to next stage
	if (UTmvMediaFrameConverter* FrameConverter = InParentJob->GetStage<UTmvMediaFrameConverter>())
	{
		TUniquePtr<FTmvMediaFrameMips> FrameMips = MakeUnique<FTmvMediaFrameMips>();
		// Extract frame number from the name.
		UE::TmvMedia::GetFrameNumberFromFilename(InImagePath, FrameMips->TimeInfo.FrameIndex);
		FrameMips->TimeInfo.FrameIndexNoOffset = FrameMips->TimeInfo.FrameIndex - InFirstFrameIndex;
		FrameMips->TimeInfo.FrameDuration = FTimespan::FromSeconds(InParentJob->Settings.FrameRate.AsInterval());
		FrameMips->TimeInfo.FrameTime = FrameMips->TimeInfo.FrameDuration * FrameMips->TimeInfo.FrameIndex;
		FrameMips->Init(Image, InParentJob->Settings.bEnableMipMapping);
		FrameConverter->ReceiveMips(InParentJob, MoveTemp(FrameMips));
	}
	return true;
}

#undef LOCTEXT_NAMESPACE