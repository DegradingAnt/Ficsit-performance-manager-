// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Session/FPMHostTier.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMOverlay.h"
#include "Session/FPMHostProbeSubsystem.h"
#include "Subsystem/SubsystemActorManager.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

namespace
{
	/**
	 * WHAT SML'S OWN JOIN GATE IS DOING RIGHT NOW, READ RATHER THAN ASSUMED.
	 *
	 * `SMLNetworkManager.cpp:15-20` declares `SML.SkipRemoteModListCheck` (default `GIsEditor`, so OFF
	 * in a shipped client) and `:145` reads it as `bAllowMissingMods` inside `ValidateSMLConnectionData`.
	 * While it reads 0, FPM's `"RequiredOnRemote": true` plus its exact `RemoteVersionRange` pin means a
	 * client CANNOT complete a join to a host that did not report FPM at that version. That one fact is
	 * what decides whether an absent probe replica is a statement about the HOST or about FPM ITSELF, so
	 * it is read here instead of guessed. Tri-state on purpose: "the cvar is not registered" is a third
	 * answer, not a 0, and reporting it as a 0 would be the same class of lie this file exists to remove.
	 *
	 * READ ONLY. FPM never writes another mod's cvar; the pointer is const and no setter is reachable.
	 */
	enum class EFPMRemoteModListCheck : uint8 { Enforced, Skipped, CVarNotFound };

	EFPMRemoteModListCheck ReadRemoteModListCheck()
	{
		const IConsoleVariable* Var =
			IConsoleManager::Get().FindConsoleVariable(TEXT("SML.SkipRemoteModListCheck"), false);
		if (Var == nullptr)
		{
			return EFPMRemoteModListCheck::CVarNotFound;
		}
		return Var->GetInt() != 0 ? EFPMRemoteModListCheck::Skipped : EFPMRemoteModListCheck::Enforced;
	}

	/** One line for `FPM.Status`, naming the cvar so a reader can check it without this source. */
	const TCHAR* DescribeRemoteModListCheck()
	{
		switch (ReadRemoteModListCheck())
		{
		case EFPMRemoteModListCheck::Enforced:
			return TEXT("ON (SML.SkipRemoteModListCheck=0, the shipped default)");
		case EFPMRemoteModListCheck::Skipped:
			return TEXT("OFF (SML.SkipRemoteModListCheck=1): the join gate is BYPASSED on this machine");
		default:
			return TEXT("UNKNOWN: SML.SkipRemoteModListCheck is not registered in this console");
		}
	}
}

const TCHAR* LexToString(EFPMHostTier Tier)
{
	switch (Tier)
	{
	case EFPMHostTier::Probing: return TEXT("PROBING");
	case EFPMHostTier::Full:    return TEXT("FULL");
	case EFPMHostTier::NoHostReplica: return TEXT("NO-HOST-REPLICA");
	default:                    return TEXT("<unclassified>");
	}
}

EFPMHostTier FPMClassifyHostTier(bool bSelfIsHost, bool bReplicaObserved, double ElapsedSeconds, double TimeoutSeconds)
{
	if (bSelfIsHost || bReplicaObserved)
	{
		return EFPMHostTier::Full;
	}
	return ElapsedSeconds >= TimeoutSeconds ? EFPMHostTier::NoHostReplica : EFPMHostTier::Probing;
}

FFPMHostTier& FFPMHostTier::Get()
{
	static FFPMHostTier Instance;
	return Instance;
}

void FFPMHostTier::Arm()
{
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] host-tier armed. Every world load re-probes from scratch: this machine's own host "
		     "status is KNOWN immediately; a client waits up to %.0fs for the host's replicated FPM "
		     "probe before reporting NO-HOST-REPLICA, and keeps watching afterwards for a late arrival."),
		TimeoutSeconds);

	// Same discipline as FPMCVarWriter::SelfTest and the wrist slot's GFPMWristSelfTest: the classifier
	// is proven on every boot, not only when someone types the console command.
	SelfTest(nullptr);
}

