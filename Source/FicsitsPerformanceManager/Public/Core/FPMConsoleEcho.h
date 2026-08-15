// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

/**
 * ★ MAKE A REPORT COMMAND ANSWER WHERE THE OPERATOR IS LOOKING.
 *
 * Ant, 2026-08-10, having typed three of FPM's own report commands into the console:
 * *"i did them and they did nothing visibly"*. All three had run. All three had written full answers.
 * Into `FactoryGame.log`, which she was not reading, while the console she typed into stayed blank.
 *
 * ══ THE MECHANISM, BECAUSE IT IS NOT OBVIOUS AND IT COST A BOOT ══
 *
 * `UE_LOG(..., Display, ...)` does NOT echo to the in-game console. A console command registered as
 * `FAutoConsoleCommand` gets no output device at all, so a command whose body is entirely `UE_LOG` is
 * invisible from the console by construction — it looks broken and is working perfectly.
 * `sf-boottest` names this exact trap, and three commands shipped with it anyway.
 *
 * ══ WHY THIS AND NOT AN `FOutputDevice*` PARAMETER ══
 *
 * The parameter is what `FPMCVarWriter::LogLedger` and `FPMUserSettingMap::LogState` already do, and it
 * is correct for them — they emit a handful of lines. The three reports that were silent emit dozens
 * between them, across branches, and threading a device through every one is a large diff whose only
 * risk is missing a line and leaving the same bug half-fixed.
 *
 * This attaches the console's device to `GLog` for the duration of the call instead, so EVERY line the
 * report writes reaches the console, including lines added later by someone who never heard of this
 * class. It fixes the category rather than the three instances.
 *
 * ⚠ IT OVER-CAPTURES, DELIBERATELY, AND HERE IS THE BOUND. While the scope is alive, anything else
 * logging on any thread also lands in the console. A report takes microseconds, so the realistic
 * over-capture is a stray line or two — and a stray line in a diagnostic dump is a far cheaper failure
 * than the one this replaces, where the answer did not appear at all.
 *
 * ⚠ NULL IS A NO-OP, not an error. Internal callers pass nullptr and keep log-only behaviour, which is
 * what the shutdown path and the automatic summaries want.
 */
struct FICSITSPERFORMANCEMANAGER_API FPMScopedConsoleEcho
{
	explicit FPMScopedConsoleEcho(class FOutputDevice* InAr);
	~FPMScopedConsoleEcho();

	/** Not copyable or movable — it owns a registration on a global for a scope. */
	FPMScopedConsoleEcho(const FPMScopedConsoleEcho&) = delete;
	FPMScopedConsoleEcho& operator=(const FPMScopedConsoleEcho&) = delete;

private:
	class FOutputDevice* Attached = nullptr;
};

