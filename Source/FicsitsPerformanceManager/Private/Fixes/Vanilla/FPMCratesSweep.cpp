// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Vanilla/FPMCratesSweep.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"

#include "FGCrate.h"
#include "FGInventoryComponent.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/Char.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "Misc/OutputDevice.h"
#include "Misc/Parse.h"
#include "UObject/UnrealType.h"

bool FFPMCratesSweep::IsRemovable(const AFGCrate* Crate)
{
	if (Crate == nullptr) { return false; }

	/*
	 * `GetInventory()` is documented "cannot be null" (FGCrate.h:87-89), but that is a claim the header
	 * makes, not a guarantee this file controls - checked rather than trusted blind. A defensive read
	 * costs nothing, and refusing on a null inventory (rather than treating it as "empty") is the
	 * conservative direction: this predicate is only ever used to say something MAY be removed, never
	 * to say something IS safe by default.
	 */
	UFGInventoryComponent* Inventory = Crate->GetInventory();
	if (Inventory == nullptr) { return false; }

	/*
	 * ★ THE PREDICATE FROM THE SAFETY PROOF (see FPMCratesSweep.h for the full receipts). Reads the
	 * component's own SaveGame-backed array directly, never a UI value:
	 *
	 *   GetSizeLinear() == 0   - a crate with no slots at all.
	 *   IsEmpty()              - a crate WITH slots where every slot is empty (loops FInventoryStack,
	 *                            false the instant one HasItems()).
	 *
	 * This is exactly what defeats the AX5 trap: on 3 of 3 logins, a crate the UI rendered ZERO-SLOT
	 * held ~70 stacks in this same component. Neither accessor below would have called that crate
	 * removable, because both read mInventoryStacks, not the render layer.
	 */
	return Inventory->GetSizeLinear() == 0 || Inventory->IsEmpty();
}

namespace
{
	/** One AFGCrate the sweep found removable. Report() lists it; Remove() acts on it. */
	struct FFPMCrateCandidate
	{
		TWeakObjectPtr<AFGCrate> Crate;
		FString Name;
		FString PathName;
		EFGCrateType CrateType = EFGCrateType::CT_None;
	};

	/** Full result of one enumeration pass. Report() and Remove() each build exactly one of these. */
	struct FFPMCratesScanResult
	{
		int32 Scanned = 0;
		int32 Removable = 0;
		int32 DismantleCrates = 0;
		int32 DeathCrates = 0;
		// Every removable candidate, uncapped. Remove() needs the full set, not a display-capped one.
		TArray<FFPMCrateCandidate> Candidates;
	};

	const TCHAR* CrateTypeString(EFGCrateType CrateType)
	{
		switch (CrateType)
		{
		case EFGCrateType::CT_DismantleCrate: return TEXT("dismantle");
		case EFGCrateType::CT_DeathCrate: return TEXT("death");
		default: return TEXT("none");
		}
	}

	/*
	 * ONE enumeration, shared by Report() and Remove(). A second, hand-written copy of this loop is
	 * exactly the two-branches-one-bug shape this project has paid for before - Report()'s candidate
	 * list and Remove()'s removal set must never be able to drift apart, so one function builds both.
	 */
	FFPMCratesScanResult ScanWorld(UWorld* World)
	{
		FFPMCratesScanResult Result;
		if (World == nullptr) { return Result; }

		for (TActorIterator<AFGCrate> It(World); It; ++It)
		{
			AFGCrate* Crate = *It;
			if (!IsValid(Crate)) { continue; }

			++Result.Scanned;
			if (!FFPMCratesSweep::IsRemovable(Crate)) { continue; }

			++Result.Removable;
			const EFGCrateType CrateType = Crate->GetCrateType();
			if (CrateType == EFGCrateType::CT_DismantleCrate) { ++Result.DismantleCrates; }
			else if (CrateType == EFGCrateType::CT_DeathCrate) { ++Result.DeathCrates; }

			FFPMCrateCandidate& Candidate = Result.Candidates.AddDefaulted_GetRef();
			Candidate.Crate = Crate;
			Candidate.Name = Crate->GetName();
			Candidate.PathName = Crate->GetPathName();
			Candidate.CrateType = CrateType;
		}
		return Result;
	}

