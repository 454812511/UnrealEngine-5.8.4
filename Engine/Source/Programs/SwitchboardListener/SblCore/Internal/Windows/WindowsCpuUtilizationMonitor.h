// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#ifndef _WIN32_WINDOWS
#define _WIN32_WINDOWS 0x0410
#endif

#include "CpuUtilizationMonitor.h"
#include "Async/Mutex.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include <Pdh.h>
#include <PdhMsg.h>
#include "Windows/HideWindowsPlatformTypes.h"


class FWindowsCpuUtilizationMonitor : public FGenericCpuUtilizationMonitor
{
public:
	FWindowsCpuUtilizationMonitor();
	~FWindowsCpuUtilizationMonitor();

	bool IsInitialized() const override
	{
		return bIsInitialized;
	}

	bool GetPerCoreUtilization(TArray<int8>& OutCoreUtilization) override;

private:
	bool QueryUpdatedUtilization();

	bool bIsInitialized = false;

	HQUERY QueryHandle = INVALID_HANDLE_VALUE;
	/** Wildcard counter covering every logical processor instance of "Processor Information". */
	PDH_HCOUNTER CounterHandle = nullptr;
	/** Guards the query, scratch buffer, and cached results against concurrent status requests. */
	UE::FMutex QueryMutex;
	/** Scratch storage for PdhGetFormattedCounterArray; grows to fit and is then reused. */
	TArray<uint8> ItemBuffer;
	double LastQueryTime = -DBL_MAX;
	bool bLastQuerySucceeded = false;
	bool bWarnedUnexpectedStatus = false;
	TArray<int8> CachedCoreUtilization;
};

typedef FWindowsCpuUtilizationMonitor FCpuUtilizationMonitor;
