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
	virtual void Arm() override;
};
