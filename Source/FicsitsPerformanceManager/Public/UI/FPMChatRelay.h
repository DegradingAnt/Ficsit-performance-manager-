// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * ★ FPM → the in-game chat window. The surface a PLAYER can actually see.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ WHY THIS EXISTS, AND WHY IT EXISTS TWICE.
 *
 * Ant, 2026-08-02: she ran a console command and reported "the command doesnt do anything". It HAD
 * worked. The confirmation went to `FactoryGame.log`, which is not on screen. A command whose only
 * feedback is in a file you cannot read while playing is indistinguishable from one that silently
 * failed, and that cost a round trip and an alt-tab to settle.
 *
 * Ant again, 2026-08-09, which is why this is being built a SECOND time, into FPM2:
 *   *"we really should print stuff to the chat instead (that is important of course) along with the
 *   log. a player wont check logs for stuff, only devs do."*
 *
 * That second sentence is the design rule, not just a preference. It splits FPM's output in two:
 *   - PLAYER-FACING (a fix engaged, a setting refused, a warning with a remedy) → chat AND log.
 *   - DEV-FACING (`FPM.Support`'s copyable bundle, ledgers, probe dumps) → log and console only.
 *     A 60-line support block does not belong in a chat window, and chat is not copyable.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ PORTED FROM THE OLD MOD, AND WHAT CHANGED. Ant's standing rule: "refine the ports, not just copy
 * straight over." Three things would have been silently broken by a straight copy:
 *
 *   1. ⚠ THE LOG CATEGORY. The old mirror filtered on `Category != TEXT("LogFPM")`. FPM2's category is
 *      `LogFicsitsPerformanceManager`. Copied verbatim, the mirror would have compiled, armed, reported
 *      "chat mirror ON" — and matched NOTHING, forever. A feature that reports success and does nothing
 *      is this project's own named defect class, and it would have been invisible until a boot.
 *   2. ⚠ THE PUMP HAD NO DRIVER. Off-thread lines are queued and drained by `FPMChatPumpQueued()`, which
 *      in the old mod was "called once per governor tick". FPM2 HAS NO GOVERNOR — that is Phase 5. A
 *      straight copy leaves every off-thread line queued forever, then silently dropped at the 64-line
 *      cap. This version owns its own ticker and starts it on first use, so the queue always drains.
 *   3. The old header claimed `FPMChatf` is "ALWAYS available" while describing `FPMChat`'s behaviour.
 *      Both are always available; the macro is only a printf wrapper. Stated correctly here.
 *
 * ─────────────────────────────────────────────────────────────────────────────────────────────────
 * ★ HARD CONSTRAINTS. Each one is load-bearing and each was paid for.
 *
 *   - CLIENT ONLY. A dedicated server has no local chat window, and `AFGChatManager::Get` is a pointless
 *     lookup on a headless host. Every entry point checks and returns.
 *   - LOCAL ONLY, and this is a DESIGN choice rather than a legal one. Uses `AddChatMessageToReceived`,
 *     never `BroadcastChatMessage`. Ant, 2026-08-02: "we can do things over the network in the mod but
 *     it needs to be stated clearly why in that case" — replication is permitted WITH a stated reason,
 *     not banned. There is no reason here and there is a reason against: this is ONE player's
 *     performance readout, and pushing it into everyone else's chat is noise. A future feature that does
 *     need to replicate states why AT ITS CALL SITE, in this shape.
 *     ⚠ This is NOT network activity in the banned sense. It opens no socket and contacts nothing. The
 *     no-network rule is about FPM reaching off the machine; this writes to a local UI widget.
 *   - GAME THREAD ONLY for the write itself. `FOutputDevice::Serialize` can be called from ANY thread and
 *     `AFGChatManager` is a UObject subsystem that is not thread-safe. Off-thread lines are queued and
 *     drained on the game thread rather than dropped or written unsafely.
 *
 * ★ ON THE "NewChat" / Chat Mk 2 MOD (Ant raised it 2026-08-09 as a possible dependency).
 * We take NO dependency on it, deliberately. Checked 2026-08-09 against the ficsit API and its GitHub:
 * it is BLUEPRINT-ONLY (no `Source/` directory), so there is no C++ API to link against and a hard
 * dependency would buy nothing callable. It is `required_on_remote: false` and GPL-3.0. It upgrades the
 * chat WINDOW, so writing to the vanilla chat system should render in it for free, and players without
 * it still get their messages. VERIFY on a boot with it installed before claiming that works.
 */

/** Post one line to the local chat window. Safe to call from any thread; no-op on a dedicated server. */
void FPMChat(const FString& Line);

/**
 * printf-style wrapper.
 *
 * A MACRO, not a template, on purpose. UE 5.6 sanitises format strings at compile time
 * (`TCheckedFormatStringPrivate`), and that check needs the literal visible AT THE CALL SITE. Forwarding
 * it through a template parameter hides it and fails with "call to immediate function is not a constant
 * expression". The macro keeps the literal where the sanitiser can see it, so a mismatched format
 * specifier is still a build error — which is the point of the check and worth keeping.
 */
#define FPMChatf(Fmt, ...) FPMChat(FString::Printf(Fmt, ##__VA_ARGS__))

/**
 * Arm/disarm the `LogFicsitsPerformanceManager` → chat mirror (the `FPM.Chat` console command).
 * OFF by default and it must stay that way: once the Phase 5 governor lands it will log a frame-trace
 * line every second, and mirroring that would bury the player's real chat.
 */
void FPMSetChatLogMirror(bool bEnabled);

/**
 * Drain lines queued from other threads. Called automatically by this module's own ticker; exposed so a
 * future governor tick can also drain it synchronously if it wants tighter latency.
 */
void FPMChatPumpQueued();
