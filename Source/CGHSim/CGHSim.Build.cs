// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CGHSim : ModuleRules
{
	public CGHSim(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.Add(ModuleDirectory);
		// Engine-independent wire contract shared with the standalone CMake server.
		PrivateIncludePaths.Add(System.IO.Path.GetFullPath(System.IO.Path.Combine(ModuleDirectory, "../../Backend/V100/include")));
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "CinematicCamera" });

		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "RHI", "Sockets", "Networking" });

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "PropertyEditor", "Slate", "SlateCore", "AssetRegistry", "Json", "ImageCore" });
		}

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