	/*
	 * ★ THE TOKEN (precondition 1, FPMCratesSweep.h). CRC32 over the world's name, the scanned and
	 * removable counts, and every removable candidate's full path name plus crate type, in enumeration
	 * order. Any change to the set that produced Scan - one candidate gone, one no longer empty, one
	 * new, the world itself changed - changes at least one input byte here, so a token can only match a
	 * fresh scan of the exact same set it was computed from.
	 */
	uint32 ComputeToken(const UWorld* World, const FFPMCratesScanResult& Scan)
	{
		FString Buffer = World != nullptr ? World->GetName() : TEXT("<null>");
		Buffer += FString::Printf(TEXT("|scanned=%d|removable=%d"), Scan.Scanned, Scan.Removable);
		for (const FFPMCrateCandidate& Candidate : Scan.Candidates)
		{
			Buffer += FString::Printf(TEXT("|%s#%d"), *Candidate.PathName,
				static_cast<int32>(Candidate.CrateType));
		}
		return FCrc::StrCrc32(*Buffer);
	}

	// A malformed argument (not hex, empty) is refused with its own message, distinct from "token does
	// not match the current scan" - the two failures need different next steps from Ant.
	bool LooksLikeToken(const FString& Text)
	{
		if (Text.IsEmpty()) { return false; }
		for (const TCHAR Ch : Text)
		{
			if (!FChar::IsHexDigit(Ch)) { return false; }
		}
		return true;
	}

	/*
	 * FOR THE AUDIT LOG ONLY (precondition 4). Re-reads the same two accessors IsRemovable() reads, to
	 * name which one is true for the audit line. This function never decides removability - only
	 * FFPMCratesSweep::IsRemovable() does that, and it is what Remove() actually calls to gate the
	 * destroy.
	 */
	FString DescribePredicateBranch(const AFGCrate* Crate)
	{
		if (Crate == nullptr) { return TEXT("null-crate"); }
		const UFGInventoryComponent* Inventory = Crate->GetInventory();
		if (Inventory == nullptr) { return TEXT("null-inventory"); }
		if (Inventory->GetSizeLinear() == 0) { return TEXT("GetSizeLinear()==0"); }
		if (Inventory->IsEmpty()) { return TEXT("IsEmpty()"); }
		return TEXT("not-removable");
	}

	/*
	 * FOR THE OPEN-AT-REMOVAL WARNING ONLY (Ant's ruling, 2026-08-15 - the one case this command's
	 * author flagged unhandled: a crate destroyed while a player has its inventory open). Her ruling:
	 * "Warn but proceed." `IsRemovable()` has already proven the crate empty at the instant of the
	 * destroy (precondition 2 in the header), so nothing of the player's is lost - this function exists
	 * only to make the case NOTICED and SAID. It is never called from `IsRemovable()`, decides nothing
	 * about removability, and never changes anything.
	 *
	 * `mInteractingPlayers` is PRIVATE on `AFGCrate` (`FGCrate.h:158-160`, under the `private:` at
	 * `:147`): `TArray< TObjectPtr<class AFGCharacterPlayer> > mInteractingPlayers;`, doc comment
	 * "Players interacting with this crate, used to toggle dormancy". `AFGCrate`'s parent,
	 * `AFGInteractActor` (`FGInteractActor.h`), holds no such array of its own -
	 * `Register`/`UnregisterInteractingPlayer_Implementation` are no-op stubs there (`{};`, `:30-31`)
	 * and `AFGCrate` overrides both. `IFGUseableInterface` documents those two calls as "Called from
	 * widgets that are opened by the use functionality" (`FGUseableInterface.h:166,170`) - so this array
	 * IS the crate's own open-inventory signal, and there is no public getter anywhere in the class
	 * hierarchy to read it with.
	 *
	 * This module's house-preferred route for a non-public FactoryGame member is a
	 * `Config/AccessTransformers.ini` `Friend=` entry - it fails at BUILD time if the field is ever
	 * renamed, not silently at runtime - but that file sits outside this fix's scope. That same ini
	 * file documents the fallback used here: "stringly-typed FProperty reflection", also already used
	 * elsewhere in this module (`FPMReflexMode.cpp`). Reflection reads `mInteractingPlayers` exactly as
	 * declared - `private` blocks direct C++ member syntax only, never UHT's own reflection data - so
	 * this is a real read of the array the running game maintains, not a guess at one.
	 *
	 * Returns -1 when the property cannot be found - reported as "cannot detect", never silently
	 * coalesced into "not open". A future game update that renames or removes the field degrades this
	 * to `undetectable`, not to a false `no`.
	 */
	int32 CountInteractingPlayers(const AFGCrate* Crate)
	{
		if (Crate == nullptr) { return -1; }

		static FArrayProperty* InteractingPlayersProp = FindFProperty<FArrayProperty>(
			AFGCrate::StaticClass(), TEXT("mInteractingPlayers"));
		if (InteractingPlayersProp == nullptr) { return -1; }

		const FScriptArrayHelper_InContainer Helper(InteractingPlayersProp, Crate);
		return Helper.Num();
	}