/**
 * ★ THE GATE THAT MAKES AN FPM REPORT COMMAND INCAPABLE OF STOPPING THE GAME.
 *
 * Ant, 2026-08-15, live: *"hook report froze the game"*, then *"had to kill the game since it froze"*.
 *
 * ══ WHAT THE LOG SHOWS, BECAUSE THE SHAPE OF THIS GATE FOLLOWS FROM IT ══
 *
 * `FPM.Hooks.Report` ran its body 49,882 times off one keystroke. Every one of those runs printed its
 * own closing line, so they were REPEATS and not nesting, and a reentrancy flag would have stopped
 * none of them. All 4,240,126 lines carry the SAME engine frame number, 238, from 20:57:17.311 to
 * 20:58:16.149. EOS measured the same gap from outside FPM: `TickTracker Tick is delayed ...
 * MaxTickInterval=[58.949076s]`. The engine never finished that tick. It ended when she closed the
 * window, not when the work did.
 *
 * ══ WHY THE FRAME COUNTER IS THE BOUND, AND A TIMER OR A LINE CAP IS NOT ══
 *
 * A line cap bounds one run and does nothing about fifty thousand of them. A wall-clock throttle needs
 * a threshold nobody can defend, and the machine was starved at the time, so any threshold picked on a
 * healthy machine is wrong on a busy one. The frame number is the one value that separates the two
 * cases outright: a person cannot type two console commands inside one engine tick, and only a driver
 * can. So the FIRST report in a frame runs and every later one in that same frame is refused, which
 * caps the cost at one report body per tick whatever is calling it and however often.
 *
 * ⚠ THE REFUSAL IS COUNTED, NOT SWALLOWED. Printing 49,882 refusals is the same storm with a different
 * message, so only the first refusal after a successful run prints. The rest are counted, and the next
 * report that succeeds prints the total. A silent refusal would read as "the command did nothing",
 * which is the exact failure `FPMScopedConsoleEcho` above exists to remove, so it is not an option.
 *
 * ⚠ IT IS PROCESS-WIDE, ON PURPOSE. The claim is shared by every report that adopts this gate, so a
 * driver cannot get around it by alternating between two report commands.
 *
 * ══ THE ONLY WAY THIS GATE CAN HURT, STATED SO IT CAN BE ARGUED WITH ══
 *
 * Adding a refusal cannot invent output, so this gate cannot raise a false alarm. There is exactly one
 * failure direction: refusing a report somebody genuinely wanted. It needs two report commands to
 * arrive inside ONE engine tick, and typing cannot produce that.
 *
 * A console command is dispatched while the game thread processes input, which happens inside a tick.
 * Submitting one CLEARS the input line, so a second command needs its text entered again, by typing or
 * by the history key, and then a second Enter. Every one of those keystrokes is delivered by a later
 * input pass, in a later tick, which advances GFrameCounter and releases the claim. Two commands in one
 * frame is not a fast operator; it is a caller that is not a keyboard.
 *
 * ⚠ WHAT CAN STILL TRIP IT, AND WHY THAT IS ACCEPTABLE. A key bound to a chained command
 * ("FPM.Stage.Report | FPM.Lever.Report"), an Exec issued from a blueprint, or any script driving the
 * console can put two reports in one tick. The second is refused. The refusal PRINTS, it names the
 * command, and it says to run it again, so a wanted report is one keystroke from arriving and never
 * silently missing. That is the whole cost, and it is paid against a fault that made the game
 * unkillable except by closing the window.
 *
 * USE: construct it FIRST, before the echo, and return when it refuses.
 *
 *     FPMReportGate Gate(Ar, TEXT("FPM.Thing.Report"));
 *     if (Gate.IsRefused()) { return; }
 *     FPMScopedConsoleEcho Echo(&Ar);
 *     ...
 */
struct FICSITSPERFORMANCEMANAGER_API FPMReportGate
{
	FPMReportGate(class FOutputDevice& InAr, const TCHAR* InReportName);

	/** True when the caller MUST skip its report body. The reason is already on Ar and in the log. */
	bool IsRefused() const { return bRefused; }

	/** Not copyable or movable. Constructing one is what claims the frame. */
	FPMReportGate(const FPMReportGate&) = delete;
	FPMReportGate& operator=(const FPMReportGate&) = delete;

private:
	bool bRefused = false;
};

/**
 * ★ A LISTING THAT WAS CUT MUST SAY WHAT IT CUT. THE CAP IS NOT THE PROBLEM; THE SILENCE IS.
 *
 * The one-report-per-frame gate above bounds how OFTEN a report runs. It says nothing about how long
 * one run is, and three of the eleven reports it was rolled out to walk a container that grows while
 * you play. So each of those walks now stops at a ceiling.
 *
 * ⚠ AND A SILENT CEILING IS A WORSE INSTRUMENT THAN NO CEILING. A report that prints twelve rows and
 * stops reads as "there are twelve", which is a confident wrong answer of exactly the shape this
 * project keeps paying for. Two of the three walks were already capped this way before this change:
 * `FFPMStallSampler::LogReport` cut its two rankings at 12 and said nothing, and
 * `FFPMMaterialEffectProbe::LogReport` cut its caller list at 12 and said nothing.
 *
 * This builds the line that closes that hole, in one wording, so no adopting site can print a cut
 * listing and forget the half that says it was cut. The wording follows what `FFPMCratesSweep::Report`
 * and `FFPMSettingsAudit::Report` already do by hand, and adds the two things they leave out: the
 * RANGE that went missing, and where, if anywhere, a reader can still find it.
 *
 * RETURNS AN EMPTY STRING WHEN NOTHING WAS DROPPED, so a caller can print it unconditionally and get
 * no line on the normal path. Callers skip empty rather than print a blank.
 *
 * @param Shown          how many rows the caller actually printed
 * @param Total          how many rows it had available
 * @param RowNoun        what one row IS, plural, e.g. TEXT("attribution row(s)")
 * @param WhereTheRestIs one sentence saying whether the dropped rows survive anywhere else. For the
 *                       detector registry the honest answer is that they do NOT, because
 *                       `FFPMDetectorRegistry::Report` accumulates without logging.
 */
FICSITSPERFORMANCEMANAGER_API FString FPMCeilingHitLine(
	int32 Shown, int32 Total, const TCHAR* RowNoun, const TCHAR* WhereTheRestIs);
