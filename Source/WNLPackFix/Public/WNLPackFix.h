#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogWNLPackFix, Log, All);

/** Set true once the contact-shadow suppression sweep (belt items + foliage) is armed. The perf
 *  governor refuses to force-enable global contact shadows until this holds — the sweep is what
 *  makes them shimmer-safe. Defined in WNLPackFix.cpp, read in WNLPerfGovernor.cpp. */
extern bool GWNLContactShadowSweepArmed;

class FWNLPackFixModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;

private:
	/** Fix 1: silence+skip the Stats mod's no-owner sign RPC dispatches (client flood → desync feel). */
	void RegisterStatsSignRpcGate();

	/** Fix 3: buildable instancing proxies are immobile — report them as static movement bases so
	 *  server corrections go world-space and stop being ignored by clients (rubber-band fix). */
	void RegisterStaticBaseFix();

	/** Fix 4: synthesize missing rain-occlusion boxes on buildable classes from their real
	 *  mesh geometry (rain stops leaking through un-authored pieces); geometry-less helper
	 *  pieces are opted out of the occlusion system instead. Kills the LogRainSystem spam
	 *  by fixing the data rather than muting the log. */
	void RegisterRainOcclusionFix();

	/** v0.8/v0.9 (client-only): stop the two mover classes that make screen-space contact shadows
	 *  shimmer — moving belt-item meshes (fast screen-space translation → rejected temporal history)
	 *  and FOLIAGE (alpha-masked canopies → crawling speckle) — from casting them. Static geometry
	 *  keeps its grounding contact shadows. A low-frequency ticker re-applies as buckets/foliage
	 *  stream in, logs a one-shot ISM-class census, and arms GWNLContactShadowSweepArmed. No-op on
	 *  dedicated servers (no render proxies). */
	void RegisterContactShadowSuppressor();

	/** v0.9.3 (SERVER-only): a dedicated server has no audio device, so every
	 *  UAkGameplayStatics::StopActor call fails FAkAudioDevice::Get(), logs
	 *  "Could not retrieve audio device." and returns having done nothing (AkGameplayStatics.cpp:974).
	 *  A per-actor FG cleanup path calls it constantly, flooding LogAkAudio (~3164 warnings/session).
	 *  We ARM the hook only on the dedicated server, where the original is a guaranteed no-op, so
	 *  cancelling it is behaviour-identical minus the log write. Clients never register it (their
	 *  audio device is real and StopActor must run). Mirrors the StatsSign RPC gate pattern. */
	void RegisterWwiseServerAudioGate();

	/** v0.9.3: CSS caps the navmesh at TileNumberHardLimit=65536, but our large modded map needs
	 *  306,440 tiles for full bounds - so ~79% of the map has no navmesh and creatures can't path
	 *  there. Raise the tile ceiling (by reflection, on every FG navmesh subclass CDO) so the whole
	 *  map is reachable. Memory-safe: only the 176 B/tile empty-slot table grows (~+40 MB per active
	 *  nav class); the heavy per-tile geometry stays World-Partition-streamed and RuntimeGeneration is
	 *  Dynamic, so blocked areas fill in incrementally with no rebuild stall. TileSizeUU is left alone
	 *  on purpose (changing it forces a full rebuild). Applies on the next world load. */
	void RegisterNavMeshCoverageFix();
};