	/** Renders CountInteractingPlayers() into audit-line text. -1 (undetectable) is never folded into
	 * "no" - see the honesty rule in that function's doc comment above. */
	FString DescribeOpenState(int32 InteractingPlayerCount)
	{
		if (InteractingPlayerCount < 0)
		{
			return TEXT("undetectable (mInteractingPlayers not found by reflection)");
		}
		if (InteractingPlayerCount == 0) { return TEXT("no"); }
		return FString::Printf(TEXT("yes (%d player(s) had this crate open)"), InteractingPlayerCount);
	}
}

void FFPMCratesSweep::Report(UWorld* World, FOutputDevice* Ar)
{
	auto EmitLine = [Ar](const FString& Line, bool bWarning)
	{
		if (Ar != nullptr) { Ar->Log(Line); }
		if (bWarning)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
		}
	};

	/*
	 * ⚠⚠ REFUSED, BY CONSTRUCTION. Ruling 7's boundary table lists "anything on the live server" as its
	 * OWN category, separate from and in addition to the emptiness test - a truly empty crate ON the
	 * dedicated server is still refused. This function does not go on to read the world at all.
	 */
	if (IsRunningDedicatedServer())
	{
		EmitLine(TEXT("[FPM] crates sweep REFUSED - IsRunningDedicatedServer() is true. Ruling 7's "
		              "boundary lists \"anything on the live server\" as its own refused category, not a "
		              "qualifier on emptiness. Stop the server, copy the save down, and run this against "
		              "the local copy instead."), true);
		return;
	}

	if (World == nullptr)
	{
		// A statement about WHEN this ran, not a finding - there is nothing here to call a clean sweep.
		EmitLine(TEXT("[FPM] crates sweep: NO WORLD - nothing to enumerate. Run FPM.Crates.Report again "
		              "once you are in a world."), true);
		return;
	}

	const FFPMCratesScanResult Scan = ScanWorld(World);

	// ★ THE DENOMINATOR SHIPS WITH THE COUNT. "0 removable" and "0 removable of 0 scanned" are different
	// claims, and only the second is evidence - the failure mode this project has paid for before.
	EmitLine(FString::Printf(
		TEXT("[FPM] crates sweep: %d AFGCrate actor(s) scanned in '%s', %d zero-inventory (removable "
		     "under Ruling 7's boundary: %d dismantle, %d death). REPORT ONLY - nothing removed."),
		Scan.Scanned, *World->GetName(), Scan.Removable, Scan.DismantleCrates, Scan.DeathCrates), false);

	/*
	 * ★ THE CONFIRM TOKEN (precondition 1, FPMCratesSweep.h). Recomputed from THIS scan, not carried
	 * over from any earlier one. FPM.Crates.Remove requires this exact token back, and recomputes it
	 * again itself from a fresh scan at that point - so a token that still matches proves the candidate
	 * set has not changed between this report and that removal.
	 */
	const uint32 Token = ComputeToken(World, Scan);
	EmitLine(FString::Printf(
		TEXT("[FPM]   confirm token for FPM.Crates.Remove: %08X (valid only against a scan that finds "
		     "this exact candidate set - run FPM.Crates.Report again for a fresh token if anything in "
		     "this world may have changed). FPM.Crates.Remove %08X previews a dry run; adding 'confirm' "
		     "as a second argument is the only thing that removes anything."),
		Token, Token), false);

	/*
	 * ⚠ NOT REFUSED, BUT FLAGGED. A pure client's replicated copy of mInventoryStacks can lag the
	 * authority's - a milder echo of the exact AX5 trap the predicate above exists to defeat. The
	 * instruction was to refuse on IsRunningDedicatedServer() specifically; widening that to refuse
	 * every non-authoritative client would be inference this boundary does not authorize. So the report
	 * still runs, and says plainly that its reading may not be the authority's. FPM.Crates.Remove makes
	 * the opposite call for this same condition - see its own doc block for why.
	 */
	if (World->GetNetMode() == NM_Client)
	{
		EmitLine(TEXT("[FPM]   ⚠ this world's NetMode is NM_Client - this reading is this client's "
		              "REPLICATED copy of each inventory, which can lag the authority's. For a reading "
		              "that is trustworthy input to any future removal, re-run this on the host or in "
		              "single player."), true);
	}

	// Capped rather than one line per candidate for every crate in a large save - the useful artefact is
	// the LIST and the count, not an unbounded log (same reasoning FPMWireNullGuard applies to its
	// offending-owner list).
	const int32 DisplayCap = 64;
	for (int32 Index = 0; Index < Scan.Candidates.Num() && Index < DisplayCap; ++Index)
	{
		const FFPMCrateCandidate& Candidate = Scan.Candidates[Index];
		EmitLine(FString::Printf(TEXT("[FPM]   %s (type=%s) at %s"),
			*Candidate.Name, CrateTypeString(Candidate.CrateType), *Candidate.PathName), false);
	}
	if (Scan.Candidates.Num() > DisplayCap)
	{
		EmitLine(FString::Printf(TEXT("[FPM]   ... and %d more (listing capped at %d)"),
			Scan.Candidates.Num() - DisplayCap, DisplayCap), false);
	}
}

