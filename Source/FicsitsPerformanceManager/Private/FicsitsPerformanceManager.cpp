// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "FicsitsPerformanceManager.h"

#include "Core/FPMFixContract.h"
#include "Core/FPMHitchMeter.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMOverlay.h"
#include "Fixes/Interop/FPMHologramNetGuard.h"
#include "Fixes/Interop/FPMInventoryInitGuard.h"
#include "Fixes/Interop/FPMNoOwnerRpcGate.h"
#include "Fixes/Interop/FPMRainOcclusionFix.h"
#include "Fixes/Interop/FPMSchematicProbe.h"
#include "Fixes/Interop/FPMStaticBaseFix.h"
#include "Fixes/Vanilla/FPMCloneSensor.h"
#include "Streaming/FPMAssetResidency.h"

#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FFicsitsPerformanceManagerModule"

DEFINE_LOG_CATEGORY(LogFicsitsPerformanceManager);

void FFicsitsPerformanceManagerModule::StartupModule()
{
	// The version is read out of the LOADED plugin descriptor rather than baked in as a literal at
	// compile time. Those two can disagree: a package that silently no-ops ships yesterday's binary
	// beside today's .uplugin, and this project has burned boots believing the source version. What
	// this line prints is what is actually running, which is the only version worth attributing a
	// measurement to.
	FString VersionName = TEXT("<no descriptor>");
	if (const TSharedPtr<IPlugin> Self = IPluginManager::Get().FindPlugin(TEXT("FicsitsPerformanceManager")))
	{
		VersionName = Self->GetDescriptor().VersionName;
	}

	constexpr const TCHAR* BuildKind = WITH_EDITOR ? TEXT("editor") : TEXT("game");

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] runtime module loaded - version %s, %s build"), *VersionName, BuildKind);

	// ARMING. There is no `if (!WITH_EDITOR)` here on purpose: the editor gate lives inside
	// FPMHookLedger::Install, so a fix's Arm() still COMPILES and RUNS in the editor while installing
	// nothing. hooking.adoc:124 warns that `#if` around the SUBSCRIBE calls hides errors until a
	// shipping build - putting the single gate at the one place every hook passes through gets the
	// protection without the blind spot.
	// Arm order IS install order, and SML calls hooks in the order they registered
	// (hooking.adoc:43). Nothing here contends for a target today, but the order is recorded by the
	// ledger so a future collision is a readable log line rather than a mystery.
	FPMFixes::Arm(FFPMStaticBaseFix::Get());
	FPMFixes::Arm(FFPMNoOwnerRpcGate::Get());
	FPMFixes::Arm(FFPMCloneSensor::Get());
	FPMFixes::Arm(FFPMRainOcclusionFix::Get());

	// The two join-crash repairs, carried from the old mod on 2026-08-08 and REBUILT rather than copied:
	// both used to prevent their crash by cancelling the work, and both cost something real for it — an
	// unrendered build preview (Ant saw it) and a possibly-destroyed item stack. They now supply the data
	// that was missing and let vanilla run.
	FPMFixes::Arm(FFPMInventoryInitGuard::Get());
	FPMFixes::Arm(FFPMHologramNetGuard::Get());

	// LOG-ONLY. Ant, on the old mod's forced-TRUE milestone override: "maybe carry it with just
	// diagnostics and we'll see what happens?" and "diagnostics are good either way so we KNOW what
	// breaks and why". It overrides nothing - the crash dumps show this is a VANILLA crash (one has
	// neither FPM nor KPrivateCodeLib on the stack), so there is nothing here for us to guard yet.
	FPMFixes::Arm(FFPMSchematicProbe::Get());

	// MEASUREMENT ONLY, AND IT INSTALLS NO HOOKS - it subscribes to two engine delegates and a ticker, so it
	// will not appear in the hook ledger below. Armed here because Ant's hitches (2026-08-09) had no
	// instrument at all: the engine's own hitch detector is compiled out of this build, which the header
	// proves from the build's preprocessor definitions rather than from reputation. It arms on the dedicated
	// server too - the 560 ms save stall is a server-side hitch and a client-only readout could never see it.
	FPMFixes::Arm(FFPMHitchMeter::Get());

	// And the thing the meter is pointed AT. Root-caused 2026-08-02, recovered unbuilt by the 2026-08-09
	// scratchpad audit: vanilla's own player-list widget blocking-loads a platform icon nobody holds a hard
	// reference to, every time it binds. It installs no hook either - it pins four vanilla textures so
	// vanilla's LoadSynchronous finds them already resident and skips its blocking branch.
	FPMFixes::Arm(FFPMAssetResidency::Get());

	// Always printed, even when empty - an empty inventory is itself a finding.
	FPMHookLedger::LogInventory();

	// The debug feed, on by default while the mod is pre-release. Ant: "i want UI to show when and what
	// the rain fix thing is doing so i can see that its working." It attaches as soon as a viewport
	// exists, which is why it can report during a loading screen at all.
	if (!IsRunningDedicatedServer())
	{
		FPMOverlay::Get().SetVisible(true);
		FPMOverlay::Post(TEXT("startup"), FString::Printf(TEXT("FPM %s loaded, %d hook(s) armed"),
			*VersionName, FPMHookLedger::Records().Num()));
	}
}

void FFicsitsPerformanceManagerModule::ShutdownModule()
{
	FPMOverlay::Get().Shutdown();
	FPMFixes::DisarmAll();
	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] runtime module unloading"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFicsitsPerformanceManagerModule, FicsitsPerformanceManager)
