// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMJoinVersionEcho.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMOverlay.h"

#include "ModLoading/ModLoadingLibrary.h"
#include "Network/NetworkHandler.h"
#include "Network/SMLConnection/SMLNetworkManager.h"

#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "HAL/IConsoleManager.h"

namespace
{
	const TCHAR* const GJoinVersionModName = TEXT("FicsitsPerformanceManager");

	bool JoinVersionSay(int32 Level = 1)
	{
		return FPMDiag::IsOn(FPMDiag::EChannel::JoinVersion, Level);
	}

	/**
	 * Our own FModInfo through the live GameInstance, the same route `ValidateSMLConnectionData` uses
	 * (SMLNetworkManager.cpp:157). Never a hardcoded literal — the version pin lives ONLY in the
	 * .uplugin, and this reads whatever is actually loaded.
	 */
	bool GetLocalModInfo(UGameInstance* GameInstance, FModInfo& OutInfo)
	{
		UModLoadingLibrary* ModLoading = GameInstance ? GameInstance->GetSubsystem<UModLoadingLibrary>() : nullptr;
		return ModLoading && ModLoading->GetLoadedModInfo(GJoinVersionModName, OutInfo);
	}

	/**
	 * ★ WHICH SIDE IS THIS. Review 2026-08-15, HIGH 2.
	 *
	 * THE OLD ANSWER, AND WHY IT WAS NOT ONE: `Connection->ClientLoginState == EClientLoginState::Invalid`.
	 * That is an inference from a login state machine that exists for a different job, and nothing proved
	 * it. SML never infers the side; it passes it down from which delegate fired
	 * (SMLNetworkManager.cpp:58-64), and this hook point carries no such parameter.
	 *
	 * THE NEW ANSWER: the engine's own connection topology, read from TWO INDEPENDENT containers.
	 *   - UNetDriver::ServerConnection  (NetDriver.h:951) "Connection to the server (this net driver is a
	 *     client)" - non-null on a client driver ONLY, and equal to this connection.
	 *   - UNetDriver::ClientConnections (NetDriver.h:955) "Array of connections to clients (this net
	 *     driver is a host)" - a server driver holds every accepted connection here.
	 * A connection may appear in exactly one of them. They are maintained by different code paths, so
	 * agreement between them is real corroboration and not the same field read twice.
	 */
	enum class EFPMHandshakeSide : uint8
	{
		Client,          // this machine is the client; the connection points at the remote server
		Server,          // this machine is the host; the connection is one of our accepted clients
		Contradictory,   // neither container claims it, or both do. The side is NOT named. Counted.
	};

	/**
	 * THE PURE DECISION, split out so it can be driven with known values instead of a live join. Same
	 * discipline as FPMClassifyHostTier (Session/FPMHostTier.h) and for the same reason: this project
	 * cannot rehearse a two-machine handshake on one machine, so the part that can be proven is isolated
	 * and proven, and the part that cannot is stated.
	 */
	EFPMHandshakeSide FPMClassifySideFromFlags(bool bIsDriversServerConnection, bool bIsOneOfOurClients)
	{
		if (bIsDriversServerConnection && !bIsOneOfOurClients) { return EFPMHandshakeSide::Client; }
		if (bIsOneOfOurClients && !bIsDriversServerConnection) { return EFPMHandshakeSide::Server; }
		return EFPMHandshakeSide::Contradictory;
	}

	/** The engine lookup half. Kept apart from the decision above so the decision stays testable. */
	EFPMHandshakeSide ClassifyHandshakeSide(UNetConnection* Connection, UNetDriver* Driver)
	{
		const bool bIsDriversServerConnection = Driver->ServerConnection == Connection;
		const bool bIsOneOfOurClients = Driver->ClientConnections.ContainsByPredicate(
			[Connection](const TObjectPtr<UNetConnection>& Candidate) { return Candidate.Get() == Connection; });
		return FPMClassifySideFromFlags(bIsDriversServerConnection, bIsOneOfOurClients);
	}

