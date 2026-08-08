// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "FicsitsPerformanceManager.h"

#include "Core/FPMFixContract.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMOverlay.h"
#include "Fixes/Interop/FPMHologramNetGuard.h"
#include "Fixes/Interop/FPMInventoryInitGuard.h"
#include "Fixes/Interop/FPMNoOwnerRpcGate.h"
#include "Fixes/Interop/FPMRainOcclusionFix.h"
#include "Fixes/Interop/FPMStaticBaseFix.h"
#include "Fixes/Vanilla/FPMCloneSensor.h"

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
