// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/StringView.h"
#include "Containers/UnrealString.h"

namespace UE::CaptureManager
{

// Token namespace/key constants used in the two-pass encoder command resolution scheme.
// CaptureManagerEncoderNamingTokens.h/cpp registers UNamingTokens subclasses that are
// auto-discovered by UNamingTokensEngineSubsystem in both LiveLinkHub and UE Editor contexts.
namespace VideoEncoderTokens
{
static constexpr FStringView Namespace = TEXTVIEW("cmvidenc");
static constexpr FStringView InputKey = TEXTVIEW("input");
static constexpr FStringView OutputKey = TEXTVIEW("output");
static constexpr FStringView ParamsKey = TEXTVIEW("params");
}

namespace AudioEncoderTokens
{
static constexpr FStringView Namespace = TEXTVIEW("cmaudenc");
static constexpr FStringView InputKey = TEXTVIEW("input");
static constexpr FStringView OutputKey = TEXTVIEW("output");
}

namespace EncoderDefaults
{
static constexpr FStringView AudioCommandArgs = TEXTVIEW("-i {input} -vn {output}");
// -fps_mode passthrough emits exactly one output image per decoded frame in decode order.
// Without it, ffmpeg's default CFR resampling duplicates a video frame at each timing-drift
// crossing (the camera runs slightly below 30 fps, e.g. 29.9939), which desyncs the RGB image
// sequence from the sequentially-read depth (RGB gains frames, drift accumulates with take
// length). Passthrough keeps the video 1:1 with depth, matching the WMF ingest route.
static constexpr FStringView VideoCommandArgs = TEXTVIEW("-noautorotate -i {input} -an {params} -q:v 1 -qmax 1 -qmin 1 -fps_mode passthrough -start_number 0 {output}");

// Previously-shipped default video command (pre -fps_mode passthrough). Earlier builds persisted the
// default straight into the per-user config (defaultconfig), freezing it. The settings migration
// rewrites any stored value still matching this to the current VideoCommandArgs default so the
// RGB/depth sync fix reaches existing users.
static constexpr FStringView LegacyVideoCommandArgs_NoFpsMode = TEXTVIEW("-noautorotate -i {input} -an {params} -q:v 1 -qmax 1 -qmin 1 -start_number 0 {output}");
}

}

// Encoder path and Pass-1-resolved encoder args supplied by the caller.
// CaptureDataConverter constructs the private command objects from this internally.
struct FCaptureManagerEncoderConfig
{
	FString EncoderPath;
	FString EncoderArgs;
};
