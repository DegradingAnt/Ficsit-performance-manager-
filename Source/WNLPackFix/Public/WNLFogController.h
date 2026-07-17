#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class UExponentialHeightFogComponent;

/**
 * WNL indoor-fog controller — client-only.
 *
 * Goal (Ant): the flat blue distance-HAZE that hangs inside player factories should clear as you
 * walk in, while god-rays and outdoor atmosphere stay exactly as they are. "Match reality."
 *
 * Lever = ExponentialHeightFogComponent::StartDistance. StartDistance is a camera-relative radius
 * inside which HEIGHT fog is not applied — so a large bubble around the player reads as fog-free
 * indoors, while distant outdoor fog is still visible through doors/windows. Crucially it affects
 * HEIGHT fog ONLY, never VOLUMETRIC fog (engine header, ExponentialHeightFogComponent line ~124).
 * Volumetric fog is what draws god-rays / light shafts, and we never touch it — so god-rays keep
 * streaming through windows and holes even in a sealed room. That single design choice (touch
 * height fog, never volumetric) is what satisfies "god-rays should still go through windows",
 * independent of the enclosure math below.
 *
 * Enclosure (0 = open sky, 1 = fully sealed by player buildables) drives HOW MUCH we act, so a
 * hole-riddled shell keeps most of its haze and only a truly sealed interior goes clear — and the
 * fade is long, so it reads as "the fog clears as you enter" rather than a hard cut.
 *
 * ADAPTIVE bubble (Ant): the fog-free radius is sized to the ACTUAL room — the distance to the
 * nearest sealing wall — not a fixed 120 m. A fixed bubble would clear exterior fog for 120 m in
 * every direction, so a small room's doorway would show a wrong (fog-free) view outside. Sizing the
 * bubble to the nearest wall makes height-fog resume right at the shell, so the view out a door/
 * window keeps its outdoor fog. IndoorStartDistance is therefore now a CAP, not a fixed target.
 *
 * Only AFGBuildable hits count as sealing → natural cave/terrain ceilings keep their fog. GLASS
 * counts as sealing too (a sealed greenhouse has still, clear interior air) — and because god-rays
 * ride volumetric fog, they still pour through the glass regardless. StartDistance is not something
 * the day/night atmosphere updater animates, so no density change-detection dance is needed; we
 * just re-adopt the game's live StartDistance whenever we're hands-off.
 *
 * All knobs live in FactoryGame/Configs/WNLPackFix.cfg under a "Fog" section.
 */
class FWNLFogController
{
public:
	static FWNLFogController& Get();

	/** Idempotent; no-op on dedicated servers (no renderer/fog to touch). */
	void Start();
	void Stop();

private:
	/** Result of the enclosure probe: how sealed, and how big the interior is. */
	struct FEnclosure
	{
		float Fraction  = 0.f;   // 0 = open sky, 1 = fully sealed by buildables (holes leak → partial)
		float NearSeal  = -1.f;  // cm to the nearest sealing wall/roof (-1 = none seen → open)
		int32 SealCount = 0;     // how many of the sample rays hit a buildable
	};

	bool Tick(float DeltaTime);
	void LoadConfig();
	/** Sky-biased line-trace probe. Only AFGBuildable hits count → caves stay foggy. Returns the
	 *  sealed fraction AND the nearest-wall distance so the bubble can be sized to the room. Traces
	 *  from the CAMERA (StartDistance is camera-relative) so the measured radius matches the lever. */
	FEnclosure ComputeEnclosure(class UWorld* World);
	UExponentialHeightFogComponent* FindFog(class UWorld* World);

	FTSTicker::FDelegateHandle TickHandle;

	// --- config (defaults; overridden by WNLPackFix.cfg "Fog") ---
	bool  bEnabled            = true;
	float IndoorStartDistance = 12000.f; // CAP on the fog-free bubble (cm); the bubble is usually the nearest wall
	float TransitionSec       = 4.0f;    // long fade both directions
	float RoofTraceUp         = 4000.f;  // cm to look up for a roof (40 m) — min trace length
	float CheckInterval       = 0.25f;   // enclosure re-check cadence (fade runs every tick)
	float MinBubble           = 200.f;   // cm floor so a tiny closet never sets a ~0 bubble
	float WallBias            = 0.9f;    // bubble = nearest wall * this, so fog resumes just INSIDE the shell
	float GrowLerp            = 0.2f;    // slow grow — stops the bubble pulsing bigger on a lucky far reading
	float ShrinkLerp          = 0.6f;    // fast shrink — collapse instantly when a doorway/near wall appears
	int32 SealCountMin        = 3;       // fewer seals than this → treat as open, game owns the fog

	// --- runtime state ---
	TWeakObjectPtr<UExponentialHeightFogComponent> FogComp;
	float CurrentStart  = 0.f;   // applied StartDistance right now
	float TargetStart   = 0.f;   // where the fade is heading (baseline..sized bubble)
	float SmoothedNear  = -1.f;  // smoothed nearest-wall estimate the bubble tracks (-1 = uncaptured)
	float BaselineStart = -1.f;  // the game's own StartDistance, re-adopted while hands-off (-1 = uncaptured)
	bool  bControlling  = false; // are we currently overriding StartDistance?
	double LastCheck    = 0.0;
	bool  bStarted      = false;
};