void FFPMHostTier::OnWorldLoad(UWorld* World)
{
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
		PollHandle.Reset();
	}

	Tier = EFPMHostTier::Probing;
	bVanishWarned = false;
	ProbedWorld = World;
	ProbeStartWorldSeconds = World ? World->GetTimeSeconds() : 0.0;

	if (!World)
	{
		return; // defensive; UGameWorldModule already skips the menu world (ModModules.adoc)
	}

	const bool bSelfIsHost = World->GetNetMode() != NM_Client;

	if (bSelfIsHost)
	{
		/*
		 * KNOWN, NOT PROBED. This machine IS the host (dedicated, listen, or singleplayer's own
		 * process), so there is no second machine to wait on — the server-authoritative fixes above
		 * this one in the arm order are already running right here.
		 *
		 * AND THE KNOWN-POSITIVE SELF-TEST THAT RUNS ON EVERY SUCH BOOT. RootGameWorld's
		 * DispatchLifecycleEvent calls Super::DispatchLifecycleEvent BEFORE FPMFixes::NotifyWorldLoad
		 * (RootGameWorld_FicsitsPerformanceManager.cpp) — and the base class's CONSTRUCTION handling
		 * is what registers AFPMHostProbeSubsystem and spawns it SYNCHRONOUSLY
		 * (SubsystemActorManager.cpp:11-43). So by the time this runs, an authoritative world MUST
		 * already find its own probe actor locally — if it does not, FPM's OWN registration is
		 * broken, which is a different and worse finding than "host lacks FPM" (this machine
		 * indisputably has FPM; it is running this line).
		 */
		USubsystemActorManager* Mgr = World->GetSubsystem<USubsystemActorManager>();
		const AFPMHostProbeSubsystem* Local = Mgr ? Mgr->GetSubsystemActor<AFPMHostProbeSubsystem>() : nullptr;

		bLocalAuthorityCheckRan = true;
		bLocalAuthorityCheckPassed = Local != nullptr;

		if (!bLocalAuthorityCheckPassed)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] host probe: this world is authoritative (NetMode=%d) and FPM is running, "
				     "but AFPMHostProbeSubsystem was not found locally right after registration. That "
				     "is FPM's OWN wiring failing, not a vanilla host — a remote client would see no "
				     "replica here and wrongly conclude this host lacks FPM."),
				static_cast<int32>(World->GetNetMode()));
		}

		Tier = EFPMHostTier::Full;
		ReportTierLine();
		return;
	}

	// NM_Client: start watching for the host's replica.
	PollHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FFPMHostTier::PollTick), PollIntervalSeconds);
	ReportTierLine();
}

bool FFPMHostTier::PollTick(float)
{
	UWorld* World = ProbedWorld.Get();
	if (!World)
	{
		return false; // world gone; OnWorldLoad re-arms for whatever loads next
	}

	USubsystemActorManager* Mgr = World->GetSubsystem<USubsystemActorManager>();
	const bool bReplicaPresent = Mgr && Mgr->GetSubsystemActor<AFPMHostProbeSubsystem>() != nullptr;
	const double Elapsed = World->GetTimeSeconds() - ProbeStartWorldSeconds;

	const EFPMHostTier Prev = Tier;
	const EFPMHostTier Classified =
		FPMClassifyHostTier(/*bSelfIsHost=*/false, bReplicaPresent, Elapsed, TimeoutSeconds);

	if (Prev == EFPMHostTier::Full && Classified != EFPMHostTier::Full)
	{
		/*
		 * Design §5.9: "Downgrades never happen mid-session ... the tier stays FULL with a warning
		 * line, because a vanishing subsystem mid-session is a fault to surface, not a tier change."
		 * Tier is deliberately left untouched below — only the warning fires, once.
		 */
		if (!bVanishWarned)
		{
			bVanishWarned = true;
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] host probe: the host's replica is no longer visible, %.0fs after this "
				     "session confirmed it. Tier STAYS FULL — reported as a fault, not a downgrade."),
				Elapsed);
			ReportTierLine();
		}
		return true;
	}

	if (Classified != Prev)
	{
		const bool bLateArrival = Prev == EFPMHostTier::NoHostReplica && Classified == EFPMHostTier::Full;
		Tier = Classified;
		if (bLateArrival)
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] host probe: late arrival. The host's replica appeared %.0fs after join, "
				     "%.0fs past the %.0fs budget. Session upgrades NO-HOST-REPLICA -> FULL."),
				Elapsed, Elapsed - TimeoutSeconds, TimeoutSeconds);
		}
		ReportTierLine();
	}

	// Keep watching for the life of this world: a late arrival can still land after this tick, and a
	// vanish can still happen after a late arrival.
	return true;
}

