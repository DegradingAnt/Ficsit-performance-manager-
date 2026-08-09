// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "UI/FPMChatRelay.h"

#include "FicsitsPerformanceManager.h"   // LogFicsitsPerformanceManager

#include "FGChatManager.h"
#include "Async/Async.h"          // AsyncTask - hopping the ticker registration to the game thread
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

#include <atomic>

namespace
{
	/**
	 * Reentrancy guard. Posting to chat can itself log — AFGChatManager and anything it touches — and if
	 * the log mirror is armed, that log posts to chat, which logs again. Without this the FIRST mirrored
	 * line is an infinite loop that hangs the game thread. `thread_local` because the mirror can be
	 * entered from any thread.
	 */
	thread_local bool GFPMInChatWrite = false;

	/** Lines produced off the game thread, drained by FPMChatPumpQueued(). */
	FCriticalSection GFPMChatQueueLock;
	TArray<FString>  GFPMChatQueue;

	/**
	 * Flood control.
	 *
	 * ⚠ GUARDED BY ITS OWN LOCK, AND IT HAS TO BE. In the old mod these were plain globals doing an
	 * unguarded read-modify-write (`++Count > Max`) in a function reachable from ANY thread, because
	 * `FOutputDevice::Serialize` runs on whichever thread emitted the line — the very reason the queue
	 * below exists. That is a data race: undefined behaviour, and in practice a cap that miscounts under
	 * exactly the log storm it exists to survive. Found by review 2026-08-02; the fix is carried forward
	 * here rather than re-discovered.
	 *
	 * A SEPARATE lock from the queue's, so the common in-window case never contends with a drain.
	 */
	FCriticalSection GFPMChatFloodLock;
	double GFPMChatWindowStart = 0.0;
	int32  GFPMChatWindowCount = 0;
	bool   GFPMChatMuted       = false;

	constexpr int32  MaxLinesPerWindow = 12;
	constexpr double FloodWindowSec    = 5.0;
	constexpr int32  MaxQueuedLines    = 64;

	/** How often the queue drains. 0.25s is well under a human's "did that do anything?" threshold. */
	constexpr float  PumpIntervalSec   = 0.25f;

	/**
	 * ★ THE PUMP'S OWN TICKER — THE PORT'S MOST IMPORTANT REFINEMENT.
	 *
	 * In the old mod the drain was "called once per governor tick". FPM2 has no governor; that is Phase 5.
	 * Copied verbatim, `FPMChatPumpQueued()` would never have been called, every off-thread line would
	 * have queued forever and then been dropped at the 64-line cap, and the failure would have looked
	 * exactly like "chat sometimes misses lines" — the worst kind of bug to chase.
	 *
	 * So the relay owns its own drain. Started lazily on the first queued line rather than at module load,
	 * because a client that never emits an off-thread line should not carry a ticker at all.
	 */
	FTSTicker::FDelegateHandle GFPMChatPumpHandle;
	std::atomic<bool>          GFPMChatPumpStarted{ false };

	void FPMChatWriteGameThread(const FString& Line)
	{
		if (GFPMInChatWrite) { return; }

		UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
		if (!World || !World->IsGameWorld()) { return; }

		AFGChatManager* Chat = AFGChatManager::Get(World);
		if (!Chat) { return; }

		TGuardValue<bool> Reentrancy(GFPMInChatWrite, true);

		FChatMessageStruct Msg;
		Msg.MessageText   = FText::FromString(Line);

		/*
		 * SENDER NAME. "FPM", not "NOX", and this is deliberate rather than an oversight.
		 * These are DIAGNOSTIC lines. NOX is a written character with a locked voice (FPG-SCRIPT), and
		 * putting raw diagnostics under his name would either break that voice or force every warning to
		 * be written in it. When the Phase 4 UI lands, NOX-voiced player messages get their own surface.
		 * Ant owns this naming call; flagged rather than decided.
		 */
		Msg.MessageSender = FText::FromString(TEXT("FPM"));

		/*
		 * CMT_CustomMessage, never CMT_AdaMessage. ADA is the game's own voice, and borrowing it would
		 * make mod output look like vanilla content. A player debugging a performance problem needs to
		 * know at a glance which lines are ours.
		 */
		Msg.MessageType   = EFGChatMessageType::CMT_CustomMessage;

		// LOCAL ONLY — see the header. AddChatMessageToReceived does not replicate.
		Chat->AddChatMessageToReceived(Msg);
	}

