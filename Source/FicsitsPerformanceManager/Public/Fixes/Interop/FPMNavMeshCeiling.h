// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * ★ NAVMESH TILE CEILING — raises a vanilla cap that leaves ~79% of the map without navmesh.
 * Design P3.4.
 *
 * ★ THE PREMISE, RECEIPTED THREE WAYS (and I got this wrong once by grepping for the wrong string).
 *
 *  1. Register R5, under "what is genuinely NOT ours":
 *       navmesh coverage (`TileNumberHardLimit=65536` against 306,440 tiles needed)
 *  2. The old mod's own analysis: CSS caps `ARecastNavMesh::TileNumberHardLimit` at 65536
 *     (`Config/DefaultEngine.ini:374`); this map needs 306,440 tiles for full bounds, so
 *     `RecastNavMeshGenerator.cpp:5542` clamps → only ~21% of the map has navmesh → creatures cannot
 *     path across the other ~79%.
 *  3. ★ THE ENGINE SAYS IT ITSELF, in the live DatHost server log, 3x per session:
 *       "Recreating dtNavMesh instance due mismatch in number of bytes required to store serialized
 *        maxTiles (65536, 16 bits) vs calculated maxtiles (306440, 19 bits)"
 *     306440 is exactly the register's figure. This line — not `Navmesh bounds are too large`, which
 *     occurs ZERO times in 87.8 MB — is the correct liveness signal and the correct before/after test.
 *
 * ⚠ WHY A STRAIGHT PORT WOULD SHIP A LIE, MEASURED FROM THE LOG.
 * The old implementation wrote the CLASS DEFAULT on six `AFG*NavMesh` CDOs at StartupModule and logged
 * `FGDefaultNavMesh: TileNumberHardLimit 65536 -> 524288` for each. Twenty-two seconds later the engine
 * still reported `serialized maxTiles (65536, 16 bits)`. Its own caveat had predicted exactly this:
 *
 *     "if a placed navmesh actor in the level carries its OWN serialized TileNumberHardLimit, that
 *      instance value beats the CDO and this no-ops. BOOT-VERIFY the tile count in the nav log; if
 *      still clamped ~65536, add a per-instance write-back."
 *
 * The predicate held and nobody checked. That is Ant's "the old mod no-oped on the server", and it is
 * the project's named worst case: a log line asserting a fix the code does not perform.
 *
 * ★ SO THIS ONE WRITES THE PLACED INSTANCES, AND REPORTS WHAT IT ACTUALLY DID.
 * At world load it walks the live `ARecastNavMesh` actors, reads each one's ceiling, writes the raised
 * value, and READS IT BACK — reporting before/after per actor and a count. If it finds no actors, or a
 * write does not stick, the log says so plainly. **It is not allowed to claim a raise it cannot
 * demonstrate**, which is the single thing its predecessor got wrong.
 *
 * ⚠ THE TIMING IS THE OPEN QUESTION AND IS DELIBERATELY LEFT FALSIFIABLE. The ceiling is consumed when
 * the navmesh generator is constructed. Whether the CONSTRUCTION-phase world-load hook runs before that
 * is NOT established, and I will not assume it: if the actors are absent or the generator has already
 * read the old value, this fix's own output will show it rather than a silent no-op. One boot settles it.
 *
 * ★ WHY RAISING IT IS MEMORY-SAFE (carried from the old analysis, which is sound and worth keeping):
 *  - only the empty tile SLOT table is allocated up front, at 176 bytes/tile — ~51 MB per navmesh actor
 *    at full coverage, up from ~11 MB. The array tracks the map's REAL tile need, not the ceiling, so a
 *    generous ceiling does not over-allocate.
 *  - the heavy per-tile geometry is STREAMED by World Partition, sized by the resident area around nav
 *    invokers, not by this ceiling.
 *  - `RuntimeGeneration=Dynamic`, so previously-blocked tiles generate incrementally rather than in one
 *    rebuild stall.
 *  - `dtPolyRef` is 64-bit, so a 2^19 ceiling leaves the poly-per-tile bit budget untouched.
 * `TileSizeUU` is NOT touched: changing it invalidates the tile grid and forces a full rebuild.
 */
class FFPMNavMeshCeiling final : public IFPMFix
{
public:
	static FFPMNavMeshCeiling& Get();

	virtual const TCHAR* Name() const override { return TEXT("navmesh-ceiling"); }

	/*
	 * `Any`, though only a machine that BUILDS navmesh will find actors to write. A client that generates
	 * none simply reports zero — which is information, not a failure. Gating it off would mean the
	 * dedicated server, the one machine where creature pathing matters, is the only one it could not run
	 * on if the side test were ever wrong.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * ChokePointRepair, NOT OriginNamed. The CAUSE is a vanilla cap chosen for vanilla's map size meeting
	 * a heavily-modded map that needs 4.7x more tiles. We raise the ceiling at the earliest reachable
	 * point; we do not fix why the cap exists, and calling that "origin named" would overclaim.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::ChokePointRepair; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::NavMesh; }

	virtual void Arm() override;

	/** The per-instance write-back the old author prescribed. Runs at CONSTRUCTION, while the loading
	 *  screen is up. */
	virtual void OnWorldLoad(UWorld* World) override;

	/** Actors seen, actors actually raised, and actors whose write did NOT stick on read-back. The third
	 *  number is the one that matters: it is how this fix proves it is not its predecessor. */
	static void GetCounts(int32& OutSeen, int32& OutRaised, int32& OutFailedReadback);
};
