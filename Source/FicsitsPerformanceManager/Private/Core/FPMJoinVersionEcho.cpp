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

	UGameInstance* GameInstance = UModNetworkHandler::GetGameInstanceFromNetDriver(Connection->GetDriver());
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

	// Re-parse the SAME message SML itself is about to parse, through the SAME public parser it uses —
	// not a reimplementation, the identical function, called a second time into a throwaway destination.
	FConnectionMetadata RemoteMeta{};
	FSMLNetworkManager::HandleModListObject(RemoteMeta, Data);
	const FVersion* RemoteVersion = RemoteMeta.InstalledRemoteMods.Find(GJoinVersionModName);

	const bool bIsClientSide = Connection->ClientLoginState == EClientLoginState::Invalid;
	const TCHAR* WeAre = bIsClientSide ? TEXT("client") : TEXT("server");
	const TCHAR* RemoteIs = bIsClientSide ? TEXT("server") : TEXT("client");

	FString AnomalyLine;
	if (RemoteVersion == nullptr)
	{
		// Rule 4: the other side's version is genuinely not available to us. Say exactly that. Never
		// infer, guess, or state a version for a side that did not send one.
		AnomalyLine = FString::Printf(
			TEXT("FPM version check (you are the %s): this side is running %s (join requires exactly %s). ")
			TEXT("The %s did not report FicsitsPerformanceManager in its mod list, so its version is NOT ")
			TEXT("known here - it may be missing FPM entirely, or running a build too old to report it. ")
			TEXT("If it is missing FPM, SML will refuse this join."),
			WeAre, *LocalInfo.Version.ToString(), *LocalInfo.RemoteVersionRange.ToString(), RemoteIs);
	}
	else if (!LocalInfo.RemoteVersionRange.Matches(*RemoteVersion))
	{
		AnomalyLine = FString::Printf(
			TEXT("FPM version check (you are the %s): this side is running %s (join requires exactly %s). ")
			TEXT("The %s reports %s. Versions do NOT match%s"),
			WeAre, *LocalInfo.Version.ToString(), *LocalInfo.RemoteVersionRange.ToString(),
			RemoteIs, *RemoteVersion->ToString(),
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
				TEXT("[FPM] join-version-echo (you are the %s): this side %s, %s %s - match."),
				WeAre, *LocalInfo.Version.ToString(), RemoteIs, *RemoteVersion->ToString());
		}
		return;
	}

	// An anomaly (mismatch or absent-remote) always reaches the operator, on both platforms, subject only
	// to the master switch — the same policy FPMOverlay::Post itself keeps (FPMDiag.h's IsSilenced doc).
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
