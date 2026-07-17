#include "WNLPerfGovernor.h"
#include "WNLQualityStages.h"
#include "WNLPackFix.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "RHI.h"                 // IsRHIDeviceNVIDIA/AMD/Intel, GRHIAdapterName (vendor-adaptive branch)
#include "Engine/Engine.h"
#include "DynamicResolutionState.h"           // native dynamic-res read (FDynamicResolutionStateInfos, GDynamicPrimaryResolutionFraction)
#include "FGBuildableSubsystem.h"             // UpdateBuildableCullDistances — the big FG-specific CPU-relief lever
#include "GameFramework/GameUserSettings.h" // user-settings adoption (menu changes can't reach our cvars)
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <dxgi1_4.h>   // IDXGIAdapter3::QueryVideoMemoryInfo — real available VRAM (budget - usage)
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	// The same config dir SML mods use (FactoryGame/Configs/) so all tuning lives in one place.
	FString ConfigFilePath()
	{
		return FPaths::Combine(FPaths::ProjectDir(), TEXT("Configs"), TEXT("WNLPackFix.cfg"));
	}

#if PLATFORM_WINDOWS
	// The discrete adapter (largest dedicated VRAM), cached. QueryVideoMemoryInfo's Budget is the
	// OS-granted local-memory budget for THIS process, which SHRINKS when other apps (the Claude
	// desktop, a browser, etc.) hold VRAM — so Budget - CurrentUsage is the true dynamic headroom.
	IDXGIAdapter3* GetDxgiAdapter3()
	{
		static IDXGIAdapter3* Cached = nullptr;
		static bool bTried = false;
		if (bTried) { return Cached; }
		bTried = true;
		IDXGIFactory4* Factory = nullptr;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&Factory))) || !Factory)
		{
			return nullptr;
		}
		IDXGIAdapter1* Best = nullptr; SIZE_T BestVram = 0; IDXGIAdapter1* Ad = nullptr;
		for (UINT i = 0; Factory->EnumAdapters1(i, &Ad) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			DXGI_ADAPTER_DESC1 Desc; Ad->GetDesc1(&Desc);
			if (!(Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && Desc.DedicatedVideoMemory > BestVram)
			{
				BestVram = Desc.DedicatedVideoMemory;
				if (Best) { Best->Release(); }
				Best = Ad; Best->AddRef();
			}
			Ad->Release();
		}
		if (Best)
		{
			Best->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&Cached));
			Best->Release();
		}
		Factory->Release();
		return Cached;
	}

	// Local VRAM info in MB. Budget = what the OS GRANTS this process (shrinks when other apps —
	// the Claude desktop, a browser — hold VRAM; INDEPENDENT of our own usage, so sizing off it
	// can't feed back into itself). Free = Budget - CurrentUsage (real headroom right now). Returns
	// false if unavailable. Sizing the pool off BUDGET (not Free) is what avoids the limit cycle
	// where the pool we set inflates CurrentUsage, which lowers Free, which shrinks the pool...
	bool QueryVramMB(int64& OutBudgetMB, int64& OutFreeMB)
	{
		if (IDXGIAdapter3* Ad = GetDxgiAdapter3())
		{
			DXGI_QUERY_VIDEO_MEMORY_INFO Info = {};
			if (SUCCEEDED(Ad->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &Info)))
			{
				OutBudgetMB = static_cast<int64>(Info.Budget) / (1024 * 1024);
				OutFreeMB   = FMath::Max<int64>(static_cast<int64>(Info.Budget) - static_cast<int64>(Info.CurrentUsage), 0) / (1024 * 1024);
				return true;
			}
		}
		return false;
	}
#else
	bool QueryVramMB(int64&, int64&) { return false; }
#endif

	IConsoleVariable* MaxFPSVar()
	{
		static IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"));
		return Var;
	}

	IConsoleVariable* VSyncVar()
	{
		static IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"));
		return Var;
	}
}

FWNLPerfGovernor& FWNLPerfGovernor::Get()
{
	static FWNLPerfGovernor Instance;
	return Instance;
}

void FWNLPerfGovernor::Start()
{
	if (bStarted || IsRunningDedicatedServer())
	{
		return; // client-only feature; the server has no renderer to govern
	}
	LoadOrCreateConfig(); // merges the in-game menu's values (Menu.cfg) on top before parsing
	if (!bEnabled)
	{
		UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] perf governor disabled via config"));
		return;
	}
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FWNLPerfGovernor::Tick), 0.25f);
	bStarted = true;
	StartTime = FPlatformTime::Seconds();

	// Vendor detection, the baseline-free set, and the stage-lever baseline capture all run ONCE
	// from Tick AFTER the game's 2s scalability settle (ApplyPostSettleGraphics + CaptureBaselines).
	// Doing it at Start would let the scalability pass clobber our writes, or capture pre-settle
	// baselines. HWRT stays impossible (DXR compiled out of the shipped content: log "not supported
	// by current RHI") — this is software Lumen scaled up to Cinematic and beyond via the stages.

	// Cap uncapped frame rates at the monitor rate — rendering past 120Hz is pure waste.
	// Respect an explicit user cap: only override when the game reports 0 (uncapped).
	if (CapFPS > 0.f)
	{
		if (IConsoleVariable* Cap = MaxFPSVar())
		{
			if (Cap->GetFloat() <= 0.f)
			{
				Cap->Set(CapFPS, ECVF_SetByConsole);
				UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] governor: uncapped frame rate -> capped at %.0f (monitor rate)"), CapFPS);
			}
		}
	}

	UE_LOG(LogWNLPackFix, Display,
		TEXT("[WNLPackFix] perf governor armed: target >%.0f FPS (cap %.0f), dyn-res band [%.0f%%..%.0f%%], ")
		TEXT("CPU floor %.0f FPS (comfort %.0f), stages -%d..+%d"),
		TargetFPS, MaxFPS, MinScreenPct, MaxScreenPct, CpuFloorFPS, CpuComfortFPS,
		MaxCutStage, MaxBonusStage);
}

void FWNLPerfGovernor::Stop()
{
	if (bStarted)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		bStarted = false;
	}
}

