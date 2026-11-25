// Copyright Epic Games, Inc. All Rights Reserved.

#include "FootageIngest/Utils/MetaHumanMediaSourceReader.h"
#include "MetaHumanCaptureSourceLog.h"

#if PLATFORM_MAC

TSharedPtr<IMetaHumanMediaAudioSourceReader> IMetaHumanMediaAudioSourceReader::Create()
{
	UE_LOGF(LogMetaHumanCaptureSource, Error,
		"MetaHumanMediaAudioSourceReader is not supported on this platform. Use Live Link Hub, or the Capture Manager Python / Blueprint ingest interface.");
	return nullptr;
}

TSharedPtr<IMetaHumanMediaVideoSourceReader> IMetaHumanMediaVideoSourceReader::Create()
{
	UE_LOGF(LogMetaHumanCaptureSource, Error,
		"MetaHumanMediaVideoSourceReader is not supported on this platform. Use Live Link Hub, or the Capture Manager Python / Blueprint ingest interface.");
	return nullptr;
}

#endif	// PLATFORM_MAC
