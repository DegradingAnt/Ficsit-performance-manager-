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

		if (Count == 1 || (Count % 100000) == 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] no-owner RPC gate: %llu buildable dispatches suppressed this session"), Count);
		}
	};

	FPM_SUBSCRIBE_VIRTUAL("no-owner-rpc-gate", UNetDriver::ProcessRemoteFunction, Sample, OnProcessRemoteFunction);
}