void FFPMCratesSweep::Remove(UWorld* World, const TArray<FString>& Args, FOutputDevice* Ar)
{
	auto EmitLine = [Ar](const FString& Line, bool bWarning)
	{
		if (Ar != nullptr) { Ar->Log(Line); }
		if (bWarning)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
		}
	};

	// Precondition 3, first half - identical refusal to Report(), see its comment for the citation.
	if (IsRunningDedicatedServer())
	{
		EmitLine(TEXT("[FPM] crates remove REFUSED - IsRunningDedicatedServer() is true. Ruling 7's "
		              "boundary lists \"anything on the live server\" as its own refused category, not a "
		              "qualifier on emptiness. Stop the server, copy the save down, and run this against "
		              "the local copy instead."), true);
		return;
	}

	if (World == nullptr)
	{
		EmitLine(TEXT("[FPM] crates remove REFUSED - NO WORLD - nothing to enumerate. Run this again "
		              "once you are in a world."), true);
		return;
	}

	/*
	 * Precondition 3, second half - "not the authority". Report() only FLAGS NM_Client and still runs,
	 * because a read-only report of a possibly-lagging value is still useful information. Remove()
	 * DESTROYS actors on the strength of that same possibly-lagging value, which is exactly the AX5 trap
	 * this whole file exists to defeat - so here it is a hard refusal, not a flag.
	 */
	if (World->GetNetMode() == NM_Client)
	{
		EmitLine(TEXT("[FPM] crates remove REFUSED - this world's NetMode is NM_Client, not the "
		              "authority. A non-authoritative client's replicated copy of an inventory can lag "
		              "the authority's - removing on the strength of that copy is the same trap the "
		              "emptiness predicate exists to defeat, so removal never acts on it. Re-run on the "
		              "host or in single player."), true);
		return;
	}

	// Precondition 1 - the token argument is mandatory for a dry run too. One required workflow:
	// FPM.Crates.Report, then FPM.Crates.Remove <token> [confirm].
	if (Args.Num() < 1 || !LooksLikeToken(Args[0]))
	{
		EmitLine(TEXT("[FPM] crates remove REFUSED - missing or malformed token. Run FPM.Crates.Report "
		              "first, then pass its printed token: FPM.Crates.Remove <token> [confirm]."), true);
		return;
	}

	const uint32 GivenToken = FParse::HexNumber(*Args[0]);
	const bool bConfirm = Args.Num() >= 2 && Args[1].Equals(TEXT("confirm"), ESearchCase::IgnoreCase);

	// A fresh, independent scan - never Report()'s old result carried across time. This is what makes
	// the token check meaningful: it compares THIS INSTANT's world state against what the token claims.
	const FFPMCratesScanResult Scan = ScanWorld(World);
	const uint32 CurrentToken = ComputeToken(World, Scan);

	if (GivenToken != CurrentToken)
	{
		EmitLine(FString::Printf(
			TEXT("[FPM] crates remove REFUSED - token mismatch (given %08X, a fresh scan computes %08X). "
			     "The candidate set has changed since that token was printed, or the token is stale or "
			     "from a different world. Run FPM.Crates.Report again and pass its exact token."),
			GivenToken, CurrentToken), true);
		return;
	}

	EmitLine(FString::Printf(
		TEXT("[FPM] crates remove: token confirmed against a fresh scan (%d AFGCrate actor(s) scanned "
		     "in '%s', %d eligible). %s"),
		Scan.Scanned, *World->GetName(), Scan.Removable,
		bConfirm ? TEXT("CONFIRM given - eligible candidates that still pass the atomic re-check below "
		                "WILL be destroyed.")
		         : TEXT("DRY RUN - nothing will be destroyed. Add 'confirm' as a second argument to "
		                "remove for real.")), false);

	const FString Session = FApp::GetSessionId().ToString();
	int32 SkippedOnRecheck = 0;
	int32 Removed = 0;
	int32 RemovedWhileOpen = 0;

	for (const FFPMCrateCandidate& Candidate : Scan.Candidates)
	{
		AFGCrate* Crate = Candidate.Crate.Get();

		/*
		 * ★★ THE LOAD-BEARING LINE. Precondition 2: re-evaluate removability AGAIN, right here, on the
		 * SAME game-thread tick as the scan above and the Destroy() call three lines below it - a
		 * console command handler runs synchronously with no yield back to the engine between
		 * statements, so nothing else can run in the gap between this re-check and that destroy. A
		 * candidate that passed the scan but fails THIS check is skipped and counted, never destroyed,
		 * never retried in this pass. Do not move this re-check earlier, cache its result, or replace it
		 * with the scan's own IsRemovable() call above - only a re-check taken at this exact point, this
		 * close to the destroy, is what Ruling 7 calls "proven empty atomically at the instant of the
		 * delete".
		 */
		const bool bStillRemovable = IsValid(Crate) && Crate->HasAuthority() && IsRemovable(Crate);
		if (!bStillRemovable)
		{
			++SkippedOnRecheck;
			EmitLine(FString::Printf(TEXT("[FPM]   skipped on recheck: %s at %s (no longer eligible - "
			                              "already gone, no longer the authority, or no longer empty)"),
				*Candidate.Name, *Candidate.PathName), true);
			continue;
		}

		const FString ClassName = Crate->GetClass() != nullptr ? Crate->GetClass()->GetName()
			: TEXT("<null class>");
		const FString Branch = DescribePredicateBranch(Crate);
		const TCHAR* TypeStr = CrateTypeString(Candidate.CrateType);

		if (!bConfirm)
		{
			EmitLine(FString::Printf(
				TEXT("[FPM]   would remove: class=%s type=%s path=%s predicate=%s"),
				*ClassName, TypeStr, *Candidate.PathName, *Branch), false);
			continue;
		}

		// Precondition 4 - the audit log. Written BEFORE the destroy so the record exists even if
		// Destroy() itself is what crashes - never capped, never sampled.
		const FString Timestamp = FDateTime::UtcNow().ToIso8601();

		// Ant's ruling on a crate a player has open at removal: warn but proceed - never refuse, never
		// skip. IsRemovable() has already proven the crate empty, so nothing is lost by proceeding. See
		// CountInteractingPlayers() above for the detection and its own honesty rule on "undetectable".
		const int32 OpenPlayerCount = CountInteractingPlayers(Crate);
		const FString OpenState = DescribeOpenState(OpenPlayerCount);
		if (OpenPlayerCount > 0) { ++RemovedWhileOpen; }

		EmitLine(FString::Printf(
			TEXT("[FPM]   REMOVING: class=%s type=%s path=%s predicate=%s session=%s time=%s ")
			TEXT("open-at-removal=%s"),
			*ClassName, TypeStr, *Candidate.PathName, *Branch, *Session, *Timestamp, *OpenState), true);

		Crate->Destroy();
		++Removed;
	}

	// Precondition 6 - coverage always ships with the count. Ant's ruling on a crate open at removal
	// ("warn but proceed") extends this line with one more count - see CountInteractingPlayers() above.
	EmitLine(FString::Printf(
		TEXT("[FPM] crates remove COVERAGE: examined=%d eligible=%d skipped-on-recheck=%d removed=%d ")
		TEXT("removed-while-open=%d%s"),
		Scan.Scanned, Scan.Removable, SkippedOnRecheck, Removed, RemovedWhileOpen,
		bConfirm ? TEXT("") : TEXT(" (dry run - re-run with 'confirm' as a second argument to remove for "
		                           "real)")), false);
}

