// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMNavMeshCeiling.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"

#include "NavMesh/RecastNavMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#include "HAL/IConsoleManager.h"

namespace
{
	/**
	 * The value this fix used to write on every navmesh. Kept ONLY so the coverage measurement that was
	 * never run can still be run, under mode 2. It is not a default and must not become one again until
	 * somebody measures what it buys.
	 */
	constexpr int32 GFPMNavLegacyCeiling = 524288;

	enum : int32
	{
		FPMNavCeiling_Off       = 0,
		FPMNavCeiling_ByteSafe  = 1,
		FPMNavCeiling_Legacy    = 2,
	};

	/**
	 * ★ WHY THE DEFAULT IS "BYTE SAFE" AND NOT 524288, AND WHY THAT IS A SMALL CHANGE.
	 *
	 * Ant's ruling, 2026-08-16: "stay inside the bit width in the meantime". The engine names the unit
	 * itself, in the line it prints on this save:
	 *
	 *   "Recreating dtNavMesh instance due mismatch in number of BYTES required to store serialized
	 *    maxTiles (65536, 16 bits) vs calculated maxtiles (306440, 19 bits)"
	 *
	 * 16 bits is 2 bytes, 19 bits is 3. Writing 524288 (19 bits) onto an actor sitting at 8192 (13 bits)
	 * moves it out of the 2-byte class. Mode 1 refuses to do that.
	 *
	 * ⚠ AND IT MUST BE SAID PLAINLY, BECAUSE IT LIMITS WHAT THIS MODE CAN EVER ACHIEVE: the largest tile
	 * count inside 2 bytes is 65536, which is exactly the vanilla cap. This map calculates 306440 tiles,
	 * and 306440 CANNOT be addressed in 16 bits. So mode 1 can only bring sub-vanilla navmeshes up to
	 * vanilla parity. It cannot deliver the full-map creature pathing this fix was originally built for
	 * (see the header). Those two goals are mutually exclusive by arithmetic, not by implementation.
	 */
	TAutoConsoleVariable<int32> CVarRaiseTileCeiling(
		TEXT("FPM.NavMesh.RaiseTileCeiling"),
		FPMNavCeiling_ByteSafe,
		TEXT("0 = do not write TileNumberHardLimit at all.\n")
		TEXT("1 = raise only within the same serialization BYTE width (default; caps at 65536).\n")
		TEXT("2 = legacy 524288 on every navmesh. Crosses into the 3-byte class. For measurement only."),
		ECVF_Default);

	/**
	 * The largest tile count that needs the SAME number of bytes to index as `Current` does.
	 *
	 * Derived, not picked: bits = CeilLogTwo(Current), which reproduces the engine's own arithmetic
	 * (65536 -> 16 bits, 306440 -> 19 bits); bytes = ceil(bits / 8); the ceiling is 2^(bytes * 8).
	 *
	 *   8192  -> 13 bits -> 2 bytes -> 65536      (an 8x raise, same width)
	 *   16384 -> 14 bits -> 2 bytes -> 65536      (a  4x raise, same width)
	 *   65536 -> 16 bits -> 2 bytes -> 65536      (already at the top of its width: NO WRITE)
	 *
	 * Returns 0 for anything it cannot reason about, and the caller treats 0 as "leave it alone". A
	 * ceiling function that guesses on bad input would be a silent downgrade, which is worse than a skip.
	 *
	 * ⚠ AND IT REFUSES TO GO ABOVE THE 2-BYTE CLASS, WHICH THE WIDTH RULE ALONE WOULD NOT STOP. An actor
	 * already in the 3-byte class has a same-width ceiling of 2^24 = 16,777,216 tiles. At the 176
	 * bytes/tile slot cost recorded in the header that is ~2.9 GB of up-front allocation, per actor, for
	 * a benefit nobody has measured. No navmesh on this save starts above 65536, so that path cannot fire
	 * today, which is exactly why it would sit unnoticed until a save did. Same-width is a SAFETY rule,
	 * not a licence to grow.
	 */
	int32 FPMNavByteWidthSafeCeiling(const int32 Current)
	{
		// The top of the 2-byte class, and identically the vanilla cap. See the header for why this is
		// also the hard limit of what mode 1 can ever achieve.
		constexpr int32 MaxByteSafeCeiling = 65536;

		if (Current <= 0 || Current >= MaxByteSafeCeiling) { return 0; }

		const int32 Bits = static_cast<int32>(FMath::CeilLogTwo(static_cast<uint32>(Current)));
		const int32 WidthBits = FMath::DivideAndRoundUp(Bits, 8) * 8;

		// Current < 65536 means Bits <= 16, so WidthBits is 8 or 16 and the shift stays well inside int32.
		return FMath::Min(1 << WidthBits, MaxByteSafeCeiling);
	}