FString FFPMHostTier::ComposeTierLine() const
{
	switch (Tier)
	{
	case EFPMHostTier::Full:
	{
		const bool bIsSelfHost = ProbedWorld.IsValid() && ProbedWorld->GetNetMode() != NM_Client;
		return bIsSelfHost
			? TEXT("FULL — this machine IS the host. KNOWN (not probed): every server-authoritative "
			       "fix runs right here.")
			: TEXT("FULL — the host's FPM replica was observed via the game's own replication. KNOWN "
			       "(observed presence), not inferred from a cvar or a guess.");
	}
	case EFPMHostTier::NoHostReplica:
	{
		/*
		 * ★ THE HONESTY FIX, review 2026-08-15 HIGH 1. This branch used to open "VANILLA" and assert
		 * "The host has no FPM at all". That sentence names an input this state cannot receive on a
		 * shipped client: see the REACHABILITY block in FPMHostTier.h for the SML line numbers. The
		 * observation is that no replica arrived. WHO that indicts depends entirely on whether SML's
		 * join gate was enforced, so that is read (DescribeRemoteModListCheck, above) rather than
		 * assumed, and the three answers are kept apart instead of collapsed into the convenient one.
		 */
		TArray<FString> AnySideNames;
		for (const IFPMFix* Fix : FPMFixes::Armed())
		{
			if (Fix && Fix->Side() == EFPMFixSide::Any)
			{
				AnySideNames.Add(Fix->Name());
			}
		}

		FString Cause;
		switch (ReadRemoteModListCheck())
		{
		case EFPMRemoteModListCheck::Enforced:
			Cause = TEXT("SML's remote mod-list check is ON, and FPM is RequiredOnRemote at an exact "
			             "version pin, so this join could NOT have completed against a host without "
			             "FPM. The host is therefore NOT what failed here: what failed is FPM's OWN "
			             "probe path, on the host or in transit (registration, spawn, or replication "
			             "of AFPMHostProbeSubsystem). Report this as an FPM bug, and do not read it "
			             "as a statement about the server.");
			break;
		case EFPMRemoteModListCheck::Skipped:
			Cause = TEXT("SML's remote mod-list check is OFF on this machine "
			             "(SML.SkipRemoteModListCheck=1), which is the one configuration where a host "
			             "genuinely without FPM can be joined at all. So EITHER the host really has no "
			             "FPM, OR FPM's own probe path failed. This probe cannot separate those two "
			             "and will not guess between them.");
			break;
		default:
			Cause = TEXT("SML.SkipRemoteModListCheck is not registered in this console, so whether the "
			             "join gate was enforced is UNKNOWN here. A genuinely FPM-less host and a "
			             "failure of FPM's own probe path both remain possible, and this probe will "
			             "not guess between them.");
			break;
		}

		return FString::Printf(
			TEXT("NO HOST REPLICA: no FPM probe actor arrived from the host within %.0fs. That is the "
			     "OBSERVATION, not a verdict about the host. %s IF the host does turn out to lack FPM, "
			     "these ANY-SIDE fixes lose their server-authoritative half: %s (%d of %d currently "
			     "armed fixes); client-side-only (NeverOnDedicatedServer) fixes are unaffected either "
			     "way. This tier can still upgrade to FULL if the replica arrives late."),
			TimeoutSeconds, *Cause, *FString::Join(AnySideNames, TEXT(", ")), AnySideNames.Num(),
			FPMFixes::Armed().Num());
	}
	case EFPMHostTier::Probing:
	default:
	{
		const double Elapsed = ProbedWorld.IsValid()
			? ProbedWorld->GetTimeSeconds() - ProbeStartWorldSeconds
			: 0.0;
		return FString::Printf(
			TEXT("PROBING — waiting for the host's FPM replica (%.0fs of %.0fs budget elapsed). "
			     "UNKNOWN until the replica appears or the budget elapses; nothing about the host is "
			     "claimed yet."),
			Elapsed, TimeoutSeconds);
	}
	}
}

