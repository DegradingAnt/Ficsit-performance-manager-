// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMConsoleEcho.h"

#include "FicsitsPerformanceManager.h"

#include "CoreGlobals.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"

#include <atomic>

FPMScopedConsoleEcho::FPMScopedConsoleEcho(FOutputDevice* InAr)
{
	/*
	 * GLog is the process-wide redirector; adding a device makes it forward every subsequent line there
	 * too. Guarded on both pointers because a console command can legitimately be invoked with no device
	 * (the internal callers), and because GLog is null in some very early and very late phases.
	 */
	if (InAr != nullptr && GLog != nullptr)
	{
		Attached = InAr;
		GLog->AddOutputDevice(Attached);
	}
}

FPMScopedConsoleEcho::~FPMScopedConsoleEcho()
{
	/*
	 * ⚠ REMOVAL IS NOT OPTIONAL AND IT IS THE WHOLE REASON THIS IS RAII. The console's output device
	 * lives only for the duration of the command call. Leaving it registered on GLog would leave the
	 * redirector holding a dangling pointer and writing into it on the next log line from any thread —
	 * a use-after-free whose symptom would be a crash somewhere else entirely, long after the command.
	 */
	if (Attached != nullptr && GLog != nullptr)
	{
		GLog->RemoveOutputDevice(Attached);
		Attached = nullptr;
	}
}

namespace
{
	/**
	 * The engine frame whose report has already been claimed. MAX_uint64 means no report has run yet,
	 * and no real frame can hold that value, so it needs no separate "first run" flag.
	 *
	 * Atomic because a console command is dispatched on whichever thread the console lives on, and this
	 * gate must hold whether or not that is the game thread. Relaxed ordering is enough: the value
	 * guards nothing but itself.
	 */
	std::atomic<uint64> GFPMReportFrame{ MAX_uint64 };

	/** Attempts refused since the last report that actually ran. Printed by that next report. */
	std::atomic<int64> GFPMReportRefusals{ 0 };
}

/*
 * ⚠ THERE IS NO DESTRUCTOR, AND THAT IS THE DESIGN. The claim is not a lock to be released at the end
 * of the report; it is a record of which frame has already had its report. It expires when the engine
 * reaches the next frame, which is the only event that makes a second report safe.
 */
FPMReportGate::FPMReportGate(FOutputDevice& Ar, const TCHAR* ReportName)
{
	const uint64 ThisFrame = GFrameCounter;

	uint64 Claimed = GFPMReportFrame.load(std::memory_order_relaxed);
	bRefused = (Claimed == ThisFrame)
		|| !GFPMReportFrame.compare_exchange_strong(Claimed, ThisFrame, std::memory_order_relaxed);

	if (bRefused)
	{
		// ONLY THE FIRST REFUSAL TALKS. The storm this gate exists for was 49,882 calls inside one
		// frame, and one message per call is that same storm wearing a different string.
		if (GFPMReportRefusals.fetch_add(1, std::memory_order_relaxed) == 0)
		{
			// Built once and sent to both sinks. The console is where she is looking; the log is where
			// it can still be read afterwards. Neither alone has been enough on this project.
			const FString Why = FString::Printf(
				TEXT("[FPM] %s REFUSED: an FPM report already ran in engine frame %llu, and one report "
				     "per frame is a hard cap. Nothing was printed and nothing was measured. If you "
				     "typed this yourself, run it again. If it repeats without you typing it, something "
				     "is re-issuing the command inside a single tick, which is what froze the game on "
				     "2026-08-15. Further refusals are counted, not printed."),
				ReportName, static_cast<unsigned long long>(ThisFrame));

			Ar.Log(*Why);
			UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("%s"), *Why);
		}
		return;
	}

	const int64 Refused = GFPMReportRefusals.exchange(0, std::memory_order_relaxed);
	if (Refused > 0)
	{
		// SAID OUT LOUD BECAUSE A DROP NOBODY PRINTS READS AS A CLEAN RUN. The refused attempts produced
		// no output at all, so without this line the only evidence they happened is their absence.
		const FString Dropped = FString::Printf(
			TEXT("[FPM] %s: %lld earlier report attempt(s) were REFUSED by the one-per-frame cap since "
			     "the last report ran, and printed nothing. That count is how many times something asked "
			     "for a report inside a frame that had already had one."),
			ReportName, static_cast<long long>(Refused));

		Ar.Log(*Dropped);
		UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("%s"), *Dropped);
	}
}

FString FPMCeilingHitLine(int32 Shown, int32 Total, const TCHAR* RowNoun, const TCHAR* WhereTheRestIs)
{
	/*
	 * EMPTY MEANS NOTHING WAS CUT, and that is the only quiet path. Shown >= Total is the whole listing;
	 * a Total of zero or a negative count is a caller bug rather than a drop, and inventing a range for
	 * it would print a sentence with no true reading.
	 */
	if (Total <= 0 || Shown >= Total || Shown < 0)
	{
		return FString();
	}

	/*
	 * ⚠ THE RANGE IS THE POINT, NOT THE COUNT. "and 35 more" tells a reader how many rows are missing.
	 * It does not tell them WHICH, so it cannot be checked, quoted or followed up. Printing the first
	 * and last index of the dropped block makes the gap a fact with edges.
	 */
	return FString::Printf(
		TEXT("[FPM]   CEILING HIT: this listing printed 1 to %d of %d %s. Rows %d to %d were DROPPED "
		     "and are NOT in this report. %s"),
		Shown, Total, RowNoun, Shown + 1, Total, WhereTheRestIs);
}
