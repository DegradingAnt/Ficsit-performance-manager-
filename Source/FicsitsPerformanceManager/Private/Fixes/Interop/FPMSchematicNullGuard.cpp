// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMSchematicNullGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMOverlay.h"

#include "FGSchematic.h"
#include "FGEventSubsystem.h"

#include "HAL/IConsoleManager.h"

#include <atomic>

/*
 * ★ THE BEHAVIOUR SWITCH, SEPARATE FROM THE DIAGNOSTIC ONE ON PURPOSE.
 *
 * `FPM.Diag.SchematicGuard` changes what this prints. This one changes what it DOES: at 0 the guard
 * still evaluates every condition and still counts, but refuses nothing — vanilla gets the call exactly
 * as it would without the mod.
 *
 * It exists because this guard's whole claim is "the crash stops", and a crash that does not happen is
 * indistinguishable from a crash that was never going to happen. One boot with this at 0 and one at 1,
 * on the same save, is the only cheap way to tell those apart. Ours, registered by this module, gone
 * when it unloads, written to no ini.
 */
static TAutoConsoleVariable<int32> CVarSchematicGuardEnabled(
	TEXT("FPM.SchematicGuard"), 1,
	TEXT("Refuse schematic access when vanilla would dereference a null event subsystem. "
	     "1 = guard (default), 0 = observe and count but change no answer. This changes BEHAVIOUR - "
	     "FPM.Diag.SchematicGuard only changes what is printed."),
	ECVF_Default);

namespace
{
	/*
	 * FIVE COUNTERS, AND THE SPLIT IS THE INSTRUMENT. The fourth and fifth are the ones that can prove
	 * this file wrong, which is why they are separate rather than folded into a total.
	 *
	 *   Calls          — every call seen. Proves the hook is live when nothing is wrong.
	 *   NullClass      — no schematic at all. Cheap, and the design asked for it.
	 *   NullCdo        — a class whose default object was never created.
	 *   RefusedEvents  — ★ THE ONE THIS FIX IS FOR: relevant events declared, event subsystem null.
	 *                    Nineteen dumps say vanilla dereferences here.
	 *   PassedEventless— ⚠ THE FALSIFIER. Null subsystem, but the schematic declares NO events, so this
	 *                    guard let it through on the reasoning in the header. If a 0x2c0 crash ever
	 *                    lands while this is non-zero, that reasoning is dead and the guard must widen.
	 */
	std::atomic<int32> GFPMSchGuardCalls{0};
	std::atomic<int32> GFPMSchGuardNullClass{0};
	std::atomic<int32> GFPMSchGuardNullCdo{0};
	std::atomic<int32> GFPMSchGuardRefusedEvents{0};
	std::atomic<int32> GFPMSchGuardPassedEventless{0};

	/**
	 * First-sighting census of refused classes, so a log names WHICH schematic rather than only how many.
	 *
	 * ⚠ GAME THREAD ONLY, like the RPC gate's. A TSet is not atomic and the counters above already say
	 * this handler is not assumed single-threaded. Losing a name off-thread costs a log line; losing the
	 * refusal would cost the fix, so the refusal is never inside this branch.
	 */
	TSet<FName> GFPMSchGuardSeen;
	bool bGFPMSchGuardCensusFull = false;

	/** Sixteen distinct broken schematics is far past the point where a reader has the picture. */
	constexpr int32 GFPMSchGuardCensusLimit = 16;
}

FFPMSchematicNullGuard& FFPMSchematicNullGuard::Get()
{
	static FFPMSchematicNullGuard Instance;
	return Instance;
}