void FFPMHostTier::ReportTierLine() const
{
	/*
	 * `FPMOverlay::PostSticky` IS the "screen + log" call (`FPMOverlay.cpp:180-191` logs internally on
	 * every call it makes), so there is deliberately no separate `UE_LOG` here — two calls announcing
	 * the same tier would be two lines telling the same story, and the log/screen policy belongs in
	 * ONE place. Gated at the call site like every other channel (`FPMDiag.h`'s own stated rule, and
	 * the pattern `FPMStallSampler.cpp:362` already uses) rather than a duplicate manual log line.
	 *
	 * NOT skipped on a dedicated server — `FPMHitchMeter`, `FPMGCMeter` and `FPMStallSampler` all call
	 * Post/PostSticky unconditionally, because the log half fires with no viewport at all; a dedicated
	 * server needs this line in ITS OWN log just as much as a client needs it on screen.
	 */
	if (FPMDiag::IsOn(FPMDiag::EChannel::HostTier))
	{
		FPMOverlay::PostSticky(TEXT("host"), TEXT("tier"), FString::Printf(TEXT("tier: %s"), *ComposeTierLine()));
	}
}

void FFPMHostTier::Disarm()
{
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
		PollHandle.Reset();
	}
}

EFPMHostTier FFPMHostTier::CurrentTier()
{
	return Get().Tier;
}

void FFPMHostTier::ReportStatus(FOutputDevice& Ar)
{
	const FFPMHostTier& Self = Get();

	Ar.Log(TEXT("================ FPM STATUS ================"));
	Ar.Logf(TEXT("host tier    : %s"), *Self.ComposeTierLine());
	Ar.Log(TEXT("---- coverage: what this probe can and cannot see ----"));
	Ar.Logf(TEXT("  local-authority wiring self-test this session : %s"),
		Self.bLocalAuthorityCheckRan
			? (Self.bLocalAuthorityCheckPassed ? TEXT("ran, PASSED") : TEXT("ran, FAILED — see the Error above"))
			: TEXT("NOT RUN — this machine is a client; there is no local host to check itself against"));
	Ar.Log(TEXT("  this probe reports PRESENCE ONLY. It does not read or compare the host's FPM VERSION;"));
	Ar.Log(TEXT("  a version mismatch is refused earlier, at JOIN TIME, by SML's RemoteVersionRange gate."));
	Ar.Logf(TEXT("  SML remote mod-list check, read right now     : %s"), DescribeRemoteModListCheck());
	Ar.Log(TEXT("  While that check is ON, a client that COMPLETED a join is provably on a host that"));
	Ar.Log(TEXT("  reported FPM at the pinned version, so NO-HOST-REPLICA means FPM's own probe path"));
	Ar.Log(TEXT("  failed. It does NOT mean the host is vanilla; that input cannot reach this state."));
	Ar.Log(TEXT("  See the REACHABILITY block in FPMHostTier.h for the SML line numbers behind that."));
	Ar.Log(TEXT("  EXISTENCE-proven this build: the classifier and the local wiring self-test (above)."));
	Ar.Log(TEXT("  NOT execution-proven: the 30s remote timeout, the late-upgrade path, and the"));
	Ar.Log(TEXT("  mid-session vanish warning all need a real second machine to exercise for real:"));
	Ar.Log(TEXT("  a host whose FPM is present but not replicating, or a client with the gate off."));
	Ar.Log(TEXT("================ END FPM STATUS ================"));
}

