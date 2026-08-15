// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMDiag.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"

#include "HAL/IConsoleManager.h"

/*
 * THE MASTER SWITCH. -1 means "defer to the per-channel value", which is the default so that turning a
 * single channel up does not require touching this one. Set it to 0 to silence everything at once —
 * the case that matters when reading a log for something else.
 */
static TAutoConsoleVariable<int32> CVarDiagMaster(
	TEXT("FPM.Diag"), -1,
	TEXT("Master FPM diagnostics level. -1 = per-channel (default), 0 = silence ALL, 1 = on, 2 = verbose. "
	     "Never changes what a fix DOES, only what it prints."),
	ECVF_Default);

/*
 * ONE CVAR PER CHANNEL, IN THE SAME ORDER AS FPMDiag::EChannel. The order is load-bearing — the lookup
 * below indexes this table by the enum — so a new channel goes in BOTH places or the compile-time size
 * check underneath fires.
 */
static TAutoConsoleVariable<int32> CVarDiagStaticBase(
	TEXT("FPM.Diag.StaticBase"), 1,
	TEXT("Immobile-base movement repair. 0 = silent, 1 = throttled totals, 2 = every correction."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagRpcGate(
	TEXT("FPM.Diag.RpcGate"), 1,
	TEXT("Owner-less RPC gate. 0 = silent, 1 = throttled totals + per-class attribution, 2 = every call."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagRain(
	TEXT("FPM.Diag.Rain"), 1,
	TEXT("Rain-occlusion sweep REPORTING. 0 = silent, 1 = the sweep summary + unrepairable classes, "
	     "2 = every class. ⚠ This changes what is PRINTED. FPM.Rain.Sweep / FPM.Rain.Hooks change what "
	     "the fix DOES - different things, deliberately different names."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagSchematic(
	TEXT("FPM.Diag.Schematic"), 1,
	TEXT("Schematic access probe. 0 = silent, 1 = anomalies + throttled totals, 2 = every call."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagHologram(
	TEXT("FPM.Diag.Hologram"), 1,
	TEXT("Replicated build-preview repair. 0 = silent, 1 = throttled totals + every unrepairable class, "
	     "2 = every hologram seen."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagInventory(
	TEXT("FPM.Diag.Inventory"), 1,
	TEXT("Inventory init repair. 0 = silent, 1 = throttled totals + every refusal, 2 = every component."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagClone(
	TEXT("FPM.Diag.Clone"), 1,
	TEXT("Join-time player-state clone sensor. 0 = silent, 1 = per-join summary, 2 = every candidate."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagHitch(
	TEXT("FPM.Diag.Hitch"), 1,
	TEXT("Frame-time hitch meter. 0 = silent, 1 = every hitch + the periodic summary, 2 = also names the "
	     "packages that were loading when it hit. Level 2 costs a string copy per async load - it is for a "
	     "deliberate boot, not for playing."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagResidency(
	TEXT("FPM.Diag.Residency"), 1,
	TEXT("Vanilla platform-icon pin. 0 = silent, 1 = the one 'N/4 pinned' line. It has nothing to say per "
	     "frame, so there is no level 2."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagOverlay(
	TEXT("FPM.Diag.Overlay"), 1,
	TEXT("The on-screen dev feed. 0 = hide it, 1 = show it. The KEYBIND is FPM.Diag.OverlayKey "
	     "(default F8) - built 2026-08-09; SML's UGameInstanceModule never had the keybind registry the "
	     "old note here was waiting for."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagSaveGuard(
	TEXT("FPM.Diag.SaveGuard"), 1,
	TEXT("SaveSettings interceptor. 0 = silent, 1 = arm line, every restore/re-apply and every refusal, "
	     "2 = also the no-op saves where FPM held nothing. Level 1 is deliberately chatty: this guard "
	     "stands between a transient write and a PERMANENT change to the player's own settings file, so "
	     "its firings are not routine events to be sampled."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagRailGuard(
	TEXT("FPM.Diag.RailGuard"), 1,
	TEXT("Unwired rail-connection guard. 0 = silent, 1 = first fire + a throttled sample of the rest. "
	     "It fires ~2,000 times in a burst at server start, so level 1 is deliberately throttled - the "
	     "COUNTER carries the true rate, not the log."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagNavMesh(
	TEXT("FPM.Diag.NavMesh"), 1,
	TEXT("Navmesh tile-ceiling write-back. 0 = silent, 1 = the per-actor before/after and the summary. "
	     "The FAILURE lines (write did not stick, or no actors found) are NOT gated by this - a fix whose "
	     "predecessor logged a raise it never performed does not get a quiet failure mode."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagHudGuard(
	TEXT("FPM.Diag.HudGuard"), 1,
	TEXT("Blueprint-hook descriptor strip. 0 = silent, 1 = the ALLOWING lines for HUD-hooking assets we "
	     "leave alone. The STRIP and CANCEL lines are NOT gated - this guard removes another mod's code, "
	     "and taking that silently is how two earlier versions cost Ant her mod UI without anyone knowing."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagZipline(
	TEXT("FPM.Diag.Zipline"), 1,
	TEXT("Zipline output-bus volume. 0 = silent, 1 = one line per VALUE CHANGE (not per equip - ziplines "
	     "are equipped constantly and per-equip logging would be the noisiest line in the log)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagTexturePool(
	TEXT("FPM.Diag.TexturePool"), 1,
	TEXT("Card-sized streaming pool. 0 = silent, 1 = the sizing decision and every raise WITH all of its "
	     "inputs, 2 = also each watchdog poll that found nothing to do. Level 1 prints the inputs on "
	     "purpose: the old implementation of this guard stood itself down on every boot while logging a "
	     "cause it had not tested, and went unnoticed for months because that one line was believed."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagWireGuard(
	TEXT("FPM.Diag.WireGuard"), 1,
	TEXT("Null entries in UFGCircuitConnectionComponent::mWires, swept before the autosave walks them. "
	     "0 = silent, 1 = a summary per sweep that found something, plus the NAME of every owning actor, "
	     "2 = also every clean sweep. Level 1 names the owners deliberately: a count cannot tell you "
	     "WHICH building carries the damage, and the name is the only route back to the mod that caused "
	     "it. On 2026-08-09 this exact state SIGSEGV'd the dedicated server inside its autosave."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagWwiseGate(
	TEXT("FPM.Diag.WwiseGate"), 1,
	TEXT("Wwise StopActor calls suppressed on a dedicated server, which has no audio device for them "
	     "to reach. 0 = silent, 1 = the arm line plus a heartbeat every 100,000 suppressions so a "
	     "deployed server can prove the gate is live from its own log. The heartbeat is deliberately "
	     "sparse - the entire point of this gate is to stop writing a line per call."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagSchematicGuard(
	TEXT("FPM.Diag.SchematicGuard"), 1,
	TEXT("Schematic access refused because vanilla was about to dereference a null event subsystem. "
	     "0 = silent, 1 = first sighting of each schematic plus a throttled sample of the rest, AND every "
	     "eventless pass-through - that last one is the line that can falsify the guard's narrowing, so it "
	     "is not held back for level 2. ⚠ This changes what is PRINTED. FPM.SchematicGuard changes what "
	     "the guard DOES - different things, deliberately different names."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagPowerWarning(
	TEXT("FPM.Diag.PowerWarning"), 1,
	TEXT("Power-circuit fuse probe. 0 = silent, 1 = every CHANGE in the sample during the post-load "
	     "window. The VERDICT line is NOT gated by this - it is the whole reason the probe exists, and a "
	     "reader should not have to have known to turn a channel on beforehand."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagEnclosure(
	TEXT("FPM.Diag.Enclosure"), 1,
	TEXT("The shared indoor probe. 0 = silent, 1 = registrations only, 2 = every completed reading. "
	     "Level 2 is one line per batch and batches only happen when the player MOVES, so it is far less "
	     "chatty than it sounds. FPM.Enclosure.Report prints the reading and what it has cost."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagDistanceField(
	TEXT("FPM.Diag.DistanceField"), 1,
	TEXT("Instanced meshes not contributing to distance fields. 0 = silent, 1 = the audit at each sample "
	     "mark plus the worst offenders by instance count. ⚠ This changes what is PRINTED. "
	     "FPM.DistanceField.Repair changes what is DONE, and it adds renderer work."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagWeatherGate(
	TEXT("FPM.Diag.WeatherGate"), 1,
	TEXT("Weather particles scaled down in a sealed room. 0 = silent, 1 = the target count, every change "
	     "of sealed state, and the FIRST time a write fails to hold. That last line matters most: these "
	     "systems are spawned from Blueprint, so a graph re-pushing the value would make the gate inert, "
	     "and it must say so rather than take credit."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagGCMeter(
	TEXT("FPM.Diag.GCMeter"), 1,
	TEXT("Garbage-collection pauses. 0 = silent, 1 = one line per pass with the pause, the gap, the "
	     "timer-vs-forced call and the object delta. About one line a minute, which is why it is not held "
	     "back for level 2 - the per-pass detail IS the measurement."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagBlueprintSweep(
	TEXT("FPM.Diag.BlueprintSweep"), 1,
	TEXT("Blueprint recipe-sweep gate. 0 = silent, 1 = one line each time a FULL sweep is ALLOWED and why. "
	     "The cancelled ones are deliberately NOT logged - they are the common case and a line every two "
	     "seconds would be the noisiest thing in the log. The counts live in FPM.Blueprint.Report, and an "
	     "audit DISAGREEMENT is a Warning that this switch does not gate."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagStallSampler(
	TEXT("FPM.Diag.StallSampler"), 1,
	TEXT("Which MODULE the game thread was inside during a measured stall. 0 = silent, 1 = the overlay row "
	     "and the ranked report. The sampling itself is controlled by FPM.Stall.* - this only gates the "
	     "reporting, so turning it off hides the answer rather than stopping the work."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagGlassQuality(
	TEXT("FPM.Diag.GlassQuality"), 1,
	TEXT("Lumen front-layer translucency reflections, held on so glass reflects properly. 0 = silent, "
	     "1 = the arm line and the state. A STOMPED hold is a Warning that this switch does not gate - "
	     "it would mean the priority argument the fix rests on is wrong, and that must not be silenceable. "
	     "FPM.Glass.Enable is the feature toggle; this only gates the talking."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagNaniteStreaming(
	TEXT("FPM.Diag.NaniteStreaming"), 1,
	TEXT("Nanite dropping geometric detail because its streaming pool is overcommitted. 0 = silent, "
	     "1 = the arm line and the pool raise. FPM.Nanite.PoolMB is the lever and FPM.Nanite.Report is "
	     "the measurement; this only gates the talking."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagMaterialEffect(
	TEXT("FPM.Diag.MaterialEffect"), 1,
	TEXT("Who calls UFGMaterialEffectComponent::SetMeshes. 0 = silent, 1 = one line per newly-seen owner "
	     "class. The census SATURATION warning is not gated by this - a census that goes quiet at its cap "
	     "reads as complete coverage. FPM.MaterialEffect.Report prints the ranking."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagSettings(
	TEXT("FPM.Diag.Settings"), 1,
	TEXT("FPM's own UFGUserSetting rows. 0 = silent, 1 = the boot row audit. It prints FPM's row count "
	     "AND the game-wide FGUserSetting count separately, because those answer different questions: a "
	     "zero for ours with a healthy game-wide total means we shipped none, while zero for BOTH means "
	     "the asset scan is broken and no count from it means anything."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagSharpness(
	TEXT("FPM.Diag.Sharpness"), 1,
	TEXT("Upscaler sharpening. 0 = silent, 1 = one line per route change. The 'DLSS is live so this is "
	     "IGNORED' and 'upscaler unknown' warnings are NOT gated - both mean the player turned a knob "
	     "that did nothing, which they must be told about."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagReflex(
	TEXT("FPM.Diag.Reflex"), 1,
	TEXT("NVIDIA Reflex mode. 0 = silent, 1 = one line per mode change naming the route used. The "
	     "'COULD NOT REACH REFLEX' warning is NOT gated by this - that line means the fix is armed and "
	     "doing nothing, which a verbosity setting must never be able to hide."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagUpscalerPreset(
	TEXT("FPM.Diag.UpscalerPreset"), 1,
	TEXT("DLSS preset hold. 0 = silent, 1 = one line when the held preset changes. The 'DLSS is not the "
	     "active upscaler' warning and the report's MISMATCH line are NOT gated by this - both mean the "
	     "fix is doing nothing while looking healthy."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagInstanceSwap(
	TEXT("FPM.Diag.InstanceSwap"), 1,
	TEXT("AbstractInstance mesh components put back on RemoveAtSwap. 0 = silent, 1 = one line per sweep "
	     "with seen / already-correct / converted. The 'NO components found at all' warning is NOT gated "
	     "by this - that one means the fix is inert while still printing an armed line, and a verbosity "
	     "setting must not be able to hide it."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagBlueprintContentSnap(
	TEXT("FPM.Diag.BlueprintContentSnap"), 1,
	TEXT("Blueprint-to-blueprint content snap. 0 = silent, 1 = one line per mode entry naming the "
	     "resolved target proxy, 2 = per-frame candidate-pair scoring. The mode-registration ARMED line "
	     "is NOT gated by this - that line is what tells a reader Hook A actually installed."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagWristSlot(
	TEXT("FPM.Diag.WristSlot"), 1,
	TEXT("Slice W wrist-slot: the add-hook, equip/deploy/release RCOs and their refusals, the "
	     "persistence handshake, and registration. 0 = silent, 1 = the arm line, every refusal, and "
	     "the equip/deploy/release transitions, 2 = also the pending-worn map's claim attempts."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagServerLevers(
	TEXT("FPM.Diag.ServerLevers"), 1,
	TEXT("Server governor lever audit: which candidate levers exist on this build and which accept "
	     "a write. 0 = silent, 1 = the per-family verdict and the coverage line, 2 = also every "
	     "individual lever with its value and SetBy layer. Reads only; writes nothing."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagJoinVersion(
	TEXT("FPM.Diag.JoinVersion"), 1,
	TEXT("Join-time version echo: what THIS side and the REMOTE side reported for FicsitsPerformanceManager "
	     "at handshake, read from the same mod-list exchange SML itself validates. Governs ONLY the routine "
	     "matched-case log line: 0/1 = silent, 2 = also log a matched join. A mismatch or an absent-remote "
	     "finding always reaches FPMOverlay (screen + log) regardless of this cvar - it is FPMOverlay::Post's "
	     "own stated policy (FPMOverlay.h) to answer only to the FPM.Diag master switch, not a per-channel "
	     "one, and a join refusal is not the kind of diagnostic this switch should be able to hide. Reads "
	     "only; writes nothing."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagHostTier(
	TEXT("FPM.Diag.HostTier"), 1,
	TEXT("Slice 4 host probe: FULL/VANILLA/PROBING tier decisions. 0 = silent, 1 = the arm line, every "
	     "tier CHANGE (including late-arrival upgrades) and the mid-session vanish warning, 2 = also "
	     "the per-poll classification while probing. The persistent overlay row and FPM.Status are NOT "
	     "gated by this - a player must be able to see what tier is active even with diagnostics off."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagLeverRegistry(
	TEXT("FPM.Diag.LeverRegistry"), 1,
	TEXT("Slice 2 lever registry: registration refusals (Law 1 / clause 2), capability-probe verdicts "
	     "and the alias table. 0 = silent, 1 = the arm line, the self-test result and the probe-pass "
	     "summary, 2 = also every individual lever's verdict. Reads only; writes nothing itself -- "
	     "applying a lever's value goes through FPMCVarWriter, a separate write path."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagIndoorFog(
	TEXT("FPM.Diag.IndoorFog"), 1,
	TEXT("Slice 5 (Section 9.7) indoor fog: the shared enclosure roof verdict, the StartDistance write "
	     "and its read-back. 0 = silent, 1 = the arm line and every roof-state CHANGE, 2 = also the "
	     "per-tick StartDistance value while under a roof."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagNetGuidCensus(
	TEXT("FPM.Diag.NetGuidCensus"), 1,
	TEXT("Net GUID census: object references the net GUID cache refuses to address, which the engine "
	     "then warns about once each. 0 = silent, 1 = first sighting of each class plus a throttled "
	     "running total. The class-list-FULL warning and FPM.NetGuidCensus.Report are NOT gated by "
	     "this: a bounded list going quiet must never be silenceable into looking complete. The census "
	     "itself is off by default and is armed with FPM.Fix.NetGuidCensus 1."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagThirdPersonToggle(
	TEXT("FPM.Diag.ThirdPersonToggle"), 1,
	TEXT("Slice 5 (Section 6.7) third-person camera keybind. 0 = silent, 1 = the arm line, whether both "
	     "input assets resolved, and every time the bound action fires."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagDetect(
	TEXT("FPM.Diag.Detect"), 1,
	TEXT("Slice 4 M-DETECT: the four community-trap detectors, the audio-voice probe, and the "
	     "registry's own self-test. 0 = silent, 1 = the arm line and each detector's per-run "
	     "coverage line, 2 = also per-flagged-class detail. FPM.Detect.Report and the UObject "
	     "watermark warning (FPMGCMeter, section 7.3) are NOT gated by this channel - a report-only "
	     "channel must still be readable with diagnostics off, same reasoning as HostTier."),
	ECVF_Default);

namespace
{
	TAutoConsoleVariable<int32>* const GChannelCVars[] = {
		&CVarDiagStaticBase,
		&CVarDiagRpcGate,
		&CVarDiagRain,
		&CVarDiagSchematic,
		&CVarDiagHologram,
		&CVarDiagInventory,
		&CVarDiagClone,
		&CVarDiagHitch,
		&CVarDiagResidency,
		&CVarDiagOverlay,
		&CVarDiagSaveGuard,
		&CVarDiagRailGuard,
		&CVarDiagNavMesh,
		&CVarDiagHudGuard,
		&CVarDiagZipline,
		&CVarDiagTexturePool,
		&CVarDiagWireGuard,
		&CVarDiagWwiseGate,
		&CVarDiagSchematicGuard,
		&CVarDiagPowerWarning,
		&CVarDiagEnclosure,
		&CVarDiagDistanceField,
		&CVarDiagWeatherGate,
		&CVarDiagGCMeter,
		&CVarDiagStallSampler,
		&CVarDiagBlueprintSweep,
		&CVarDiagGlassQuality,
		&CVarDiagNaniteStreaming,
		&CVarDiagMaterialEffect,
		&CVarDiagSettings,
		&CVarDiagSharpness,
		&CVarDiagReflex,
		&CVarDiagUpscalerPreset,
		&CVarDiagInstanceSwap,
		&CVarDiagBlueprintContentSnap,
		&CVarDiagWristSlot,
		&CVarDiagServerLevers,
		&CVarDiagJoinVersion,
		&CVarDiagHostTier,
		&CVarDiagLeverRegistry,
		&CVarDiagIndoorFog,
		&CVarDiagNetGuidCensus,
		&CVarDiagThirdPersonToggle,
		&CVarDiagDetect,
	};

	/*
	 * ⚠ THIS ASSERT CHECKS COUNT ONLY, NOT ORDER — corrected 2026-08-09 after a review caught the comment
	 * claiming more than the code delivered. It used to read "this is the whole reason the indexing is
	 * safe", which is false: adding two channels and putting them in the wrong ORDER keeps the count equal,
	 * sails past this line, and silently reports one channel's level under another channel's name. A
	 * comment that overstates its guarantee is worse than no comment, because it stops the next reader
	 * looking. The order check is a RUNTIME one, in LogAll below.
	 */
	static_assert(UE_ARRAY_COUNT(GChannelCVars) == static_cast<int32>(FPMDiag::EChannel::Count),
		"FPMDiag::EChannel and GChannelCVars are out of sync - add the new channel to BOTH.");

	const TCHAR* ChannelName(FPMDiag::EChannel Channel)
	{
		switch (Channel)
		{
		case FPMDiag::EChannel::StaticBase:     return TEXT("FPM.Diag.StaticBase");
		case FPMDiag::EChannel::RpcGate:        return TEXT("FPM.Diag.RpcGate");
		case FPMDiag::EChannel::Rain:           return TEXT("FPM.Diag.Rain");
		case FPMDiag::EChannel::SchematicProbe: return TEXT("FPM.Diag.Schematic");
		case FPMDiag::EChannel::HologramNet:    return TEXT("FPM.Diag.Hologram");
		case FPMDiag::EChannel::InventoryInit:  return TEXT("FPM.Diag.Inventory");
		case FPMDiag::EChannel::CloneSensor:    return TEXT("FPM.Diag.Clone");
		case FPMDiag::EChannel::Hitch:          return TEXT("FPM.Diag.Hitch");
		case FPMDiag::EChannel::Residency:      return TEXT("FPM.Diag.Residency");
		case FPMDiag::EChannel::Overlay:        return TEXT("FPM.Diag.Overlay");
		case FPMDiag::EChannel::SaveGuard:      return TEXT("FPM.Diag.SaveGuard");
		case FPMDiag::EChannel::RailGuard:      return TEXT("FPM.Diag.RailGuard");
		case FPMDiag::EChannel::NavMesh:        return TEXT("FPM.Diag.NavMesh");
		case FPMDiag::EChannel::HudGuard:       return TEXT("FPM.Diag.HudGuard");
		case FPMDiag::EChannel::Zipline:        return TEXT("FPM.Diag.Zipline");
		case FPMDiag::EChannel::TexturePool:    return TEXT("FPM.Diag.TexturePool");
		case FPMDiag::EChannel::WireGuard:      return TEXT("FPM.Diag.WireGuard");
		case FPMDiag::EChannel::WwiseGate:      return TEXT("FPM.Diag.WwiseGate");
		case FPMDiag::EChannel::SchematicGuard: return TEXT("FPM.Diag.SchematicGuard");
		case FPMDiag::EChannel::PowerWarning:   return TEXT("FPM.Diag.PowerWarning");
		case FPMDiag::EChannel::Enclosure:      return TEXT("FPM.Diag.Enclosure");
		case FPMDiag::EChannel::DistanceField:  return TEXT("FPM.Diag.DistanceField");
		case FPMDiag::EChannel::WeatherGate:    return TEXT("FPM.Diag.WeatherGate");
		case FPMDiag::EChannel::GCMeter:        return TEXT("FPM.Diag.GCMeter");
		case FPMDiag::EChannel::StallSampler:   return TEXT("FPM.Diag.StallSampler");
		case FPMDiag::EChannel::BlueprintSweep: return TEXT("FPM.Diag.BlueprintSweep");
		case FPMDiag::EChannel::GlassQuality:   return TEXT("FPM.Diag.GlassQuality");
		case FPMDiag::EChannel::NaniteStreaming: return TEXT("FPM.Diag.NaniteStreaming");
		case FPMDiag::EChannel::MaterialEffect: return TEXT("FPM.Diag.MaterialEffect");
		case FPMDiag::EChannel::Settings:       return TEXT("FPM.Diag.Settings");
		case FPMDiag::EChannel::Sharpness:      return TEXT("FPM.Diag.Sharpness");
		case FPMDiag::EChannel::Reflex:         return TEXT("FPM.Diag.Reflex");
		case FPMDiag::EChannel::UpscalerPreset: return TEXT("FPM.Diag.UpscalerPreset");
		case FPMDiag::EChannel::InstanceSwap:   return TEXT("FPM.Diag.InstanceSwap");
		case FPMDiag::EChannel::BlueprintContentSnap: return TEXT("FPM.Diag.BlueprintContentSnap");
		case FPMDiag::EChannel::WristSlot:      return TEXT("FPM.Diag.WristSlot");
		case FPMDiag::EChannel::ServerLevers:   return TEXT("FPM.Diag.ServerLevers");
		case FPMDiag::EChannel::JoinVersion:    return TEXT("FPM.Diag.JoinVersion");
		case FPMDiag::EChannel::HostTier:       return TEXT("FPM.Diag.HostTier");
		case FPMDiag::EChannel::LeverRegistry:  return TEXT("FPM.Diag.LeverRegistry");
		case FPMDiag::EChannel::IndoorFog:      return TEXT("FPM.Diag.IndoorFog");
		case FPMDiag::EChannel::NetGuidCensus:  return TEXT("FPM.Diag.NetGuidCensus");
		case FPMDiag::EChannel::ThirdPersonToggle: return TEXT("FPM.Diag.ThirdPersonToggle");
		case FPMDiag::EChannel::Detect:         return TEXT("FPM.Diag.Detect");
		default:                                return TEXT("<unknown>");
		}
	}
}

const TCHAR* FPMDiag::NameOf(EChannel Channel)
{
	return ChannelName(Channel);
}

int32 FPMDiag::LevelOf(EChannel Channel)
{
	const int32 Master = CVarDiagMaster.GetValueOnAnyThread();
	if (Master >= 0)
	{
		return Master;   // master wins in BOTH directions: 0 silences, 2 turns everything up
	}

	const int32 Index = static_cast<int32>(Channel);
	if (Index < 0 || Index >= UE_ARRAY_COUNT(GChannelCVars))
	{
		return 0;        // fail QUIET, not fail loud: an unknown channel must not spam
	}
	return GChannelCVars[Index]->GetValueOnAnyThread();
}

bool FPMDiag::IsOn(EChannel Channel, int32 Level)
{
	return LevelOf(Channel) >= Level;
}

bool FPMDiag::IsSilenced()
{
	// Explicitly 0, not merely "<= 0" — the default is -1 and that means "defer to the channel", which
	// is the opposite of silence. Conflating the two would silence everything by default.
	return CVarDiagMaster.GetValueOnAnyThread() == 0;
}

void FPMDiag::LogAll()
{
	const int32 Master = CVarDiagMaster.GetValueOnAnyThread();
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] diagnostics — FPM.Diag = %d (%s)"),
		Master, Master < 0 ? TEXT("per-channel") : TEXT("OVERRIDING every channel"));

	for (int32 i = 0; i < static_cast<int32>(EChannel::Count); ++i)
	{
		const EChannel Ch = static_cast<EChannel>(i);
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %-22s = %d   (effective %d)"),
			ChannelName(Ch), GChannelCVars[i]->GetValueOnAnyThread(), LevelOf(Ch));

		/*
		 * THE ORDER CHECK THE static_assert CANNOT DO. `ChannelName` is a hand-written switch and
		 * `GChannelCVars` is a hand-written table; nothing but this compares them. Asking the console
		 * manager what the cvar at THIS index is actually called (`IConsoleManager.h:1104`) turns a silent
		 * mislabelling into a loud one, and this is the command someone runs precisely when they are trying
		 * to work out why a switch is not doing what they expect.
		 */
		const FString Registered = IConsoleManager::Get().FindConsoleObjectName(GChannelCVars[i]->AsVariable());
		UE_CLOG(!Registered.IsEmpty() && Registered != ChannelName(Ch),
			LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]   ^^ CHANNEL TABLE IS OUT OF ORDER: index %d is named '%s' by ChannelName() but is "
			     "registered as '%s'. Every level printed above may be attributed to the wrong channel. "
			     "Fix the enum / GChannelCVars / ChannelName ordering in FPMDiag before trusting any of it."),
			i, ChannelName(Ch), *Registered);
	}
}

/*
 * `FPM.Diag.List` — because a switch you cannot read the state of is a switch you end up guessing at,
 * and this project has burned boots on exactly that shape of guess.
 */
static FAutoConsoleCommandWithOutputDevice GDiagListCmd(
	TEXT("FPM.Diag.List"),
	TEXT("Print every FPM diagnostic channel and its effective level."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FPMDiag::LogAll();
	}));