	/**
	 * How long after the raise to re-read. It only has to outlast the engine's dtNavMesh rebuild, which
	 * the 2026-08-11 server log shows firing ~100 ms after the write, so this is generous by two orders
	 * of magnitude and costs one pass over a nineteen-entry array, once, per world load.
	 */
	constexpr float GFPMNavVerifyDelaySec = 20.f;

	int32 GFPMNavSeen = 0;
	int32 GFPMNavRaised = 0;
	int32 GFPMNavFailedReadback = 0;
	int32 GFPMNavSkippedAtWidth = 0;

	/**
	 * What we wrote, per actor, so the delayed verify can check each one against ITS OWN target rather
	 * than against a global constant. The targets differ per navmesh now, and a verify that compared
	 * them all to one number would report the low-starting navmeshes as clamped when they are fine.
	 */
	TArray<TPair<TWeakObjectPtr<ARecastNavMesh>, int32>> GFPMNavWritten;
}

FFPMNavMeshCeiling& FFPMNavMeshCeiling::Get()
{
	static FFPMNavMeshCeiling Instance;
	return Instance;
}

void FFPMNavMeshCeiling::GetCounts(int32& OutSeen, int32& OutRaised, int32& OutFailedReadback)
{
	OutSeen = GFPMNavSeen;
	OutRaised = GFPMNavRaised;
	OutFailedReadback = GFPMNavFailedReadback;
}

