// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMWeatherIndoorGate.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMEnclosure.h"
#include "Core/FPMOverlay.h"

#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

/*
 * THE BEHAVIOUR SWITCH. Separate from FPM.Diag.WeatherGate, which only changes what is printed.
 *
 * 0 disables the suppression while leaving the enclosure consumer registered and the reporting live —
 * so one boot can A/B whether this is what stopped the dust, without a rebuild.
 */
static TAutoConsoleVariable<int32> CVarWeatherGateEnabled(
	TEXT("FPM.Weather.Gate"), 1,
	TEXT("Scale weather particle intensity down while the player is in a sealed room. "
	     "1 = on (default), 0 = observe and report but change nothing."),
	ECVF_Default);

/*
 * How far down, not off. Deactivating would make weather vanish the instant a wall is detected, which
 * reads as a bug rather than as shelter. A small residue also keeps the system alive so releasing it is
 * a fade rather than a pop.
 */
static TAutoConsoleVariable<float> CVarWeatherGateIndoorScale(
	TEXT("FPM.Weather.IndoorScale"), 0.05f,
	TEXT("Fraction of the game's own weather intensity to leave running while sealed indoors. "
	     "0 would be a hard cut and reads as a bug; the default keeps a trace alive so release fades."),
	ECVF_Default);

namespace
{
	/** The user parameters the weather systems actually expose, read from the game export 2026-08-10. */
	const FName GFPMWeatherParams[] = { FName(TEXT("User.WindIntensity")), FName(TEXT("User.RainAlpha")) };

	/** One tracked component and the value the GAME wants it to have. */
	struct FFPMWeatherTarget
	{
		TWeakObjectPtr<UNiagaraComponent> Comp;
		FName Param;
		float GameValue = 0.f;   // the last value observed while hands-off — what to restore
		float WroteValue = 0.f;  // what we last wrote, so a read-back can be compared against it
		bool bHolding = false;
	};

	TArray<FFPMWeatherTarget> GFPMWeatherTargets;
	int32 GFPMWeatherEnclosureToken = INDEX_NONE;
	FTSTicker::FDelegateHandle GFPMWeatherTicker;

	bool GFPMWeatherSealed = false;
	double GFPMWeatherLastScan = 0.0;
	int32 GFPMWeatherWrites = 0;
	int32 GFPMWeatherWritesHeld = 0;
	int32 GFPMWeatherWritesOverwritten = 0;
	bool bGFPMWeatherReportedOverwrite = false;

	/** Rescan cadence. Weather components are spawned once and live; this is a safety net, not a poll. */
	constexpr double GFPMWeatherScanSec = 15.0;

	/**
	 * Read a User float back off a component.
	 *
	 * ⚠ `UNiagaraComponent::GetVariableFloat` DOES NOT EXIST in this engine build. `ue-niagara-effects`
	 * lists it, and the compiler disagreed — a skill's API list is not an oracle for a specific engine,
	 * the same shape as engine source not being an oracle for the retail binary. The real route is the
	 * public override store (`NiagaraComponent.h:664`) plus `RedirectUserVariable`
	 * (`NiagaraUserRedirectionParameterStore.h:29`), which resolves the `User.` prefix.
	 *
	 * ⚠ AND KNOW WHAT THIS CAN AND CANNOT SEE. The override store holds values SET on the component —
	 * by us or by a Blueprint calling the same setter. It does NOT reflect a value the system computes
	 * internally or receives through a binding. So a read-back that disagrees with what we wrote is
	 * proof somebody re-pushed it; a read-back that agrees is NOT proof the effect changed on screen.
	 * The report says so rather than overclaiming.
	 *
	 * @return the sentinel when the parameter is absent. Intensity and alpha are non-negative, so a
	 *         negative sentinel cannot collide with a real value.
	 */
	constexpr float GFPMWeatherAbsent = -1.f;

	float ReadUserFloat(UNiagaraComponent* Comp, const FName& Param)
	{
		if (Comp == nullptr) { return GFPMWeatherAbsent; }
		FNiagaraVariableBase Var(FNiagaraTypeDefinition::GetFloatDef(), Param);
		FNiagaraUserRedirectionParameterStore& Store = Comp->GetOverrideParameters();
		Store.RedirectUserVariable(Var);
		return Store.GetParameterValueOrDefault<float>(Var, GFPMWeatherAbsent);
	}

