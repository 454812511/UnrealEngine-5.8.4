// Copyright Epic Games, Inc. All Rights Reserved.
using UnrealBuildTool;
using System.IO;
using EpicGames.Core;

public class PLCrashReporter : ModuleRules
{
	public PLCrashReporter(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string PLGitRepoRoot = "PLCrashReporter-1.12.0";

		string[] PLDefines = new string[] {};

		string PLCrashReporterPath = Path.Combine(Target.UEThirdPartySourceDirectory,"PLCrashReporter");
		string PLSourcePath = Path.Combine(PLCrashReporterPath,PLGitRepoRoot,"Source");
		string LibConfig = "Release";
		string DefaultLibFolder = "lib-Xcode-16.2";
		string PLLibPath = Path.Combine(PLCrashReporterPath, "lib", DefaultLibFolder);

		if (Target.Platform == UnrealTargetPlatform.Mac || Target.Platform == UnrealTargetPlatform.IOS)
		{
			PublicSystemIncludePaths.Add(PLSourcePath);

			if (Target.Platform == UnrealTargetPlatform.IOS && Target.Architecture == UnrealArch.IOSSimulator)
			{
				LibConfig = "Simulator";
			}
			else if (Target.Configuration == UnrealTargetConfiguration.Debug && Target.bDebugBuildsActuallyUseDebugCRT)
			{
				LibConfig = "Debug";
			}
			else
			{
				LibConfig = "Release";
			}

			string LibDir = Path.Combine(PLLibPath, Target.Platform.ToString(), LibConfig);

			if (Target.Platform == UnrealTargetPlatform.IOS)
			{
				// Some installs have the incorrect path of "iOS" instead of "IOS", 
				// so use the correct case sensitive path to keep UBA happy
				DirectoryReference LibDirRef = new DirectoryReference(LibDir);
				DirectoryReference FixedLibDirRef = DirectoryReference.FindCorrectCase(LibDirRef);

				LibDir = FixedLibDirRef.FullName;
			}

			string Lib = Path.Combine(LibDir, "libCrashReporter.a");
			PublicAdditionalLibraries.Add(Lib);
			PublicDefinitions.AddRange(PLDefines);
		}
	}
}
