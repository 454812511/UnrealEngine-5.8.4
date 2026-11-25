// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UbaNinja : ModuleRules
{
	public UbaNinja(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivatePCHHeaderFile = "../Core/Public/UbaCorePch.h";

		bRequiresPlatformSDK = true;

		PrivateDependencyModuleNames.AddRange([
			"UbaCommon",
		]);
	}
}
