// Copyright Epic Games, Inc. All Rights Reserved.

#include "Windows/WindowsCpuUtilizationMonitor.h"
#include "HAL/PlatformTime.h"
#include "Async/UniqueLock.h"


FWindowsCpuUtilizationMonitor::FWindowsCpuUtilizationMonitor()
{
	PDH_STATUS PdhStatus = ::PdhOpenQuery(nullptr, 0, &QueryHandle);
	if (PdhStatus != ERROR_SUCCESS)
	{
		UE_LOGF(LogCpuUtilizationMonitor, Warning, "PdhOpenQuery failed. Error code: 0x%08X", PdhStatus);
		return;
	}

	// The legacy "\Processor(N)" object only exposes the first 64 logical processors.
	// "Processor Information" covers every logical processor, and we use a single
	// wildcard counter and let PDH enumerate whatever instances actually exist.
	PdhStatus = ::PdhAddEnglishCounter(QueryHandle, TEXT("\\Processor Information(*)\\% Processor Time"), 0, &CounterHandle);
	if (PdhStatus != ERROR_SUCCESS)
	{
		UE_LOGF(LogCpuUtilizationMonitor, Warning, "PdhAddEnglishCounter failed. Error code: 0x%08X", PdhStatus);
		return;
	}

	// Initial condition: ensures we've collected twice before the first attempt to get values.
	PdhStatus = ::PdhCollectQueryData(QueryHandle);
	if (PdhStatus != ERROR_SUCCESS)
	{
		UE_LOGF(LogCpuUtilizationMonitor, Warning, "PdhCollectQueryData failed. Error code: 0x%08X", PdhStatus);
		return;
	}

	bIsInitialized = true;
}

FWindowsCpuUtilizationMonitor::~FWindowsCpuUtilizationMonitor()
{
	if (QueryHandle != INVALID_HANDLE_VALUE)
	{
		PDH_STATUS PdhStatus = ::PdhCloseQuery(QueryHandle);
		if (PdhStatus != ERROR_SUCCESS)
		{
			UE_LOGF(LogCpuUtilizationMonitor, Warning, "PdhCloseQuery failed. Error code: 0x%08X", PdhStatus);
		}
	}
}

bool FWindowsCpuUtilizationMonitor::GetPerCoreUtilization(TArray<int8>& OutCoreUtilization)
{
	if (!IsInitialized())
	{
		return false;
	}

	// Status requests are serviced concurrently on the thread pool and share this monitor.
	UE::TUniqueLock<UE::FMutex> Lock(QueryMutex);

	// Rate limiting. MSDN recommends <= 1 Hz.
	const double MinQueryIntervalSec = 1.0;
	const double TimeNow = FPlatformTime::Seconds();
	if (TimeNow - LastQueryTime >= MinQueryIntervalSec)
	{
		// Updated unconditionally so that a persistently failing query (and its warning) is also rate limited.
		LastQueryTime = TimeNow;

		bLastQuerySucceeded = QueryUpdatedUtilization();
	}

	if (!bLastQuerySucceeded)
	{
		return false;
	}

	OutCoreUtilization = CachedCoreUtilization;

	return true;
}

bool FWindowsCpuUtilizationMonitor::QueryUpdatedUtilization()
{
	PDH_STATUS PdhStatus = ::PdhCollectQueryData(QueryHandle);
	if (PdhStatus != ERROR_SUCCESS)
	{
		UE_LOGF(LogCpuUtilizationMonitor, Warning, "PdhCollectQueryData failed. Error code: 0x%08X", PdhStatus);
		return false;
	}

	// Use PDH_FMT_DOUBLE here because PDH_FMT_LONG truncates instead of rounding.
	const DWORD Format = PDH_FMT_DOUBLE;

	// Always pass zero to get required size; per MSDN, a non-zero but insufficient buffer is unreliable.
	DWORD BufferSize = 0;
	DWORD ItemCount = 0;
	PdhStatus = ::PdhGetFormattedCounterArray(CounterHandle, Format, &BufferSize, &ItemCount, nullptr);
	if (PdhStatus != PDH_MORE_DATA)
	{
		UE_LOGF(LogCpuUtilizationMonitor, Warning, "PdhGetFormattedCounterArray (size query) failed. Error code: 0x%08X", PdhStatus);
		return false;
	}

	if (static_cast<DWORD>(ItemBuffer.Num()) < BufferSize)
	{
		ItemBuffer.SetNumUninitialized(BufferSize);
	}
	BufferSize = ItemBuffer.Num();
	PdhStatus = ::PdhGetFormattedCounterArray(CounterHandle, Format, &BufferSize, &ItemCount, reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM>(ItemBuffer.GetData()));
	if (PdhStatus != ERROR_SUCCESS)
	{
		UE_LOGF(LogCpuUtilizationMonitor, Warning, "PdhGetFormattedCounterArray failed. Error code: 0x%08X", PdhStatus);
		return false;
	}

	const PPDH_FMT_COUNTERVALUE_ITEM Items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM>(ItemBuffer.GetData());

	CachedCoreUtilization.Reset(ItemCount);
	for (DWORD ItemIdx = 0; ItemIdx < ItemCount; ++ItemIdx)
	{
		const PDH_FMT_COUNTERVALUE_ITEM& Item = Items[ItemIdx];

		// The wildcard also matches the per-node and machine-wide "_Total" aggregates.
		if (FCString::Strstr(Item.szName, TEXT("_Total")) != nullptr)
		{
			continue;
		}

		switch (Item.FmtValue.CStatus)
		{
		case PDH_CSTATUS_VALID_DATA:
		case PDH_CSTATUS_NEW_DATA:
			CachedCoreUtilization.Add(static_cast<int8>(FMath::Clamp(FMath::RoundToInt(Item.FmtValue.doubleValue), 0, 100)));
			break;

		case PDH_CSTATUS_INVALID_DATA:
		case PDH_CALC_NEGATIVE_DENOMINATOR:
		case PDH_CALC_NEGATIVE_VALUE:
		case PDH_NO_DATA:
			// "% Processor Time" sometimes spuriously fails with these codes.
			// Treating them as zero is consistent with System.Diagnostics.PerformanceCounter.
			CachedCoreUtilization.Add(0);
			break;

		default:
			// Keep one entry per instance so the array length (the core count, downstream) stays stable.
			CachedCoreUtilization.Add(0);
			if (!bWarnedUnexpectedStatus)
			{
				bWarnedUnexpectedStatus = true;
				UE_LOGF(LogCpuUtilizationMonitor, Warning, "Unexpected status for counter instance '%ls': 0x%08X (further occurrences logged as Verbose)", Item.szName, Item.FmtValue.CStatus);
			}
			else
			{
				UE_LOGF(LogCpuUtilizationMonitor, Verbose, "Unexpected status for counter instance '%ls': 0x%08X", Item.szName, Item.FmtValue.CStatus);
			}
			break;
		}
	}

	if (CachedCoreUtilization.IsEmpty())
	{
		UE_LOGF(LogCpuUtilizationMonitor, Warning, "No per-processor utilization values were available.");
		return false;
	}

	return true;
}