bool FFPMHostTier::SelfTest(FOutputDevice* Ar)
{
	auto Emit = [Ar](const FString& Line) { if (Ar != nullptr) { Ar->Log(Line); } };

	const EFPMHostTier SelfHost   = FPMClassifyHostTier(true,  false, 0.0,  TimeoutSeconds);
	const EFPMHostTier InBudget   = FPMClassifyHostTier(false, false, 5.0,  TimeoutSeconds);
	const EFPMHostTier PastBudget = FPMClassifyHostTier(false, false, 31.0, TimeoutSeconds);
	const EFPMHostTier LateArrive = FPMClassifyHostTier(false, true,  45.0, TimeoutSeconds);

	// The known-negative is InBudget == Probing (NOT NoHostReplica) — a correct FULL host whose replica
	// has simply not arrived yet must never be misread as an absent one. LateArrive proves presence
	// always wins,
	// which is what makes the late-upgrade path in PollTick safe.
	const bool bOk =
		SelfHost   == EFPMHostTier::Full &&
		InBudget   == EFPMHostTier::Probing &&
		PastBudget == EFPMHostTier::NoHostReplica &&
		LateArrive == EFPMHostTier::Full;

	const FFPMHostTier& Self = Get();
	const TCHAR* Coverage = Self.bLocalAuthorityCheckRan
		? (Self.bLocalAuthorityCheckPassed ? TEXT("ran, PASSED") : TEXT("ran, FAILED"))
		: TEXT("not run yet this session (no authoritative world has loaded)");

	if (bOk)
	{
		const FString Line = FString::Printf(
			TEXT("[FPM] host-tier self-test PASSED: self-is-host=FULL, absent-in-budget=PROBING (not "
			     "an early NO-HOST-REPLICA — the mirror case), absent-past-budget=NO-HOST-REPLICA, "
			     "present-past-budget=FULL (late arrival wins). Local-authority wiring check this "
			     "session: %s."), Coverage);
		Emit(Line);
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
		return true;
	}

	const FString Line = FString::Printf(
		TEXT("[FPM] host-tier self-test FAILED: self-is-host=%s (want FULL), absent-in-budget=%s (want "
		     "PROBING), absent-past-budget=%s (want NO-HOST-REPLICA), present-past-budget=%s (want FULL). "
		     "Local-authority wiring check this session: %s. The tier line is UNVERIFIED this boot - "
		     "do not trust it."),
		LexToString(SelfHost), LexToString(InBudget), LexToString(PastBudget), LexToString(LateArrive),
		Coverage);
	Emit(Line);
	UE_LOG(LogFicsitsPerformanceManager, Error, TEXT("%s"), *Line);
	return false;
}

/*
 * `FPM.Status` — design §5.9's own named destination for the persistent tier line. Scoped deliberately
 * to the host tier ONLY: the design's other references to "FPM.Status" (§3.5.9's pinned-mode notice,
 * §7.3's GI-group line, and others) describe a broader running-status surface that does not exist yet
 * anywhere in this codebase (checked: zero hits for "FPM.Status" before this file). Building that whole
 * surface is not this task; claiming the name for a single-purpose command that a later change can
 * extend is the honest scope for what §5.9 actually asks this trio to ship. `FPM.Support`
 * (`Core/FPMSupport.cpp`) remains the separate, existing copy-paste diagnostic bundle.
 */
static FAutoConsoleCommandWithOutputDevice GFPMStatusCmd(
	TEXT("FPM.Status"),
	TEXT("Print the host tier (FULL/NO-HOST-REPLICA/PROBING), what it does and does not know, "
	     "including whether SML's join gate is enforced right now, and — on NO-HOST-REPLICA — "
	     "which currently armed fixes would lose their server-authoritative half IF the host did "
	     "turn out to lack FPM."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMHostTier::ReportStatus(Ar);
	}));

static FAutoConsoleCommandWithOutputDevice GFPMHostProbeSelfTestCmd(
	TEXT("FPM.HostProbe.SelfTest"),
	TEXT("Drive the host-tier classifier against a known-positive and a known-negative for every "
	     "branch, and report whether this session's local-authority wiring check ran and passed."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMHostTier::SelfTest(&Ar);
	}));
