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
 * ZERO RESIDUE: the game's own `StartDistance` is read once, hands-off, before this ever writes it, and
 * restored the moment the player is no longer under a built roof or the gate is disarmed. Nothing is
 * persisted; `UExponentialHeightFogComponent::StartDistance` is a transient render property, not SaveGame
 * state.
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

	/** `FPM.Fog.Report` — under-roof state, the baseline held, and whether the write is holding. */
	static void ReportNow();
};
