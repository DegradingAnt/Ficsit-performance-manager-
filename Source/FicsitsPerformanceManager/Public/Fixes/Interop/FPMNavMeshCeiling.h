// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Core/FPMFixContract.h"

/**
 * ★ NAVMESH TILE CEILING, raises a vanilla cap that leaves ~79% of the map without navmesh.
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
 *     306440 is exactly the register's figure, so the line corroborates the map's tile need.
 *
 * ⚠ BUT THAT LINE IS NOT A BEFORE/AFTER TEST, AND THIS COMMENT SAID IT WAS. CORRECTED 2026-08-16
 * AGAINST A CONTROL. `FactoryGame.log.prev-preview4`, 2026-07-19: FPM wrote only the six CDOs that
 * session (no `UAID` appears in any of its TileNumberHardLimit lines, so no placed actor was touched)
 * and the engine logged that same line, byte-identical, 12 times across 4 world loads. It compares the
 * SERIALIZED tile count against the CALCULATED one; `TileNumberHardLimit` is neither operand. Its
 * presence or absence says nothing about this fix, and no value we write can remove it.
 *
 * ★ WHAT ELSE THAT CONTROL SETTLES, so it is not re-litigated: the `LogCreature: Error: Nav Data for
 * agent Elite/Giraffe was not found` pair is NOT caused by this fix. It fires at identical
 * per-world-load counts (4 `registration queue full`, 3 recreates, 3 AgentAlreadySupported, 9
 * AgentNotValid, 2 creature errors) with the placed actors at VANILLA limits and with them raised to
 * 524288. The cause is that this save has 20 nav data actors against the navigation system's 16-deep
 * deferred registration queue, and Elite and Giraffe are last in level actor order. That is a vanilla
 * defect and a separate piece of work.
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
 * value, and READS IT BACK, reporting before/after per actor and a count. If it finds no actors, or a
 * write does not stick, the log says so plainly. **It is not allowed to claim a raise it cannot
 * demonstrate**, which is the single thing its predecessor got wrong.
 *
 * ⚠ THE TIMING IS THE OPEN QUESTION AND IS DELIBERATELY LEFT FALSIFIABLE. The ceiling is consumed when
 * the navmesh generator is constructed. Whether the CONSTRUCTION-phase world-load hook runs before that
 * is NOT established, and I will not assume it: if the actors are absent or the generator has already
 * read the old value, this fix's own output will show it rather than a silent no-op. One boot settles it.
 *
 * ★ HOW FAR IT RAISES, AND WHY THAT IS NOW SMALL. Ant's ruling 2026-08-16: "stay inside the bit width
 * in the meantime". `FPM.NavMesh.RaiseTileCeiling` defaults to 1, which raises each navmesh only to the
 * largest tile count needing the SAME number of bytes to index as its current value, so 8192 and 16384
 * go to 65536, and anything already at 65536 is not written at all. 0 disables the write; 2 restores the
 * legacy 524288.
 *
 * ⚠ MODE 1 CANNOT DELIVER THE PREMISE ABOVE, AND THE ARITHMETIC SAYS SO. The largest tile count inside
 * 2 bytes is 65536, which IS the vanilla cap. 306440 tiles need 19 bits, which is 3 bytes. Full-map
 * coverage and "stay inside the byte width" are mutually exclusive. Mode 1 only brings the five
 * sub-vanilla navmeshes on this save up to vanilla parity.
 *
 * ⚠ AND THE PREMISE ITSELF WAS NEVER MEASURED [HYPOTHESIS]. The original line, `LogFPM` 2026-07-19,
 * called this a "navmesh coverage fix ... full-map creature pathing; ~+40 MB slot table per active nav
 * class". That is a COST plus a goal, not an observation: nobody has ever recorded creature behaviour
 * with and without it. The ~79% figure is the clamp ratio 65536/306440, not a measurement. Mode 2 exists
 * so that measurement can still be made.
 *
 * ★ WHY RAISING IT IS MEMORY-SAFE (carried from the old analysis, which is sound and worth keeping):
 *  - only the empty tile SLOT table is allocated up front, at 176 bytes/tile, ~51 MB per navmesh actor
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
	 * none simply reports zero, which is information, not a failure. Gating it off would mean the
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

	/** `FPM.NavMesh.Report` prints these. A guard that has never fired must look like one. */
	static void LogReport(class FOutputDevice* Ar = nullptr);

	/** Drops a pending delayed verify, so a disarm cannot leave a ticker firing into a dead world. */
	virtual void Disarm() override;

private:
	/**
	 * ★ THE READ-BACK AFTER THE ENGINE'S RECREATE, board m6333090's stated decider.
	 *
	 * The read-back inside the raise loop happens one line after the write, so it proves the assignment
	 * landed in the UPROPERTY and nothing more. The engine then rebuilds the dtNavMesh instance, and the
	 * value either survives that rebuild or is clamped back, in which case every "read back OK" line is
	 * certifying a write the engine discarded. Only a later read separates them, and until this existed
	 * the fix could not tell you which one it was, while logging twenty confident successes.
	 *
	 * Each actor is checked against the target WE WROTE FOR IT, not against a global constant: under
	 * mode 1 the targets differ per navmesh, and one shared number would report the low-starting
	 * navmeshes as clamped when they are fine.
	 */
	void ScheduleDelayedVerify(UWorld* World);

	FTSTicker::FDelegateHandle VerifyHandle;
};
