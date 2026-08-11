// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMNavMeshCeiling.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"

#include "NavMesh/RecastNavMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	/**
	 * 2^19. Clears the map's measured 306,440-tile request with headroom, and stays far below the
	 * ~134M-tile point where the 64-bit dtPolyRef's per-tile bit budget would start to starve.
	 */
	constexpr int32 GFPMNavTileCeiling = 524288;

	int32 GFPMNavSeen = 0;
	int32 GFPMNavRaised = 0;
	int32 GFPMNavFailedReadback = 0;
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
	 * NO HOOK. This fix installs nothing and therefore will not appear in the hook ledger — stated here
	 * so an empty ledger row is not read as "it failed to arm". All the work happens at world load,
	 * where the placed actors exist.
	 *
	 * The old implementation ALSO wrote the six `AFG*NavMesh` CDOs here at startup. That write is
	 * deliberately NOT carried: it is what logged success while changing nothing, and keeping it would
	 * mean the boot log again contains a raise that may not have taken. If a navmesh actor is ever
	 * spawned fresh (rather than loaded from the level) and needs the default, that is a real gap — and
	 * the per-instance counters below are what would expose it, rather than a hopeful CDO write hiding it.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] navmesh ceiling ARMED (no hook; acts at world load). Vanilla caps "
		     "TileNumberHardLimit at 65536 while this map calculates 306440 tiles, which the engine "
		     "reports itself as 'serialized maxTiles (65536, 16 bits) vs calculated maxtiles (306440, 19 "
		     "bits)'. It writes the PLACED actors and reads back — the previous attempt wrote only the "
		     "class default, logged a raise, and the engine was still on 65536 twenty-two seconds later."));
}