void FWNLPerfGovernor::LoadOrCreateConfig()
{
	const FString Path = ConfigFilePath();
	FString Raw;
	if (FFileHelper::LoadFileToString(Raw, *Path))
	{
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
		{
			// The in-game config MENU (SML settings page → Configs/WNLPackFix/Menu.cfg) exposes EVERY
			// governor key. Merge its values over the file's BEFORE parsing, so one parse path serves
			// both and what the player sees in the menu is always what actually applies.
			MergeMenuConfig(Json);
			Json->TryGetBoolField(TEXT("Enabled"), bEnabled);
			Json->TryGetNumberField(TEXT("TargetFPS"), TargetFPS);
			Json->TryGetNumberField(TEXT("MinScreenPct"), MinScreenPct);
			Json->TryGetNumberField(TEXT("MaxScreenPct"), MaxScreenPct);
			// v0.9 migration: the legacy toggle keys collapse into the stage engine. QualityNudge=false
			// used to disable cuts, QualityBonus=false the bonus — map them onto the stage limits.
			{
				bool bLegacy = true;
				if (Json->TryGetBoolField(TEXT("QualityNudge"), bLegacy) && !bLegacy) { MaxCutStage = 0; }
				bLegacy = true;
				if (Json->TryGetBoolField(TEXT("QualityBonus"), bLegacy) && !bLegacy) { MaxBonusStage = 0; }
				for (const TCHAR* Dead : { TEXT("LumenCinematicBaseline"), TEXT("VSMUplift"),
					TEXT("VolumetricFogUplift"), TEXT("LumenFinalGatherUplift"), TEXT("SoftwareLumenMax"), TEXT("BonusAfterSec") })
				{
					if (Json->HasField(Dead))
					{
						UE_LOG(LogWNLPackFix, Display,
							TEXT("[WNLPackFix] config: legacy key '%s' migrated into the stage engine (ignored)"), Dead);
					}
				}
			}
			Json->TryGetNumberField(TEXT("NudgeAfterSec"), NudgeAfterSec);
			Json->TryGetNumberField(TEXT("RestoreAfterSec"), RestoreAfterSec);
			Json->TryGetNumberField(TEXT("CapFPS"), CapFPS);
			// v0.9 stage engine
			Json->TryGetNumberField(TEXT("MaxBonusStage"), MaxBonusStage);
			Json->TryGetNumberField(TEXT("MaxCutStage"), MaxCutStage);
			Json->TryGetNumberField(TEXT("PromoteDwellSec"), PromoteDwellSec);
			Json->TryGetNumberField(TEXT("DemoteDwellSec"), DemoteDwellSec);
			Json->TryGetNumberField(TEXT("PromoteGpuFrac"), PromoteGpuFrac);
			Json->TryGetNumberField(TEXT("PromoteHeadroomFactor"), PromoteHeadroomFactor);
			Json->TryGetNumberField(TEXT("VerifyWindowSec"), VerifyWindowSec);
			Json->TryGetNumberField(TEXT("PromoteCooldownSec"), PromoteCooldownSec);
			Json->TryGetNumberField(TEXT("PromoteCooldownMaxSec"), PromoteCooldownMaxSec);
			// baseline-free set
			Json->TryGetBoolField(TEXT("VSMStability"), bVSMStability);
			Json->TryGetBoolField(TEXT("ReflectionDenoise"), bReflectionDenoise);
			Json->TryGetBoolField(TEXT("ContactShadows"), bContactShadows);
			Json->TryGetNumberField(TEXT("ContactShadowLength"), ContactShadowLength);
			Json->TryGetBoolField(TEXT("NaniteSharpen"), bNaniteSharpen);
			Json->TryGetNumberField(TEXT("NanitePixelsPerEdge"), NanitePixelsPerEdge);
			Json->TryGetBoolField(TEXT("LumenPerfLevers"), bLumenPerfLevers);
			Json->TryGetBoolField(TEXT("DontLimitOnBattery"), bDontLimitOnBattery);
			Json->TryGetNumberField(TEXT("MaxFrameQueue"), MaxFrameQueue);
			// v0.9 baseline-free knobs
			Json->TryGetNumberField(TEXT("ReflexMode"), ReflexMode);
			Json->TryGetNumberField(TEXT("FSRSharpness"), FSRSharpness);
			Json->TryGetBoolField(TEXT("AsyncTick"), bAsyncTick);
			Json->TryGetNumberField(TEXT("GrassTickInterval"), GrassTickInterval);
			Json->TryGetNumberField(TEXT("GrassDensityScale"), GrassDensityScale);
			Json->TryGetNumberField(TEXT("StreamingPoolMB"), StreamingPoolMB);
			Json->TryGetBoolField(TEXT("IncrementalGC"), bIncrementalGC);
			// v0.8 vendor-adaptive
			Json->TryGetBoolField(TEXT("VendorAdaptive"), bVendorAdaptive);
			Json->TryGetNumberField(TEXT("AmdMinScreenPct"), AmdMinScreenPct);
			Json->TryGetNumberField(TEXT("TsrXessSharpen"), TsrXessSharpen);
			Json->TryGetBoolField(TEXT("ForceUpscalerIfNone"), bForceUpscalerIfNone);
			// v0.9.7 vendor upscaler auto-select + AMD/Intel guards (vendor bug audit 2026-07-17)
			Json->TryGetBoolField(TEXT("UpscalerAutoSelect"), bUpscalerAutoSelect);
			Json->TryGetBoolField(TEXT("AmdAntiFlicker"), bAmdAntiFlicker);
			Json->TryGetNumberField(TEXT("AmdDFShadowCullTile"), AmdDFShadowCullTile);
			Json->TryGetNumberField(TEXT("IntelArcMaxBonusStage"), IntelArcMaxBonusStage);
			Json->TryGetNumberField(TEXT("IntelIGpuMaxBonusStage"), IntelIGpuMaxBonusStage);
			Json->TryGetNumberField(TEXT("IntelIGpuMinScreenPct"), IntelIGpuMinScreenPct);
			Json->TryGetBoolField(TEXT("AssertRayReconstructionOff"), bAssertRayReconstructionOff);
			Json->TryGetNumberField(TEXT("GpuBoundFraction"), GpuBoundFraction);
			Json->TryGetNumberField(TEXT("LadderGraceSec"), LadderGraceSec);
			Json->TryGetNumberField(TEXT("VramFloorMB"), VramFloorMB);
				Json->TryGetNumberField(TEXT("PoolBudgetFraction"), PoolBudgetFraction);
			// v0.9.6 CPU-relief controller
			Json->TryGetBoolField(TEXT("CpuRelief"), bCpuRelief);
			Json->TryGetNumberField(TEXT("CpuFloorFPS"), CpuFloorFPS);
			Json->TryGetNumberField(TEXT("CpuComfortFPS"), CpuComfortFPS);
			Json->TryGetNumberField(TEXT("CpuBuildCullMin"), CpuBuildCullMin);
			Json->TryGetNumberField(TEXT("CpuFoliageCullMin"), CpuFoliageCullMin);
			// v0.9.8 visibility-banded CPU levers (invisible band A)
			Json->TryGetNumberField(TEXT("ConveyorItemFreqMin"), ConveyorItemFreqMin);
			Json->TryGetNumberField(TEXT("ConveyorDrawDistMin"), ConveyorDrawDistMin);
			Json->TryGetNumberField(TEXT("NiagaraQualityMin"), NiagaraQualityMin);
			// Review findings: a hand-edited config must not divide by zero, invert the band,
			// or silently defeat hysteresis/"sustained" gates.
			TargetFPS    = FMath::Clamp(TargetFPS, 30.f, 240.f);
			MinScreenPct = FMath::Clamp(MinScreenPct, 25.f, 100.f);
			MaxScreenPct = FMath::Clamp(MaxScreenPct, MinScreenPct, 100.f);
			NudgeAfterSec   = FMath::Clamp(NudgeAfterSec, 0.5f, 300.f);
			RestoreAfterSec = FMath::Clamp(RestoreAfterSec, 0.5f, 300.f);
			MaxBonusStage   = FMath::Clamp(MaxBonusStage, 0, 6);
			MaxCutStage     = FMath::Clamp(MaxCutStage, 0, 4);
			PromoteDwellSec = FMath::Clamp(PromoteDwellSec, 5.f, 600.f);
			DemoteDwellSec  = FMath::Clamp(DemoteDwellSec, 0.5f, 60.f);
				// v0.9.6 CPU-relief clamps (comfort must sit above the floor so the span is positive).
				CpuFloorFPS       = FMath::Clamp(CpuFloorFPS, 30.f, 120.f);
				CpuComfortFPS     = FMath::Clamp(CpuComfortFPS, CpuFloorFPS + 5.f, 240.f);
				CpuBuildCullMin   = FMath::Clamp(CpuBuildCullMin, 0.3f, 1.f);
				CpuFoliageCullMin = FMath::Clamp(CpuFoliageCullMin, 0.3f, 1.f);
				ConveyorItemFreqMin = FMath::Clamp(ConveyorItemFreqMin, 5, 120);
				ConveyorDrawDistMin = FMath::Clamp(ConveyorDrawDistMin, 10000.f, 200000.f);
				NiagaraQualityMin   = FMath::Clamp(NiagaraQualityMin, 0, 4);
			PromoteGpuFrac  = FMath::Clamp(PromoteGpuFrac, 0.5f, 0.95f);
			PromoteHeadroomFactor = FMath::Clamp(PromoteHeadroomFactor, 1.f, 5.f);
			VerifyWindowSec = FMath::Clamp(VerifyWindowSec, 2.f, 60.f);
			PromoteCooldownSec    = FMath::Clamp(PromoteCooldownSec, 10.f, 3600.f);
			PromoteCooldownMaxSec = FMath::Clamp(PromoteCooldownMaxSec, PromoteCooldownSec, 7200.f);
			CapFPS = (CapFPS <= 0.f) ? 0.f : FMath::Clamp(CapFPS, 30.f, 480.f); // <=0 keeps "uncapped" sentinel
			// MaxFPS (the headroom line) is derived from the real cap so the two can't desync.
			MaxFPS = FMath::Clamp((CapFPS > 0.f ? CapFPS : 120.f), TargetFPS, 480.f);
			// Fixed floor snapshot: the live floor may follow the user DOWN but never compounds.
			ConfiguredMinScreenPct = MinScreenPct;
			// v0.8 clamps (a hand-edit must not push a silly value into a cvar).
			ContactShadowLength = FMath::Clamp(ContactShadowLength, 0.f, 0.1f);   // >0.1 = peter-panning streaks
			NanitePixelsPerEdge = FMath::Clamp(NanitePixelsPerEdge, 0.25f, 8.f);
			AmdMinScreenPct     = FMath::Clamp(AmdMinScreenPct, 40.f, 90.f);
			TsrXessSharpen      = FMath::Clamp(TsrXessSharpen, 0.f, 2.f);
			AmdDFShadowCullTile    = FMath::Clamp(AmdDFShadowCullTile, 100.f, 2000.f);
			IntelArcMaxBonusStage  = FMath::Clamp(IntelArcMaxBonusStage, 0, 6);
			IntelIGpuMaxBonusStage = FMath::Clamp(IntelIGpuMaxBonusStage, 0, 6);
			IntelIGpuMinScreenPct  = FMath::Clamp(IntelIGpuMinScreenPct, 25.f, 80.f);
			GpuBoundFraction    = FMath::Clamp(GpuBoundFraction, 0.5f, 0.99f);
			LadderGraceSec      = FMath::Clamp(LadderGraceSec, 0.f, 600.f);
			MaxFrameQueue       = FMath::Clamp(MaxFrameQueue, 0, 3);
			VramFloorMB         = FMath::Clamp(VramFloorMB, 256.f, 8192.f);
				PoolBudgetFraction  = FMath::Clamp(PoolBudgetFraction, 0.1f, 0.8f);
			ReflexMode          = FMath::Clamp(ReflexMode, 0, 2);
			FSRSharpness        = FMath::Clamp(FSRSharpness, 0.f, 1.f);
			GrassTickInterval   = FMath::Clamp(GrassTickInterval, 0, 60);
			GrassDensityScale   = FMath::Clamp(GrassDensityScale, 0.5f, 4.f);
			StreamingPoolMB     = FMath::Clamp(StreamingPoolMB, 0, 16384);
			return;
		}
		UE_LOG(LogWNLPackFix, Warning, TEXT("[WNLPackFix] %s unreadable — using defaults"), *Path);
		return; // keep the user's broken file for inspection; don't overwrite
	}

	// First run: write the defaults so every knob is discoverable + tunable without a rebuild.
	const FString Defaults = FString::Printf(
		TEXT("{\n")
		TEXT("\t\"Enabled\": true,\n")
		TEXT("\t\"TargetFPS\": %.0f,\n")
		TEXT("\t\"MinScreenPct\": %.0f,\n")
		TEXT("\t\"MaxScreenPct\": %.0f,\n")
		TEXT("\t\"MaxBonusStage\": %d,\n")
		TEXT("\t\"MaxCutStage\": %d,\n")
		TEXT("\t\"PromoteDwellSec\": 30,\n")
		TEXT("\t\"DemoteDwellSec\": 1.5,\n")
		TEXT("\t\"CpuRelief\": true,\n")
		TEXT("\t\"CpuFloorFPS\": 75,\n")
		TEXT("\t\"CpuComfortFPS\": 90,\n")
		TEXT("\t\"CpuBuildCullMin\": 0.65,\n")
		TEXT("\t\"CpuFoliageCullMin\": 0.85,\n")
		TEXT("\t\"ConveyorItemFreqMin\": 24,\n")
		TEXT("\t\"ConveyorDrawDistMin\": 40000,\n")
		TEXT("\t\"NiagaraQualityMin\": 2,\n")
		TEXT("\t\"PromoteGpuFrac\": 0.80,\n")
		TEXT("\t\"PromoteHeadroomFactor\": 1.5,\n")
		TEXT("\t\"VerifyWindowSec\": 8,\n")
		TEXT("\t\"PromoteCooldownSec\": 120,\n")
		TEXT("\t\"PromoteCooldownMaxSec\": 600,\n")
		TEXT("\t\"VSMStability\": true,\n")
		TEXT("\t\"ReflectionDenoise\": true,\n")
		TEXT("\t\"ContactShadows\": true,\n")
		TEXT("\t\"ContactShadowLength\": 0.035,\n")
		TEXT("\t\"NaniteSharpen\": true,\n")
		TEXT("\t\"NanitePixelsPerEdge\": 1.0,\n")
		TEXT("\t\"LumenPerfLevers\": true,\n")
		TEXT("\t\"DontLimitOnBattery\": true,\n")
		TEXT("\t\"MaxFrameQueue\": 1,\n")
		TEXT("\t\"ReflexMode\": 1,\n")
		TEXT("\t\"FSRSharpness\": 0.5,\n")
		TEXT("\t\"AsyncTick\": true,\n")
		TEXT("\t\"GrassTickInterval\": 10,\n")
		TEXT("\t\"GrassDensityScale\": 1.0,\n")
		TEXT("\t\"StreamingPoolMB\": 0,\n")
		TEXT("\t\"IncrementalGC\": true,\n")
		TEXT("\t\"VendorAdaptive\": true,\n")
		TEXT("\t\"AmdMinScreenPct\": 62,\n")
		TEXT("\t\"TsrXessSharpen\": 0.8,\n")
		TEXT("\t\"ForceUpscalerIfNone\": true,\n")
		TEXT("\t\"UpscalerAutoSelect\": true,\n")
		TEXT("\t\"AmdAntiFlicker\": true,\n")
		TEXT("\t\"AmdDFShadowCullTile\": 400,\n")
		TEXT("\t\"IntelArcMaxBonusStage\": 2,\n")
		TEXT("\t\"IntelIGpuMaxBonusStage\": 0,\n")
		TEXT("\t\"IntelIGpuMinScreenPct\": 50,\n")
		TEXT("\t\"AssertRayReconstructionOff\": true,\n")
		TEXT("\t\"GpuBoundFraction\": 0.85,\n")
		TEXT("\t\"LadderGraceSec\": 45,\n")
		TEXT("\t\"VramFloorMB\": 1500,\n")
		TEXT("\t\"PoolBudgetFraction\": 0.4,\n")
		TEXT("\t\"NudgeAfterSec\": %.0f,\n")
		TEXT("\t\"RestoreAfterSec\": %.0f,\n")
		TEXT("\t\"CapFPS\": %.0f,\n")
		TEXT("\t\"Fog\": {\n")
		TEXT("\t\t\"Enabled\": true,\n")
		TEXT("\t\t\"IndoorStartDistance\": 12000,\n")
		TEXT("\t\t\"TransitionSec\": 4,\n")
		TEXT("\t\t\"RoofTraceUp\": 4000,\n")
		TEXT("\t\t\"CheckInterval\": 0.25,\n")
		TEXT("\t\t\"MinBubble\": 200,\n")
		TEXT("\t\t\"WallBias\": 0.9,\n")
		TEXT("\t\t\"GrowLerp\": 0.2,\n")
		TEXT("\t\t\"ShrinkLerp\": 0.6,\n")
		TEXT("\t\t\"SealCountMin\": 3\n")
		TEXT("\t}\n")
		TEXT("}\n"),
		TargetFPS, MinScreenPct, MaxScreenPct,
		MaxBonusStage, MaxCutStage,
		NudgeAfterSec, RestoreAfterSec, CapFPS);
	FFileHelper::SaveStringToFile(Defaults, *Path);
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] wrote default governor config: %s"), *Path);
}