void FFPMSchematicNullGuard::Arm()
{
	/*
	 * Signature re-derived from the header rather than from the sibling probe (sf-packfix step 2 —
	 * copying a descriptor from another file is still copying it from memory). FGSchematic.h:160:
	 *   static bool CanGiveAccessToSchematic( TSubclassOf< UFGSchematic > inClass, UObject* worldContext );
	 * public, STATIC, UFUNCTION(BlueprintPure), not virtual -> plain SUBSCRIBE_METHOD.
	 *
	 * HOOKABILITY IS PROVEN BY THE DUMPS, not inferred from the prologue: nineteen crash stacks show
	 * `FicsitPerformanceManager!HookInvokerExecutorGlobalFunction<bool (__cdecl*)(TSubclassOf<UFGSchematic>...`
	 * as a live frame, and two of them show KPrivateCodeLib's trampoline stacked beneath ours. funchook
	 * has patched this exact target in the field, repeatedly, alongside another mod.
	 */
	auto OnCanGiveAccess = [](auto& Scope, TSubclassOf<UFGSchematic> InClass, UObject* WorldContext)
	{
		/*
		 * ⚠ HOT PATH. Opening the HUB or searching the build menu enumerates the whole schematic set —
		 * 611 BlueprintGeneratedClasses whose Super is FGSchematic, counted from the game export. FPM1
		 * 0.58.54 logged and flushed here and froze the game. Everything on the common path below is
		 * pointer work; the first allocation happens only after the subsystem has already come back null.
		 */
		++GFPMSchGuardCalls;

		const bool bGuardEnabled = CVarSchematicGuardEnabled.GetValueOnAnyThread() != 0;

		// ---- 1. No schematic at all. Design-specified, cheap, and harmless to keep even though the
		//         fourteen post-0.58.52 dumps prove it is not what kills the process.
		if (!InClass)
		{
			const int32 K = ++GFPMSchGuardNullClass;
			if (bGuardEnabled) { Scope.Override(false); }

			if ((K == 1 || (K % FPMLog::ThrottleNotable) == 0)
				&& FPMDiag::IsOn(FPMDiag::EChannel::SchematicGuard))
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] schematic-null-guard: NULL class (#%d). %s. There is no reading of "
					     "\"may the player access <no schematic>\" other than no."),
					K, bGuardEnabled ? TEXT("Answered false") : TEXT("OBSERVED ONLY - FPM.SchematicGuard is 0"));
			}
			return;
		}

		// ---- 2. A real class whose default object was never created. Also design-specified, also not
		//         the observed killer, also cheap. GetDefaultObject(false) - never true: the default
		//         CREATES the CDO on demand, and a guard that mutates state to answer is not a guard.
		if (InClass->GetDefaultObject(/*bCreateIfNeeded=*/false) == nullptr)
		{
			const int32 K = ++GFPMSchGuardNullCdo;
			if (bGuardEnabled) { Scope.Override(false); }

			if (FPMDiag::IsOn(FPMDiag::EChannel::SchematicGuard))
			{
				// Unthrottled: FPM1 shipped this exact check and reported it firing ZERO times while the
				// crashes continued. Every single fire is therefore news.
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] schematic-null-guard: %s has a NULL DEFAULT OBJECT (#%d). %s. FPM1 0.58.52 "
					     "shipped this same check and it never once fired - so this line is a genuine finding, "
					     "not a routine catch."),
					*InClass->GetName(), K,
					bGuardEnabled ? TEXT("Answered false") : TEXT("OBSERVED ONLY"));
			}
			return;
		}

		/*
		 * ---- 3. A NULL WORLD CONTEXT IS A LEGITIMATE CALL PATTERN AND MUST NOT BE REFUSED.
		 *
		 * FPM1 learned this the expensive way: its first guard refused on a null world context, and Ant
		 * got *"i cant input stuff into the HUB for milestones for certain mods and even some vanilla
		 * ones. some work."* Plenty of callers pass nullptr and let vanilla resolve the world itself.
		 * We cannot evaluate the events condition without a context, so we do the only safe thing.
		 */
		if (WorldContext == nullptr)
		{
			return;
		}

		/*
		 * ---- 4. ★ THE ACTUAL CONDITION, AND THE COMMON EXIT.
		 *
		 * `GetEventSubsystem` is the world-context-taking accessor (FGEventSubsystem.h:130), so this is
		 * one call rather than a manual world resolve plus a lookup. A non-null subsystem is the
		 * overwhelmingly common case and leaves through here having cost one call and one compare.
		 *
		 * ⚠ THE LOCAL `Source/FactoryGame/Private/FGEventSubsystem.cpp` STUB RETURNS nullptr
		 * UNCONDITIONALLY, AND THAT IS NOT WHAT RUNS. Those Private/*.cpp bodies exist to produce an
		 * import library; at runtime the call binds to the retail FactoryGame module. Verified rather
		 * than assumed: FPM1 ships four live call sites of this identical `AFGSubsystem::Get` shape -
		 * AFGChatManager, AFGCircuitSubsystem, AFGBuildableSubsystem, AFGConveyorItemSubsystem - and the
		 * chat relay and circuit enumeration both demonstrably work in the field.
		 */
		if (AFGEventSubsystem::GetEventSubsystem(WorldContext) != nullptr)
		{
			return;
		}

		/*
		 * ---- 5. The subsystem is null. Does vanilla actually need it for THIS schematic?
		 *
		 * Only now do we pay for an allocation - GetRelevantEvents returns a TArray by value - and only
		 * on a path that is already abnormal.
		 */
		const TArray<EEvents> RelevantEvents = UFGSchematic::GetRelevantEvents(InClass);

		if (RelevantEvents.IsEmpty())
		{
			/*
			 * ⚠ THE FALSIFIER, AND IT IS DELIBERATELY A PASS-THROUGH.
			 *
			 * On both plausible shapes of vanilla's body - test-events-then-fetch, or fetch-then-deref
			 * inside the loop - a schematic with no relevant events never touches the subsystem, so
			 * refusing here would be over-refusal, which is the failure this guard is narrowed to avoid.
			 * But that reasoning comes from a header comment and an offset, NOT from reading vanilla.
			 *
			 * So it is counted. If a 0x2c0 crash lands while this counter is non-zero, the narrowing is
			 * wrong and the guard should refuse on a null subsystem alone.
			 */
			const int32 K = ++GFPMSchGuardPassedEventless;
			if ((K == 1 || (K % FPMLog::ThrottleNotable) == 0)
				&& FPMDiag::IsOn(FPMDiag::EChannel::SchematicGuard))
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] schematic-null-guard: %s passed through with a NULL EVENT SUBSYSTEM (#%d) "
					     "because it declares no relevant events. THIS IS THE CASE THAT CAN FALSIFY THE "
					     "GUARD: if the game dies at 0x2c0 in CanGiveAccessToSchematic after this line, the "
					     "narrowing is wrong and the guard must refuse on a null subsystem alone."),
					*InClass->GetName(), K);
			}
			return;
		}

		// ---- 6. Relevant events declared AND no subsystem to evaluate them against. This is the
		//         combination nineteen dumps died on.
		const int32 N = ++GFPMSchGuardRefusedEvents;
		if (bGuardEnabled) { Scope.Override(false); }

		if (IsInGameThread())
		{
			const FName ClassName = InClass->GetFName();
			const bool bFirstSighting = !GFPMSchGuardSeen.Contains(ClassName);

			if (bFirstSighting && GFPMSchGuardSeen.Num() < GFPMSchGuardCensusLimit)
			{
				GFPMSchGuardSeen.Add(ClassName);
			}
			else if (GFPMSchGuardSeen.Num() >= GFPMSchGuardCensusLimit && !bGFPMSchGuardCensusFull)
			{
				bGFPMSchGuardCensusFull = true;
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] schematic-null-guard: census FULL at %d classes - further schematics are "
					     "still guarded but no longer named. The list above is not the complete set."),
					GFPMSchGuardCensusLimit);
			}

			/*
			 * THE HOUSE THROTTLE POSTURE THE DESIGN ASKS FOR (P3.10(a)): first fire PER CLASS unthrottled,
			 * FPMLog divisor after, and the counter carries the true rate.
			 *
			 * The reason it is per-CLASS and not per-fire: the six dumps the design reasoned from were all
			 * FIRST fires by construction — pre-guard, every one of these killed the session, so
			 * observation was capped at one per session-death. Post-guard the session survives and the
			 * per-session volume is unbounded a priori. The first sighting of each schematic is the datum
			 * that names an origin; the rest is rate, and rate belongs in a counter.
			 */
			if ((bFirstSighting || (N % FPMLog::ThrottleNotable) == 0)
				&& FPMDiag::IsOn(FPMDiag::EChannel::SchematicGuard))
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] schematic-null-guard: %s declares %d relevant event(s) but the event "
					     "subsystem is NULL (#%d). %s. This is the EXCEPTION_ACCESS_VIOLATION at 0x2c0 in "
					     "UFGSchematic::CanGiveAccessToSchematic - 19 dumps, 12 of them in FGMainMenuState."),
					*ClassName.ToString(), RelevantEvents.Num(), N,
					bGuardEnabled
						? TEXT("Answered false instead of crashing")
						: TEXT("NOT refused - FPM.SchematicGuard is 0, so vanilla is about to dereference it"));

				FPMOverlay::Post(TEXT("schematic-guard"),
					FString::Printf(TEXT("%s refused — no event subsystem (%d total)"),
						*ClassName.ToString(), N));
			}
		}
	};

	FPM_SUBSCRIBE("schematic-null-guard", UFGSchematic::CanGiveAccessToSchematic, OnCanGiveAccess);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] schematic-null-guard ARMED on UFGSchematic::CanGiveAccessToSchematic, AFTER the probe "
		     "so the probe still counts every call. It refuses ONLY when a schematic declares relevant "
		     "events and the event subsystem is null - the combination 19 crash dumps died on. The "
		     "argument-only check the design specified shipped in FPM1 0.58.52 and 14 crashes followed it; "
		     "that check is kept here but is not what this fix rests on. FPM.SchematicGuard 0 to observe "
		     "without refusing."));
}

