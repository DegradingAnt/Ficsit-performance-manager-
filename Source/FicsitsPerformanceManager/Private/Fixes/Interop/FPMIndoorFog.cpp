// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMIndoorFog.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"
#include "Core/FPMEnclosure.h"

#include "FGWorldSettings.h"

#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/Engine.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Containers/Ticker.h"

/*
 * THE BEHAVIOUR SWITCH. Separate from FPM.Diag.IndoorFog, which only changes what is printed.
 *
 * 0 disables the push while leaving the enclosure consumer registered and the reporting live, so one
 * boot can A/B whether this is what cleared the haze, without a rebuild.
 */
static TAutoConsoleVariable<int32> CVarIndoorFogGateEnabled(
	TEXT("FPM.Fog.Gate"), 1,
	TEXT("Push the height fog's StartDistance back while the player is under a built roof. "
	     "1 = on (default), 0 = observe and report but change nothing."),
	ECVF_Default);

/*
 * UNMEASURED, ON PURPOSE — see FPMIndoorFog.h. StartDistance's own UIMax
 * (ExponentialHeightFogComponent.h) is 5000 cm, a slider hint rather than a clamp, and a factory
 * interior easily exceeds that. 20000 cm (200 m) is a starting guess wide enough to clear a large hall
 * without being unreachable for a small room; one boot standing in a real base answers this properly,
 * the same way FPM.Enclosure.StreakToFlip was tuned.
 */
static TAutoConsoleVariable<float> CVarIndoorFogStartDistance(
	TEXT("FPM.Fog.IndoorStartDistance"), 20000.f,
	TEXT("StartDistance (cm) to hold while under a built roof. UNMEASURED default - raise or lower it "
	     "live and watch FPM.Fog.Report until the haze clears without over-reaching."),
	ECVF_Default);

namespace
{
	int32 GFPMFogEnclosureToken = INDEX_NONE;
	FTSTicker::FDelegateHandle GFPMFogTicker;

	bool GFPMFogUnderRoof = false;
	bool GFPMFogHolding = false;
	float GFPMFogBaseline = 0.f;    // the game's own StartDistance, shadowed hands-off
	float GFPMFogWroteValue = 0.f;  // what we last wrote, for the read-back check

	int32 GFPMFogWrites = 0;
	int32 GFPMFogWritesHeld = 0;
	int32 GFPMFogWritesOverwritten = 0;
	bool bGFPMFogReportedOverwrite = false;

	/** AFGWorldSettings -> AExponentialHeightFog -> its component. Null at any link means nothing to gate. */
	UExponentialHeightFogComponent* ResolveFogComponent(UWorld* World)
	{
		if (World == nullptr) { return nullptr; }
		const AFGWorldSettings* WS = Cast<AFGWorldSettings>(World->GetWorldSettings());
		AExponentialHeightFog* FogActor = WS ? WS->GetExponentialHeightFog() : nullptr;
		return FogActor ? FogActor->GetComponent() : nullptr;
	}
}

FFPMIndoorFog& FFPMIndoorFog::Get()
{
	static FFPMIndoorFog Instance;
	return Instance;
}