void FFPMNavMeshCeiling::Arm()
{
	/*
	 * NO HOOK. This fix installs nothing and therefore will not appear in the hook ledger, stated here
	 * so an empty ledger row is not read as "it failed to arm". All the work happens at world load,
	 * where the placed actors exist.
	 *
	 * The old implementation ALSO wrote the six `AFG*NavMesh` CDOs here at startup. That write is
	 * deliberately NOT carried: it is what logged success while changing nothing.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] navmesh ceiling ARMED (no hook; acts at world load). Default is mode 1, which raises "
		     "TileNumberHardLimit only inside the same serialization BYTE width, so it caps at 65536, "
		     "the vanilla value, and skips any navmesh already there. FPM.NavMesh.RaiseTileCeiling 0 "
		     "disables it, 2 restores the legacy 524288 for measurement. ⚠ Mode 1 CANNOT deliver "
		     "full-map pathing: this map calculates 306440 tiles and 306440 does not fit in 16 bits."));
}

void FFPMNavMeshCeiling::OnWorldLoad(UWorld* World)
{
	if (!World) { return; }

	GFPMNavSeen = 0;
	GFPMNavRaised = 0;
	GFPMNavFailedReadback = 0;
	GFPMNavSkippedAtWidth = 0;
	GFPMNavWritten.Reset();

	const int32 Mode = CVarRaiseTileCeiling.GetValueOnGameThread();
	if (Mode == FPMNavCeiling_Off)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] navmesh ceiling: DISABLED by FPM.NavMesh.RaiseTileCeiling=0. No navmesh was "
			     "written. Say so rather than staying silent, because silence and a no-op look identical."));
		return;
	}

	/*
	 * ★ ENUMERATE BY BASE CLASS, not by a hardcoded list of six FG subclasses.
	 *
	 * A hardcoded list covers exactly the navmeshes that existed when it was written.
	 * `TActorIterator<ARecastNavMesh>` covers every navmesh actually placed in THIS world, including any
	 * a mod adds, and the count it reports is a fact about the world rather than about our list.
	 */
	for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
	{
		ARecastNavMesh* Nav = *It;
		if (!Nav) { continue; }

		++GFPMNavSeen;

		// PUBLIC, both read and write, RecastNavMesh.h:752. ARecastNavMesh opens with
		// GENERATED_UCLASS_BODY() at :571, and that macro ends in `public:`
		// (RecastNavMesh.generated.h:74-81), with no specifier between it and :752. No access
		// transformer is needed; an entry claiming the field was protected was removed on 2026-08-10,
		// where the `protected:` it cited at :490 belongs to the nested FNavMeshTileData::FNavData.
		const int32 Before = Nav->TileNumberHardLimit;

		const int32 Target = (Mode == FPMNavCeiling_Legacy)
			? GFPMNavLegacyCeiling
			: FPMNavByteWidthSafeCeiling(Before);

		if (Target <= Before)
		{
			/*
			 * Covers both reasons to leave an actor alone, and they are different facts worth counting:
			 * the actor is already at or above the target (never lower a value config or another mod
			 * set), or the byte-safe ceiling IS its current value because it already sits at the top of
			 * its width. The second is the common case here, fourteen of nineteen actors on this save.
			 */
			++GFPMNavSkippedAtWidth;
			continue;
		}

		Nav->TileNumberHardLimit = Target;

		/*
		 * ★ READ IT BACK. Its predecessor logged `65536 -> 524288` and the engine kept using 65536. A
		 * write that is not read back is a claim, not a result.
		 */
		const int32 After = Nav->TileNumberHardLimit;
		if (After != Target)
		{
			++GFPMNavFailedReadback;
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] navmesh ceiling: WRITE DID NOT STICK on %s, wrote %d, read back %d. The "
				     "value is being re-applied from somewhere else; do NOT treat this navmesh as raised."),
				*Nav->GetName(), Target, After);
			continue;
		}

		++GFPMNavRaised;
		GFPMNavWritten.Emplace(Nav, Target);

		if (FPMDiag::IsOn(FPMDiag::EChannel::NavMesh))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] navmesh ceiling: %s  TileNumberHardLimit %d -> %d (read back OK)"),
				*Nav->GetName(), Before, Target);
		}
	}

	/*
	 * ⚠ ZERO ACTORS IS A FINDING, NOT SILENCE. If the world-load hook runs before the navmesh actors
	 * exist, the honest output is "we found none", not nothing at all. An absent log line and a no-op
	 * look identical, and that ambiguity is what let the previous version pass for working.
	 */
	if (GFPMNavSeen == 0)
	{
		/*
		 * ★ THE TWO CASES ARE SEPARATED, because measurement showed they are not the same event.
		 * The same 0.11.13 session on both machines, 2026-08-11: the SERVER found four navmeshes and
		 * raised them all; the CLIENT found none. The client case is CORRECT, a joined client builds no
		 * navmesh because the server owns pathfinding, and warning about it means the machine where
		 * this fix behaves perfectly prints a warning on every world load. That is how a log stops being
		 * read. The alarming case, no navmesh on a machine that OWNS pathfinding, keeps Warning.
		 */
		const bool bNavMeshExpectedHere = !World->IsNetMode(NM_Client);

		UE_CLOG(bNavMeshExpectedHere, LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] navmesh ceiling: NO ARecastNavMesh actors at world-load time on a machine that "
			     "OWNS pathfinding. The ceiling was NOT raised, and this hook probably runs before the "
			     "actors exist and needs a later phase."));

		UE_CLOG(!bNavMeshExpectedHere, LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] navmesh ceiling: no ARecastNavMesh actors, and none expected, this is a joined "
			     "client and the server owns pathfinding. Nothing to raise, and not a fault."));
		return;
	}

	/*
	 * ★★★ DO NOT JUDGE THIS FIX BY THE ENGINE'S "Recreating dtNavMesh instance" LINE. MEASURED, AND IT
	 * CORRECTS WHAT THIS FILE USED TO SAY.
	 *
	 * Two earlier versions of this comment argued about what that line MEANS, first that its presence
	 * proved failure, then that it proved the engine adopting our headroom. Both were reasoning about a
	 * line neither of them had ever seen without our write. `FactoryGame.log.prev-preview4`, 2026-07-19,
	 * is that missing control: FPM wrote only the six CDOs that session (no `UAID` in any of its
	 * TileNumberHardLimit lines, so no placed actor was touched), and the engine still logged
	 *
	 *   "Recreating dtNavMesh instance due mismatch in number of bytes required to store serialized
	 *    maxTiles (65536, 16 bits) vs calculated maxtiles (306440, 19 bits)"
	 *
	 * twelve times across four world loads, byte-identical to the line it prints today.
	 *
	 * The comparison is SERIALIZED (what the save holds) against CALCULATED (what the map needs).
	 * `TileNumberHardLimit` is NEITHER OPERAND. So that line is not evidence about this fix in either
	 * direction, and no value we write can remove it. Anyone using its absence as an acceptance test
	 * will report a working change as broken.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] navmesh ceiling (mode %d): %d actor(s) seen, %d raised, %d already at the top of "
		     "their byte width or higher, %d write(s) did not stick. ⚠ The engine's 'Recreating dtNavMesh "
		     "instance ... serialized maxTiles vs calculated maxtiles' line is NOT a verdict on this fix: "
		     "it compares the save against the map, TileNumberHardLimit is neither operand, and it fired "
		     "12 times on 2026-07-19 when no placed actor had been written at all."),
		Mode, GFPMNavSeen, GFPMNavRaised, GFPMNavSkippedAtWidth, GFPMNavFailedReadback);

	ScheduleDelayedVerify(World);
}

void FFPMNavMeshCeiling::ScheduleDelayedVerify(UWorld* World)
{
	/*
	 * ★ THE READ-BACK AFTER THE RECREATE, the one thing that separates "the assignment landed in the
	 * UPROPERTY" from "the engine kept it".
	 *
	 * The read-back in the raise loop happens one line after the write, so it proves only the former.
	 * Moments later the engine rebuilds the dtNavMesh instance. If the value came back lower than what
	 * we wrote, the raise is cosmetic and every "read back OK" line above is certifying a write the
	 * engine discarded.
	 *
	 * WHY A TIMER AND NOT A HOOK. The rebuild exposes no completion delegate a mod can reach, and
	 * hooking Recast internals to observe one value would be far more invasive than sampling the public
	 * UPROPERTY once. The engine logs the rebuild ~100 ms after the write, so seconds are generous.
	 *
	 * ⚠ IT REPORTS A CLAMP AS A FINDING, NOT AS A FAILURE OF THIS FILE. The correct response to a clamp
	 * is to stop claiming the ceiling is raised, not to write a louder retry loop against an engine that
	 * has already decided.
	 */
	if (GFPMNavWritten.Num() == 0 || World == nullptr) { return; }

	/*
	 * Cancel a verify still pending from a previous world load before overwriting its handle. Two loads
	 * inside the delay window would otherwise leak the first ticker, which then fires against the array
	 * the SECOND load has already rebuilt and reports a verdict about actors it never wrote. Not fatal,
	 * but a verify that can report someone else's numbers is not a verify.
	 */
	if (VerifyHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(VerifyHandle);
		VerifyHandle.Reset();
	}

	VerifyHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			int32 Checked = 0, Held = 0, Clamped = 0, Gone = 0;

			for (const TPair<TWeakObjectPtr<ARecastNavMesh>, int32>& Written : GFPMNavWritten)
			{
				const ARecastNavMesh* Nav = Written.Key.Get();
				if (Nav == nullptr) { ++Gone; continue; }

				++Checked;
				// Each actor against ITS OWN target. The targets differ per navmesh under mode 1.
				if (Nav->TileNumberHardLimit >= Written.Value) { ++Held; } else { ++Clamped; }
			}

			if (Checked == 0)
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] navmesh ceiling: delayed verify found none of the %d actor(s) it wrote "
					     "still alive. They were destroyed or replaced, which means the raise did not "
					     "survive, or the world was torn down first, which does not."), Gone);
				return false;
			}

			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] navmesh ceiling DELAYED VERIFY (after the engine's dtNavMesh recreate): %d "
				     "of %d written actor(s) still alive, %d held their target, %d CLAMPED BACK."),
				Checked, Checked + Gone, Held, Clamped);

			UE_CLOG(Clamped > 0, LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   %d actor(s) CLAMPED BACK below what we wrote. The raise is COSMETIC and "
				     "every 'read back OK' line above is certifying a write the engine then discarded. "
				     "Stop claiming the ceiling is raised before writing any more code against it."),
				Clamped);

			return false;   // one shot
		}),
		GFPMNavVerifyDelaySec);
}

