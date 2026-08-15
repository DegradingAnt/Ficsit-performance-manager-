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
			"AkAudio",   // P3.9 zipline volume: UAkGameplayStatics::SetOutputBusVolume. Ant 2026-08-09: "the wwise is fine to depend on since its part of vanilla".
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
			// UNiagaraComponent / UNiagaraSystem, for the weather indoor gate (Tier 2 of the particle
			// work). ue-niagara-effects names Niagara + NiagaraCore as the pair to declare.
			// ⚠ It shipped COMMENTED OUT in the template, and a presence check that greps for the
			// name matches the comment and silently decides it is already there. Caught by the
			// linker, but only because something used it.
			"Niagara", "NiagaraCore",
			// Slice 0 item 5. The wrist slot needs a real input action, and the wrist slot gates 1.0.0.
			// ⚠ NOTHING CONSUMES THIS YET, which is deliberate rather than an oversight: the module is
			// declared first so the slot work compiles from its first commit. SML ships no keybind API
			// of its own (grepped its whole tree), and FPM's existing F8 is a Slate IInputProcessor, not
			// Enhanced Input, so there is no in-repo precedent to copy from.
			"EnhancedInput",
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

			// RHIGetTextureMemoryStats, for the texture-pool guard's card-size read. PRIVATE for the
			// same reason as Projects: RHIStats.h is included only from Private/.
			// ⚠ RenderCore is NOT a substitute, and the failure mode is a LINK error rather than a
			// compile one: RHIGetTextureMemoryStats is inline and dereferences the exported global
			// GDynamicRHI, so the header compiles happily and LNK2019 fires at the very end of the
			// build. The old mod listed "RHI" explicitly; the rebuild dropped it.
			"RHI",
		});
		
		DynamicallyLoadedModuleNames.AddRange(new string[] {
			// ... add any modules that your module loads dynamically here ...
		});
	}
}