	const TCHAR* LexToString(EFPMHandshakeSide Side)
	{
		switch (Side)
		{
		case EFPMHandshakeSide::Client: return TEXT("client");
		case EFPMHandshakeSide::Server: return TEXT("server");
		default:                        return TEXT("CONTRADICTORY");
		}
	}
}

FFPMJoinVersionEcho& FFPMJoinVersionEcho::Get()
{
	static FFPMJoinVersionEcho Instance;
	return Instance;
}

bool FFPMJoinVersionEcho::IsClassifierProven()
{
	return Get().bClassifierProven;
}

void FFPMJoinVersionEcho::OnWorldLoad(UWorld* World)
{
	// Once is enough — our own version and range do not change mid-session, so re-running this at every
	// world load would prove the same thing repeatedly for no reason.
	if (bSelfTestRun) { return; }
	bSelfTestRun = true;

	/*
	 * ★ THE SIDE-CLASSIFIER SELF-TEST — the proof review HIGH 2 found missing. Four known inputs against
	 * the pure decision, and BOTH impossible topologies are included as known-negatives: neither
	 * container claims the connection, and both do. This needs no world, no GameInstance and no live
	 * connection, so it runs BEFORE the FModInfo lookup below and can never be skipped by that lookup
	 * failing. If it fails, the side is never named at runtime; the version facts still are.
	 */
	const bool bSideOk =
		FPMClassifySideFromFlags(true,  false) == EFPMHandshakeSide::Client &&
		FPMClassifySideFromFlags(false, true ) == EFPMHandshakeSide::Server &&
		FPMClassifySideFromFlags(false, false) == EFPMHandshakeSide::Contradictory &&
		FPMClassifySideFromFlags(true,  true ) == EFPMHandshakeSide::Contradictory;

	bSideClassifierProven = bSideOk;

	if (!bSideOk)
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]   ⚠ JOIN VERSION ECHO SIDE CLASSIFIER IS DEAD: driver-server-connection-only "
			     "classified %s (want client), our-client-only classified %s (want server), neither "
			     "classified %s and both classified %s (want CONTRADICTORY for each). No join message "
			     "this session will name a side; the version facts are unaffected."),
			LexToString(FPMClassifySideFromFlags(true, false)),
			LexToString(FPMClassifySideFromFlags(false, true)),
			LexToString(FPMClassifySideFromFlags(false, false)),
			LexToString(FPMClassifySideFromFlags(true, true)));
	}
	else if (JoinVersionSay(2))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] join-version-echo side-classifier self-test passed: exactly one of the net "
			     "driver's two connection containers names a client side and a server side, and both "
			     "impossible topologies classify CONTRADICTORY rather than picking one."));
	}

	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	FModInfo LocalInfo;
	if (!GetLocalModInfo(GameInstance, LocalInfo))
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]   ⚠ JOIN VERSION ECHO SELF-TEST COULD NOT RUN: UModLoadingLibrary has no entry for "
			     "our own mod. The runtime handler will keep logging the raw facts it observes but will not "
			     "assert a match/mismatch verdict this session."));
		bClassifierProven = false;
		return;
	}

	// Known-MATCH: our own live version must satisfy our own live range. If it does not, either the range
	// parser or FVersionRange::Matches itself is broken — and SML's real join gate is equally broken.
	const bool bMatchOk = LocalInfo.RemoteVersionRange.Matches(LocalInfo.Version);

	// Known-MISMATCH: one patch BELOW our own version must fail. Decrementing rather than incrementing on
	// purpose — it is the probe that stays a mismatch under an exact pin ("=x.y.z") AND under an
	// open-ended one (">=x.y.z", "^x.y.z"), where a HIGHER patch would wrongly still satisfy the range.
	// Only an exact pin is shipped today (RemoteVersionRange = "=0.11.26"), but the probe should not
	// silently start lying the day that changes.
	const int64 ProbePatch = LocalInfo.Version.Patch > 0 ? LocalInfo.Version.Patch - 1 : LocalInfo.Version.Patch + 1;
	const FVersion MismatchProbe(LocalInfo.Version.Major, LocalInfo.Version.Minor, ProbePatch);
	const bool bMismatchOk = !LocalInfo.RemoteVersionRange.Matches(MismatchProbe);

	// Known-ABSENT is not re-tested here: an empty TMap::Find returning nullptr is container behaviour
	// this fix cannot break, and the runtime handler's own branch on that nullptr is what is being
	// trusted, not TMap. Stated as a checked assumption rather than silently skipped.

	bClassifierProven = bMatchOk && bMismatchOk;

	if (bClassifierProven)
	{
		if (JoinVersionSay(2))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] join-version-echo self-test passed: %s matches its own range %s, %s does not."),
				*LocalInfo.Version.ToString(), *LocalInfo.RemoteVersionRange.ToString(),
				*MismatchProbe.ToString());
		}
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM]   ⚠ JOIN VERSION ECHO CLASSIFIER IS DEAD: known-match returned %s (expected true), "
			     "known-mismatch returned %s (expected true, i.e. Matches() should have said false for %s "
			     "against range %s). FVersionRange::Matches cannot be trusted this session — SML's own join "
			     "gate reads exactly as broken, which is the far bigger problem. The runtime handler will "
			     "keep logging raw facts but will not assert a verdict."),
			bMatchOk ? TEXT("true") : TEXT("FALSE"),
			bMismatchOk ? TEXT("true") : TEXT("FALSE"),
			*MismatchProbe.ToString(), *LocalInfo.RemoteVersionRange.ToString());
	}
}