void FFPMSchematicNullGuard::LogStatus()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] schematic-null-guard: %d call(s) — null class %d · null CDO %d · REFUSED (events, no "
		     "subsystem) %d · passed-through eventless-with-null-subsystem %d. Guard is %s."),
		GFPMSchGuardCalls.load(), GFPMSchGuardNullClass.load(), GFPMSchGuardNullCdo.load(),
		GFPMSchGuardRefusedEvents.load(), GFPMSchGuardPassedEventless.load(),
		CVarSchematicGuardEnabled.GetValueOnAnyThread() != 0 ? TEXT("ON") : TEXT("OFF (observing only)"));

	UE_CLOG(GFPMSchGuardPassedEventless.load() > 0, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM]   ^^ the eventless count is NON-ZERO. If a 0x2c0 crash in CanGiveAccessToSchematic "
		     "has occurred on this build, the narrowing is refuted and the guard must widen to refuse on "
		     "a null event subsystem alone."));

	// A zero call-count means the hook never fired, which is a different finding from a clean run and
	// must not read as one. The probe learned this lesson for the whole project.
	UE_CLOG(GFPMSchGuardCalls.load() == 0, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM]   ^^ ZERO calls seen. Either nothing has queried schematic access yet, or the hook did "
		     "not install - check the hook ledger, not this line."));
}

/*
 * `FPM.SchematicGuard.Status` — read the counters without waiting for a crash.
 *
 * Display-level UE_LOG does not echo to the in-game console, so this is for the log and for a support
 * bundle. The overlay carries the live view for a screenshot.
 */
static FAutoConsoleCommand GFPMSchGuardStatusCmd(
	TEXT("FPM.SchematicGuard.Status"),
	TEXT("Print the schematic null-guard's counters: calls, null class, null CDO, refusals, and the "
	     "eventless pass-throughs that would falsify the guard's narrowing."),
	FConsoleCommandDelegate::CreateStatic([]() { FFPMSchematicNullGuard::LogStatus(); }));
