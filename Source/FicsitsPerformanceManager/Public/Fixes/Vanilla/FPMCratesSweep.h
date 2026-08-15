// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"

class AFGCrate;
class UWorld;
class FOutputDevice;

/**
 * ZERO-SLOT CRATE SWEEP. `Report()` (v1) counts and lists, never removes anything, and ships
 * permanently as the regression guard. `Remove()` (v2) is a guarded, confirm-gated removal built on
 * top of the same safety predicate. Read both doc blocks below before touching either function.
 *
 * ★★ RULING 7, VERBATIM (`ANT-RULINGS-2026-08-15-POST-DESIGN.md:18-42`). Ant: *"the 0 inventory crates
 * needs to die. we can delete things BUT carefully."* Her boundary, ruled as law and "not to be widened
 * by inference, convenience, or a later agent's reading of 'carefully'":
 *
 *   PERMITTED - an empty container, proven empty ATOMICALLY AT THE INSTANT OF THE DELETE, never read
 *               earlier and trusted.
 *   PERMITTED - null / dangling entries that point at nothing (the class FPMWireNullGuard already
 *               removes; `FPMWireNullGuard.h:60`: "a null hole is not a player's property").
 *   REFUSED   - anything with contents, items, characters, blueprints, ANYTHING ON THE LIVE SERVER.
 *
 * Her framing: *"if it holds nothing a player made or earned, it is not theirs to lose."* The permission
 * covers HOLES, never PROPERTY.
 *
 * ⚠⚠ THE STAGED LADDER, AND WHERE THIS FILE SITS ON IT NOW.
 *   v1 `Report()`  - count and report. NEVER removes anything. Ships permanently as the regression
 *                    guard, per the design's own staged plan - it does not get retired now that v2
 *                    exists in the same file.
 *   v2 `Remove()`  - confirm-per-run removal, built here. A JUDGEMENT CALL command: it needs a fresh
 *                    scan token, a dry run by default, and Ant's own save backup before the first real
 *                    run on any save. See its own doc block below for the full contract.
 *   v3 (NOT BUILT) - automatic removal, no human confirm. Ruled IN by Ruling 17b, but explicitly
 *                    sequenced after v2 "has run clean on her save once". Not proposed here.
 *
 * ★ THE TRAP THAT MAKES THIS DANGEROUS, AND WHY THE PREDICATE BELOW DEFEATS IT.
 *
 * An apparently-empty crate may be FULL and undisplayable. `FPM-ISSUE-REGISTER-2026-08-07.md:3702-3706`
 * (item AX5): on 3 of 3 logins, `FPMInventoryInitGuard`'s own guard caught ~70 stacks being pushed into a
 * crate whose inventory the CLIENT rendered as ZERO-SLOT. A UI-derived emptiness check would have
 * destroyed her property while believing it destroyed nothing.
 *
 * So `IsRemovable()` below never asks the UI anything. It reads `UFGInventoryComponent`'s own
 * SaveGame-backed state directly:
 *
 *   `AFGCrate::GetInventory()` - documented "cannot be null" (`FGCrate.h:87-89`).
 *   `UFGInventoryComponent::GetSizeLinear() const { return mInventoryStacks.Num(); }`
 *       (`FGInventoryComponent.h:443`) - a crate with no slots at all.
 *   `UFGInventoryComponent::IsEmpty() const` (`FGInventoryComponent.h:353-365`) - loops every
 *       `FInventoryStack` and returns false the instant one `HasItems()` (`:104-107`,
 *       `NumItems > 0 && Item.IsValid()`) - a crate WITH slots where every slot is empty.
 *
 * Both read `mInventoryStacks` directly (`FGInventoryComponent.h:653`, `UPROPERTY(SaveGame)`) - the
 * component's own live, server-authoritative state, never a cached display value. That is precisely what
 * closes the AX5 trap: the UI rendered ZERO-SLOT while the component held ~70 stacks, but these two
 * accessors would have reported the true (non-empty) state in that exact incident. Both are
 * `FORCEINLINE`, cheap, and side-effect-free. `Remove()` calls this exact same function again at the
 * instant of every delete - it never invents a second predicate.
 *
 * ★ THE CLASS WHITELIST. `AFGCrate` (`FGCrate.h:25-26`: `AFGCrate : public AFGInteractActor`) is a
 * distinct hierarchy from the player-character and blueprint-storage classes - this sweep enumerates
 * `AFGCrate` actors ONLY, by class, never by "looks like a container". A character, blueprint storage
 * component, or anything else that is not an `AFGCrate` is outside this predicate by construction and
 * can never reach it. `Remove()` inherits this whitelist unchanged - it only ever iterates the same
 * `TActorIterator<AFGCrate>` that `Report()` does.
 *
 * VIEWER ONLY - `Report()`. Installs no hook, writes no console variable, reads no ini, and - this is
 * the whole point - never calls `Destroy()` on anything. `FPM.Crates.Report` is safe to run at any time,
 * in any session, as many times as wanted.
 *
 * ⚠ REFUSES ON A DEDICATED SERVER - both `Report()` and `Remove()`. Ruling 7's boundary lists "anything
 * on the live server" as its OWN refused category, separate from and in addition to the emptiness test.
 * Even a truly empty crate sitting on the dedicated server is refused by this gate, on principle, not
 * because reading it would be unsafe. The documented workaround for the server's save is entirely
 * process, not code: stop the server, copy the save down, run this against the local copy, save, deploy
 * the save back - all while the server stays stopped.
 *
 * ⚠ `Report()` FLAGS A NON-AUTHORITATIVE CLIENT BUT STILL RUNS. `Remove()` REFUSES ONE OUTRIGHT. A pure
 * client's replicated copy of `mInventoryStacks` can lag the authority's, which is a milder echo of the
 * AX5 trap. For a read-only report that is a fact worth stating, not a reason to refuse, so `Report()`
 * still runs and says so when `World->GetNetMode() == NM_Client`. For a command that DESTROYS an actor,
 * acting on a copy that can lag the authority is exactly the AX5 trap again, so `Remove()` treats
 * `NM_Client` as "not the authority" and refuses under precondition 3 below, no exception.
 *
 * ============================================================================================
 * ★★★ v2 `Remove()` - THE SIX PRECONDITIONS. Every one is load-bearing. None is optional.
 * ============================================================================================
 *
 * 1. CONFIRM TOKEN. `Report()` now prints one alongside its count: a `%08X` CRC32
 *    (`FCrc::StrCrc32`, `Misc/Crc.h`) over the world's name, the scanned and removable counts, and
 *    every removable candidate's full path name plus crate type, in enumeration order. `Remove()`
 *    requires that exact token back as its first argument, and requires it for BOTH a dry run and a
 *    real run - there is one required workflow, not two. `Remove()` never reuses `Report()`'s old
 *    candidate list: it runs its OWN fresh scan and recomputes the token from scratch, then compares.
 *    Any change to the candidate set between the scan that printed the token and the `Remove` call that
 *    presents it - one candidate destroyed by something else, an item dropped into one so it drops out
 *    of the set, a new crate spawned, the world reloaded - changes at least one input byte and therefore
 *    the token. A token that still matches can only have come from a scan of the identical set being
 *    acted on right now; a token that does not match is refused before anything else runs. See
 *    `ComputeToken()` in the `.cpp`.
 *
 * 2. ★ ATOMIC RE-CHECK, THE MOST IMPORTANT LINE IN THE FILE. For every candidate from the fresh scan,
 *    `Remove()` re-evaluates `IsValid(Crate) && Crate->HasAuthority() && IsRemovable(Crate)` again,
 *    immediately before the `Destroy()` call, inside the same loop iteration, on the same game-thread
 *    tick as the scan that found it - a console command handler runs synchronously on the game thread
 *    with no yield back to the engine's event loop between statements, so nothing else can run between
 *    this re-check and the destroy that follows it. A candidate that passed the scan but fails the
 *    re-check is SKIPPED and COUNTED as `skipped-on-recheck`, never destroyed, never retried in the same
 *    pass. See the marked line in `Remove()` in the `.cpp` - do not remove that comment if this code
 *    moves.
 *
 * 3. AUTHORITY. `Remove()` refuses outright when `IsRunningDedicatedServer()` (see above - this is
 *    Ruling 7's own refused category, not a qualifier on emptiness) and refuses outright when
 *    `World->GetNetMode() == NM_Client` (not the authority - see above). The per-candidate re-check in
 *    precondition 2 also re-tests `Crate->HasAuthority()` on each individual actor, so even a crate that
 *    somehow disagrees with the world's own net mode is caught before its `Destroy()` call.
 *
 * 4. AUDIT LOG PER REMOVAL. Every actual destroy - never capped, never sampled - writes one line
 *    through `UE_LOG(LogFicsitsPerformanceManager, Warning, ...)` AND the console output device with:
 *    crate class (`Crate->GetClass()->GetName()`), crate type (dismantle / death / none), full path
 *    (`Crate->GetPathName()`), which predicate branch passed (`DescribePredicateBranch()` in the
 *    `.cpp` - reports `GetSizeLinear()==0` or `IsEmpty()` by name, read fresh off the crate being
 *    destroyed, not inferred), the session (`FApp::GetSessionId()`), and the timestamp
 *    (`FDateTime::UtcNow().ToIso8601()`). `FactoryGame.log` is append-only and already the durable
 *    record for every other fix in this module, so no second log file is invented here - if anything is
 *    ever lost, this is where to look.
 *
 * 5. DRY RUN IS THE DEFAULT. `FPM.Crates.Remove <token>` alone runs the full scan, the full
 *    per-candidate re-check, and reports what WOULD be removed and what WOULD be skipped - but the
 *    branch that calls `Destroy()` never executes. Only `FPM.Crates.Remove <token> confirm` - the
 *    token AND the literal second argument `confirm` - reaches the destroy branch. There is no partial
 *    or implicit form of confirmation.
 *
 * 6. COVERAGE ALWAYS. Every `Remove()` run, dry or real, ends with one line naming all four numbers:
 *    examined (every `AFGCrate` the iterator visited), eligible (how many passed `IsRemovable()` on the
 *    fresh scan), skipped-on-recheck (eligible candidates that failed the atomic re-check in precondition
 *    2), and removed (candidates actually destroyed - always `0` on a dry run, because a dry run destroys
 *    nothing). A "removed" count with no denominator is not shipped here.
 *
 * THE OPEN-AT-REMOVAL WARNING - NOT A SEVENTH PRECONDITION, NEVER A GATE. The one case this command's
 * author flagged unhandled: a crate destroyed while a player has its inventory open. Ant's ruling:
 * "Warn but proceed." `IsRemovable()` has already proven the crate empty at the instant of the destroy
 * (precondition 2), so nothing of the player's is lost - this exists only to make the case NOTICED and
 * SAID, never to refuse or skip a candidate. See `CountInteractingPlayers()` in the `.cpp` for the
 * detection (reflection over the private `AFGCrate::mInteractingPlayers`, since no public accessor
 * exists anywhere in the class hierarchy) and its own honesty rule: "cannot detect" is reported as
 * `undetectable`, never silently folded into "not open". Every real removal's audit line (precondition
 * 4) now carries an `open-at-removal=` field, and the coverage line (precondition 6) now carries a
 * `removed-while-open=` count of how many actually-removed candidates were open at that instant.
 *
 * ★ WHY THE DRY RUN SHARES THE RE-CHECK LOOP INSTEAD OF A SEPARATE, SIMPLER PATH. A dry run that only
 * echoed `Report()`'s numbers back would not tell Ant whether the re-check in precondition 2 is likely to
 * skip anything before she types `confirm` - and a second, hand-written copy of that re-check would be
 * exactly the kind of two-branches-one-bug drift this project has paid for before. So `Remove()` has one
 * loop, one re-check, one place `Destroy()` is called from - the dry run runs everything up to and
 * excluding that one call, so what it reports is what `confirm` will actually do, not an approximation
 * of it.
 */
