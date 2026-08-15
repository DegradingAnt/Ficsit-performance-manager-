// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * ★ TWO ONE-BOOT READS FOR THE B9 / B11 QUESTIONS IN FPM2-DESIGN-ASSEMBLED.md SECTION 17.
 *
 * Neither of these fixes anything, hooks anything, or writes anything — they are read-only probes,
 * same family as `FPM.D0` / `FPM.Support` / `FPM.CVars`. That is why this is a plain class with static
 * functions and NOT an `IFPMFix`: there is no hook to arm and no fix to disarm, and routing a
 * do-nothing-but-read command through the fix ledger would be the "unarmed fix" shape
 * `check_structure.py` exists to catch, for a class that was never a fix to begin with.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ B9 — `FGTimeOfDaySubsystem`'s non-cheat pin API.
 *
 * [MEASURED 2026-08-15, direct read of `FGTimeSubsystem.h`, the game's own shipped header]: the answer
 * is already YES from source, no boot required for the EXISTENCE half. `AFGTimeOfDaySubsystem::
 * SetDaySeconds(float)` (`FGTimeSubsystem.h:48`) and `SetTimeSpeedMultiplier(float)` (`:128`) are both
 * plain `public:` members — not `UFUNCTION(exec, CheatBoard, ...)` like `UFGCheatManager`'s equivalents,
 * not gated behind `EnableCheats` at all. `SetDaySeconds`'s own doc comment says "most useful for
 * editor preview", which is exactly a pin use case, and native FPM code can call either once it holds
 * a valid `AFGTimeOfDaySubsystem*` from `Get(UWorld*)` (`:61`).
 *
 * `FPM.Probe.TimeOfDay` proves REACHABILITY (a valid subsystem instance exists at runtime, its read
 * accessors resolve) without calling either setter — flipping the day/night cycle from a diagnostic
 * command has a real player-visible side effect and this probe is not the place to spend that.
 * Confirming the PIN behaviour itself (call `SetDaySeconds`, observe the clock hold) is left as a
 * one-line follow-up for whoever builds the M-DAWN-style lever, not for this read-only command.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ B11 — vanilla player skeleton sockets, forearm/hand, both sides.
 *
 * [MEASURED 2026-08-15, direct read of `FPMWristItemBase.h:118,121`]: the wrist-slot system ALREADY
 * ships a concrete guess — `mSocketLeft = "hand_lSocket"`, `mSocketRight = "hand_rSocket"` — and the
 * header itself says so: "B11 has not measured the real vanilla socket names against the shipped
 * skeleton, so mSocketLeft/mSocketRight's DEFAULTS are placeholders." So the useful probe is not a
 * blind dump of every socket name; it is a targeted PASS/FAIL against the two names the shipped code
 * already depends on, on both the first-person mesh (`GetMesh1P()`) and the third-person mesh
 * (`GetMesh3P()`) — printed beside the full socket list so an operator can read off the real name if
 * the guess is wrong, in the same command, without a second boot.
 *
 * `FPM.Probe.Sockets` requires a spawned local player character to answer anything; run it after
 * loading into a world, never from the main menu. Coverage is stated explicitly when that precondition
 * fails, per this project's own "an instrument must print its own coverage" rule.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ B10 — does the jetpack/hoverpack C++ constructor set `ES_BACK`?
 *
 * [MEASURED 2026-08-15, direct read of the game's own shipped `.cpp` sources]: the answer is already
 * YES from source. `AFGJetPack::AFGJetPack()` (`FGJetPack.cpp:14`) and `AFGHoverPack::AFGHoverPack()`
 * (`FGHoverPack.cpp:39`) both run `this->mEquipmentSlot = EEquipmentSlot::ES_BACK;` in the
 * constructor body. `mEquipmentSlot` is a plain `public:` `UPROPERTY(EditDefaultsOnly)` on
 * `AFGEquipment` (`FGEquipment.h:514-517`) — no access transformer needed to read it.
 *
 * `FPM.Probe.EquipSlot` proves this LIVE, on whatever the local character actually has equipped in
 * `ES_BACK` right now, via `AFGCharacterPlayer::GetEquipmentInSlot(EEquipmentSlot)` (public,
 * `FGCharacterPlayer.h:552`) — a read, no setter called. Requires a jetpack or hoverpack equipped to
 * answer anything beyond "nothing is worn"; that precondition is stated when it is not met.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ B4 — the `sg.*` scalability group count: 15 (per the banked table) or 10 (per an earlier finding,
 * m6560634)?
 *
 * [MEASURED 2026-08-15, direct read of `tools/sg_expansions.tsv`]: the banked table itself carries
 * exactly 15 distinct group names — AntiAliasingQuality, CloudQuality, EffectsQuality, FoliageQuality,
 * GlobalIlluminationQuality, NetworkQuality, PoolLightQuality, PostProcessQuality, ReflectionQuality,
 * ShadingQuality, ShadowQuality, TSRPreset, TextureQuality, VideoQuality, ViewDistanceQuality. Ten of
 * those are the STOCK Unreal scalability groups; five (Cloud, Network, PoolLight, TSRPreset, Video)
 * are Satisfactory's own additions. What is unproven without a boot is whether all 15 `sg.<Name>`
 * selector console variables actually REGISTER on a live client — the table could be stale, or a group
 * could have been removed upstream.
 *
 * `FPM.Probe.ScalabilityGroups` checks each of the 15 candidate `sg.<Name>` cvars for existence (this
 * alone settles "15 vs 10"), then diffs every banked row whose GROUP is live AND whose LEVEL matches
 * that group's CURRENT live level against the actual cvar value — printing only mismatches and missing
 * cvars (a diff, not a full dump), with a stated count of everything skipped and why. Reads
 * `tools/sg_expansions.tsv` from this plugin's own directory via `IPluginManager`; if the file cannot
 * be found, that is reported as this probe's own coverage gap, not silently treated as "nothing to
 * check".
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ B6 — is `EnableBuildableTick`'s mechanism reachable without the cheat manager?
 *
 * [PARTIALLY MEASURED 2026-08-15]: `UFGCheatManager::EnableBuildableTick` is a plain
 * `UFUNCTION(exec, CheatBoard, category="Factory")` (`FGCheatManager.h:797-798`) — the only KNOWN
 * access path runs through the cheat manager's exec surface. What is NOT measurable from this header
 * package: its `.cpp` implementation ships as a stripped stub (`{ }`, no body) in this checkout, so the
 * mechanism BEHIND the exec call — a cvar, a static flag, anything — cannot be read statically at all.
 *
 * `FPM.Probe.BuildableTick` searches every registered console object for a name containing
 * "BuildableTick", "FactoryTick" or "EffectUpdate", which is the only reachability test this probe can
 * run without guessing an unverified engine-module accessor. It deliberately does NOT attempt to read
 * `APlayerController::CheatManager` — that member lives in the Engine module, not in this project's own
 * FactoryGame header package, and this project's rule is never to write a signature from memory. That
 * half is reported as UNVERIFIED BY THIS PROBE, explicitly, rather than guessed at.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ B20 — does `r.DynamicGlobalIlluminationMethod` read 0 while Lumen is visibly ON, and which config
 * layer set it? (m6147438)
 *
 * Nothing here is measurable ahead of a boot — this is a live-moment read, by design (design section
 * 9.1's own default: "trust her primary observation, treat the strand's read as stale or wrong-layer").
 *
 * `FPM.Probe.GIMethod` reads `r.DynamicGlobalIlluminationMethod`'s current value AND its `GetSetBy`
 * layer, exactly as the design's own "exact measurement" column specifies, beside a corroborating read
 * of `r.Lumen.DiffuseIndirect.Allow` — the cvar the banked `sg.*` table actually ties to the
 * `GlobalIlluminationQuality` group's Lumen switch (`tools/sg_expansions.tsv`). Writes nothing.
 */
class FPMBootProbes
{
public:
	/** `FPM.Probe.TimeOfDay` — read the day/night subsystem's current state and report the pin API's
	 *  reachability. Writes nothing; calls no setter. */
	static void ReportTimeOfDay(class UWorld* World, class FOutputDevice* Ar);

	/** `FPM.Probe.Sockets` — enumerate socket names on the local player's first- and third-person mesh,
	 *  and check the two names FPMWristItemBase already assumes on each. */
	static void ReportSockets(class UWorld* World, class FOutputDevice* Ar);

	/** `FPM.Probe.EquipSlot` — B10: read what, if anything, the local character has equipped in
	 *  `ES_BACK` right now, and confirm its `mEquipmentSlot` matches. Writes nothing; calls no setter. */
	static void ReportEquipSlot(class UWorld* World, class FOutputDevice* Ar);

	/** `FPM.Probe.ScalabilityGroups` — B4: check all 15 banked `sg.*` groups for live existence, then
	 *  diff the banked cvar table against the live console for whichever level each live group is
	 *  currently at. Reads `tools/sg_expansions.tsv`; writes no cvar. */
	static void ReportScalabilityGroups(class FOutputDevice* Ar);

	/** `FPM.Probe.BuildableTick` — B6: search the live console for a name suggesting an independent
	 *  route to `EnableBuildableTick`'s underlying mechanism. Writes nothing; calls no setter. */
	static void ReportBuildableTick(class FOutputDevice* Ar);

	/** `FPM.Probe.GIMethod` — B20: read `r.DynamicGlobalIlluminationMethod` and
	 *  `r.Lumen.DiffuseIndirect.Allow`, each with its `GetSetBy` layer. Writes nothing. */
	static void ReportGIMethod(class FOutputDevice* Ar);
};