void FWNLPerfGovernor::ApplyDynamicResolution()
{
	// Hand resolution scaling to the ENGINE's native dynamic-res system instead of poking
	// r.ScreenPercentage ourselves. Verified: CSS emits the per-frame heartbeat (stock LaunchEngineLoop)
	// and installs a default state proxy, so OperationMode=2 really scales. It debounces changes
	// (MinResolutionChangePeriod=8, left at default) and drives whichever upscaler the user has active
	// (DLSS/FSR/TSR since UE5.1) -> flash-free, unlike our old manual poke which the engine now IGNORES
	// once dyn-res is on. Set once at post-settle.
	IConsoleManager& CM = IConsoleManager::Get();
	auto Set = [&CM](const TCHAR* Name, float Value, const TCHAR* Label)
	{
		if (IConsoleVariable* V = CM.FindConsoleVariable(Name))
		{
			const float Was = V->GetFloat();
			V->Set(Value, ECVF_SetByConsole);
			UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   %s: %s %.1f -> %.1f"), Label, Name, Was, Value);
		}
		else { UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   %s: %s ABSENT (skipped)"), Label, Name); }
	};

	// Floor: AMD/FSR/TSR blur worse at low res than DLSS, so their floor is higher; never below 50 (the
	// DLSS DRS hard floor — below it DLSS returns min==max and can't scale). Range [floor,100].
	const float VendorFloor = (Vendor == EGpuVendor::AMD) ? AmdMinScreenPct : MinScreenPct;
	// The 50% lower bound is a DLSS reconstruction floor; TSR/XeSS/FSR/iGPU can scale below it, so only
	// clamp to 50 when DLSS is the live upscaler — otherwise honor the configured floor (down to 25).
	const float HardFloor = (ActiveUpscaler == EUpscaler::DLSS) ? 50.f : 25.f;
	const float MinPct = FMath::Clamp(VendorFloor, HardFloor, MaxScreenPct);

	Set(TEXT("r.DynamicRes.OperationMode"), 2.f, TEXT("dyn-res mode"));            // force-enable
	Set(TEXT("r.DynamicRes.MinScreenPercentage"), MinPct, TEXT("dyn-res floor"));
	Set(TEXT("r.DynamicRes.MaxScreenPercentage"), MaxScreenPct, TEXT("dyn-res ceil"));
	// Budget honors a frame cap (review finding: a 60-capped rig given a 90fps budget would drag
	// resolution to the floor while comfortably making its cap — permanent needless blur).
	float BudgetTargetFPS = TargetFPS;
	if (const IConsoleVariable* CapCV = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
	{
		const float Cap = CapCV->GetFloat();
		if (Cap > 0.f) { BudgetTargetFPS = FMath::Min(BudgetTargetFPS, Cap); }
	}
	Set(TEXT("r.DynamicRes.FrameTimeBudget"), 1000.f / BudgetTargetFPS, TEXT("dyn-res budget ms"));
	Set(TEXT("r.DynamicRes.TargetedGPUHeadRoomPercentage"), 10.f, TEXT("dyn-res headroom"));   // Epic/Fortnite default
	// CPU-bound handling: make the heuristic weigh game/render-thread time (UseCPUTimeLogic +
	// UseGameThreadCriticalPath, both verified real in this engine). When CPU-bound the GPU is idle, so
	// the heuristic sees GPU headroom and holds resolution HIGH on its own — no need to drop res
	// futilely. (The earlier r.DynamicRes.CPUBoundScreenPercentage does NOT exist in this build — it was
	// a bad cvar from web docs; verified absent in DynamicResolution.cpp. Removed.)
	Set(TEXT("r.DynamicRes.UseCPUTimeLogic"), 1.f, TEXT("dyn-res cpu-time logic"));
	Set(TEXT("r.DynamicRes.UseGameThreadCriticalPath"), 1.f, TEXT("dyn-res gt critpath"));

	bDynResApplied = true;
	CurrentPct = MaxScreenPct; // seeded; ReadDynamicResPct() tracks the live fraction each Tick
	UE_LOG(LogWNLPackFix, Display,
		TEXT("[WNLPackFix] governor: native dynamic resolution armed [%.0f-%.0f%%] via user's upscaler (flash-free)"),
		MinPct, MaxScreenPct);
}

float FWNLPerfGovernor::ReadDynamicResPct() const
{
	// The engine's live smoothed primary resolution fraction (0..100). This is the same read the engine
	// itself uses (stat unit / LegacyScreenPercentageDriver), so it's the authoritative value for the
	// stage engine's res-floor gating. Game-thread only (we're in Tick).
	if (GEngine)
	{
		FDynamicResolutionStateInfos Infos;
		GEngine->GetDynamicResolutionCurrentStateInfos(Infos);
		if (Infos.Status == EDynamicResolutionStatus::Enabled)
		{
			const float Frac = Infos.ResolutionFractionApproximations[GDynamicPrimaryResolutionFraction];
			if (Frac > 0.f) { return Frac * 100.f; }
		}
	}
	return MaxScreenPct; // disabled/unknown -> full res (stage engine won't false-fire res-sag)
}

void FWNLPerfGovernor::UpdateCpuRelief(float FrameMs, float GameThreadMs, float RenderThreadMs,
                                       bool bGpuBound, bool bGpuKnown, float DT)
{
	// ============================== CPU-RELIEF CONTROLLER (v0.9.8) ==============================
	// Resolution + GPU quality only help GPU-bound frames; when the game/render THREAD is the limiter
	// (dense factory), THIS is what buys frame time.
	//
	// DESIGN (Ant, 2026-07-17): "scale things on the CPU that are barely visible to the player first;
	// only close stuff if the game really needs it — keep graphics as good as possible for as long as
	// possible." So one SMOOTH intensity I in [0..1] drives a VISIBILITY-ORDERED ladder, each lever
	// engaging only inside its own window of I:
	//
	//   Band A — INVISIBLE (I 0.00→0.40): things a player can't reasonably notice.
	//     A1 conveyor item update rate  (FG.ConveyorItemFrequency, 60Hz→min)   — belt items refresh less
	//     A2 conveyor item draw radius  (CSS.Conveyor.MaxDrawDistance, →min)   — FAR belts stop drawing items
	//     A3 skeletal LOD bias          (r.SkeletalMeshLODBias, →max)          — far creatures coarsen
	//   Band B — MODERATE (I 0.40→0.75): visible if you look for it.
	//     B1 building detail cull       (AFGBuildableSubsystem, 1.0→min)       — distant deco meshes cull
	//     B2 effects quality            (fx.Niagara.QualityLevel, →min)        — particle density one notch
	//   Band C — EMERGENCY (I 0.75→1.00): visibly touches the near world; last resort, SHALLOW.
	//     C1 foliage+grass cull scale   (→min, default only 0.85)              — v0.9.7's 0.6 looked
	//        terrible (Ant boot-test) — foliage is the MOST visible cull, so it now moves last + least.
	//
	// The intensity SIGNAL is real thread time (GGameThreadTime / GRenderThreadTime — what "stat unit"
	// shows), not FPS: FPS conflates GPU and CPU, thread-time is the actual saturation of the side we
	// can help (Ant: "cpu need to track proper cpu usage, not just frames"). FPS is kept as a fallback
	// for the rare build where the thread timers read 0. Targets: relief starts as the CPU nears the
	// 90fps soft target's budget and is FULL at the 75fps hard floor's budget.
	// Disabled relief must still DECAY: if the player toggles CpuRelief off mid-episode, an early return
	// would leave every lever stuck below baseline until relaunch (review finding — graphics-floor
	// violation). So run the controller with a forced target of 0 until intensity fully restores.
	if (!bCpuRelief && CpuReliefIntensity <= 0.f) { return; }
	// Grace gate (mirror the stage engine): don't react during the post-settle / PSO-compile storm,
	// where frame times are erratic and would spuriously trigger relief.
	if (!bGraphicsApplied || (FPlatformTime::Seconds() - GraphicsAppliedTime) <= LadderGraceSec) { return; }

	IConsoleManager& CM = IConsoleManager::Get();
	static IConsoleVariable* GrassCV = nullptr;
	static IConsoleVariable* FoliageCV = nullptr;
	static IConsoleVariable* SkelCV = nullptr;
	static IConsoleVariable* ConvFreqCV = nullptr;
	static IConsoleVariable* ConvDistCV = nullptr;
	static IConsoleVariable* NiagaraCV = nullptr;

	// Capture the pre-relief baselines + cache the cvar pointers ONCE, logging ABSENT for any missing
	// knob (the log doubles as the cvar-existence inventory). Restore targets the CAPTURED baseline so
	// intensity-0 returns the user's TRUE value, not a hardcoded default. All levers are relief-only:
	// Min()/Max() against the baseline so we never "improve" past what the user chose.
	if (!bCpuBaselinesCaptured)
	{
		GrassCV    = CM.FindConsoleVariable(TEXT("grass.CullDistanceScale"));
		FoliageCV  = CM.FindConsoleVariable(TEXT("foliage.CullDistanceScale"));
		SkelCV     = CM.FindConsoleVariable(TEXT("r.SkeletalMeshLODBias"));
		ConvFreqCV = CM.FindConsoleVariable(TEXT("FG.ConveyorItemFrequency"));
		ConvDistCV = CM.FindConsoleVariable(TEXT("CSS.Conveyor.MaxDrawDistance"));
		NiagaraCV  = CM.FindConsoleVariable(TEXT("fx.Niagara.QualityLevel"));
		// Grass and foliage get SEPARATE baselines (review finding: one shared baseline let a relief
		// episode end with grass RAISED past a user who ran grass lower than foliage — floor violation).
		BaseCpuFoliageCull  = FoliageCV ? FoliageCV->GetFloat() : 1.f;
		BaseCpuGrassCull    = GrassCV ? GrassCV->GetFloat() : 1.f;
		BaseCpuSkelLOD      = SkelCV ? SkelCV->GetInt() : 0;
		BaseConveyorFreq    = ConvFreqCV ? ConvFreqCV->GetInt() : 60;
		BaseConveyorDist    = ConvDistCV ? ConvDistCV->GetFloat() : 100000.f;
		BaseNiagaraQuality  = NiagaraCV ? NiagaraCV->GetInt() : 3;
		CurCpuFoliageCull   = BaseCpuFoliageCull;
		CurCpuGrassCull     = BaseCpuGrassCull;
		CurCpuSkelLOD       = BaseCpuSkelLOD;
		CurConveyorFreq     = BaseConveyorFreq;
		CurConveyorDist     = BaseConveyorDist;
		CurNiagaraQuality   = BaseNiagaraQuality;
		if (!GrassCV)    { UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   CPU relief: grass.CullDistanceScale ABSENT (skipped)")); }
		if (!FoliageCV)  { UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   CPU relief: foliage.CullDistanceScale ABSENT (skipped)")); }
		if (!SkelCV)     { UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   CPU relief: r.SkeletalMeshLODBias ABSENT (skipped)")); }
		if (!ConvFreqCV) { UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   CPU relief: FG.ConveyorItemFrequency ABSENT (skipped)")); }
		if (!ConvDistCV) { UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   CPU relief: CSS.Conveyor.MaxDrawDistance ABSENT (skipped)")); }
		if (!NiagaraCV)  { UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   CPU relief: fx.Niagara.QualityLevel ABSENT (skipped)")); }
		bCpuBaselinesCaptured = true;
	}

	// -------- Intensity signal: REAL thread time first, FPS fallback. --------
	// The CPU cost of a frame is the slower of the game thread and render thread (the other threads
	// pipeline behind these). Smoothed like the frame time so a single spike doesn't slam the ladder.
	const float RawCpuMs = FMath::Max(GameThreadMs, RenderThreadMs);
	SmoothedCpuMs = (SmoothedCpuMs <= 0.f) ? RawCpuMs : FMath::Lerp(SmoothedCpuMs, RawCpuMs, 0.2f);

	// Frame-cap awareness (review finding: cap-blind targets would run PERMANENT relief on a healthy
	// rig capped below the comfort target — e.g. a 60fps-capped player can never reach "90"). Clamp
	// both targets to the live cap so a capped rig's comfort point is the cap itself.
	float EffComfortFPS = CpuComfortFPS;
	float EffFloorFPS   = CpuFloorFPS;
	if (const IConsoleVariable* CapCV = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
	{
		const float Cap = CapCV->GetFloat();
		if (Cap > 0.f)
		{
			EffComfortFPS = FMath::Min(EffComfortFPS, Cap);
			EffFloorFPS   = FMath::Min(EffFloorFPS, EffComfortFPS - 5.f);
		}
	}

	// Intensity target. Thread-time path gates on the SIGNAL ITSELF, not on !bGpuBound — in a dense
	// base BOTH sides can miss the floor at once, and the game thread still needs defending (review
	// finding: the old gate zeroed relief exactly when it was needed most). The timers are idle- and
	// present-wait-subtracted in this engine, so a capped/vsynced rig doesn't false-fire. The FPS
	// fallback (timers read 0) can't tell CPU from GPU, so IT keeps the GPU-bound gate. Disabled
	// relief forces target 0 → the slew below walks every lever back to baseline, then we early-out.
	float Target = 0.f;
	if (bCpuRelief)
	{
		const float ComfortMs = 1000.f / EffComfortFPS;
		const float FloorMs   = 1000.f / EffFloorFPS;
		if (SmoothedCpuMs > 0.05f)
		{
			Target = FMath::Clamp((SmoothedCpuMs - ComfortMs) / FMath::Max(FloorMs - ComfortMs, 0.1f), 0.f, 1.f);
		}
		else if ((bGpuKnown ? !bGpuBound : true) && FrameMs > 0.f)
		{
			const float FPS  = 1000.f / FrameMs;
			const float Span = FMath::Max(EffComfortFPS - EffFloorFPS, 1.f);
			Target = FMath::Clamp((EffComfortFPS - FPS) / Span, 0.f, 1.f);
		}
	}
	// FAST up (defend the floor), SLOW down (gentle restore — avoids pop-in flicker on the way back).
	const float Rate = (Target > CpuReliefIntensity) ? CpuReliefUpPerSec : CpuReliefDnPerSec;
	const float Step = Rate * FMath::Clamp(DT, 0.f, 1.f);
	CpuReliefIntensity = FMath::Clamp(FMath::Clamp(Target, CpuReliefIntensity - Step, CpuReliefIntensity + Step), 0.f, 1.f);
	const float I = CpuReliefIntensity;

	// Band helper: how far a lever inside window [S..E] of the master intensity is engaged (0..1).
	auto Band = [I](float S, float E) { return FMath::Clamp((I - S) / FMath::Max(E - S, 0.01f), 0.f, 1.f); };

	// External-writer re-adoption (review finding): several relief cvars are ALSO owned by the game's
	// settings/scalability systems (conveyor options, effects quality, skeletal LOD). If the live value
	// differs from what WE last wrote, the player changed it via the menu — re-adopt it as the new
	// baseline instead of fighting the settings system with our stale one.
	if (ConvFreqCV && ConvFreqCV->GetInt()   != CurConveyorFreq)   { BaseConveyorFreq   = ConvFreqCV->GetInt();   CurConveyorFreq   = BaseConveyorFreq; }
	if (ConvDistCV && FMath::Abs(ConvDistCV->GetFloat() - CurConveyorDist) > 1.f) { BaseConveyorDist = ConvDistCV->GetFloat(); CurConveyorDist = BaseConveyorDist; }
	if (NiagaraCV  && NiagaraCV->GetInt()    != CurNiagaraQuality) { BaseNiagaraQuality = NiagaraCV->GetInt();    CurNiagaraQuality = BaseNiagaraQuality; }
	if (SkelCV     && SkelCV->GetInt()       != CurCpuSkelLOD)     { BaseCpuSkelLOD     = SkelCV->GetInt();       CurCpuSkelLOD     = BaseCpuSkelLOD; }

	// ---- Band A (0.00→0.40): invisible relief. ----
	// A1: belt items refresh less often. 60Hz→24Hz is imperceptible beyond a few meters and saves real
	//     game-thread time in belt-heavy bases. Min() so a user who already runs lower stays lower.
	if (ConvFreqCV)
	{
		const int32 FreqTarget = FMath::Min(BaseConveyorFreq, ConveyorItemFreqMin);
		const int32 Freq = FMath::RoundToInt(FMath::Lerp((float)BaseConveyorFreq, (float)FreqTarget, Band(0.f, 0.4f)));
		if (Freq != CurConveyorFreq) { ConvFreqCV->Set(Freq, ECVF_SetByConsole); CurConveyorFreq = Freq; }
	}
	// A2: distant belts stop rendering their items (the belts themselves keep rendering). The epsilon
	//     gate skips micro-writes mid-slew; at band 0 we snap to the EXACT baseline (review finding:
	//     epsilon-gated levers otherwise leave permanent sub-baseline residue after an episode).
	if (ConvDistCV)
	{
		const float ABand = Band(0.f, 0.4f);
		const float DistTarget = FMath::Min(BaseConveyorDist, ConveyorDrawDistMin);
		const float Dist = (ABand <= 0.f) ? BaseConveyorDist : FMath::Lerp(BaseConveyorDist, DistTarget, ABand);
		if ((ABand <= 0.f && CurConveyorDist != BaseConveyorDist) || FMath::Abs(Dist - CurConveyorDist) > 500.f)
		{
			ConvDistCV->Set(Dist, ECVF_SetByConsole); CurConveyorDist = Dist;
		}
	}
	// A3: far skeletal meshes (creatures/other players) coarsen one-two LODs. Integer step lever.
	if (SkelCV)
	{
		const int32 SkelMax = FMath::Max(BaseCpuSkelLOD, (int32)CpuSkelLODMax);
		const int32 SkelLOD = FMath::RoundToInt(FMath::Lerp((float)BaseCpuSkelLOD, (float)SkelMax, Band(0.f, 0.4f)));
		if (SkelLOD != CurCpuSkelLOD) { SkelCV->Set(SkelLOD, ECVF_SetByConsole); CurCpuSkelLOD = SkelLOD; }
	}

	// ---- Band B (0.40→0.75): moderate relief. ----
	// B1: FG building distance-cull — distant buildable DETAIL meshes cull sooner. The modifier is
	//     relative (1.0 = game default), so Lerp from 1.0. Re-apply on a WORLD CHANGE (a fresh world's
	//     subsystem hasn't seen our modifier) even when the value itself didn't move.
	UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
	const float BuildCull = FMath::Lerp(1.f, CpuBuildCullMin, Band(0.4f, 0.75f));
	if (World && (World != LastCpuWorld || FMath::Abs(BuildCull - CurCpuBuildCull) > 0.02f))
	{
		if (AFGBuildableSubsystem* BS = AFGBuildableSubsystem::Get(World))
		{
			BS->UpdateBuildableCullDistances(BuildCull);
			CurCpuBuildCull = BuildCull;
			LastCpuWorld = World;
		}
	}
	// B2: particle/effects density one notch. Integer lever with HYSTERESIS (review finding: a single
	//     threshold made intensity hovering at the midpoint flap the quality level, and every write
	//     reinitializes Niagara systems = hitch + particle pop). Engage past 0.6, release below 0.45.
	if (NiagaraCV)
	{
		const float BBand = Band(0.4f, 0.75f);
		const int32 NiagTarget = FMath::Min(BaseNiagaraQuality, NiagaraQualityMin);
		int32 NiagQ = CurNiagaraQuality;
		if      (BBand >= 0.6f)  { NiagQ = NiagTarget; }
		else if (BBand <= 0.45f) { NiagQ = BaseNiagaraQuality; }
		if (NiagQ != CurNiagaraQuality) { NiagaraCV->Set(NiagQ, ECVF_SetByConsole); CurNiagaraQuality = NiagQ; }
	}

	// ---- Band C (0.75→1.00): emergency only — the first lever a player can clearly SEE. ----
	// C1: foliage + grass cull distance, SHALLOW (default floor 0.85 = only 15% shorter). v0.9.7
	//     shipped 0.6 across the whole intensity range and Ant's boot-test verdict was "foliage looks
	//     terrible" — hence both the late window AND the shallow floor. Each cvar Lerps from its OWN
	//     baseline (Min() so we only ever shorten), and band 0 snaps both to their exact baselines.
	{
		const float CBand = Band(0.75f, 1.f);
		if (FoliageCV)
		{
			const float Tgt = FMath::Min(BaseCpuFoliageCull, CpuFoliageCullMin);
			const float V = (CBand <= 0.f) ? BaseCpuFoliageCull : FMath::Lerp(BaseCpuFoliageCull, Tgt, CBand);
			if ((CBand <= 0.f && CurCpuFoliageCull != BaseCpuFoliageCull) || FMath::Abs(V - CurCpuFoliageCull) > 0.02f)
			{
				FoliageCV->Set(V, ECVF_SetByConsole); CurCpuFoliageCull = V;
			}
		}
		if (GrassCV)
		{
			const float Tgt = FMath::Min(BaseCpuGrassCull, CpuFoliageCullMin);
			const float V = (CBand <= 0.f) ? BaseCpuGrassCull : FMath::Lerp(BaseCpuGrassCull, Tgt, CBand);
			if ((CBand <= 0.f && CurCpuGrassCull != BaseCpuGrassCull) || FMath::Abs(V - CurCpuGrassCull) > 0.02f)
			{
				GrassCV->Set(V, ECVF_SetByConsole); CurCpuGrassCull = V;
			}
		}
	}
}

bool FWNLPerfGovernor::Tick(float TickDelta)
{
	const float FrameMs = FApp::GetDeltaTime() * 1000.0f;
	if (FrameMs <= 0.f || FrameMs > 500.f)
	{
		return true; // hitch/loading frame — never steer off a single outlier
	}
	SmoothedFrameMs = (SmoothedFrameMs <= 0.f)
		? FrameMs
		: FMath::Lerp(SmoothedFrameMs, FrameMs, 0.2f);

	// GPU frame time (boot-test finding): the server world is CPU/game-thread-bound (~55fps in a
	// dense base) — cutting GPU quality there gains ZERO fps. GPU timestamps tell us which side is
	// the limiter so we only spend quality when the GPU is actually the bottleneck. 0 = timestamps
	// unavailable on this RHI → conservatively assume GPU-bound (the old wall-time behavior).
	{
		const float GpuMs = RHIGetGPUFrameCycles() * float(FPlatformTime::GetSecondsPerCycle() * 1000.0);
		if (GpuMs > 0.f && GpuMs < 500.f)
		{
			SmoothedGpuMs = (SmoothedGpuMs <= 0.f) ? GpuMs : FMath::Lerp(SmoothedGpuMs, GpuMs, 0.2f);
		}
	}

	// (v0.9.6: the old r.ScreenPercentage baseline-sync is gone — the engine drives resolution now via
	//  native dynamic res, and CurrentPct is read from it below. The user's res-scale slider is adopted
	//  as the dyn-res ceiling in the user-settings poll above.)

	const double Now = FPlatformTime::Seconds();

	// ---- cap-aware frame budget (review finding): a machine sitting AT a frame cap or vsync
	// plateau is healthy, not overloaded. Without this a 60Hz-capped rig reads as permanently
	// over the 95-FPS budget — resolution pins to the floor and the ladder strips every uplift
	// for the whole session. The budget follows the live cap, and near-cap frames count as
	// clear headroom so restores keep working while capped. ----
	float BudgetMs = 1000.f / TargetFPS;
	float RaiseMs  = 1000.f / FMath::Min(TargetFPS + 15.f, MaxFPS);
	{
		float CapMs = 0.f;
		if (const IConsoleVariable* Cap = MaxFPSVar())
		{
			const float LiveCap = Cap->GetFloat();
			if (LiveCap > 0.f) { CapMs = 1000.f / LiveCap; }
		}
		const IConsoleVariable* VSync = VSyncVar();
		if (VSync && VSync->GetInt() != 0)
		{
			// The display refresh isn't readable from a mod, so LEARN the vsync plateau: track
			// the lowest smoothed frame time (with a very slow upward drift so a one-off fast
			// frame can't understate it forever). Under load the frame time rises ABOVE the
			// plateau, so overload detection still works.
			if (VsyncPlateauMs <= 0.f || SmoothedFrameMs < VsyncPlateauMs)
			{
				VsyncPlateauMs = SmoothedFrameMs;
			}
			else
			{
				VsyncPlateauMs += 0.05f * FMath::Clamp(TickDelta, 0.f, 1.f); // ~3ms/min recovery
			}
			CapMs = FMath::Max(CapMs, VsyncPlateauMs);
		}
		if (CapMs * 1.06f > BudgetMs)
		{
			BudgetMs = CapMs * 1.06f; // frames at the cap are in budget, not overload
			RaiseMs  = CapMs * 1.02f; // near-cap = clear headroom (ladder restores work while capped)
		}
	}

	const bool bOverBudget  = SmoothedFrameMs > BudgetMs;
	const bool bClearHeadroom = SmoothedFrameMs < RaiseMs;
	// GPU-bound = the GPU is the limiter, so res/quality cuts actually buy frame time. When the
	// game thread is the limiter (dense base, multiplayer replication) cuts buy NOTHING — never
	// degrade then. Timestamps unavailable → conservatively assume GPU-bound (old behavior).
	const bool bGpuKnown  = SmoothedGpuMs > 0.f;
	const bool bGpuBound  = !bGpuKnown || SmoothedGpuMs > SmoothedFrameMs * GpuBoundFraction;

	// ---- user-settings adoption (review finding): menu writes land BELOW our SetByConsole
	// priority, so the user's sliders would otherwise silently do nothing. Poll GameUserSettings
	// for intent and adopt CHANGES as new baselines, writing them through ourselves. On ANY adopted
	// change the stage engine RESETS to 0 (drop overlays), cancels any in-flight verify window (else
	// a stale verify could fire a below-baseline cut at full res — review finding), and re-captures
	// baselines after a settle so stage 0 means the user's CURRENT settings. ----
	auto ResetStageEngineToBaseline = [&](const TCHAR* Why)
	{
		ApplyStage(0, Why);                 // no-ops if already at 0; drops all overlays otherwise
		VerifyUntil = 0.0; VerifyingStage = 0;
		PendingKind = 0; StageTimer = 0.0;
		bBaselinesCaptured = false;          // force a fresh capture of the user's new settings
		RecaptureAt = Now + 2.5;             // after the game's scalability pass re-applies them
	};
	if (Now - LastUserPoll > 1.0)
	{
		LastUserPoll = Now;
		if (UGameUserSettings* GUS = GEngine ? GEngine->GetGameUserSettings() : nullptr)
		{
			const float UserLimit = GUS->GetFrameRateLimit();
			if (LastSeenUserFPSLimit < 0.f)
			{
				LastSeenUserFPSLimit = UserLimit; // first capture — observe only
			}
			else if (!FMath::IsNearlyEqual(UserLimit, LastSeenUserFPSLimit, 0.5f))
			{
				LastSeenUserFPSLimit = UserLimit;
				const float NewCap = (UserLimit > 0.f) ? UserLimit : CapFPS; // 0 = uncapped → our monitor-rate policy
				if (IConsoleVariable* Cap = MaxFPSVar())
				{
					Cap->Set(NewCap, ECVF_SetByConsole);
					UE_LOG(LogWNLPackFix, Display,
						TEXT("[WNLPackFix] governor: adopted user frame limit -> %.0f"), NewCap);
				}
			
					// The budget just changed -> re-earn bonuses from the new baseline.
					ResetStageEngineToBaseline(TEXT("user changed frame limit - stage reset"));
				}
			// Resolution-scale slider (vanilla UGameUserSettings path — if the game's menu
			// bypasses it this simply never fires; frame limit above is the certain one).
			float ScaleNorm = 0.f, ScaleVal = 0.f, MinScale = 0.f, MaxScale = 0.f;
			GUS->GetResolutionScaleInformationEx(ScaleNorm, ScaleVal, MinScale, MaxScale);
			if (LastSeenUserResScale < 0.f)
			{
				LastSeenUserResScale = ScaleVal;
			}
			else if (!FMath::IsNearlyEqual(ScaleVal, LastSeenUserResScale, 0.5f))
			{
				LastSeenUserResScale = ScaleVal;
				MinScreenPct = FMath::Min(ConfiguredMinScreenPct, ScaleVal);
				// The user's res-scale slider becomes the dynamic-res CEILING (they're choosing a max
				// render resolution); dyn-res scales DOWN from there when GPU-bound. Directly poking
				// r.ScreenPercentage is ignored once dyn-res is on, so re-arm the dyn-res bounds instead.
				MaxScreenPct = FMath::Clamp(ScaleVal, MinScreenPct, 100.f);
				if (bDynResApplied) { ApplyDynamicResolution(); }
				ResetStageEngineToBaseline(TEXT("user changed resolution scale — stage reset"));
				UE_LOG(LogWNLPackFix, Display,
					TEXT("[WNLPackFix] governor: adopted user resolution scale -> %.0f%% (dyn-res ceiling)"), ScaleVal);
			}
			// GI quality is a STAGED lever (sg.GlobalIlluminationQuality at +5/-4). A menu change to
			// it writes SetByScalability, which our SetByConsole overlay shadows — so we'd silently
			// fight the user (review finding). Detect it, and honor it: unshadow their value (write
			// it SetByConsole) BEFORE the reset schedules the re-capture, so the new baseline reads
			// the user's intent, not our stale overlay.
			const int32 UserGI = GUS->GetGlobalIlluminationQuality();
			if (LastSeenUserGIQuality < 0)
			{
				LastSeenUserGIQuality = UserGI; // first capture — observe only
			}
			else if (UserGI != LastSeenUserGIQuality)
			{
				LastSeenUserGIQuality = UserGI;
				ResetStageEngineToBaseline(TEXT("user changed GI quality — stage reset"));
				if (IConsoleVariable* GI = IConsoleManager::Get().FindConsoleVariable(TEXT("sg.GlobalIlluminationQuality")))
				{
					GI->Set(UserGI, ECVF_SetByConsole); // unshadow so the pending re-capture sees it
				}
				UE_LOG(LogWNLPackFix, Display,
					TEXT("[WNLPackFix] governor: adopted user GI quality -> %d"), UserGI);
			}
		}
	}

	// ---- one-time post-settle pass: vendor detection, Ray-Reconstruction guard, the baseline-free
	//      set (anti-shimmer, async CPU relief, Reflex, streaming) — then CAPTURE the stage-lever
	//      baselines. Order matters: baselines must reflect the settled stage-0 truth. ----
	if (!bGraphicsApplied && (Now - StartTime) > 2.0)
	{
		ApplyPostSettleGraphics();
		CaptureBaselines();
		bGraphicsApplied = true;
		GraphicsAppliedTime = Now; // anchors the stage-engine grace period
	}
	// Re-capture after a user settings change: the reset dropped our overlays and scheduled this so
	// the game's scalability pass could re-apply the new settings first; now they become stage 0.
	if (RecaptureAt > 0.0 && Now >= RecaptureAt && bGraphicsApplied)
	{
		Baselines.Reset(); LastWritten.Reset(); FloatFlags.Reset();
		CaptureBaselines();
		GraphicsAppliedTime = Now; // re-arm the settle grace: a settings change has its own PSO storm
		RecaptureAt = 0.0;
	}

	// ---- DYNAMIC VRAM (Ant): the OS-granted VRAM budget shrinks when the Claude app / other apps
	//      grab VRAM, so re-poll every 2s and resize the streaming pool to a fraction of the BUDGET
	//      (not of free headroom — that would feed back on our own pool and limit-cycle, review
	//      finding). Grace-gated like the stage engine (a resize reallocs = a hitch, don't do it in
	//      the join/PSO storm), hysteresis so it only moves on a real change, and skipped entirely
	//      when the user set an explicit StreamingPoolMB. ----
	if (bGraphicsApplied && StreamingPoolMB <= 0 && (Now - GraphicsAppliedTime) > LadderGraceSec
		&& Now - LastVramPoll > 2.0)
	{
		LastVramPoll = Now;
		QueryVramMB(BudgetVramMB, FreeVramMB);
		const int32 TierCap = (VramMB >= 15500) ? 6144 : (VramMB >= 11500) ? 4096 : 0;
		if (TierCap > 0 && BudgetVramMB > 0)
		{
			const int32 Target = FMath::Clamp<int32>((int32)(BudgetVramMB * PoolBudgetFraction), 512, TierCap);
			if (FMath::Abs(Target - CurStreamingPoolMB) > 256) // realloc is a hitch — only on a real move
			{
				if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.PoolSize")))
				{
					V->Set(Target, ECVF_SetByConsole);
					UE_LOG(LogWNLPackFix, Display,
						TEXT("[WNLPackFix] governor: VRAM budget %lld MB -> streaming pool %d -> %d MB"),
						BudgetVramMB, CurStreamingPoolMB, Target);
					CurStreamingPoolMB = Target;
				}
			}
		}
	}

	// Once-a-minute heartbeat: the frame/GPU split + where the levers sit. This is the receipt for
	// diagnosing bound-ness from a log instead of guessing (boot-test lesson).
	{
		static double LastHeartbeat = 0.0;
		if (Now - LastHeartbeat > 60.0)
		{
			LastHeartbeat = Now;
			UE_LOG(LogWNLPackFix, Display,
				TEXT("[WNLPackFix] governor: frame %.1fms gpu %.1fms (%s) res %.0f%% stage %+d"),
				SmoothedFrameMs, SmoothedGpuMs,
				!bGpuKnown ? TEXT("gpu-unknown") : (bGpuBound ? TEXT("gpu-bound") : TEXT("cpu-bound")),
				CurrentPct, QualityStage);
		}
	}

	// ---- lever 1: RESOLUTION — handed to the engine's native dynamic-res system (v0.9.6). The engine
	//      varies render resolution flash-free (built-in debounce) and hands it to the user's upscaler
	//      (DLSS/FSR/TSR); it holds full res when CPU-bound (UseCPUTimeLogic/UseGameThreadCriticalPath). We just READ its
	//      live fraction so the stage engine's res-floor gate below still works. (The old manual
	//      r.ScreenPercentage slew is gone — the engine now ignores it once dyn-res is on, and it was
	//      the geometry-flashing cause.) ----
	CurrentPct = ReadDynamicResPct();

	// ---- lever 1b: SMOOTH CPU-RELIEF controller (v0.9.6) — the CPU-bound lever. Defends a hard 60fps
	//      floor by scaling CPU-cheapening levers (building/foliage cull, skeletal LOD) proportionally,
	//      fast-up/slow-down, may dip below the user's baseline. No-op while GPU-bound. ----
	// Real thread times (what "stat unit" shows): cycle counters the engine updates every frame.
	// These feed the CPU-relief intensity so it reacts to actual CPU saturation, not blended FPS.
	UpdateCpuRelief(SmoothedFrameMs,
		FPlatformTime::ToMilliseconds(GGameThreadTime),
		FPlatformTime::ToMilliseconds(GRenderThreadTime),
		bGpuBound, bGpuKnown, FMath::Clamp(TickDelta, 0.f, 1.f));

	// ---- THE STAGE ENGINE (v0.9): one QualityStage in [-MaxCutStage..+MaxBonusStage] walked one
	//      step at a time. Promotion needs full res + measured GPU headroom held for a long dwell
	//      and is verified after the fact (a burn demotes, learns the stage's real cost, and starts
	//      a growing cooldown). Demotion under load is fast — bonuses shed BEFORE resolution is
	//      meaningfully spent. Cuts below 0 engage only at the res floor while GPU-bound (cutting a
	//      CPU-bound frame buys nothing) and restore LIFO the moment the pressure lifts. ----
	if (bGraphicsApplied && bBaselinesCaptured)
	{
		const bool bGraceOver = (Now - GraphicsAppliedTime) > LadderGraceSec;
		const bool bAtFullRes = CurrentPct >= MaxScreenPct - 0.1f;
		// Res-sag only counts as bonus pressure when we're NOT already recovering — otherwise the
		// slow +2%/s up-slew after any dip keeps "sagging" for seconds and cascades a whole tower of
		// bonuses off one transient spike (review finding).
		const bool bResSag    = (CurrentPct < MaxScreenPct - 3.f) && !bClearHeadroom;
		// Cuts are an EMERGENCY below the HARD floor (CpuFloorFPS, 75), not the soft 90 target:
		// resolution is the FIRST GPU scaler (DLSS/FSR/TSR absorb it well — Ant 2026-07-17), so
		// baseline quality only bleeds when res is already pinned at MinScreenPct AND the frame still
		// misses the floor. Between 75 and 90 the dyn-res controller carries it alone.
		// Max() with the cap-aware BudgetMs (review finding): on a rig frame-capped BELOW the 75 floor,
		// the raw floor test would be permanently true — the cap budget is the real "overloaded" line there.
		const float HardFloorMs = FMath::Max(1000.f / CpuFloorFPS, BudgetMs);
		const bool bAtFloorOverloaded = SmoothedFrameMs > HardFloorMs && bGpuBound && CurrentPct <= MinScreenPct + 0.1f;

		// Each dwell belongs to exactly ONE transition kind; if the pending kind changes, the dwell
		// anchor resets (review finding: a shared StageTimer let promote-dwell leak into an instant
		// demote and defeat the hysteresis). 1=verify 2=demote 3=promote 4=cut 5=restore.
		auto Arm = [&](int32 Kind) -> bool
		{
			if (PendingKind != Kind) { PendingKind = Kind; StageTimer = Now; }
			return true;
		};

		// Post-promote verify window: the promote must PROVE it fit. GPU over budget AND GPU-bound
		// inside the window = burn (a CPU-bound hitch is not the bonus's fault, so it doesn't burn).
		// Learned cost is clamped so one bad frame can't record a giant cost that locks the stage out
		// for the session (review finding).
		if (VerifyUntil > 0.0)
		{
			if (SmoothedGpuMs > BudgetMs && bGpuBound)
			{
				LearnedCostMs[VerifyingStage] = FMath::Clamp(SmoothedGpuMs - PreVerifyGpuMs, 0.5f, BudgetMs);
				BurnCooldownSec[VerifyingStage] = (BurnCooldownSec[VerifyingStage] <= 0.f)
					? PromoteCooldownSec
					: FMath::Min(BurnCooldownSec[VerifyingStage] * 2.f, PromoteCooldownMaxSec);
				CooldownUntil[VerifyingStage] = Now + BurnCooldownSec[VerifyingStage];
				VerifyUntil = 0.0;
				ApplyStage(QualityStage - 1, TEXT("promote burned — over budget in verify window"));
				UE_LOG(LogWNLPackFix, Display,
					TEXT("[WNLPackFix] governor: stage +%d cost %.1fms (learned), cooldown %.0fs"),
					VerifyingStage, LearnedCostMs[VerifyingStage], BurnCooldownSec[VerifyingStage]);
			}
			else if (Now > VerifyUntil)
			{
				LearnedCostMs[VerifyingStage] = FMath::Clamp(SmoothedGpuMs - PreVerifyGpuMs, 0.f, BudgetMs);
				VerifyUntil = 0.0; // survived — cost recorded, stage stands
			}
		}
		else if (QualityStage > 0 && ((bOverBudget && bGpuBound) || bResSag) && Arm(2))
		{
			// Shed a bonus fast: real load, or the res controller had to sag (not just recovering).
			if (Now - StageTimer > DemoteDwellSec)
			{
				ApplyStage(QualityStage - 1, bResSag ? TEXT("resolution sag — bonus shed") : TEXT("load — bonus shed"));
				StageTimer = Now;
			}
		}
		else if (QualityStage >= 0 && QualityStage < MaxBonusStage && bGraceOver && bAtFullRes
			&& bClearHeadroom && bGpuKnown
			&& SmoothedGpuMs < BudgetMs * PromoteGpuFrac
			&& (FreeVramMB < 0 || FreeVramMB > VramFloorMB) // don't climb into VRAM pressure (dynamic)
			&& Now >= CooldownUntil[QualityStage + 1]
			&& (BudgetMs - SmoothedGpuMs) > PromoteHeadroomFactor * StageCostMs(QualityStage + 1)
			&& Arm(3))
		{
			// Promote: full res, clear headroom, enough measured margin for the next stage's cost,
			// held continuously for the dwell. One stage per dwell; verified above.
			if (Now - StageTimer > PromoteDwellSec)
			{
				PreVerifyGpuMs = SmoothedGpuMs;
				VerifyingStage = QualityStage + 1;
				ApplyStage(QualityStage + 1, TEXT("sustained headroom — bonus"));
				VerifyUntil = Now + VerifyWindowSec;
				PendingKind = 0; StageTimer = 0.0;
			}
		}
		else if (QualityStage <= 0 && QualityStage > -MaxCutStage && bGraceOver && bAtFloorOverloaded && Arm(4))
		{
			// Emergency cuts below the user baseline: res floor + GPU-bound + sustained.
			if (Now - StageTimer > NudgeAfterSec)
			{
				ApplyStage(QualityStage - 1, TEXT("sustained overload at res floor — cut"));
				StageTimer = Now; // next rung times from here
			}
		}
		else if (QualityStage < 0 && (bClearHeadroom || (bGpuKnown && !bGpuBound)) && Arm(5))
		{
			// Restore cuts LIFO: pressure lifted (or the frame is CPU-bound — cuts buy nothing there).
			if (Now - StageTimer > RestoreAfterSec)
			{
				ApplyStage(QualityStage + 1, TEXT("recovered — cut restored"));
				StageTimer = Now; // stagger further restores
			}
		}
		else
		{
			PendingKind = 0; StageTimer = 0.0; // no transition pending — re-earn from zero
		}
	}

	return true; // keep ticking
}

void FWNLPerfGovernor::CaptureBaselines()
{
	// Snapshot the live value of every cvar the stage tables reference. This is the stage-0 truth:
	// every policy (MaxOf/BaseDelta/BaseScale) computes from it and ApplyStage(0) restores it
	// EXACTLY — no drift, no hardcoded "restore" values (the v0.8.4 restore bug class).
	IConsoleManager& CM = IConsoleManager::Get();
	auto Capture = [&](const FWNLStageLever& L)
	{
		if (Baselines.Contains(L.CVar))
		{
			return;
		}
		if (IConsoleVariable* V = CM.FindConsoleVariable(L.CVar))
		{
			Baselines.Add(L.CVar, V->GetFloat());
			FloatFlags.Add(L.CVar, L.bFloat);
		}
		else
		{
			// Absent = the lever silently no-ops at every stage. Display on purpose: inventory.
			UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   stage lever ABSENT: %s"), L.CVar);
		}
	};
	for (const FWNLQualityStage& S : WNLGetBonusStages())
	{
		for (const FWNLStageLever& L : S.Levers) { Capture(L); }
	}
	for (const FWNLQualityStage& S : WNLGetCutStages())
	{
		for (const FWNLStageLever& L : S.Levers) { Capture(L); }
	}
	LastWritten = Baselines;
	bBaselinesCaptured = true;
	UE_LOG(LogWNLPackFix, Display,
		TEXT("[WNLPackFix] governor: %d stage-lever baselines captured (stage 0 = user settings)"),
		Baselines.Num());
}

float FWNLPerfGovernor::StageCostMs(int32 BonusStage) const
{
	if (BonusStage >= 1 && BonusStage <= 6 && LearnedCostMs[BonusStage] > 0.f)
	{
		return LearnedCostMs[BonusStage]; // measured beats estimated
	}
	const TArray<FWNLQualityStage>& Bonus = WNLGetBonusStages();
	return Bonus.IsValidIndex(BonusStage - 1) ? Bonus[BonusStage - 1].EstCostMs : 1.f;
}

void FWNLPerfGovernor::ApplyStage(int32 NewStage, const TCHAR* Reason)
{
	if (!bBaselinesCaptured || NewStage == QualityStage)
	{
		return;
	}
	// Declarative recompute: desired = baseline overlaid with the CUMULATIVE stage tables up to
	// NewStage (later stages override earlier ones), then write only what differs from our last
	// writes. Idempotent and drift-free; stage 0 is an exact baseline restore by construction.
	TMap<FString, float> Desired = Baselines;
	auto Overlay = [&](const FWNLStageLever& L)
	{
		const float* Base = Baselines.Find(L.CVar);
		if (!Base)
		{
			return; // cvar absent at capture
		}
		if (L.MinVramMB > 0 && VramMB < L.MinVramMB)
		{
			return; // VRAM-gated lever on a smaller card
		}
		float Out = *Base;
		switch (L.Policy)
		{
		case EWNLLeverPolicy::Absolute:  Out = L.Value;                       break;
		case EWNLLeverPolicy::MaxOf:     Out = FMath::Max(*Base, L.Value);    break;
		case EWNLLeverPolicy::MinOf:     Out = FMath::Min(*Base, L.Value);    break;
		case EWNLLeverPolicy::BaseDelta: Out = *Base + L.Value;               break;
		case EWNLLeverPolicy::BaseScale: Out = *Base * L.Value;               break;
		}
		Desired.Add(L.CVar, FMath::Clamp(Out, L.ClampMin, L.ClampMax));
	};
	if (NewStage > 0)
	{
		for (const FWNLQualityStage& S : WNLGetBonusStages())
		{
			if (S.Stage > NewStage) { break; }
			for (const FWNLStageLever& L : S.Levers) { Overlay(L); }
		}
	}
	else if (NewStage < 0)
	{
		for (const FWNLQualityStage& S : WNLGetCutStages())
		{
			if (S.Stage < NewStage) { break; }
			for (const FWNLStageLever& L : S.Levers) { Overlay(L); }
		}
	}

	IConsoleManager& CM = IConsoleManager::Get();
	int32 Writes = 0;
	for (const auto& Pair : Desired)
	{
		float* Last = LastWritten.Find(Pair.Key);
		if (Last && FMath::IsNearlyEqual(*Last, Pair.Value, 0.0001f))
		{
			continue;
		}
		if (IConsoleVariable* V = CM.FindConsoleVariable(*Pair.Key))
		{
			const bool* bAsFloat = FloatFlags.Find(Pair.Key);
			if (bAsFloat && *bAsFloat)
			{
				V->Set(Pair.Value, ECVF_SetByConsole);
			}
			else
			{
				V->Set(FMath::RoundToInt(Pair.Value), ECVF_SetByConsole);
			}
			LastWritten.Add(Pair.Key, Pair.Value);
			++Writes;
		}
	}
	const int32 OldStage = QualityStage;
	QualityStage = NewStage;
	// Name the destination stage in the log (0 = baseline) for readable boot diagnostics.
	const TCHAR* StageName = TEXT("user baseline");
	if (NewStage > 0)      { const auto& B = WNLGetBonusStages(); if (B.IsValidIndex(NewStage - 1)) StageName = B[NewStage - 1].Name; }
	else if (NewStage < 0) { const auto& C = WNLGetCutStages();   if (C.IsValidIndex(-NewStage - 1)) StageName = C[-NewStage - 1].Name; }
	UE_LOG(LogWNLPackFix, Display,
		TEXT("[WNLPackFix] governor: stage %+d -> %+d '%s' (%d cvar writes) — %s"),
		OldStage, NewStage, StageName, Writes, Reason);
}

const TCHAR* FWNLPerfGovernor::VendorName(EGpuVendor V)
{
	switch (V)
	{
	case EGpuVendor::NVIDIA: return TEXT("NVIDIA");
	case EGpuVendor::AMD:    return TEXT("AMD");
	case EGpuVendor::Intel:  return TEXT("Intel");
	case EGpuVendor::Other:  return TEXT("Other");
	default:                 return TEXT("Unknown");
	}
}

const TCHAR* FWNLPerfGovernor::UpscalerName(EUpscaler U)
{
	switch (U)
	{
	case EUpscaler::TSR:  return TEXT("TSR");
	case EUpscaler::DLSS: return TEXT("DLSS");
	case EUpscaler::XeSS: return TEXT("XeSS");
	case EUpscaler::FSR:  return TEXT("FSR");
	default:              return TEXT("None");
	}
}

// Which temporal upscaler is LIVE right now. DLSS/XeSS/FSR each expose an explicit enable cvar; TSR is
// r.AntiAliasingMethod==4. Best-effort read — where a knob hasn't been set yet (timing) the caller falls
// back to the vendor default (NVIDIA + DLSS knobs ⇒ assume DLSS). Reads only; never writes.
FWNLPerfGovernor::EUpscaler FWNLPerfGovernor::DetectActiveUpscaler() const
{
	IConsoleManager& CM = IConsoleManager::Get();
	auto On = [&CM](const TCHAR* Name) -> bool
	{
		const IConsoleVariable* V = CM.FindConsoleVariable(Name);
		return V && V->GetInt() != 0;
	};
	if (On(TEXT("r.NGX.DLSS.Enable")))                            return EUpscaler::DLSS;
	if (On(TEXT("r.XeSS.Enabled")))                              return EUpscaler::XeSS; // Quality is a preset selector, not enable
	if (On(TEXT("r.FidelityFX.FSR.Enabled")))                    return EUpscaler::FSR;
	if (const IConsoleVariable* AA = CM.FindConsoleVariable(TEXT("r.AntiAliasingMethod")))
	{
		if (AA->GetInt() == 4) return EUpscaler::TSR;
	}
	return EUpscaler::None;
}

// ============================ IN-GAME CONFIG (v0.9.8, console interface) ============================
// "Simple for now" (Ant): two console commands usable from the in-game console (` key) —
//   WNLPackFix.Status              — one-glance live state of the whole governor
//   WNLPackFix.Set <Key> <Value>   — edit any top-level WNLPackFix.cfg key; safe knobs apply LIVE
// The full SML settings-menu UI (with a first-launch popup) is the 1.0 release item; this gives every
// knob an in-game path today without cooking content.

void FWNLPerfGovernor::PrintStatus() const
{
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] ---- status ----"));
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]  enabled=%d  vendor=%s  upscaler=%s"),
		bEnabled ? 1 : 0, VendorName(Vendor), UpscalerName(ActiveUpscaler));
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]  targets: soft %.0f fps / hard floor %.0f fps  (res floor %.0f%%)"),
		TargetFPS, CpuFloorFPS, MinScreenPct);
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]  frame %.1fms  gpu %.1fms  cpu(threads) %.1fms  res %.0f%%  stage %+d"),
		SmoothedFrameMs, SmoothedGpuMs, SmoothedCpuMs, CurrentPct, QualityStage);
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]  cpu relief %.0f%%: conveyor %dHz/%.0fcm  buildCull %.2f  fxQ %d  foliage %.2f  skelLOD %d"),
		CpuReliefIntensity * 100.f, CurConveyorFreq, CurConveyorDist, CurCpuBuildCull,
		CurNiagaraQuality, CurCpuFoliageCull, CurCpuSkelLOD);
}

