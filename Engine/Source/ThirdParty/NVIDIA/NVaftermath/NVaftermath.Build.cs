// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class NVAftermath : ModuleRules
{
    public NVAftermath(ReadOnlyTargetRules Target)
        : base(Target)
	{
		Type = ModuleType.External;

		if (Target.Platform.IsInGroup(UnrealPlatformGroup.Windows))
		{
			string Arch = Target.Architecture.WindowsLibDir;

			string ThirdPartyDir = Path.Combine(Target.UEThirdPartySourceDirectory, "NVIDIA", "NVaftermath");
			string IncludeDir = Path.Combine(ThirdPartyDir, "include");
			string LibrariesDir = Path.Combine(ThirdPartyDir, "lib", Arch);
			string BinariesDir = Path.Combine("$(EngineDir)", "Binaries", "ThirdParty", "NVIDIA", "NVaftermath", "Win64");

			string LibName = $"GFSDK_Aftermath_Lib.{Arch}.lib";
			string DllName = $"GFSDK_Aftermath_Lib.{Arch}.dll";

			if (Target.Architecture == UnrealArch.Arm64 &&
				(!File.Exists(Path.Combine(LibrariesDir, LibName)) || !File.Exists(Path.Combine(BinariesDir, DllName))))
			{
				PublicDefinitions.Add("NV_AFTERMATH=0"); // arm64 nvaftermath not found
			}
			else
			{
				PublicDefinitions.Add("NV_AFTERMATH=1");

				PublicSystemIncludePaths.Add(IncludeDir);
				PublicAdditionalLibraries.Add(Path.Combine(LibrariesDir, LibName));
				RuntimeDependencies.Add(Path.Combine(BinariesDir, DllName));
				PublicDelayLoadDLLs.Add(DllName);
			}
		}
		else
		{
			PublicDefinitions.Add("NV_AFTERMATH=0");
		}
	}
}