void FFPMJoinVersionEcho::Arm()
{
	// Named first, not left as the macro's inline argument — FPMHookLedger.h's own warning: the handler
	// must not contain a top-level comma, and naming it here removes the question entirely.
	auto OnMessage = [](UNetConnection* Connection, FString Data)
	{
		FFPMJoinVersionEcho::Get().HandleHandshakeMessage(Connection, Data);
	};

	JoinHandle = FPM_SUBSCRIBE_AFTER("join-version-echo", FSMLNetworkManager::HandleMessageReceived, OnMessage);

	if (JoinHandle.IsValid())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] join-version-echo armed - observes the SML mod-list handshake on both sides and "
			     "names both versions ahead of a version-pin refusal. Does not touch SML's own decision."));
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] join-version-echo NOT armed - hook install FAILED on "
			     "FSMLNetworkManager::HandleMessageReceived. A version-mismatch refusal this session will "
			     "carry only whatever SML itself puts on the wire, unaugmented."));
	}
}

void FFPMJoinVersionEcho::Disarm()
{
	if (JoinHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(FSMLNetworkManager::HandleMessageReceived, JoinHandle);
		JoinHandle.Reset();
	}
}

void FFPMJoinVersionEcho::HandleHandshakeMessage(UNetConnection* Connection, const FString& Data)
{
	if (!Connection || !Connection->GetDriver()) { return; }

	UNetDriver* Driver = Connection->GetDriver();

	UGameInstance* GameInstance = UModNetworkHandler::GetGameInstanceFromNetDriver(Driver);
	FModInfo LocalInfo;
	if (!GetLocalModInfo(GameInstance, LocalInfo))
	{
		if (!FPMDiag::IsSilenced())
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] join-version-echo: could not read our own FModInfo via UModLoadingLibrary for "
				     "this handshake (no GameInstance yet) - skipping this join attempt's echo."));
		}
		return;
	}

	/*
	 * ★ THE SIDE, FROM THE ENGINE'S OWN TOPOLOGY RATHER THAN A LOGIN-STATE GUESS (HIGH 2). Counted every
	 * time, agreeing or not, so FPM.JoinVersion.Report can state coverage with a denominator instead of
	 * a reassuring silence.
	 */
	++SideChecksTotal;
	const EFPMHandshakeSide Side = ClassifyHandshakeSide(Connection, Driver);
	if (Side == EFPMHandshakeSide::Contradictory)
	{
		++SideContradictions;
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] join-version-echo: this connection is claimed by NEITHER or BOTH of the net "
			     "driver's two connection containers (UNetDriver::ServerConnection and "
			     "UNetDriver::ClientConnections), so which end this machine is cannot be established. "
			     "The version facts below are still exact; the SIDE is left unnamed, because naming it "
			     "would be the guess this fix exists to avoid."));
	}

	// bSideClassifierProven is the fail-closed half: a dead decision function must not name a side either.
	const bool bSideKnown = bSideClassifierProven && Side != EFPMHandshakeSide::Contradictory;
	const FString SideClause = bSideKnown
		? FString::Printf(TEXT("you are the %s"), LexToString(Side))
		: FString(TEXT("FPM could NOT establish which end this machine is"));
	const FString RemoteName = bSideKnown
		? FString::Printf(TEXT("the %s"), Side == EFPMHandshakeSide::Client ? TEXT("server") : TEXT("client"))
		: FString(TEXT("the other end"));

	/*
	 * ★ THE PARSE RESULT IS A DECISION, NOT NOISE (HIGH 3). Re-parse the SAME message SML itself is about
	 * to parse, through the SAME public parser, into a throwaway destination - and KEEP THE BOOL. SML
	 * treats this exact return value as fatal and closes the connection on false
	 * (SMLNetworkManager.cpp:45-48). Discarding it made a MALFORMED MESSAGE reach the player as "the
	 * other end did not report FicsitsPerformanceManager ... it may be missing FPM entirely": a confident
	 * wrong cause, on the wrong fault, forced onto the overlay.
	 */
	FConnectionMetadata RemoteMeta{};
	const bool bParsed = FSMLNetworkManager::HandleModListObject(RemoteMeta, Data);
	const FVersion* RemoteVersion = bParsed ? RemoteMeta.InstalledRemoteMods.Find(GJoinVersionModName) : nullptr;

	FString AnomalyLine;
	if (!bParsed)
	{
		AnomalyLine = FString::Printf(
			TEXT("FPM version check (%s): the mod-list handshake message could NOT BE PARSED. This says ")
			TEXT("NOTHING about which mods %s is running. SML's own HandleModListObject returned false, ")
			TEXT("which happens when the JSON fails to deserialise, when it carries no ModList object, or ")
			TEXT("when a version string inside it does not parse (SMLNetworkManager.cpp:117-141). SML ")
			TEXT("treats that same result as fatal and closes the connection (SMLNetworkManager.cpp:45-48), ")
			TEXT("so expect this join to end. This side is running %s (join requires exactly %s); the ")
			TEXT("other end's FPM version is NOT KNOWN here, and is not guessed."),
			*SideClause, *RemoteName, *LocalInfo.Version.ToString(),
			*LocalInfo.RemoteVersionRange.ToString());
	}
	else if (RemoteVersion == nullptr)
	{
		// Rule 4: the other side's version is genuinely not available to us. Say exactly that. Never
		// infer, guess, or state a version for a side that did not send one. The message is now only
		// reached on a message that PARSED, so "did not report it" is a fact and no longer a cover for
		// a parse failure.
		AnomalyLine = FString::Printf(
			TEXT("FPM version check (%s): this side is running %s (join requires exactly %s). ")
			TEXT("The message parsed cleanly, and %s did not report FicsitsPerformanceManager in its ")
			TEXT("mod list, so its version is NOT known here - it may be missing FPM entirely, or ")
			TEXT("running a build too old to report it. If it is missing FPM, SML will refuse this join."),
			*SideClause, *LocalInfo.Version.ToString(), *LocalInfo.RemoteVersionRange.ToString(),
			*RemoteName);
	}
	else if (!LocalInfo.RemoteVersionRange.Matches(*RemoteVersion))
	{
		AnomalyLine = FString::Printf(
			TEXT("FPM version check (%s): this side is running %s (join requires exactly %s). ")
			TEXT("%s reports %s. Versions do NOT match%s"),
			*SideClause, *LocalInfo.Version.ToString(), *LocalInfo.RemoteVersionRange.ToString(),
			*RemoteName, *RemoteVersion->ToString(),
			IsClassifierProven()
				? TEXT(" - SML will refuse this join.")
				: TEXT(" (self-test did not pass this session - treat this verdict as unverified)."));
	}
	else
	{
		// Routine match. Verbose only — an ordinary successful join should not spam the screen feed.
		if (JoinVersionSay(2))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] join-version-echo (%s): this side %s, %s %s - match."),
				*SideClause, *LocalInfo.Version.ToString(), *RemoteName, *RemoteVersion->ToString());
		}
		return;
	}

	// An anomaly (unparseable, mismatched, or absent-remote) always reaches the operator, on both
	// platforms, subject only to the master switch — the same policy FPMOverlay::Post itself keeps
	// (FPMDiag.h's IsSilenced doc).
	if (!IsRunningDedicatedServer())
	{
		FPMOverlay::Post(TEXT("join-version"), AnomalyLine);   // also logs — one line, one source
		FPMOverlay::Get().SetVisible(true);
	}
	else if (!FPMDiag::IsSilenced())
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("[FPM] %s"), *AnomalyLine);
	}
}