void FWNLPerfGovernor::SetConfigKey(const FString& Key, const FString& Value)
{
	// 1. Load the existing config so we edit, not clobber (and so unknown keys can be rejected —
	//    a typo'd key silently "working" would be worse than an error).
	const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("Configs"), TEXT("WNLPackFix.cfg"));
	FString Raw;
	TSharedPtr<FJsonObject> Json;
	if (!FFileHelper::LoadFileToString(Raw, *Path) ||
	    !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Json) || !Json.IsValid())
	{
		UE_LOG(LogWNLPackFix, Error, TEXT("[WNLPackFix] Set: cannot read %s (launch the game once to generate it)"), *Path);
		return;
	}
	if (!Json->HasField(Key))
	{
		UE_LOG(LogWNLPackFix, Error, TEXT("[WNLPackFix] Set: unknown key '%s' (top-level keys only — see the cfg file for names)"), *Key);
		return;
	}

	// 2. Type-check the value against the EXISTING field's type (review finding: writing "1" to a bool
	//    key used to both flip the feature OFF and corrupt the key's json type). Bool keys accept
	//    true/false/1/0; number keys require a number; mismatches are rejected with a clear error.
	const bool bIsTrue    = Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
	const bool bIsFalse   = Value.Equals(TEXT("false"), ESearchCase::IgnoreCase);
	const bool bIsNumeric = Value.IsNumeric();
	const EJson FieldType = Json->Values[Key]->Type;
	bool  BoolVal = false;
	float NumVal  = 0.f;
	if (FieldType == EJson::Boolean)
	{
		if (bIsTrue || bIsFalse)                 { BoolVal = bIsTrue; }
		else if (bIsNumeric)                     { BoolVal = (FCString::Atod(*Value) != 0.0); } // 1/0 shorthand
		else
		{
			UE_LOG(LogWNLPackFix, Error, TEXT("[WNLPackFix] Set: '%s' is a true/false setting"), *Key);
			return;
		}
		Json->SetBoolField(Key, BoolVal);
	}
	else if (FieldType == EJson::Number)
	{
		if (!bIsNumeric)
		{
			UE_LOG(LogWNLPackFix, Error, TEXT("[WNLPackFix] Set: '%s' needs a number, got '%s'"), *Key, *Value);
			return;
		}
		NumVal = FCString::Atof(*Value);
		Json->SetNumberField(Key, FCString::Atod(*Value));
	}
	else
	{
		UE_LOG(LogWNLPackFix, Error, TEXT("[WNLPackFix] Set: '%s' is a section — set its inner keys instead"), *Key);
		return;
	}
	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
	FFileHelper::SaveStringToFile(Out, *Path);

	// 3. Live-apply the knobs that are safe to move mid-session (targets, floors, toggles, stage caps).
	//    Everything else (hook gates, one-time post-settle levers) genuinely needs a relaunch, so we say
	//    so instead of pretending. Clamps mirror the loader's so a console typo can't wedge the governor.
	//    NOTE "Enabled" is deliberately NOT live: it's only read in Start(), and pretending otherwise
	//    was a review finding. CpuRelief→false IS safe live — the relief controller keeps ticking and
	//    walks every lever back to its exact baseline before idling (see UpdateCpuRelief's early-out).
	bool bLive = true;
	if      (Key == TEXT("TargetFPS"))     { TargetFPS = FMath::Clamp(NumVal, 30.f, 240.f); if (bDynResApplied) { ApplyDynamicResolution(); } }
	else if (Key == TEXT("CpuFloorFPS"))   { CpuFloorFPS = FMath::Clamp(NumVal, 30.f, 120.f); }
	else if (Key == TEXT("CpuComfortFPS")) { CpuComfortFPS = FMath::Clamp(NumVal, CpuFloorFPS + 5.f, 240.f); }
	else if (Key == TEXT("CpuRelief"))     { bCpuRelief = BoolVal; }
	else if (Key == TEXT("MaxBonusStage")) { MaxBonusStage = FMath::Clamp((int32)NumVal, 0, 6); }
	else if (Key == TEXT("MaxCutStage"))   { MaxCutStage = FMath::Clamp((int32)NumVal, 0, 4); }
	else if (Key == TEXT("MinScreenPct"))  { MinScreenPct = FMath::Clamp(NumVal, 25.f, 100.f); ConfiguredMinScreenPct = MinScreenPct; if (bDynResApplied) { ApplyDynamicResolution(); } }
	else if (Key == TEXT("MaxScreenPct"))  { MaxScreenPct = FMath::Clamp(NumVal, MinScreenPct, 100.f); if (bDynResApplied) { ApplyDynamicResolution(); } }
	else                                   { bLive = false; }

	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] Set %s = %s  (%s)"),
		*Key, *Value, bLive ? TEXT("applied LIVE + saved") : TEXT("saved — takes effect next launch"));
}

