using UnrealBuildTool;

public class WNLPackFix : ModuleRules
{
    public WNLPackFix(ReadOnlyTargetRules Target) : base(Target)
    {
        CppStandard = CppStandardVersion.Cpp20;
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine"
        });

        // SML for NativeHookManager (funchook-based native hooking);
        // Json for the governor's WNLPackFix.cfg read/write;
        // FactoryGame for UFGColoredInstanceMeshProxy (static-base movement fix) + conveyor classes;
        // RHI for IsRHIDeviceNVIDIA/AMD/Intel + GRHIAdapterName + RHIGetTextureMemoryStats;
        // Foliage for UFoliageInstancedStaticMeshComponent (the base foliage class the census found);
        // AkAudio for UAkGameplayStatics::StopActor (server-only Wwise log-spam gate);
        // RenderCore for DynamicRenderScaling::FBudget (native dynamic-resolution fraction read);
        // UMG because SML's ModConfiguration.h references UUserWidget (in-game config menu)
        // NavigationSystem for ARecastNavMesh (navmesh coverage fix, direct member via AccessTransformers)
        PrivateDependencyModuleNames.AddRange(new[] {
            "SML", "Json", "FactoryGame", "RHI", "Foliage", "AkAudio", "RenderCore", "UMG", "NavigationSystem"
        });

        // dxgi.lib for IDXGIAdapter3::QueryVideoMemoryInfo — real available VRAM (client/Windows only).
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PublicSystemLibraries.Add("dxgi.lib");
        }
    }
}