void FFPMNavMeshCeiling::OnWorldLoad(UWorld* World)
{
	if (!World) { return; }

	GFPMNavSeen = 0;
	GFPMNavRaised = 0;
	GFPMNavFailedReadback = 0;

	/*
	 * ★ ENUMERATE BY BASE CLASS, not by a hardcoded list of six FG subclasses.
	 *
	 * A refinement over the old version, and not a cosmetic one: a hardcoded list covers exactly the
	 * navmeshes that existed when it was written. `TActorIterator<ARecastNavMesh>` covers every navmesh
	 * actually placed in THIS world, including any a mod adds — and the count it reports is a fact about
	 * the world rather than about our list.
	 */
	for (TActorIterator<ARecastNavMesh> It(World); It; ++It)
	{
		ARecastNavMesh* Nav = *It;
		if (!Nav) { continue; }

		++GFPMNavSeen;

		// PUBLIC, both read and write — RecastNavMesh.h:752. ARecastNavMesh opens with
		// GENERATED_UCLASS_BODY() at :571, and that macro ends in `public:`
		// (RecastNavMesh.generated.h:74-81), with no specifier between it and :752. No access
		// transformer is needed; an entry claiming the field was protected was removed on 2026-08-10,
		// where the `protected:` it cited at :490 belongs to the nested FNavMeshTileData::FNavData.
		const int32 Before = Nav->TileNumberHardLimit;
		if (Before >= GFPMNavTileCeiling)
		{
			// Never lower a value someone else set higher. Config or another mod may have raised it.
			continue;
		}

		Nav->TileNumberHardLimit = GFPMNavTileCeiling;

		/*
		 * ★ READ IT BACK. THIS LINE IS THE ENTIRE POINT OF THE REWRITE.
		 *
		 * Its predecessor logged `65536 -> 524288` and the engine kept using 65536. A write that is not
		 * read back is a claim, not a result, and this fix is forbidden from making that claim.
		 */
		const int32 After = Nav->TileNumberHardLimit;
		if (After != GFPMNavTileCeiling)
		{
			++GFPMNavFailedReadback;
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] navmesh ceiling: WRITE DID NOT STICK on %s — wrote %d, read back %d. The "
				     "value is being re-applied from somewhere else; do NOT treat this navmesh as raised."),
				*Nav->GetName(), GFPMNavTileCeiling, After);
			continue;
		}

		++GFPMNavRaised;

		if (FPMDiag::IsOn(FPMDiag::EChannel::NavMesh))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] navmesh ceiling: %s  TileNumberHardLimit %d -> %d (read back OK)"),
				*Nav->GetName(), Before, After);
		}
	}

	/*
	 * ⚠ ZERO ACTORS IS A FINDING, NOT SILENCE. If the world-load hook runs before the navmesh actors
	 * exist — which is the open timing question this fix refuses to assume away — the honest output is
	 * "we found none", not nothing at all. An absent log line and a no-op look identical, and that
	 * ambiguity is what let the previous version pass for working.
	 */
	if (GFPMNavSeen == 0)
	{
		/*
		 * ★ THE TWO CASES ARE SEPARATED, because measurement showed they are not the same event.
		 *
		 * The same 0.11.13 session on both machines, 2026-08-11:
		 *   SERVER — four navmeshes found and raised, "read back OK" on every one.
		 *   CLIENT — this line, at Warning.
		 *
		 * The client case is CORRECT and expected: a joined client builds no navmesh because the server
		 * owns pathfinding. The old text already guessed that ("a client may not") — and then said it at
		 * Warning level anyway, so the machine where this fix is behaving perfectly printed a warning on
		 * every single world load.
		 *
		 * That is how a log stops being read, and this mod's whole value rests on its log being worth
		 * reading. The genuinely alarming case — no navmesh on a machine that OWNS pathfinding, which
		 * would mean the hook runs too early — keeps Warning and keeps the full diagnosis.
		 */
		const bool bNavMeshExpectedHere = !World->IsNetMode(NM_Client);

		UE_CLOG(bNavMeshExpectedHere, LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] navmesh ceiling: NO ARecastNavMesh actors at world-load time on a machine that "
			     "OWNS pathfinding. The ceiling was NOT raised, and this hook probably runs before the "
			     "actors exist and needs a later phase — the nav log's 'serialized maxTiles (65536' line "
			     "will still be there if so."));

		UE_CLOG(!bNavMeshExpectedHere, LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] navmesh ceiling: no ARecastNavMesh actors, and none expected — this is a joined "
			     "client and the server owns pathfinding. Nothing to raise, and not a fault."));
		return;
	}

	/*
	 * ⚠ THE VERIFY INSTRUCTION THIS LINE USED TO CARRY WAS PROBABLY BACKWARDS. Corrected 2026-08-10
	 * against the first 0.7.0 server boot, and the correction matters more than the fix does.
	 *
	 * It read: "the engine's 'serialized maxTiles (65536, 16 bits) vs calculated maxtiles (306440'
	 * warning should be ABSENT... If it is still there, this fix did not take effect regardless of
	 * what the counts above say." The live log then showed 19 actors raised, every one read back OK,
	 * and that warning firing three times ~100 ms later — which by the instruction's own wording meant
	 * FAILURE.
	 *
	 * But read the two numbers in the warning: serialized 65536 (16 bits) vs calculated 306440
	 * (19 bits). 306440 FITS COMFORTABLY UNDER the 524288 we raise to. The mismatch is between the
	 * navmesh SAVED at 65536 and what the map now calculates — so "Recreating dtNavMesh instance" is
	 * the engine RECONCILING to the new headroom, which is what a successful raise should cause. An
	 * absent warning might instead mean the raise never happened.
	 *
	 * That is a symmetrical trap: a wrong criterion makes a WORKING fix look broken, and the next
	 * person "fixes" it into something worse. It was written into both the design doc and this log
	 * line, so it would have been believed twice.
	 *
	 * ⚠ STILL NOT SETTLED. The alternative — the engine clamps back to the serialized value and the
	 * raise is cosmetic — is not excluded. The read-back below happens after the WRITE, not after the
	 * RECREATE, so it cannot tell those apart. Board m6333090 carries what would settle it.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] navmesh ceiling: %d actor(s) seen, %d raised to %d, %d write(s) did not stick. "
		     "⚠ READ THE ENGINE'S OWN LINE, DO NOT ASSUME ITS MEANING: 'serialized maxTiles (65536, 16 "
		     "bits) vs calculated maxtiles (306440, 19 bits)'. 306440 fits under %d, so 'Recreating "
		     "dtNavMesh instance' is most likely the engine ADOPTING the new headroom, not rejecting "
		     "it - an earlier version of this line claimed the opposite and was probably wrong. What "
		     "would actually settle it is a read-back AFTER the recreate, which this fix does not yet "
		     "do (board m6333090)."),
		GFPMNavSeen, GFPMNavRaised, GFPMNavTileCeiling, GFPMNavFailedReadback, GFPMNavTileCeiling);
}