/*
 * `FPM.Crates.Report` and `FPM.Crates.Remove` - Ruling 7, item 31, the staged removal ladder's v1 and
 * v2. Output goes through the OUTPUT DEVICE, not only UE_LOG: `Display`-level lines do not echo to the
 * in-game console in this game, and a command that looks dead when it actually ran has cost whole boot
 * cycles before.
 *
 * ⚠ THESE COMMANDS ARE SELF-REGISTERING STATIC GLOBALS, NOT WIRED INTO THE CENTRAL FIX LIST. Every other
 * fix in this module is an `IFPMFix` armed from `FicsitsPerformanceManager.cpp`'s `StartupModule`, but
 * that file was out of scope for the lane this was built under. A static `FAutoConsoleCommand...` self-
 * registers at module load regardless - the same mechanism every other `.Report`/`.Sweep`/`.Audit`
 * command in this module already uses (their registration lines sit outside any `Arm()` body too) - so
 * both commands are live the moment the module loads, with or without that wiring. If a future version
 * needs a diagnostic-channel toggle or an armed/disarmed lifecycle, promoting `FFPMCratesSweep` to a full
 * `IFPMFix` (a `Get()` singleton, `Arm()`/`Disarm()`, a channel) and adding one `FPMFixes::Arm(...)`
 * line to the central list is the follow-up.
 */