void FWNLPerfGovernor::MergeMenuConfig(TSharedPtr<FJsonObject>& MainJson)
{
	// The in-game menu (UWNLModConfiguration) exposes EVERY governor key, grouped into visual sections.
	// SML saves each UI section as a NESTED json object, while the governor's keys are FLAT — so hoist
	// every section's children up one level (menu property names == real config key names by design),
	// then overwrite the main json's fields with them. Menu wins. "Fog" stays nested: the fog controller
	// overlays that section itself. Absent file = menu never touched → the main config stands.
	const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("Configs"), TEXT("WNLPackFix"), TEXT("Menu.cfg"));
	FString Raw;
	TSharedPtr<FJsonObject> Menu;
	if (!FFileHelper::LoadFileToString(Raw, *Path) ||
	    !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Menu) || !Menu.IsValid())
	{
		return; // no menu file yet — nothing to merge
	}
	int32 Merged = 0;
	for (const auto& Pair : Menu->Values)
	{
		if (Pair.Value->Type == EJson::Object && Pair.Key != TEXT("Fog"))
		{
			for (const auto& Inner : Pair.Value->AsObject()->Values)   // hoist section children
			{
				// An explicit menu res-floor must WIN over the vendor floor adjustments at post-settle
				// (review finding: they silently overrode the player's choice in both directions).
				if (Inner.Key == TEXT("MinScreenPct")) { bMenuResFloor = true; }
				MainJson->SetField(Inner.Key, Inner.Value); ++Merged;
			}
		}
		else if (Pair.Key != TEXT("SML_ModVersion_DoNotChange"))       // SML bookkeeping, not a knob
		{
			MainJson->SetField(Pair.Key, Pair.Value); ++Merged;        // flat key (or the Fog object)
		}
	}
	UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] in-game menu config merged (%d values — menu wins)"), Merged);
}

