// Copyright Epic Games, Inc. All Rights Reserved.

#include "SampleConverter/TmvMediaFrameMipBuffer.h"

#include "SampleConverter/TmvMediaTileResidency.h"

bool FTmvMediaFrameMipBuffer::IsAllocatedVirtualized() const
{
	const FTmvMediaTileResidency* Res = GetResidency();
	return Res != nullptr && (Res->Capacity > 0 || Res->IsShared());
}