	/** Queue an off-thread line, and make sure something will come along to drain it. */
	void QueueOffThread(const FString& Line)
	{
		{
			FScopeLock Lock(&GFPMChatQueueLock);
			// Bounded: an unbounded queue fed by a log storm is a memory leak with extra steps.
			if (GFPMChatQueue.Num() >= MaxQueuedLines) { return; }
			GFPMChatQueue.Add(Line);
		}

		// Start the drain exactly once. `exchange` rather than a load-then-store so two worker threads
		// queueing simultaneously cannot both register a ticker.
		if (!GFPMChatPumpStarted.exchange(true))
		{
			/*
			 * AddTicker is not documented as thread-safe, and we are on a worker thread here. Hop to the
			 * game thread to register. FTSTicker itself then calls us back on the game thread, which is
			 * where FPMChatWriteGameThread must run anyway.
			 */
			AsyncTask(ENamedThreads::GameThread, []()
			{
				GFPMChatPumpHandle = FTSTicker::GetCoreTicker().AddTicker(
					TEXT("FPMChatPump"), PumpIntervalSec,
					[](float) { FPMChatPumpQueued(); return true; });
			});
		}
	}
}

void FPMChat(const FString& Line)
{
	// A dedicated server has no local chat window to write into.
	if (IsRunningDedicatedServer()) { return; }
	if (Line.IsEmpty()) { return; }

	/*
	 * FLOOD CONTROL. The engine can emit hundreds of warnings in a burst — one recorded session logged
	 * 1805 identical inventory warnings, 1715 of them inside ONE minute — and the Phase 5 governor will
	 * add a frame-trace line every second. Mirroring that into chat buries the player's real messages and
	 * makes the feature worse than no feature. Past the cap we go quiet and say so ONCE, so silence is
	 * never mistaken for "nothing is happening".
	 */
	const double Now = FPlatformTime::Seconds();
	bool bJustMuted = false;
	{
		// The whole window update is ONE critical section: the reset, the increment and the mute flag are
		// a single decision, and splitting them is what made the old version racy.
		FScopeLock FloodLock(&GFPMChatFloodLock);
		if (Now - GFPMChatWindowStart > FloodWindowSec)
		{
			GFPMChatWindowStart = Now;
			GFPMChatWindowCount = 0;
			GFPMChatMuted       = false;
		}
		if (++GFPMChatWindowCount > MaxLinesPerWindow)
		{
			if (!GFPMChatMuted)
			{
				GFPMChatMuted = true;
				bJustMuted    = true;   // emitted BELOW, outside the lock
			}
			else
			{
				return;                 // already muted, already announced
			}
		}
	}

	if (bJustMuted)
	{
		/*
		 * The notice takes the SAME delivery path as any other line. In the old mod this was
		 * `if (IsInGameThread()) { write }` with no else, so a cap tripped from a worker thread dropped
		 * the notice on the floor and the player got silence with no explanation — precisely the outcome
		 * the comment above says this exists to prevent. Review 2026-08-02.
		 */
		const FString Notice = FString::Printf(
			TEXT("[FPM] ...muting chat output (more than %d lines in %.0fs). The full text is still in "
			     "FactoryGame.log."), MaxLinesPerWindow, FloodWindowSec);

		if (IsInGameThread()) { FPMChatWriteGameThread(Notice); }
		else                  { QueueOffThread(Notice); }
		return;   // the line that tripped the cap is itself dropped
	}

	if (IsInGameThread())
	{
		FPMChatWriteGameThread(Line);
		return;
	}

	// Off the game thread: queue it. AFGChatManager is a UObject subsystem and touching it from a worker
	// is a crash waiting for a bad frame.
	QueueOffThread(Line);
}

void FPMChatPumpQueued()
{
	if (!IsInGameThread()) { return; }

	TArray<FString> Drained;
	{
		FScopeLock Lock(&GFPMChatQueueLock);
		if (GFPMChatQueue.Num() == 0) { return; }
		Drained = MoveTemp(GFPMChatQueue);
		GFPMChatQueue.Reset();
	}
	for (const FString& Line : Drained)
	{
		FPMChatWriteGameThread(Line);
	}
}

/**
 * The log → chat mirror.
 *
 * A custom FOutputDevice registered with GLog. Serialize() is called for EVERY log line in the process,
 * on whatever thread emitted it, so it must be cheap and must filter early.
 */
class FFPMChatLogRelay : public FOutputDevice
{
public:
	/** Toggled rather than unregistered — see FPMSetChatLogMirror. Atomic: read from every log thread. */
	std::atomic<bool> bArmed{ false };

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		if (!bArmed.load(std::memory_order_relaxed)) { return; }   // cheapest reject when off