// Console command registration. File-scope statics register once at module load; both commands guard
// against the dedicated server (no governor there) inside the lambda rather than at registration.
static FAutoConsoleCommand GWNLStatusCmd(
	TEXT("WNLPackFix.Status"),
	TEXT("Print the WNLPackFix governor's live state."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		if (!IsRunningDedicatedServer()) { FWNLPerfGovernor::Get().PrintStatus(); }
	}));

static FAutoConsoleCommand GWNLSetCmd(
	TEXT("WNLPackFix.Set"),
	TEXT("WNLPackFix.Set <Key> <Value> — edit a WNLPackFix.cfg key; safe knobs apply live."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (IsRunningDedicatedServer()) { return; }
		if (Args.Num() != 2)
		{
			UE_LOG(LogWNLPackFix, Error, TEXT("[WNLPackFix] usage: WNLPackFix.Set <Key> <Value>"));
			return;
		}
		FWNLPerfGovernor::Get().SetConfigKey(Args[0], Args[1]);
	}));

void FWNLPerfGovernor::ApplyPostSettleGraphics()
{
	IConsoleManager& CM = IConsoleManager::Get();
	// Null-guarded setters that LOG the live baseline -> new value, so a boot confirms each take
	// against measured reality instead of a hardcoded guess (and absent cvars silently no-op).
	auto SetI = [&CM](const TCHAR* Name, int32 Value, const TCHAR* Label) -> bool
	{
		if (IConsoleVariable* V = CM.FindConsoleVariable(Name))
		{
			const int32 Was = V->GetInt();
			V->Set(Value, ECVF_SetByConsole);
			UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   %s: %s %d -> %d"), Label, Name, Was, Value);
			return true;
		}
		UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   %s: %s ABSENT (skipped)"), Label, Name);
		return false;
	};
	auto SetF = [&CM](const TCHAR* Name, float Value, const TCHAR* Label) -> bool
	{
		if (IConsoleVariable* V = CM.FindConsoleVariable(Name))
		{
			const float Was = V->GetFloat();
			V->Set(Value, ECVF_SetByConsole);
			UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   %s: %s %.3f -> %.3f"), Label, Name, Was, Value);
			return true;
		}
		// ABSENT at Display on purpose: the log doubles as the cvar-existence inventory (the
		// v0.8.3 boots proved silent-absent cvars hide real bugs — a wrong name and a dead knob).
		UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   %s: %s ABSENT (skipped)"), Label, Name);
		return false;
	};

	// --- 1. GPU vendor detection — ALWAYS runs (Reflex + FSR gate on Vendor, so it must be known
	//        even when the per-vendor tuning branch below is disabled). ---
	if      (IsRHIDeviceNVIDIA()) Vendor = EGpuVendor::NVIDIA;
	else if (IsRHIDeviceAMD())    Vendor = EGpuVendor::AMD;
	else if (IsRHIDeviceIntel())  Vendor = EGpuVendor::Intel;
	else                          Vendor = EGpuVendor::Other;
	bDlssKnobsPresent = (CM.FindConsoleVariable(TEXT("r.NGX.DLSS.Enable")) != nullptr);

	// Intel sub-split: a weak Iris/UHD iGPU must NOT inherit Arc dGPU (or AMD) tuning — it wants the
	// OPPOSITE (drop res hard, no bonuses). Classify by adapter name.
	// Discrete Arc carries an A#/B# MODEL token ("Arc A770", "Arc B580"). Integrated "Intel(R) Arc(TM)
	// Graphics" (Core Ultra iGPU) has "Arc" but NO model number → it must land in the iGPU path, not dGPU.
	const bool bIntelArc  = (Vendor == EGpuVendor::Intel) &&
		( GRHIAdapterName.Contains(TEXT("A7")) || GRHIAdapterName.Contains(TEXT("A5"))
		|| GRHIAdapterName.Contains(TEXT("A3")) || GRHIAdapterName.Contains(TEXT("B5"))
		|| GRHIAdapterName.Contains(TEXT("B7")) );
	const bool bIntelIGpu = (Vendor == EGpuVendor::Intel) && !bIntelArc &&
		( GRHIAdapterName.Contains(TEXT("Iris")) || GRHIAdapterName.Contains(TEXT("UHD"))
		|| GRHIAdapterName.Contains(TEXT("HD Graphics")) || GRHIAdapterName.Contains(TEXT("Arc")) );

	// Tune on the user's LIVE upscaler, not the vendor. If disabled, or the probe is inconclusive on
	// NVIDIA with DLSS knobs present, assume DLSS (preserves the NVIDIA fast-path: low floor, no sharpen).
	ActiveUpscaler = bUpscalerAutoSelect ? DetectActiveUpscaler() : EUpscaler::None;
	// DLSS only when it reads LIVE. Post-settle runs after the menu applies, so a genuine timing race is
	// unlikely; if r.NGX.DLSS.Enable is present-and-0 the user simply has DLSS off — don't assume it on
	// (that would suppress the force-TSR safety net and arm dyn-res with no temporal upscaler).
	const bool bDlssActive = (ActiveUpscaler == EUpscaler::DLSS);
	// DLSS/XeSS/FSR each condition their own image (DLSS/XeSS AI, FSR via RCAS) → never stack the
	// tonemapper sharpen on top. Only TSR / None take it.
	const bool bConditionedImage = bDlssActive
		|| ActiveUpscaler == EUpscaler::XeSS || ActiveUpscaler == EUpscaler::FSR;

	UE_LOG(LogWNLPackFix, Display,
		TEXT("[WNLPackFix] governor: GPU vendor=%s adapter='%s' upscaler=%s%s%s dlssKnobs=%d"),
		VendorName(Vendor), *GRHIAdapterName, UpscalerName(ActiveUpscaler),
		bIntelArc ? TEXT(" (Arc dGPU)") : TEXT(""), bIntelIGpu ? TEXT(" (iGPU)") : TEXT(""),
		bDlssKnobsPresent ? 1 : 0);

	if (bVendorAdaptive)
	{
		// Res floor: only DLSS reconstructs cleanly at low res, so only DLSS keeps the low MinScreenPct;
		// TSR/XeSS/FSR/None get the higher AmdMinScreenPct floor. iGPU is the exception (handled below).
		// SKIPPED entirely when the player set an explicit floor in the in-game menu — their word wins.
		if (!bDlssActive && !bIntelIGpu && !bMenuResFloor)
		{
			ConfiguredMinScreenPct = FMath::Max(ConfiguredMinScreenPct, AmdMinScreenPct);
			MinScreenPct           = FMath::Max(MinScreenPct, AmdMinScreenPct);
		}

		// Bonus-stage ceilings per vendor/class (cap Lumen/Nanite ambition until each card is measured).
		if      (Vendor == EGpuVendor::AMD)   MaxBonusStage = FMath::Min(MaxBonusStage, 4); // RDNA weak at long-range SW-Lumen
		else if (Vendor == EGpuVendor::Intel) MaxBonusStage = FMath::Min(MaxBonusStage,     // ALL Intel capped (incl. unknown Intel)
			bIntelIGpu ? IntelIGpuMaxBonusStage : IntelArcMaxBonusStage);                   // iGPU: 0 · Arc/unknown: 2

		// iGPU: let dynamic res drop HARD to hold FPS (the opposite of the discrete floor above).
		// Also skipped when the player pinned an explicit menu floor — never lower a user-set value.
		if (bIntelIGpu && !bMenuResFloor)
		{
			MinScreenPct           = FMath::Min(MinScreenPct, IntelIGpuMinScreenPct);
			ConfiguredMinScreenPct = FMath::Min(ConfiguredMinScreenPct, IntelIGpuMinScreenPct);
		}

		// Sharpen: TSR/None only (DLSS/XeSS/FSR own their sharpening).
		if (TsrXessSharpen > 0.f && !bConditionedImage)
		{
			SetF(TEXT("r.Tonemapper.Sharpen"), TsrXessSharpen, TEXT("TSR/None tonemapper sharpen"));
		}

		// FSR polish ONLY when FSR is the live upscaler. (Old code wrote these on EVERY AMD user — but an
		// AMD player is just as likely on TSR/XeSS, so those writes were dead unless FSR was actually picked.)
		if (ActiveUpscaler == EUpscaler::FSR)
		{
			SetF(TEXT("r.FidelityFX.FSR.Sharpness"), FSRSharpness, TEXT("FSR RCAS sharpness"));
			SetI(TEXT("r.FidelityFX.FSR.UseSSRExperimentalDenoiser"), 1, TEXT("FSR SSR denoise"));
		}

		// AMD anti-flicker: RDNA2/3 shader-precision edge cases destabilise Lumen's temporal filters,
		// worst on the specular metal interiors that are every Satisfactory factory. TSR rejects it; TAA
		// amplifies it. Strictly AMD-gated, baseline-free, cheap.
		if (bAmdAntiFlicker && Vendor == EGpuVendor::AMD)
		{
			SetI(TEXT("r.TSR.ShadingRejection.Flickering"), 1, TEXT("AMD Lumen anti-flicker"));
			// Own kill-switch (0 = skip) so the two AMD anti-flicker cvars stay independently bisectable
			// if the friend's test pins a distant DF-shadow pop on this one.
			if (AmdDFShadowCullTile > 0.f)
			{
				SetF(TEXT("r.DFShadowCullTileWorldSize"), AmdDFShadowCullTile, TEXT("AMD DF-shadow terrain flicker"));
			}
		}

		// Force a temporal upscaler only when NONE is active (dynamic res needs one). TSR is the only one
		// settable from a raw cvar. On AMD, ALSO upgrade TAA→TSR (TAA is the RDNA Lumen-flicker trap).
		// Never fires when DLSS/XeSS/FSR/TSR is already live → we don't stomp the user's choice.
		if (bForceUpscalerIfNone && !bDlssActive && ActiveUpscaler == EUpscaler::None)
		{
			if (IConsoleVariable* AA = CM.FindConsoleVariable(TEXT("r.AntiAliasingMethod")))
			{
				const int32 CurAA = AA->GetInt(); // 0 None,1 FXAA,2 TAA,3 MSAA,4 TSR
				const bool bAmdTaa = (Vendor == EGpuVendor::AMD && bAmdAntiFlicker && CurAA == 2);
				if (CurAA == 0 || CurAA == 1 || CurAA == 3 || bAmdTaa)
				{
					AA->Set(4, ECVF_SetByConsole);
					ActiveUpscaler = EUpscaler::TSR;
					UE_LOG(LogWNLPackFix, Display,
						TEXT("[WNLPackFix]   forced TSR (r.AntiAliasingMethod %d -> 4)%s"), CurAA,
						bAmdTaa ? TEXT(" — AMD TAA->TSR anti-flicker") : TEXT(" — no temporal upscaler was active"));
				}
			}
		}

		UE_LOG(LogWNLPackFix, Display,
			TEXT("[WNLPackFix] governor: tuned for %s/%s — floor %.0f%%, sharpen %.2f, bonusCap %d"),
			VendorName(Vendor), UpscalerName(ActiveUpscaler), MinScreenPct,
			bConditionedImage ? 0.f : TsrXessSharpen, MaxBonusStage);
	}

	// --- 2. Ray Reconstruction hard guard (inert on software Lumen here; force off so nothing flips it) ---
	if (bAssertRayReconstructionOff)
	{
		if (IConsoleVariable* RR = CM.FindConsoleVariable(TEXT("r.NGX.DLSS.DenoiserMode")))
		{
			if (RR->GetInt() != 0)
			{
				RR->Set(0, ECVF_SetByConsole);
				UE_LOG(LogWNLPackFix, Display,
					TEXT("[WNLPackFix] governor: Ray Reconstruction guarded OFF (DenoiserMode -> 0)"));
			}
		}
	}

	// (VSM sharpen + SMRT quality moved to bonus stage +4 — the stage engine promotes them only
	// with measured headroom at full res and restores the captured baseline exactly on demote.)

	// --- 3. VSM anti-shimmer stability: page pool + cache retention (VRAM-only) + panning bias
	//        (coarser pages ONLY while the camera moves = anti-shimmer, free). Baseline-free. ---
	if (bVSMStability)
	{
		SetI(TEXT("r.Shadow.Virtual.MaxPhysicalPages"), 12288, TEXT("VSM pages"));
		// v0.9 FIX: the v0.8 name had a bogus "Clipmap." segment — silent no-op both boots
		// (engine-src: VirtualShadowMapClipmap.cpp registers it WITHOUT the segment).
		SetF(TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectionalMoving"), 1.0f, TEXT("VSM moving-bias"));
		SetI(TEXT("r.Shadow.Virtual.Cache.StaticSeparate"), 1, TEXT("VSM static cache"));
		SetI(TEXT("r.Shadow.Virtual.Cache.MaxUnreferencedLightAge"), 120, TEXT("VSM cache age"));
	}

	// --- 4. Frame-queue PROBE: RHI.MaximumFrameLatency is ABSENT on 1.2.3.1 (proven by the
	//         ABSENT log both boots). Kept null-guarded in case a future build registers it; the
	//         real input-latency mechanism is Reflex (block 11). ---
	if (MaxFrameQueue > 0)
	{
		SetI(TEXT("RHI.MaximumFrameLatency"), MaxFrameQueue, TEXT("frame queue (probe)"));
	}

	// (Volumetric-fog sharpen, Lumen final-gather density, and mesh-SDF tracing all moved into the
	// bonus stage tables (+3/+6) — headroom-gated where heavy levers belong.)

	// --- 5. Lumen reflection denoise (near-free; cleaner metal/floors) ---
	if (bReflectionDenoise)
	{
		SetI(TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction"), 1, TEXT("refl reconstruct"));
		SetI(TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction.NumSamples"), 5, TEXT("refl samples"));
		SetI(TEXT("r.Lumen.Reflections.DownsampleFactor"), 1, TEXT("refl full-res"));
	}

	// --- 5b. Lumen free-win levers. Trace compaction + trace-tile sort are pure compute
	//         SCHEDULING knobs — they speed reflection tracing with NO image change. (Probe-retention was
	//         dropped in review: NumFramesToKeepCachedProbes is a GI-cache TTL, not scheduling — raising it
	//         reuses stale probes and lags indirect light under day/night + moving lights, degrading GI
	//         freshness as a BASELINE. Revisit only via the stage engine with a night-scene boot-test.) ---
	if (bLumenPerfLevers)
	{
		SetI(TEXT("r.Lumen.Reflections.TraceCompaction.WaveOps"), 1, TEXT("Lumen refl trace compaction"));
		SetI(TEXT("r.Lumen.RadianceCache.SortTraceTiles"), 1, TEXT("Lumen radiance-cache tile sort"));
	}

	// --- 5c. Trivial anti-throttle: don't down-clock on battery (irrelevant on desktop, free on laptops) ---
	if (bDontLimitOnBattery)
	{
		SetI(TEXT("r.DontLimitOnBattery"), 1, TEXT("no battery down-clock"));
	}

	// --- 6. Contact shadows: static-geometry grounding (machine legs, pipes, foundations). The sun's
	//        baked ContactShadowLength may be 0, so force it. HARD-GATED on the suppression sweep
	//        being armed — the sweep (belt items + FOLIAGE, the v0.8.3 tree-shimmer fix) is what
	//        makes global contact shadows shimmer-safe. No sweep → no forced contact shadows. ---
	if (bContactShadows && GWNLContactShadowSweepArmed)
	{
		SetI(TEXT("r.ContactShadows"), 1, TEXT("contact shadows"));
		SetI(TEXT("r.ContactShadows.Standalone.Method"), 1, TEXT("contact Bend method"));
		SetF(TEXT("r.ContactShadows.OverrideLength"), ContactShadowLength, TEXT("contact length"));
		SetI(TEXT("r.ContactShadows.OverrideLengthInWS"), 0, TEXT("contact screen-relative"));
	}

	// --- 7. Nanite geometry-LOD sharpen on the already-Nanite terrain/rocks/foliage (optional polish).
	//        SKIPPED on Intel: the 5.6.1-CSS Nanite base-pass is unoptimised on Arc/iGPU (the fix is a
	//        UE5.7 backport), so a <1 pixels-per-edge sharpen costs disproportionately there. ---
	if (bNaniteSharpen)
	{
		if (Vendor != EGpuVendor::Intel)
		{
			SetF(TEXT("r.Nanite.MaxPixelsPerEdge"), NanitePixelsPerEdge, TEXT("Nanite pixels/edge"));
		}
		else
		{
			UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix]   Nanite sharpen skipped on Intel (slow 5.6 base-pass)"));
		}
	}

	// --- 8. NVIDIA Reflex (the REAL input-latency mechanism — the 91.6ms felt-lag fix). The game
	//         ships the StreamlineReflex plugin but defaults Enable=0 and has NO menu option for it,
	//         so defaulting ON usurps nothing. Enable is the master switch; Mode 1 = low-latency
	//         (2 = +boost, pins GPU clocks — opt-in). Auto/markers/HandleMaxTickRate left at plugin
	//         defaults. NVIDIA-gated; cvars simply don't resolve elsewhere. ---
	if (ReflexMode > 0 && Vendor == EGpuVendor::NVIDIA)
	{
		SetI(TEXT("t.Streamline.Reflex.Enable"), 1, TEXT("Reflex enable"));
		SetI(TEXT("t.Streamline.Reflex.Mode"), ReflexMode, TEXT("Reflex mode"));
	}

	// --- 9. CPU/game-thread relief (universal): async tick dispatch/cleanup + FX
	//         batching + parallel distance-field updates. Our base is game-thread bound (~55fps
	//         measured), so these attack the ACTUAL bottleneck. Config kill-switch: AsyncTick. ---
	if (bAsyncTick)
	{
		SetI(TEXT("tick.AllowAsyncTickDispatch"), 1, TEXT("async tick dispatch"));
		SetI(TEXT("tick.AllowAsyncTickCleanup"), 1, TEXT("async tick cleanup"));
		SetI(TEXT("FX.AllowAsyncTick"), 1, TEXT("FX async tick"));
		SetI(TEXT("FX.BatchAsync"), 1, TEXT("FX batch async"));
		SetI(TEXT("FX.BatchAsyncBatchSize"), 8, TEXT("FX batch size"));
		SetI(TEXT("FX.EarlyScheduleAsync"), 1, TEXT("FX early schedule"));
		SetI(TEXT("fx.AllowFastPathFunctionLibrary"), 1, TEXT("FX fast path"));
		SetI(TEXT("r.DistanceFields.ParallelUpdate"), 1, TEXT("DF parallel update"));
		SetI(TEXT("au.RenderThreadPriority"), 1, TEXT("audio RT priority"));
		// REMOVED (v0.9.4 flashing regression): D3D12.InsertOuterOcclusionQuery=1 +
		// r.DownsampledOcclusionQueries=1 are aggressive occlusion-culling opts that make geometry
		// test as occluded incorrectly -> "entire geometry flashing in/out" a few seconds after load
		// (boot 2026-07-17, prime suspect). They are a minor CPU win only. The rest of the async block
		// (tick dispatch/cleanup, FX batching, DF parallel update) is pure CPU threading, no cull risk.
	}

	// --- 10. Texture streaming pool — VRAM-gated by TOTAL card size, then clamped to what's actually
	//         FREE right now (never set LimitPoolSizeToVRAM=0 — that removes the OOM valve).
	//         The clamp keeps re-checking dynamically in Tick (block below) so the Claude app opening
	//         mid-session shrinks the pool instead of thrashing VRAM. ---
	{
		FTextureMemoryStats MemStats;
		RHIGetTextureMemoryStats(MemStats);
		VramMB = MemStats.DedicatedVideoMemory / (1024 * 1024); // TOTAL — card-capability stage gate
		QueryVramMB(BudgetVramMB, FreeVramMB);                   // granted budget + free headroom
		int32 PoolMB;
		if (StreamingPoolMB > 0)
		{
			PoolMB = StreamingPoolMB;                            // explicit user config — honored verbatim, no dynamic
		}
		else
		{
			// Card tier caps it; the granted budget fraction sizes it (reacts to other apps, no self-feedback).
			const int32 TierCap = (VramMB >= 15500) ? 6144 : (VramMB >= 11500) ? 4096 : 0;
			PoolMB = TierCap;
			if (TierCap > 0 && BudgetVramMB > 0)
			{
				PoolMB = FMath::Clamp<int32>((int32)(BudgetVramMB * PoolBudgetFraction), 512, TierCap);
			}
		}
		UE_LOG(LogWNLPackFix, Display,
			TEXT("[WNLPackFix]   VRAM: %lld MB total, %lld MB budget, %lld MB free -> streaming pool %d MB"),
			VramMB, BudgetVramMB, FreeVramMB, PoolMB);
		if (PoolMB > 0)
		{
			SetI(TEXT("r.Streaming.PoolSize"), PoolMB, TEXT("streaming pool"));
			CurStreamingPoolMB = PoolMB;
		}
	}

	// --- 10b REMOVED (v0.9.4 regression fix): raising r.Nanite.MaxVisibleClusters/MaxCandidateClusters/
	//     MaxNodes at post-settle caused ENTIRE-GEOMETRY FLASHING (boot 2026-07-17). Root cause: Nanite
	//     allocates its persistent cluster-culling buffers ONCE at init, sized off the STARTUP cvar value
	//     (CSS 100k). Bumping the cap at runtime makes the culling shader index past those fixed buffers
	//     -> overflow -> clusters flicker in/out. ECVF_RenderThreadSafe only means safe to READ on the
	//     render thread; it does NOT reallocate the buffers. The CSS caps CAN cause dropped geometry in
	//     extreme builds, but that fix MUST happen before Nanite init (early cvar/config), never here.
	//     Left out entirely until an init-time path is built and boot-tested.

	// --- 11. Grass: refresh every N frames (CPU relief, near-free); density is a STATIC opt-in
	//         only (a mid-session density change forces a rebuild pop). ---
	if (GrassTickInterval > 0)
	{
		SetI(TEXT("grass.TickInterval"), GrassTickInterval, TEXT("grass tick interval"));
	}
	if (!FMath::IsNearlyEqual(GrassDensityScale, 1.0f))
	{
		SetF(TEXT("grass.DensityScale"), GrassDensityScale, TEXT("grass density (opt-in)"));
	}

	// --- 12. Incremental GC — BASELINE CPU relief, zero downside: slices reachability/gather over
	//         frames so a GC pass doesn't stall the game thread into a hitch (helps exactly our
	//         CPU-bound case). GC only reclaims dead UObjects — no gameplay/sim effect. ---
	// Config-gated (review finding: these are marked experimental in this engine version — a kill-switch
	// keeps them bisectable if a GC-related hitch/crash ever needs isolating).
	if (bIncrementalGC)
	{
		SetI(TEXT("gc.AllowIncrementalReachability"), 1, TEXT("incremental GC reachability"));
		SetI(TEXT("gc.AllowIncrementalGather"), 1, TEXT("incremental GC gather"));
	}

	// --- 13. Native DYNAMIC RESOLUTION (v0.9.6): replaces the old manual r.ScreenPercentage controller
	//         (which glitched TSR -> geometry flashing). Vendor is known now (block 1), so the floor is
	//         vendor-correct. Runs last so nothing after it clobbers the dyn-res cvars. ---
	ApplyDynamicResolution();

	UE_LOG(LogWNLPackFix, Display,
		TEXT("[WNLPackFix] governor: post-settle graphics pass complete (vendor=%s)"), VendorName(Vendor));
}