void FFPMIndoorFog::Arm()
{
	/*
	 * Overhead, not SealedRoom: the predicate IS a roof (see the header — a natural cave ceiling should
	 * stay foggy, only a player-built one should not). Registering Overhead does not by itself force the
	 * wall band to trace; the sampler only adds that cost if some OTHER consumer also needs SealedRoom.
	 */
	GFPMFogEnclosureToken = FPMEnclosure::Register(TEXT("indoor-fog"), EFPMEnclosureNeed::Overhead);

	GFPMFogTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
			if (World == nullptr || !World->IsGameWorld()) { return true; }

			const APlayerController* PC = GEngine->GetFirstLocalPlayerController(World);
			if (PC == nullptr || PC->GetPawn() == nullptr) { return true; }

			UExponentialHeightFogComponent* Comp = ResolveFogComponent(World);
			if (Comp == nullptr) { return true; }   // no fog actor placed in this map; not a fault

			const bool bUnderRoof = FPMEnclosure::IsUnderBuiltRoof();
			const bool bEnabled = CVarIndoorFogGateEnabled.GetValueOnGameThread() != 0;
			const float IndoorStart = CVarIndoorFogStartDistance.GetValueOnGameThread();

			if (bUnderRoof != GFPMFogUnderRoof)
			{
				GFPMFogUnderRoof = bUnderRoof;
				if (FPMDiag::IsOn(FPMDiag::EChannel::IndoorFog))
				{
					UE_LOG(LogFicsitsPerformanceManager, Display,
						TEXT("[FPM] indoor fog: under built roof = %s (gate %s)"),
						bUnderRoof ? TEXT("YES") : TEXT("no"),
						bEnabled ? TEXT("on") : TEXT("OFF - observing only"));
				}
			}

			if (!GFPMFogHolding)
			{
				// Hands-off: the live value IS the game's own, so shadow it as the restore point.
				GFPMFogBaseline = Comp->StartDistance;
			}
			else
			{
				/*
				 * ★ THE READ-BACK. `FPMWeatherIndoorGate` found the game re-driving a value it had just
				 * written, on a different component family, and this fix does not get to assume that
				 * cannot happen here just because StartDistance is a plain scene-component property. The
				 * write is checked, not trusted.
				 */
				++GFPMFogWrites;
				if (FMath::IsNearlyEqual(Comp->StartDistance, GFPMFogWroteValue, 1.f))
				{
					++GFPMFogWritesHeld;
				}
				else
				{
					++GFPMFogWritesOverwritten;
					GFPMFogBaseline = Comp->StartDistance;   // something else drives it; keep the shadow honest

					if (!bGFPMFogReportedOverwrite && FPMDiag::IsOn(FPMDiag::EChannel::IndoorFog))
					{
						bGFPMFogReportedOverwrite = true;
						UE_LOG(LogFicsitsPerformanceManager, Warning,
							TEXT("[FPM] indoor fog: our StartDistance write did NOT hold - wrote %.0f, read "
							     "back %.0f. Something else drives this fog actor's StartDistance; the gate "
							     "is INERT for it. That is a finding, not a failure."),
							GFPMFogWroteValue, Comp->StartDistance);
					}
				}
			}

			const bool bWant = bUnderRoof && bEnabled;
			if (bWant)
			{
				GFPMFogWroteValue = IndoorStart;
				Comp->SetStartDistance(IndoorStart);
				GFPMFogHolding = true;
			}
			else if (GFPMFogHolding)
			{
				Comp->SetStartDistance(GFPMFogBaseline);
				GFPMFogHolding = false;
			}

			return true;
		}),
		0.25f);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] indoor fog ARMED - no hook. It asks the shared enclosure check for a built roof "
		     "overhead and pushes the map's height fog StartDistance to %.0f cm while under one. "
		     "SetStartDistance ONLY - volumetric fog is never touched. It reads its own write back, "
		     "because a property this fix does not own may be driven from elsewhere."),
		CVarIndoorFogStartDistance.GetValueOnAnyThread());
}

void FFPMIndoorFog::Disarm()
{
	if (GFPMFogTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GFPMFogTicker);
		GFPMFogTicker.Reset();
	}

	if (GFPMFogHolding)
	{
		if (UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr)
		{
			if (UExponentialHeightFogComponent* Comp = ResolveFogComponent(World))
			{
				Comp->SetStartDistance(GFPMFogBaseline);
			}
		}
		GFPMFogHolding = false;
	}

	if (GFPMFogEnclosureToken != INDEX_NONE)
	{
		FPMEnclosure::Unregister(GFPMFogEnclosureToken);
		GFPMFogEnclosureToken = INDEX_NONE;
	}
}

void FFPMIndoorFog::ReportNow()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] indoor fog: under-roof=%s * gate %s * holding=%s * baseline %.0f cm * indoor target "
		     "%.0f cm"),
		GFPMFogUnderRoof ? TEXT("YES") : TEXT("no"),
		CVarIndoorFogGateEnabled.GetValueOnGameThread() != 0 ? TEXT("on") : TEXT("OFF"),
		GFPMFogHolding ? TEXT("yes") : TEXT("no"),
		GFPMFogBaseline, CVarIndoorFogStartDistance.GetValueOnGameThread());

	if (GFPMFogWrites == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   no writes checked yet - the player has not been under a built roof with the "
			     "gate on, or no fog actor exists in this map."));
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   %d write(s) checked: %d HELD, %d overwritten by something else (%.0f%% held)."),
		GFPMFogWrites, GFPMFogWritesHeld, GFPMFogWritesOverwritten,
		100.0 * GFPMFogWritesHeld / FMath::Max(1, GFPMFogWrites));

	UE_CLOG(GFPMFogWritesOverwritten > 0 && GFPMFogWritesHeld == 0, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM]   NOT ONE write held. This gate is inert - something drives StartDistance every tick "
		     "from elsewhere. Do not report the haze as fixed; find the real lever."));
}

static FAutoConsoleCommandWithOutputDevice GFPMIndoorFogReportCmd(
	TEXT("FPM.Fog.Report"),
	TEXT("Indoor fog gate: under-roof state, StartDistance baseline/target, and whether the write held."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMIndoorFog::ReportNow();
	}));
