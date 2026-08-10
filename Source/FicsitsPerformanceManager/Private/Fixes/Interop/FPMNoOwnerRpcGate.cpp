// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMNoOwnerRpcGate.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMHookLedger.h"

#include "Buildables/FGBuildable.h"
#include "Engine/NetDriver.h"

#include <atomic>

namespace
{
	/** Total suppressed this session. Atomic because the hook is reachable off the game thread. */
	std::atomic<uint64> GSuppressedTotal{0};

	/**
	 * First-sighting census. Bounded so it cannot become the spam it exists to replace.
	 *
	 * ⚠ GAME THREAD ONLY. GSuppressedTotal above is atomic, so somebody already recognised this hook
	 * runs on more than one thread — but a TSet is not atomic and the old version left it unguarded.
	 * Not theoretical for THIS hook: its own filter is IsA<AFGBuildable>, and buildables are exactly
	 * what the multithreaded Factory Tick ticks. Two workers racing Add() corrupt the set's storage.
	 * CSS's own warning names the shape — calling a main-thread-only function from Factory Tick "could
	 * at first look like it's working", until the load balancer moves the tick to another thread.
	 */
	TSet<FName> GSeenClasses;
	bool bGCensusSaturationReported = false;

	/**
	 * The cap. Sixteen distinct offending buildable classes is far past the point where a human reading
	 * a log has the picture, and the whole value of the census is naming the NEXT Stats-sign-style
	 * offender, not enumerating every one.
	 */
	constexpr int32 GCensusLimit = 16;

	/**
	 * ★ THE ENGINE ASSERTION THIS GATE STANDS IN FRONT OF, reported instead of swallowed.
	 *
	 * Cancelling ProcessRemoteFunction skips its whole body, and the first thing in that body — inside
	 * `#if !UE_BUILD_SHIPPING` — is
	 *   NetDriver.cpp:7821  checkf(IsInGameThread(), TEXT("Attempted to call ProcessRemoteFunction from
	 *                       a thread other than the game thread, which is not supported. ..."))
	 * So in Development and Test builds this gate silently absorbs a hard engine check on every
	 * buildable RPC it suppresses. That is the exact shape Ant ruled out: "all errors and warnings need
	 * fixing at the source so we actually fix issues and not just quiet logs."
	 *
	 * ⚠ IT IS AN ERROR LOG AND NOT A check(), DELIBERATELY. Reproducing the engine's checkf would mean
	 * FPM crashing a build that vanilla would have crashed anyway — a crash FPM then owns, in a mod
	 * whose whole job is reading other people's crash reports. Ant plays Shipping, where the engine's
	 * own check is compiled out entirely and a log line is the only diagnostic that can exist at all.
	 * The line is UNGATED by the diag channel: a suppressed engine assertion must not be silenceable by
	 * a verbosity setting.
	 *
	 * Latched, not counted, and read-modify-write with exchange so two racing workers cannot both print.
	 */
	std::atomic<bool> bGOffThreadReported{false};
}

FFPMNoOwnerRpcGate& FFPMNoOwnerRpcGate::Get()
{
	static FFPMNoOwnerRpcGate Instance;
	return Instance;
}