void FFPMNavMeshCeiling::Disarm()
{
	/*
	 * The delayed verify is a one-shot ticker, so leaving it to fire would be safe, but it would print
	 * a navmesh verdict for a fix the master switch has just reported as disarmed, and a log that
	 * contradicts FPM.Fix.List is exactly the inventory-lying failure the ledger exists to prevent.
	 *
	 * There is no hook to remove: this fix acts at world load and installs no detour.
	 */
	if (VerifyHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(VerifyHandle);
		VerifyHandle.Reset();
	}
	GFPMNavWritten.Reset();
}

void FFPMNavMeshCeiling::LogReport(FOutputDevice* Ar)
{
	int32 Seen = 0, Raised = 0, FailedReadback = 0;
	GetCounts(Seen, Raised, FailedReadback);

	const FString Line = FString::Printf(
		TEXT("[FPM] navmesh ceiling (mode %d): %d navmesh actor(s) seen · %d raised · %d already at the "
		     "top of their byte width · %d write(s) that did NOT stick on read-back. ⚠ This fix has "
		     "never been shown to change creature behaviour, and the 'Nav Data for agent ... was not "
		     "found' errors are NOT its doing: they reproduce identically when it writes nothing."),
		CVarRaiseTileCeiling.GetValueOnGameThread(),
		Seen, Raised, GFPMNavSkippedAtWidth, FailedReadback);

	if (Ar != nullptr)
	{
		Ar->Log(Line);
	}
	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
}

/*
 * `FPM.NavMesh.Report`, takes the output device so it prints in the console she is looking at as well
 * as the log. A Display-level UE_LOG alone does not echo to the in-game console, and a command that
 * answers somewhere the operator is not looking reads as a broken command.
 */
static FAutoConsoleCommandWithOutputDevice GFPMNavMeshReportCmd(
	TEXT("FPM.NavMesh.Report"),
	TEXT("Print how many navmesh actors the tile-ceiling raise has seen, raised, and failed to verify "
	     "this session."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMNavMeshCeiling::LogReport(&Ar);
	}));