	bool IsWeatherSystem(const UNiagaraComponent* Comp)
	{
		const UNiagaraSystem* Sys = Comp ? Comp->GetAsset() : nullptr;
		if (Sys == nullptr) { return false; }

		/*
		 * Matched by ASSET NAME, and that is not laziness. Nothing in FactoryGame's public headers
		 * references NS_Wind at all — the weather systems are spawned from Blueprint, so there is no
		 * class or component type to key on. The names come from the game export rather than memory:
		 * NS_Rain, NS_Wind, NS_Thunder, NS_Desert_Dune_Wind, NS_Forest_Field_Wind, NS_Forest_Red_Wind.
		 *
		 * NS_Thunder is deliberately not matched — lightning is not weather that a roof should stop.
		 */
		const FString Name = Sys->GetName();
		return Name.StartsWith(TEXT("NS_Rain")) || Name.Contains(TEXT("Wind"));
	}

	void Rescan(UWorld* World)
	{
		GFPMWeatherTargets.Reset();
		if (World == nullptr) { return; }

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UNiagaraComponent*> Comps;
			It->GetComponents<UNiagaraComponent>(Comps);
			for (UNiagaraComponent* Comp : Comps)
			{
				if (!IsWeatherSystem(Comp)) { continue; }

				for (const FName& Param : GFPMWeatherParams)
				{
					const float Current = ReadUserFloat(Comp, Param);
					if (Current == GFPMWeatherAbsent) { continue; }   // this system does not expose it

					FFPMWeatherTarget T;
					T.Comp = Comp;
					T.Param = Param;
					T.GameValue = Current;
					GFPMWeatherTargets.Add(T);
				}
			}
		}

		if (FPMDiag::IsOn(FPMDiag::EChannel::WeatherGate))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] weather gate: tracking %d weather parameter(s) across the world's Niagara "
				     "components."), GFPMWeatherTargets.Num());
		}
	}

	void Release()
	{
		for (FFPMWeatherTarget& T : GFPMWeatherTargets)
		{
			if (!T.bHolding) { continue; }
			if (UNiagaraComponent* C = T.Comp.Get())
			{
				C->SetVariableFloat(T.Param, T.GameValue);
			}
			T.bHolding = false;
		}
	}
}

FFPMWeatherIndoorGate& FFPMWeatherIndoorGate::Get()
{
	static FFPMWeatherIndoorGate Instance;
	return Instance;
}