		/*
		 * ⚠ THE CATEGORY NAME IS THE PORT'S SILENT-FAILURE TRAP. The old mod compared against "LogFPM".
		 * FPM2's category is LogFicsitsPerformanceManager. Copied verbatim this would have compiled,
		 * armed, printed "chat mirror ON", and matched nothing for the rest of the mod's life.
		 * Taken from the declared category itself so a future rename cannot desync it again.
		 */
		static const FName FPMCategory = LogFicsitsPerformanceManager.GetCategoryName();
		if (Category != FPMCategory) { return; }

		if (GFPMInChatWrite) { return; }                     // never mirror our own chat write
		if (Verbosity > ELogVerbosity::Display) { return; }  // skip Verbose/VeryVerbose spam
		FPMChat(V);
	}
};

namespace
{
	FFPMChatLogRelay* GFPMLogRelay = nullptr;
}

void FPMSetChatLogMirror(bool bEnabled)
{
	if (IsRunningDedicatedServer()) { return; }

	/*
	 * REGISTERED ONCE, NEVER UNREGISTERED, NEVER DELETED — and that is the fix, not laziness.
	 *
	 * An earlier version did RemoveOutputDevice then delete, with a comment asserting that was safe
	 * because the device had been unhooked. Review 2026-08-02: unhooking stops FUTURE dispatches, but it
	 * does not drain a Serialize() already running on another thread — and Serialize runs on whichever
	 * thread emitted the line, which is this file's entire premise. Freeing the object under an in-flight
	 * call is a use-after-free whose window is exactly a log storm, i.e. the busiest possible moment.
	 *
	 * Toggling an atomic removes the question instead of answering it: no teardown, nothing to race with.
	 * The cost is one always-registered device whose first act when disarmed is a relaxed load and a
	 * return — cheaper than the category comparison it replaced at the front of the filter chain.
	 */
	if (!GFPMLogRelay)
	{
		GFPMLogRelay = new FFPMChatLogRelay();   // deliberately leaked; lives for the process
		GLog->AddOutputDevice(GFPMLogRelay);
	}

	const bool bWas = GFPMLogRelay->bArmed.exchange(bEnabled);
	if (bWas == bEnabled) { return; }            // no change, say nothing

	if (bEnabled)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] chat mirror ON. [FPM] log lines now also print to chat. 'FPM.Chat 0' turns it off."));
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] chat mirror OFF."));
	}
}

static FAutoConsoleCommand GFPMChatCmd(
	TEXT("FPM.Chat"),
	TEXT("FPM.Chat 1 - mirror [FPM] log lines into the in-game chat; FPM.Chat 0 - stop."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (IsRunningDedicatedServer()) { return; }
		const bool bOn = (Args.Num() == 0) || (Args[0] != TEXT("0"));
		FPMSetChatLogMirror(bOn);
	}));

/**
 * `FPM.Chat.Test` — proves the relay end to end from in-game, which is the only place it can be proven.
 *
 * It exists because this whole file is about the gap between "it worked" and "the player can see that it
 * worked". A relay with no self-test would have to be verified by reading the log, which is the exact
 * failure it was built to remove.
 */
static FAutoConsoleCommandWithOutputDevice GFPMChatTestCmd(
	TEXT("FPM.Chat.Test"),
	TEXT("Post one test line to chat, and report to the console whether the write path was reached."),
	FConsoleCommandWithOutputDeviceDelegate::CreateLambda([](FOutputDevice& Ar)
	{
		if (IsRunningDedicatedServer())
		{
			Ar.Log(TEXT("[FPM] chat relay is client-only; this is a dedicated server. Nothing to test."));
			return;
		}

		UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
		const bool bWorld = World && World->IsGameWorld();
		const bool bMgr   = bWorld && AFGChatManager::Get(World) != nullptr;

		FPMChat(TEXT("[FPM] chat relay test. If you can read this, player-facing output works."));

		// Reported to the CONSOLE deliberately: if the chat write failed, chat cannot tell you why.
		Ar.Logf(TEXT("[FPM] chat.test: game world=%s, AFGChatManager=%s. %s"),
			bWorld ? TEXT("yes") : TEXT("NO"),
			bMgr   ? TEXT("found") : TEXT("NOT FOUND"),
			bMgr ? TEXT("A line was posted to chat.")
			     : TEXT("No chat manager, so nothing was posted. Are you in a loaded save?"));
	}));