void FFPMNoOwnerRpcGate::Arm()
{
	UNetDriver* Sample = GetMutableDefault<UNetDriver>();

	auto OnProcessRemoteFunction = [](auto& Scope, UNetDriver* Driver, AActor* Actor, UFunction* Function,
	                                  void* Parms, FOutParmRec* OutParms, FFrame* Stack, UObject* SubObject)
	{
		if (!Actor || !Function) { return; }

		// Multicasts return before the owning-connection check ever runs. See the header.
		if (Function->FunctionFlags & FUNC_NetMulticast) { return; }

		// Owned actor: a legitimate dispatch the engine will actually send.
		if (Actor->GetNetConnection() != nullptr) { return; }

		// Buildables only. Everything else keeps vanilla behaviour, warning included.
		if (!Actor->IsA<AFGBuildable>()) { return; }

		// Say what the engine would have said before we take away its chance to. See bGOffThreadReported.
		if (!IsInGameThread() && !bGOffThreadReported.exchange(true, std::memory_order_relaxed))
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] no-owner RPC gate: ProcessRemoteFunction was called from a NON-GAME THREAD "
				     "for %s (function %s). The engine asserts on this in non-Shipping builds "
				     "(NetDriver.cpp:7821) and this gate cancels before that assert can fire, so this "
				     "line is the only report you will get. It is a real bug in whoever dispatched the "
				     "RPC, not in the gate. Reported once per session."),
				*Actor->GetClass()->GetName(), *Function->GetName());
		}

		// The vanilla outcome — dropped — without the cost of getting there.
		Scope.Cancel();

		const uint64 Count = ++GSuppressedTotal;

		/*
		 * ONLY THE CENSUS IS GUARDED, NEVER THE CANCEL. Scope.Cancel() above must work on every thread
		 * — that is the gate's actual job, and skipping it off-thread would reopen the desync flood
		 * this hook exists to stop. Losing a log line on a worker costs nothing; losing the cancel
		 * costs the fix.
		 */
		if (IsInGameThread())
		{
			const FName ClassName = Actor->GetClass()->GetFName();

			if (GSeenClasses.Num() < GCensusLimit)
			{
				if (!GSeenClasses.Contains(ClassName))
				{
					GSeenClasses.Add(ClassName);
					UE_LOG(LogFicsitsPerformanceManager, Display,
						TEXT("[FPM] no-owner RPC gate: suppressing dispatches from %s (function %s) — "
						     "vanilla drops these anyway"),
						*ClassName.ToString(), *Function->GetName());
				}
			}
			else if (!bGCensusSaturationReported)
			{
				/*
				 * ADDED ON CARRY. The old census stopped at the cap and said nothing, so a stack with
				 * twenty offending classes showed sixteen and read as complete. This gate SUPPRESSES an
				 * engine diagnostic, so its replacement going quiet without saying so is the worst
				 * shape available: a silent truncation that looks like full coverage.
				 */
				bGCensusSaturationReported = true;
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] no-owner RPC gate: census FULL at %d classes — further offenders will "
					     "be suppressed but NOT named. The list above is not the complete set."),
					GCensusLimit);
			}
		}

		/*
		 * ⚠ A BARE LITERAL, AND A STATED EXCEPTION TO THE FPMLog POLICY RATHER THAN AN OVERSIGHT.
		 * Flagged by review 2026-08-09. FPMLog offers Routine=200 and Notable=50, and its header says a
		 * third tier is DELIBERATELY absent — so adding one to fit this site would break a stated freeze.
		 * This gate suppresses dispatches at a rate no other fix approaches (millions per session on a busy
		 * factory), where even Routine would produce thousands of lines. The divisor still encodes how
		 * expected the event is, which is the policy's actual rule; it is simply an order of magnitude the
		 * two named tiers do not cover. Named here so it is an exception on the record, not a stray number.
		 */
		constexpr uint64 ThrottleFlood = 100000;
		if (Count == 1 || (Count % ThrottleFlood) == 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] no-owner RPC gate: %llu buildable dispatches suppressed this session"), Count);
		}
	};

	ProcessRemoteFunctionHandle = FPM_SUBSCRIBE_VIRTUAL("no-owner-rpc-gate", UNetDriver::ProcessRemoteFunction, Sample, OnProcessRemoteFunction);
}

void FFPMNoOwnerRpcGate::Disarm()
{
	/*
	 * UNSUBSCRIBE_METHOD is correct for a _VIRTUAL subscribe: both drive the same
	 * HookInvoker<decltype(&M), &M>, and RemoveHandler clears the BEFORE and AFTER maps
	 * alike, uninstalling the detour once both are empty (NativeHookManager.h:359-378).
	 *
	 * ⚠ Guarded on IsValid() because the editor path installs nothing and returns an
	 * invalid handle; RemoveHandler would then walk maps SML never allocated.
	 */
	if (ProcessRemoteFunctionHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UNetDriver::ProcessRemoteFunction, ProcessRemoteFunctionHandle);
		ProcessRemoteFunctionHandle.Reset();
	}
}