void FFPMWeatherIndoorGate::Arm()
{
	/*
	 * Registering is what STARTS the shared enclosure sampler. Nothing traces until something asks, so
	 * this line is the difference between the probe costing nothing and costing a ray batch.
	 *
	 * SealedRoom, not Overhead: a canopy overhead should not stop dust, a closed room should. That
	 * choice also makes the sampler trace the wall band, which a roof-only consumer would skip.
	 */
	GFPMWeatherEnclosureToken = FPMEnclosure::Register(TEXT("weather-indoor-gate"), EFPMEnclosureNeed::SealedRoom);

	GFPMWeatherTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
			if (World == nullptr || !World->IsGameWorld()) { return true; }

			const double Now = FPlatformTime::Seconds();
			if (GFPMWeatherTargets.Num() == 0 || Now - GFPMWeatherLastScan > GFPMWeatherScanSec)
			{
				GFPMWeatherLastScan = Now;
				Rescan(World);
			}

			const bool bSealed = FPMEnclosure::IsInSealedRoom();
			const bool bEnabled = CVarWeatherGateEnabled.GetValueOnGameThread() != 0;
			const float Scale = FMath::Clamp(CVarWeatherGateIndoorScale.GetValueOnGameThread(), 0.f, 1.f);

			if (bSealed != GFPMWeatherSealed)
			{
				GFPMWeatherSealed = bSealed;
				if (FPMDiag::IsOn(FPMDiag::EChannel::WeatherGate))
				{
					UE_LOG(LogFicsitsPerformanceManager, Display,
						TEXT("[FPM] weather gate: sealed room = %s (%d target(s), gate %s)"),
						bSealed ? TEXT("YES") : TEXT("no"), GFPMWeatherTargets.Num(),
						bEnabled ? TEXT("on") : TEXT("OFF - observing only"));
				}
			}

			for (FFPMWeatherTarget& T : GFPMWeatherTargets)
			{
				UNiagaraComponent* C = T.Comp.Get();
				if (C == nullptr) { continue; }

				const float Live = ReadUserFloat(C, T.Param);
				if (Live == GFPMWeatherAbsent) { continue; }

				if (!T.bHolding)
				{
					/*
					 * Hands-off: the live value IS the game's answer, so shadow it. While HOLDING we must
					 * not read it back as the baseline — that would latch our own write and ratchet the
					 * value toward zero, which is the same trap the governor's vanilla-defaults rule
					 * exists to prevent.
					 */
					T.GameValue = Live;
				}
				else
				{
					/*
					 * ★ THE READ-BACK, AND IT IS WHY THIS FIX IS NOT A DEAD INSTRUMENT.
					 *
					 * Whether writing a Niagara user parameter sticks is not knowable from the headers:
					 * these systems are spawned from Blueprint and a graph that re-pushes the value every
					 * tick would silently overwrite us. So compare what is there now against what we
					 * wrote. If they disagree, the gate is inert and says so instead of taking credit.
					 */
					++GFPMWeatherWrites;
					if (FMath::IsNearlyEqual(Live, T.WroteValue, 0.001f))
					{
						++GFPMWeatherWritesHeld;
					}
					else
					{
						++GFPMWeatherWritesOverwritten;
						T.GameValue = Live;   // the game is driving it; keep the shadow honest

						if (!bGFPMWeatherReportedOverwrite && FPMDiag::IsOn(FPMDiag::EChannel::WeatherGate))
						{
							bGFPMWeatherReportedOverwrite = true;
							UE_LOG(LogFicsitsPerformanceManager, Warning,
								TEXT("[FPM] weather gate: our write to %s did NOT hold - wrote %.3f, read "
								     "back %.3f. Something re-pushes this parameter every tick, so this "
								     "gate is INERT for it. That is a finding, not a failure: the lever is "
								     "wrong, and suppression needs a different one."),
								*T.Param.ToString(), T.WroteValue, Live);
						}
					}
				}

				const bool bWant = bSealed && bEnabled;
				if (bWant)
				{
					T.WroteValue = T.GameValue * Scale;
					C->SetVariableFloat(T.Param, T.WroteValue);
					T.bHolding = true;
				}
				else if (T.bHolding)
				{
					C->SetVariableFloat(T.Param, T.GameValue);
					T.bHolding = false;
				}
			}

			return true;
		}),
		0.25f);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] weather indoor gate ARMED - no hook. It asks the shared enclosure check for a SEALED "
		     "ROOM and scales weather particle intensity toward %.0f%% while inside. It is NOT collision: "
		     "NS_Wind, NS_Forest_Field_Wind and NS_Forest_Red_Wind ship with no collision module at all, "
		     "and no runtime call can add one. It READS ITS OWN WRITE BACK, because these systems are "
		     "spawned from Blueprint and a graph that re-pushes the value would silently overwrite us."),
		CVarWeatherGateIndoorScale.GetValueOnAnyThread() * 100.f);
}

void FFPMWeatherIndoorGate::Disarm()
{
	Release();

	if (GFPMWeatherTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GFPMWeatherTicker);
		GFPMWeatherTicker.Reset();
	}
	if (GFPMWeatherEnclosureToken != INDEX_NONE)
	{
		FPMEnclosure::Unregister(GFPMWeatherEnclosureToken);
		GFPMWeatherEnclosureToken = INDEX_NONE;
	}
	GFPMWeatherTargets.Reset();
}

void FFPMWeatherIndoorGate::ReportNow()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] weather gate: %d target parameter(s) · sealed=%s · gate %s · indoor scale %.2f"),
		GFPMWeatherTargets.Num(), GFPMWeatherSealed ? TEXT("YES") : TEXT("no"),
		CVarWeatherGateEnabled.GetValueOnGameThread() != 0 ? TEXT("on") : TEXT("OFF"),
		CVarWeatherGateIndoorScale.GetValueOnGameThread());

	if (GFPMWeatherWrites == 0)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   no writes yet. Either the player has not been in a sealed room, or nothing was "
			     "found to write to. %d target(s) tracked."), GFPMWeatherTargets.Num());
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   %d write(s) checked: %d HELD, %d overwritten by the game (%.0f%% held)."),
		GFPMWeatherWrites, GFPMWeatherWritesHeld, GFPMWeatherWritesOverwritten, 100.0 * GFPMWeatherWritesHeld / FMath::Max(1, GFPMWeatherWrites));

	UE_CLOG(GFPMWeatherWritesOverwritten > 0 && GFPMWeatherWritesHeld == 0, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM]   NOT ONE write held. This gate is inert - the parameter is driven every tick from "
		     "elsewhere. Do not report the dust as fixed; find the real lever."));
}

static FAutoConsoleCommand GFPMWeatherReportCmd(
	TEXT("FPM.Weather.Report"),
	TEXT("Weather indoor gate: targets found, sealed state, and whether our writes actually held."),
	FConsoleCommandDelegate::CreateStatic(&FFPMWeatherIndoorGate::ReportNow));
