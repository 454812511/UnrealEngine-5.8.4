// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaDefinitions.h"
#include "UbaPlatform.h"
#include <sys/stat.h>

namespace uba
{
#if UBA_SUPPORTS_GO

	bool IsGoBinary(const char* path);
	void PatchGoSyscalls();
	void PatchGoExit();
	// GoHandle*/GoHandlesPath are now file-local statics inside UbaDetoursGo.cpp (only callers live in the same TU).

#endif
}
