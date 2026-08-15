// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * INDOOR FOG — §9.7, the fog third of "one system, not three patches".
 *
 * The map's `AExponentialHeightFog` is a single global actor: it has no idea a player just walked into a
 * sealed room. Its haze keeps building with distance exactly as authored outdoors, so a large interior
 * reads as foggy inside a building that has a roof and four walls — which is not what the atmosphere is
 * meant to represent. m5637364 and m6341168 name this.
 *
 * ★ SetStartDistance ONLY, NEVER VOLUMETRIC — §9.7's own words, restated as the one thing this file may
 * never grow into. Pushing `UExponentialHeightFogComponent::StartDistance` further from the camera while
 * under a roof is a one-line, fully-reversible nudge to WHERE the existing fog begins; it touches no other
 * property on the component. `bEnableVolumetricFog`, the scattering distribution, the emissive and every
 * other VolumetricFog field are never read or written here — those govern a separate simulated volume this
 * fix has no business touching, and the project's standing graphics rule is MATCH REALITY: no fake
 * ambience, and no frame generation, ever. This is a subtraction (less haze somewhere it should not be),
 * never an addition of an effect that was not already there.
 *
 * ★ REAL GODRAYS SURVIVE THIS UNCHANGED. Light shafts through a window come from the directional light and
 * the sky atmosphere, not from `UExponentialHeightFogComponent::StartDistance`. Pushing the height-fog
 * start distance back does not touch either, so a window still lets a real beam through — this only stops
 * the room itself reading as hazy.
 *
 * ★ PLAYER BUILDABLES ONLY, PER THE SHARED SENSOR. Registered as `EFPMEnclosureNeed::Overhead`, the same
 * predicate the visor-rain half of §9.7 is reserved for and the one `FPMEnclosure.h` names as counting
 * player buildables and ignoring natural terrain — a cave ceiling stays foggy on purpose, only a roof the
 * player built pushes the fog back. `EFPMEnclosureNeed::Overhead` and `FPMEnclosure::IsUnderBuiltRoof()`
 * were dead code with zero callers before this file; the enclosure header names them as this mechanism's
 * reserved consumers, and this fix is the receipt.
 *
 * ★ THE SERVER REFUSAL IS EXPLICIT. `Side()` is `NeverOnDedicatedServer`, which routes through
 * `FPMFixContract.cpp`'s own `IsRunningDedicatedServer()` check before `Arm()` ever runs — height fog is
 * a rendering-only actor a dedicated server never draws. `FPMEnclosure::TickInternal` also now checks
 * `IsRunningDedicatedServer()` directly rather than relying on a null local player controller to no-op
 * incidentally, per the same §9.7 ruling.
 *
 * ★ ORIGIN STATUS IS `Guard`, NOT `ChokePointRepair`. There is no single misbehaving line to name: global
 * exponential height fog not knowing about interior volumes is how the engine's own fog model works, not
 * a defect in it. This guards against that model's side effect indoors; it does not claim to have found
 * and fixed a cause inside CSS's code.
 *
 * ★ THE VALUE IS UNMEASURED, ON PURPOSE. `FPM.Fog.IndoorStartDistance` is exposed as a cvar rather than a
 * literal so one boot standing inside a real base can find the distance that clears the haze without
 * pushing so far that a genuinely large interior looks unnaturally clean — the same "expose it, don't
 * guess it" discipline `FPM.Enclosure.StreakToFlip` already used for the sealed-room damping.
 *
 * ★ THE LEVER IS ORDERING, NOT FORCE, AND THAT IS THE WHOLE OF THIS FIX.
 *
 * `StartDistance` has two writers. Ant reported the consequence directly: *"lights flicker badly"*,
 * *"only inside, outside does not pulse"*, gone the moment she typed `FPM.Fog.Gate 0`. The first version
 * of this file wrote from a 4 Hz `FTSTicker` with no defined position relative to the game's own writer,
 * so the live value alternated between 20000 cm and the map's own 8.7 cm several times a second, and that
 * alternation IS the pulse. It then read its write back and self-disarmed after three losses, which
 * stopped the harm and left the feature doing nothing.
 *
 * This version stops competing for the property and takes a position instead. It hooks
 * `UFGAtmosphereUpdater::Tick` (`FGAtmosphereUpdater.h:47`, public) with `FPM_SUBSCRIBE_AFTER`, and SML
 * runs an AFTER handler only once the real function has returned: `NativeHookManager.h:442-451`,
 * `ApplyCallVoid` calls the trampoline first and iterates `HandlersAfter` second. So FPM writes last in
 * the frame, every frame, on the game thread, and the renderer reads our value at the end-of-frame flush.
 * There is no contest left to lose, so the read-back, the lost-write counter and the whole self-disarm
 * latch are gone rather than kept as scaffolding. Removing them was not a quiet feature deletion: under
 * the new ordering the old detector would have seen the game's value at the top of every frame, called it
 * a lost write, and disarmed the fix on frame four of every session.
 *
 * ★ WHAT IS PROVEN, WHAT IS INFERRED, AND WHAT THE REPORT MEASURES.
 *
 * PROVEN, from Ant's own session: our `SetStartDistance` write does reach the renderer, and something
 * re-drives the property between our writes. Both follow from one observation, that the pulse appeared
 * with the gate on and stopped with `FPM.Fog.Gate 0`.
 *
 * SHIPPED HEADERS, so also fact: the game holds a `UFGAtmosphereUpdater` on `FFGEngineCommon`
 * (`FGEngineCommon.h:163`, under CSS's comment *"Height fog properties that's controlled from camera"*),
 * ticks it from `FFGEngineCommon::Tick(float)` (`FGEngineCommon.h:115`), and `UFGGameEngine` holds that
 * struct by value (`FGGameEngine.h:28`). The updater's protected surface is `UpdateWorld` ->
 * `InterpolateFogSettings` -> `ApplyFogSettings(const FExponentialFogSettings&, UWorld*)`
 * (`FGAtmosphereUpdater.h:100-113`), and `FExponentialFogSettings::StartDistance`
 * (`FGAtmosphereVolume.h:81`) is blended from `AFGAtmosphereVolume::mStartDistance`, a time-of-day curve
 * (`FGAtmosphereVolume.h:266`), or from `AFGWorldSettings::mDefaultHeightFogSettings`
 * (`FGWorldSettings.h:159`). The map ships `ExternalCurves/Curves/Main_Middle_StartDistance`, every key
 * holding 8.710419, which the first report's `%.0f` printed as the 9 in her log.
 *
 * ⚠ STILL INFERRED: that `ApplyFogSettings` is the exact line reaching `SetStartDistance` on this
 * component. The bodies are not in the header drop and nobody has stepped it. THIS FIX NO LONGER DEPENDS
 * ON THAT. It needs only that `UFGAtmosphereUpdater::Tick` is the last game-side writer in the frame, and
 * `FPM.Fog.Report` measures exactly that rather than assuming it: the re-drive count is the number of
 * frames the game moved `StartDistance` off our value. A climbing count says the game owns the property
 * and writing last is what makes the push hold. A ZERO count across many held frames says the game never
 * touched it, which contradicts the reason this file hooks anything at all, and the report says so in a
 * Warning instead of leaving the reader to notice.
 *
 * ★ THE BETTER LEVER, NAMED RATHER THAN QUIETLY SKIPPED. `ApplyFogSettings` takes the settings struct by
 * const reference before the engine setter's own `if (Name != NewValue)` test, so overriding
 * `StartDistance` there would cost ZERO render-state churn instead of the one recreate per frame this
 * file accepts below. It is `protected` (`FGAtmosphereUpdater.h:100`), so reaching it needs one
 * `friend` line in `Config/AccessTransformers.ini` — a FactoryGame class, which that file's own rules
 * allow. That line was out of this change's file scope and is a real follow-up, not a rejected idea.
 * Two other candidates are closed rather than pending: `AFGAtmosphereVolume::GetSettings` is overloaded
 * (`FGAtmosphereVolume.h:189` and `:190`) so `decltype(&...)` is ambiguous and the hook ledger has no
 * `_EXPLICIT` variant, and `FBiomeHelpers::GetExponentialFogSettings` (`Atmosphere/BiomeHelpers.h:76`)
 * sits on a struct with no `FACTORYGAME_API`, so the symbol is not exported to a mod at all.
 *
 * ★ THE COST IS ONE FOG RENDER-STATE RECREATE PER HELD FRAME, AND THE REPORT PRINTS IT.
 * `UExponentialHeightFogComponent::SetStartDistance` is generated by one macro
 * (`ExponentialHeightFogComponent.cpp:231-240`): assign only when the value differs, then
 * `MarkRenderStateDirty()`. Without FPM the game rewrites the same 8.710419 each frame, the test rejects
 * it, and the component is dirtied zero times. While FPM holds, the game's value and ours differ, so the
 * component is dirtied once per frame; repeats WITHIN a frame collapse
 * (`ActorComponent.cpp:2550-2564`, guarded on `!bRenderStateDirty`), repeats across frames do not. In a
 * performance mod that cost is not something to leave implicit, so the written-frame count in
 * `FPM.Fog.Report` IS the count of recreates this fix caused. Frames where the property already holds our
 * number are counted separately and cost nothing.
 *
 * ★ NOTHING RUNS IN AN EDITOR BUILD, AND THAT IS A REGRESSION THIS FILE OWNS. `FPMHookLedger::Install` is
 * `if constexpr (WITH_EDITOR)` -> record REFUSED, return an empty handle (`FPMHookLedger.cpp:60-70`), so
 * the push cannot work in PIE the way the old ticker did. `Arm()` prints INERT rather than ARMED in that
 * case instead of claiming a hook it does not have. Verifying this fix now costs a packaged boot, the same
 * as every other hook-based fix in this mod.
 *
 * ZERO RESIDUE, and it is simpler than it used to be. The restore point is re-read from the game EVERY
 * frame, at the one instant it is provably uncontested: immediately after the updater ran and before FPM
 * writes. A sample equal to what FPM last wrote is recognised as ours and does not become the restore
 * point, so the value held for restoring can only ever be one the game chose. It is put back the moment
 * the player leaves the roof, the gate goes to 0, the pawn goes away, or `Disarm()` runs, and a hold is
 * dropped without being carried into a different world. Even if all of that failed, the game's next tick
 * writes its own value over ours. Nothing is persisted; `UExponentialHeightFogComponent::StartDistance`
 * is a transient render property, not SaveGame state.
 */
class FFPMIndoorFog final : public IFPMFix
{
public:
	static FFPMIndoorFog& Get();

	virtual const TCHAR* Name() const override { return TEXT("indoor-fog"); }

	/** Height fog is a rendering-only actor. A dedicated server draws none. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/** See the header: global height fog not knowing about interiors is the engine's own model, not a
	 *  named defect in it. This guards against the side effect; it does not claim a fixed cause. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::IndoorFog; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** `FPM.Fog.Report`: under-roof state, the game's own StartDistance, held frames split into written
	 *  and already-at-target, and the re-drive count. NONE of it is behind `FPM.Diag.IndoorFog`: the
	 *  operator asked for this report by name, and the re-drive count in particular is the one line that
	 *  answers whether this fix is buying anything, so it may not be the line a diagnostics level hides. */
	static void ReportNow();
};
