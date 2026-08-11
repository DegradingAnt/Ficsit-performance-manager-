// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * NO-OWNER RPC GATE — cancels buildable RPC dispatches the engine is going to drop anyway.
 *
 * THE BUG: when a remote function is dispatched for an actor with no owning connection, the engine
 * logs a warning and DROPS it (NetDriver.cpp:7922-7935). The Stats mod's signs hit that path about 166
 * times per second — roughly 100,000 suppressed in ten minutes, measured 2026-07-20 — and
 * LinearMotion's elevators do the same. Cancelling the dispatch reproduces vanilla's outcome exactly,
 * minus the dispatch machinery, the log write, and the channel pressure.
 *
 * ⚠ RE-VERIFIED ON CARRY, AND THE OLD CLAIM WAS TOO WEAK. The old comment justified this for the
 * server only. The engine source shows the null-connection drop is the SAME path on both sides:
 *     Connection = Actor->GetNetConnection();
 *     if (Connection) { if (ServerConnection) { Connection = ServerConnection; } Internal...(); }
 *     else { UE_LOG(LogNet, Warning, "No owning connection ... will not be processed."); }
 * A client redirects to ServerConnection only AFTER Actor->GetNetConnection() comes back non-null, so
 * a client-side Server RPC from an unowned buildable is dropped identically. Cancelling is bit-exact
 * on client and server.
 *
 * ⚠ MULTICASTS ARE NEVER TOUCHED, AND THAT COST A REGRESSION. An early version suppressed
 * Multicast_PlayDockingEffects, door sounds and elevator StopMoving within 20 seconds of boot. The
 * multicast branch iterates every client connection and RETURNS before the owning-connection check
 * ever runs, so the "no owner means dropped" reasoning simply does not apply to them.
 *
 * ⚠ THE HOOK TARGET WAS CHECKED FOR THE SERVER, NOT ASSUMED FROM A CLIENT MEASUREMENT. The 166/sec
 * figure came from a client log, and SUBSCRIBE_METHOD_VIRTUAL patches only the class it is handed — a
 * subclass overriding without calling Super would leave this gate doing NOTHING on the server, silently,
 * while the mod still reported itself loaded. Verified 2026-08-08: nothing in FactoryGame overrides
 * ProcessRemoteFunction, and in the whole engine only UDemoNetDriver does. That one is replay
 * recording and is deliberately NOT hooked — a replay should capture what the game did, not what FPM
 * suppressed.
 *
 * SCOPE: AFGBuildable only. Player, pawn and vehicle RPC flows are never touched, and non-buildables
 * keep the engine's diagnostic warning, which may matter to someone debugging another mod.
 *
 * ⚠ IT SUPPRESSES AN ENGINE DIAGNOSTIC, SO IT MUST REPLACE IT. That is what the per-class census is
 * for: every offending class names itself once instead of hiding in warning spam. The census is capped
 * so it cannot itself become spam — and on carry it now SAYS when it saturates, because a bounded
 * census that goes quiet reads exactly like "that was all of them".
 */
class FFPMNoOwnerRpcGate final : public IFPMFix
{
public:
	static FFPMNoOwnerRpcGate& Get();

	virtual const TCHAR* Name() const override { return TEXT("no-owner-rpc-gate"); }
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** Guard: the cause lives in ANOTHER mod's sign dispatch. We prevent the harm; naming and reporting it upstream
	 * IS the origin work, and it is not ours to close. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::Guard; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::RpcGate; }
	virtual void Arm() override;

	/**
	 * Removes the hook.
	 *
	 * ⚠ Without this, `FPMFixes::DisarmAll()` reports this fix disarmed while its handler keeps
	 * running. Near-harmless at process exit, which is the only place DisarmAll has ever been called
	 * from and why the omission survived; it is what blocked P4.2's master OFF switch.
	 */
	virtual void Disarm() override;

	/**
	 * ★ OFF BY DEFAULT SINCE 0.11.16, ON MEASUREMENT — not on doubt.
	 *
	 * Ant's 2026-08-11 session, both logs read directly:
	 *   · this gate's own census printed NOTHING in her client log and NOTHING in her dedicated-server
	 *     log, so it suppressed zero dispatches on either machine;
	 *   · the engine warning it pre-empts, "No owning connection for actor", appeared **0** times in the
	 *     server log and **once** in the client log — on `Char_Player_C`, which the `IsA<AFGBuildable>`
	 *     filter deliberately excludes.
	 *
	 * The condition it was built for does not occur in her stack any more. The likeliest reason is this
	 * gate's OWN multicast exemption: FPM1 measured ~166 suppressions/second before that exemption
	 * existed, and if what it was catching was mostly multicast, exempting them correctly removed nearly
	 * all of it. That would make this a fix its own bug-fix retired.
	 *
	 * Meanwhile it holds a funchook detour on `UNetDriver::ProcessRemoteFunction` — the path EVERY RPC in
	 * the game takes — and she measured `FPM.Fix.NoOwnerRpcGate 0` restoring her hoverpack's sound and
	 * animation. Zero measured benefit against one measured cost.
	 *
	 * ⚠ NOT DELETED, DELIBERATELY. The Stats-sign flood was real, and it is a property of HER MOD SET
	 * rather than of the game, so a mod update can bring it back. `FPM.Fix.NoOwnerRpcGate 1` arms it and
	 * the census then reports within seconds whether it has anything to do — which is a better answer
	 * than either keeping it on forever or dropping the capability.
	 */
	virtual bool DefaultArmed() const override { return false; }

private:
	/** Handle from Arm(), so Disarm() removes exactly this handler. */
	FDelegateHandle ProcessRemoteFunctionHandle;
};