void FFPMJoinVersionEcho::ReportNow(FOutputDevice* Ar)
{
	const FFPMJoinVersionEcho& Self = Get();
	auto Emit = [Ar](const FString& Line)
	{
		if (Ar != nullptr) { Ar->Log(Line); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
	};

	Emit(FString::Printf(TEXT("[FPM] join-version-echo: hook installed = %s"),
		Self.JoinHandle.IsValid() ? TEXT("yes") : TEXT("NO - nothing below can have been measured")));

	const TCHAR* NotRun = TEXT("NOT RUN YET (no world has loaded this session)");
	Emit(FString::Printf(TEXT("[FPM]   version classifier self-test : %s"),
		Self.bSelfTestRun
			? (Self.bClassifierProven ? TEXT("ran, PASSED") : TEXT("ran, FAILED"))
			: NotRun));
	Emit(FString::Printf(TEXT("[FPM]   side classifier self-test    : %s"),
		Self.bSelfTestRun
			? (Self.bSideClassifierProven ? TEXT("ran, PASSED") : TEXT("ran, FAILED"))
			: NotRun));

	if (Self.SideChecksTotal == 0)
	{
		Emit(TEXT("[FPM]   side-check coverage          : 0 of 0. NO HANDSHAKE HAS BEEN OBSERVED THIS "
		          "SESSION, so this is an EMPTY DENOMINATOR, not a clean bill of health."));
	}
	else
	{
		Emit(FString::Printf(
			TEXT("[FPM]   side-check coverage          : %d of %d handshakes classified without "
			     "contradiction, %d contradictory. Any non-zero contradiction count means the net "
			     "driver's two connection containers disagreed and no side was named."),
			Self.SideChecksTotal - Self.SideContradictions, Self.SideChecksTotal,
			Self.SideContradictions));
	}

	Emit(TEXT("[FPM]   EXISTENCE-proven: both self-tests above, and the hook install. NOT "
	          "execution-proven: no real two-machine handshake has ever driven either classifier."));
}

static FAutoConsoleCommandWithOutputDevice GFPMJoinVersionReportCmd(
	TEXT("FPM.JoinVersion.Report"),
	TEXT("Join version echo: both classifier self-tests, and the side-check coverage with its own "
	     "denominator (handshakes classified, of which contradictory)."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMJoinVersionEcho::ReportNow(&Ar);
	}));