static FAutoConsoleCommandWithWorldArgsAndOutputDevice GFPMCratesReportCmd(
	TEXT("FPM.Crates.Report"),
	TEXT("Enumerate every AFGCrate in this world, apply the zero-inventory predicate (component state "
	     "only, never the UI - see FPMCratesSweep.h for the safety proof), and report the count, the "
	     "candidates, and a confirm token for FPM.Crates.Remove. NEVER removes anything; ships "
	     "permanently as the regression guard. Refuses on a dedicated server."),
	/*
	 * ⚠ THE GATE IS ON REPORT AND DELIBERATELY NOT ON FPM.Crates.Remove BELOW. Remove is not a report:
	 * it is already held shut by a confirm token that only a fresh Report can mint, and by a second
	 * 'confirm' argument. Adding a frame cap there would buy nothing and could refuse the second half
	 * of a two-step the operator is in the middle of.
	 *
	 * ⚠ A REFUSAL HERE COSTS THE TOKEN, NOT JUST THE LISTING. No report means no fresh token, so
	 * FPM.Crates.Remove has nothing valid to take. That is the safe direction: the failure is "you must
	 * ask again", never "something was removed on a stale reading".
	 */
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
		{
			FPMReportGate Gate(Ar, TEXT("FPM.Crates.Report"));
			if (Gate.IsRefused())
			{
				return;
			}

			FFPMCratesSweep::Report(World, &Ar);
		}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GFPMCratesRemoveCmd(
	TEXT("FPM.Crates.Remove"),
	TEXT("Usage: FPM.Crates.Remove <token> [confirm]. <token> must be the exact confirm token a fresh "
	     "FPM.Crates.Report just printed - a stale or mismatched token is refused. Without 'confirm' "
	     "this is a dry run: it re-checks every candidate but destroys nothing. With 'confirm' it "
	     "destroys every candidate that still passes an atomic re-check of the same predicate, taken on "
	     "the same game-thread tick as the destroy. Refuses on a dedicated server and on a "
	     "non-authoritative client. Always prints examined/eligible/skipped-on-recheck/removed."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World, FOutputDevice& Ar)
		{
			FFPMCratesSweep::Remove(World, Args, &Ar);
		}));
