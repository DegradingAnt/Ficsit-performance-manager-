using UnrealBuildTool;
using System.IO;
using System;

public class FicsitsPerformanceManager : ModuleRules
{
	public FicsitsPerformanceManager(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		// FactoryGame transitive dependencies
		// Not all of these are required, but including the extra ones saves you from having to add them later.
		// Some entries are commented out to avoid compile-time warnings about depending on a module that you don't explicitly depend on.
		// You can uncomment these as necessary when your code actually needs to use them.
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject",
			"Engine",
			"DeveloperSettings",
			"PhysicsCore",
			"InputCore",
			//"OnlineSubsystem", "OnlineSubsystemUtils", "OnlineSubsystemNull",
			//"SignificanceManager",
			"GeometryCollectionEngine",
			//"ChaosVehiclesCore", "ChaosVehicles", "ChaosSolverEngine",
			"AnimGraphRuntime",
			//"AkAudio",
			"AssetRegistry",
			"NavigationSystem",
			//"ReplicationGraph",
			"AIModule",
			"GameplayTasks",
			"SlateCore", "Slate", "UMG",
			//"InstancedSplines",
			"RenderCore",
			"CinematicCamera",
			"Foliage",
			//"Niagara",
			//"EnhancedInput",
			//"GameplayCameras",
			//"TemplateSequence",
			"NetCore",
			"GameplayTags",
			// LexToString(EOnlineServices) and ToLogString(FAccountId), used by the clone sensor to print
			// WHICH online identities a joiner carries rather than just how many. Both are COREONLINE_API
			// (CoreOnline.h:292, :341) -- declared here rather than assumed transitive, after the link
			// failed with exactly those two unresolved externals on 2026-08-09.
			"CoreOnline",
			"Json", "JsonUtilities"
		});

		// FactoryGame plugins
		PublicDependencyModuleNames.AddRange(new string[] {
			// AbstractInstance owns UAbstractInstanceDataObject and FInstanceData (InstanceData.h:17,
			// :305). That is where a lightweight buildable's real geometry lives — the rain fix reads
			// it because FGColoredInstanceMeshProxy carries no StaticMesh of its own.
			"AbstractInstance",
			//"InstancedSplinesComponent",
			//"SignificanceISPC"
		});

		// Header stubs
		PublicDependencyModuleNames.AddRange(new string[] {
			"DummyHeaders",
		});

		if (Target.Type == TargetRules.TargetType.Editor) {
			PublicDependencyModuleNames.AddRange(new string[] {/*"OnlineBlueprintSupport",*/ "AnimGraph"});
		}
		PublicDependencyModuleNames.AddRange(new string[] {"FactoryGame", "SML"});
		
		PublicIncludePaths.AddRange(new string[] {
			// ... add public include paths required here ...
		});
		
		PrivateIncludePaths.AddRange(new string[] {
			// ... add private include paths required here ...
		});
		
		PublicDependencyModuleNames.AddRange(new string[] {
			// ... add public dependencies that you statically link with here ...
		});
		
		PrivateDependencyModuleNames.AddRange(new string[] {
			// IPluginManager, used by StartupModule to read this plugin's own descriptor.
			// PRIVATE deliberately: IPluginManager.h is included only from Private/, and a public
			// entry would push its include paths onto every downstream module for nothing.
			// It also very likely arrives transitively through Engine — depending on that is the
			// IWYU anti-pattern, so it is declared rather than assumed.
			"Projects",
		});
		
		DynamicallyLoadedModuleNames.AddRange(new string[] {
			// ... add any modules that your module loads dynamically here ...
		});
	}
}
