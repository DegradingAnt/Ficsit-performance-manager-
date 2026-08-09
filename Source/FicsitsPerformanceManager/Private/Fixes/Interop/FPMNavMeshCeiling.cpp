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
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] navmesh ceiling: NO ARecastNavMesh actors present at world-load time. The ceiling "
			     "was NOT raised. Either this world builds no navmesh (a client may not), or this hook "
			     "runs before the actors exist and needs a later phase — the nav log's 'serialized "
			     "maxTiles (65536' line will still be there if so."));
		return;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] navmesh ceiling: %d actor(s) seen, %d raised to %d, %d write(s) did not stick. "
		     "VERIFY: the engine's 'serialized maxTiles (65536, 16 bits) vs calculated maxtiles (306440' "
		     "warning should be ABSENT from this session's nav log. If it is still there, this fix did "
		     "not take effect regardless of what the counts above say."),
		GFPMNavSeen, GFPMNavRaised, GFPMNavTileCeiling, GFPMNavFailedReadback);
}