class FFPMCratesSweep
{
public:
	/**
	 * Enumerate every `AFGCrate` in `World`, apply `IsRemovable()`, and print the result plus a confirm
	 * token for `Remove()`. NEVER removes anything. Bound to `FPM.Crates.Report`.
	 *
	 * @param World  the world to sweep. If null, the report says so and enumerates nothing.
	 * @param Ar     when non-null, the report is also written to this output device - `Display`-level
	 *               `UE_LOG` lines do not echo to the in-game console in this game, so a console-invoked
	 *               report routes through here as well as the log, matching the house pattern used by
	 *               every other `.Report` command in this module.
	 */
	static void Report(UWorld* World, FOutputDevice* Ar = nullptr);

	/**
	 * Guarded removal of zero-inventory `AFGCrate` actors. Dry run unless `Args` is exactly
	 * `{Token, "confirm"}` (case-insensitive on `confirm`) - see the six-precondition contract in the
	 * class doc block above; this function implements every one of them. Bound to `FPM.Crates.Remove`.
	 *
	 * @param World  the world to sweep and, if confirmed, remove from. Null or a dedicated server or a
	 *               non-authoritative client refuses before any scan runs.
	 * @param Args   `Args[0]` must be the exact `%08X` token a fresh `FPM.Crates.Report` just printed for
	 *               THIS candidate set - a stale or mismatched token refuses before any candidate is
	 *               touched. `Args[1]`, if present and equal to `"confirm"`, is the only thing that turns
	 *               on real deletion; its absence means a dry run, which still runs the full atomic
	 *               re-check but never calls `Destroy()`.
	 * @param Ar     when non-null, every line (including every per-removal audit line) is also written
	 *               here, same reasoning as `Report()`.
	 */
	static void Remove(UWorld* World, const TArray<FString>& Args, FOutputDevice* Ar = nullptr);

	/**
	 * ★ THE SAFETY PREDICATE. True only when `Crate`'s inventory proves itself empty by reading the
	 * component's own state directly - see the class header for the full safety proof and its file:line
	 * receipts. False (never removable) for a null crate or a crate whose inventory could not be read.
	 *
	 * Read-only. Makes no mutation, no matter the result. `Report()` and `Remove()` both call this exact
	 * function and nothing else decides removability - there is one predicate in this file, not two.
	 */
	static bool IsRemovable(const AFGCrate* Crate);
};
