// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * WEATHER STOPS AT THE WALL — the cheap half of the particle work, and it is honest about being that.
 *
 * Ant, 2026-08-10: *"I want the dirt particle stuffs to be included in the fog stuff so stuff stops
 * going through the walls when inside."*
 *
 * ★ WHAT THIS IS NOT. It is **not collision.** It cannot give her the case she actually described —
 * *"looking through a window the player should be able to see the dirt particles collide with the
 * window"* — because from inside a sealed room this leaves no particles outside the glass to watch.
 * That needs Tier 3, which means authoring replacement Niagara systems in the editor. This is the
 * floor: it stops dust appearing INSIDE a closed room, it costs almost nothing, and it works on the
 * three systems that have no collision module at all.
 *
 * ⚠ THE THREE THAT HAVE NONE ARE THE POINT. Counted from the game export 2026-08-10: `NS_Rain` and
 * `NS_Desert_Dune_Wind` carry a `NiagaraDataInterfaceCollisionQuery`; `NS_Wind`,
 * `NS_Forest_Field_Wind` and `NS_Forest_Red_Wind` carry **none**. CSS authored collision for one biome
 * and not the others. No console variable and no user parameter can add a module that was never
 * compiled into the emitter script, so for those three there is no fix cheaper than this one.
 *
 * ★ HOW IT DECIDES. It asks `FPMEnclosure`, the one shared indoor check (Ant: *"one check for all
 * 'inside' stuff"*), registered as a `SealedRoom` consumer — walls, not merely a roof. A canopy
 * overhead should not stop dust; a closed room should.
 *
 * ★ HOW IT ACTS, AND WHY IT READS THE WRITE BACK.
 *
 * The weather systems expose `User.WindIntensity` and `User.RainAlpha`. Whether writing those actually
 * sticks is **not knowable from here**: nothing in FactoryGame's public headers references `NS_Wind`,
 * so the systems are spawned from Blueprint, and a Blueprint that re-pushes the value every tick would
 * simply overwrite us. That is a real possibility and this fix refuses to assume either way.
 *
 * So it writes, then reads the parameter back on a later tick and reports whether it held. A gate that
 * cannot tell "I suppressed the dust" from "my write was overwritten" is the dead-instrument shape this
 * project has paid for five times. `FPM.Weather.Report` prints the answer.
 *
 * ⚠ AND IT SCALES RATHER THAN KILLS. Deactivating the emitter would make weather vanish the instant a
 * roof is detected, which reads as a bug rather than as shelter. The value is scaled toward zero and
 * the system's own fade does the rest.
 *
 * ZERO RESIDUE: it remembers the game's own value and writes it back on release and on `Disarm()`.
 * Nothing is persisted; these are transient component parameters.
 */
class FFPMWeatherIndoorGate final : public IFPMFix
{
public:
	static FFPMWeatherIndoorGate& Get();

	virtual const TCHAR* Name() const override { return TEXT("weather-indoor-gate"); }

	/** Weather particles are cosmetic and client-side. A dedicated server draws none. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/**
	 * ChokePointRepair. The cause IS named — three vanilla systems ship without a collision module —
	 * but this does not repair that. It suppresses the symptom at the nearest reachable point while
	 * Tier 3 goes after the cause. Calling it `OriginNamed` would claim the fix that Tier 3 has not
	 * shipped yet.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::ChokePointRepair; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::WeatherGate; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** `FPM.Weather.Report` — what it found, what it wrote, and whether the write held. */
	static void ReportNow();
};
