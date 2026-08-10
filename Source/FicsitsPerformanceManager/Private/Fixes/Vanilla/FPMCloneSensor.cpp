// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Vanilla/FPMCloneSensor.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"

#include "FGGameMode.h"
#include "FGPlayerState.h"
#include "Online/ClientIdentification.h"
#include "Online/CoreOnline.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"   // GetUniqueId() — the field vanilla reports failing to serialize
#include "Engine/NetConnection.h"        // PlayerId — the server's own copy of the identity

/*
 * ★ PRINT THE IDS THEMSELVES, NOT JUST HOW MANY THERE ARE — added 2026-08-09.
 *
 * The sensor's first live catch reported `onlineAccountIds=2` and that count carried the whole
 * investigation for a day. It was enough to say "this joiner has two identities" and never enough to say
 * WHICH two, so the duplicate pair could be counted but not characterised.
 *
 * ⚠ AND THE GAME'S OWN HEADER SAYS WHY THAT MATTERS. `ClientIdentification.h:17-19`, on
 * `CalcClientIdentityMatchQuality`: *"When no online ids are available, the offline id is used.
 * Otherwise, IF ONE ONLINE ID MATCHES, the identities are considered to match."* So a joiner carrying a
 * Steam id AND an Epic id matches ANY saved state that shares EITHER — and if one state was saved under
 * Steam and another under Epic, both match, and vanilla picks arbitrarily. Measured 2026-08-09: two joins
 * ten minutes apart in ONE server process bound two DIFFERENT states, and Ant's outfit changed colour
 * with them. Nothing here is a matcher bug; it is the documented rule meeting a two-identity account.
 *
 * ⚠ NAME IS UNIQUE ACROSS THE MODULE because this is a UNITY BUILD — see FPMFixContract.h:166-171.
 */
static FString FPMCloneFormatAccountIds(const FClientIdentityInfo& Identity)
{
	if (Identity.AccountIds.Num() == 0) { return TEXT("none"); }

	FString Out;
	for (const TPair<UE::Online::EOnlineServices, UE::Online::FAccountId>& Pair : Identity.AccountIds)
	{
		if (!Out.IsEmpty()) { Out += TEXT(", "); }
		// Service name AND handle: the service alone cannot distinguish two accounts on one platform,
		// and the handle alone does not say which platform minted it.
		Out += FString::Printf(TEXT("%s=%s"), LexToString(Pair.Key), *ToLogString(Pair.Value));
	}
	return Out;
}

FFPMCloneSensor& FFPMCloneSensor::Get()
{
	static FFPMCloneSensor Instance;
	return Instance;
}

void FFPMCloneSensor::Arm()
{
	AFGGameMode* Sample = GetMutableDefault<AFGGameMode>();

	/*
	 * BOTH HALVES ARE NEEDED. The candidate list only exists BEFORE vanilla consumes it, and the
	 * outcome only exists after. Neither half alone answers "did the right state lose, and by how
	 * much".
	 *
	 * The handlers are NAMED lambdas rather than inline arguments so their bodies never sit inside a
	 * function-like macro — see the comma warning on FPM_SUBSCRIBE_VIRTUAL. That is what lets the
	 * range-for below use an explicit type without breaking the preprocessor.
	 */
	auto OnBeforeMatch = [](auto& Scope, AFGGameMode* Self, APlayerController* PC)
	{
		if (!Self || !PC || !Self->HasAuthority()) { return; }

		/*
		 * FPM.Diag.Clone 0 silences the sensor outright. Safe as a single early return BECAUSE THIS
		 * FIX HAS NO BEHAVIOUR TO LOSE - it destroys nothing, moves nothing, binds nothing, and its
		 * entire output is log lines. Wired 2026-08-08 after review found the channel was declared
		 * and connected to nothing: a switch that silently does nothing is worse than no switch,
		 * because setting it teaches you to distrust the rest of them.
		 */
		if (!FPMDiag::IsOn(FPMDiag::EChannel::CloneSensor)) { return; }

		const AFGPlayerState* JoinerState = Cast<AFGPlayerState>(PC->PlayerState);
		if (!JoinerState)
		{
			/*
			 * ★ WIDENED 2026-08-09, because this branch FIRED IN ANT'S GAME and told us almost nothing.
			 *
			 * What happened: she could not revive SunFry, and the log carried exactly one line from here —
			 * "join by FGPlayerControllerBase_2147481134 arrived with NO AFGPlayerState". Twenty-six
			 * seconds later vanilla logged, three times:
			 *     LogProperty: Warning: Native NetSerialize StructProperty
			 *     /Script/Engine.PlayerState:UniqueID (FUniqueNetIdRepl) failed.
			 * Those two facts almost certainly describe one mechanism — an identity that failed to
			 * replicate leaves nothing for FindInactivePlayer to match, so vanilla mints a fresh state and
			 * the old one (with the body attached to it) is orphaned. But 26 seconds apart in two
			 * different log categories is CORRELATION, and the design's P3.7 stage is explicitly
			 * "candidate-side ORIGIN NAMING", which correlation cannot deliver.
			 *
			 * So this now captures, IN ONE LINE AT THE ONE INSTANT THAT MATTERS, the three things that
			 * distinguish the competing explanations:
			 *
			 *   1. IS PlayerState NULL, OR PRESENT-BUT-WRONG-TYPE? `Cast<AFGPlayerState>` returns null for
			 *      BOTH, and they are completely different diagnoses — "the state never arrived" versus
			 *      "something replaced the state class". The old line could not tell them apart and
			 *      silently implied the first.
			 *   2. THE PLAYER STATE'S OWN UniqueId, if there is a state at all. This is the exact field
			 *      whose NetSerialize vanilla reports failing.
			 *   3. THE NET CONNECTION'S PlayerId, which is the identity the SERVER holds independently of
			 *      the replicated PlayerState. If the connection has a valid id while the state does not,
			 *      the loss is in replication of the state — and that is a named origin rather than a
			 *      suspicion.
			 *
			 * Read-only. Every one of these is a getter; the sensor still moves nothing.
			 */
			const APlayerState* AnyState = PC->PlayerState;
			const UNetConnection* Conn   = PC->GetNetConnection();

			const TCHAR* StateShape =
				!AnyState ? TEXT("PlayerState is NULL — nothing replicated at all")
				          : TEXT("PlayerState EXISTS but is NOT an AFGPlayerState — wrong class");

			const FString StateClass = AnyState ? GetNameSafe(AnyState->GetClass()) : FString(TEXT("<none>"));
			const bool bStateIdValid = AnyState && AnyState->GetUniqueId().IsValid();
			const FString StateId    = AnyState && AnyState->GetUniqueId().IsValid()
				? AnyState->GetUniqueId().ToString() : FString(TEXT("<invalid>"));

			const bool bConnIdValid = Conn && Conn->PlayerId.IsValid();
			const FString ConnId    = (Conn && Conn->PlayerId.IsValid())
				? Conn->PlayerId.ToString() : FString(TEXT("<invalid>"));

			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] clone sensor: join by %s has NO USABLE AFGPlayerState — vanilla is about to "
				     "match against nothing, which alone would explain a fresh state being minted."),
				*GetNameSafe(PC));
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   shape        : %s  (class=%s)"), StateShape, *StateClass);
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   state uniqueId: %s  '%s'"),
				bStateIdValid ? TEXT("VALID") : TEXT("INVALID"), *StateId);
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   conn  playerId: %s  '%s'  (connection %s)"),
				bConnIdValid ? TEXT("VALID") : TEXT("INVALID"), *ConnId,
				Conn ? TEXT("present") : TEXT("ABSENT"));

			/*
			 * The reading, stated here so the next person does not have to re-derive it from four lines:
			 * conn VALID + state INVALID/absent  ⇒ the identity reached the server and was lost on the
			 *   way into the PlayerState. That is the UniqueNetIdRepl NetSerialize failure, and it names
			 *   the origin as replication of the state rather than anything mod-side.
			 * conn INVALID too                   ⇒ the identity never arrived. Look upstream at the
			 *   online subsystem (the failure sits 1 ms after an EOS SetPresenceResult in Ant's log).
			 * state present, wrong class         ⇒ a mod replaced the player-state class. Different bug.
			 */
			return;
		}

		const FClientIdentityInfo& JoinerId = JoinerState->GetClientIdentity();

		FString Fallbacks;
		// (see FPMCloneFormatAccountIds below the lambda's enclosing scope for why the ids are printed)
		for (const FString& FallbackName : JoinerId.FallbackAccountNames)
		{
			Fallbacks += (Fallbacks.IsEmpty() ? TEXT("") : TEXT(", ")) + FallbackName;
		}

		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] ---- clone sensor: JOIN by '%s' ----"), *JoinerState->GetPlayerName());
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   joiner: state=%s  offlineId='%s'  onlineAccountIds=%d [%s]  fallbackNames=[%s]"),
			*GetNameSafe(JoinerState),
			*JoinerId.OfflineId, JoinerId.AccountIds.Num(),
			*FPMCloneFormatAccountIds(JoinerId),
			Fallbacks.IsEmpty() ? TEXT("none") : *Fallbacks);

		/*
		 * ⚠ WE DO NOT CALL CalcClientIdentityMatchQuality, AND THAT IS NOT A STYLE CHOICE.
		 *
		 * It is declared public and static at ClientIdentification.h:20 and it is NOT EXPORTED from the
		 * shipping FactoryGame binary. Calling it compiles clean and dies at link:
		 *     ld.lld: error: undefined symbol:
		 *       FClientIdentityInfo::CalcClientIdentityMatchQuality(FClientIdentityInfo const&, ...)
		 * Header presence proves a SIGNATURE, never a symbol. Most of FactoryGame's private sources are
		 * autogenerated stubs, so a public declaration with no shipped body is normal here.
		 *
		 * So this reconstructs the comparison from the raw fields, following the rule CSS documents in
		 * that same header at :16-18 — "When no online ids are available, the offline id is used.
		 * Otherwise, if one online id matches, the identities are considered to match."
		 *
		 * ⚠ THIS IS FPM'S RECONSTRUCTION, NOT VANILLA'S SCORE, and every line says so. It cannot be used
		 * to second-guess vanilla's verdict. What it is good for is the actual question: the raw
		 * identity fields on both sides, printed together, so a human can see WHICH field went missing.
		 */
		int32 Considered = 0;
		int32 PlausibleMatches = 0;

		// The field is PUBLIC — GameMode.h:138, inside the `public:` at :106; the `protected:` at :140
		// comes after it. No access transformer is needed and there is not one; an entry claiming
		// otherwise was removed on 2026-08-10. What is genuinely absent is a public ENUMERATOR, which
		// is why this iterates the array directly. READ-ONLY: scored and printed, never written, and
		// no AFGCharacterPlayer is touched. FPM NEVER DESTROYS PLAYER CHARACTERS.
		for (APlayerState* Candidate : Self->InactivePlayerArray)
		{
			const AFGPlayerState* FGCandidate = Cast<AFGPlayerState>(Candidate);
			if (!FGCandidate) { continue; }

			++Considered;
			const FClientIdentityInfo& CandidateId = FGCandidate->GetClientIdentity();
			const FString CandidateName = FGCandidate->GetPlayerName();

			const bool bOfflineMatch = !JoinerId.OfflineId.IsEmpty()
			                         && JoinerId.OfflineId == CandidateId.OfflineId;

			int32 SharedOnlineIds = 0;
			for (const auto& JoinerPair : JoinerId.AccountIds)
			{
				if (const UE::Online::FAccountId* Found = CandidateId.AccountIds.Find(JoinerPair.Key))
				{
					if (*Found == JoinerPair.Value) { ++SharedOnlineIds; }
				}
			}

			// CSS's documented precedence: online ids decide when either side has any, offline id only
			// otherwise.
			const bool bWouldMatch = (JoinerId.AccountIds.Num() > 0 || CandidateId.AccountIds.Num() > 0)
			                       ? (SharedOnlineIds > 0)
			                       : bOfflineMatch;
			if (bWouldMatch) { ++PlausibleMatches; }

			/*
			 * THE OBJECT NAME TIES THIS LINE BACK TO THE SAVE BYTES. THE UID SETTLES A QUESTION THE
			 * NAME CANNOT.
			 *
			 * The 2026-08-08 boot printed 26 candidate lines carrying only 13 distinct NAMES, and
			 * InactivePlayerArray.Num() agreed at 26. That has two readings and the name cannot tell
			 * them apart: either the array holds each entry twice, or there are 26 distinct objects
			 * whose names collide because UObject names are unique per OUTER, not globally.
			 *
			 * GetUniqueID() is the engine's internal object index, so two live objects can never
			 * share one. Equal uid twice => a genuine duplicate ENTRY. Different uids => 26 real
			 * states and the name collision was the illusion.
			 */
			/*
			 * Verbose tier: 26 lines per join on Ant's save, which is detail for one deliberate boot
			 * rather than for every join.
			 *
			 * ⚠ A BRACED `if`, NOT AN EARLY `continue`. The counters above (`++Considered`,
			 * `++PlausibleMatches`) already ran, so a `continue` here happens to be correct TODAY — and
			 * would silently skip anything a later edit appends after this log. Scoping the gate to the
			 * statement it guards cannot rot that way. An unbraced `if` before a UE_LOG macro has the
			 * same shape of problem and is avoided for the same reason.
			 */
			/*
			 * ★ A MATCHING CANDIDATE PRINTS AT LEVEL 1; THE OTHER SIXTY PRINT AT VERBOSE — 2026-08-09.
			 *
			 * It used to be verbose-or-nothing, which meant the DUPLICATE PAIR — the entire finding — was
			 * only visible on a deliberate boot nobody had reason to run until after the damage. On Ant's
			 * save that is 1-2 lines against 61-62, so the interesting rows cost almost nothing and the
			 * noise still costs what it always did. The signal should not be the expensive tier.
			 */
			const bool bVerbose = FPMDiag::IsOn(FPMDiag::EChannel::CloneSensor, 2);
			if (bWouldMatch || bVerbose)
			{
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM]   candidate: state=%s(uid %u)  name='%s'%s  ownsPawn=%s  offlineId='%s'(%s)  "
					     "onlineIds=%d [%s] shared=%d  -> reconstructed: %s"),
					*GetNameSafe(FGCandidate), FGCandidate->GetUniqueID(),
					CandidateName.IsEmpty() ? TEXT("(EMPTY)") : *CandidateName,
					CandidateName.IsEmpty() ? TEXT("  <-- DEGENERATE") : TEXT(""),
					FGCandidate->GetOwnedPawn() ? TEXT("yes") : TEXT("NO"),
					*CandidateId.OfflineId, bOfflineMatch ? TEXT("SAME") : TEXT("differs"),
					CandidateId.AccountIds.Num(), *FPMCloneFormatAccountIds(CandidateId), SharedOnlineIds,
					bWouldMatch ? TEXT("WOULD match") : TEXT("would NOT match"));
			}
		}

		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %d candidate state(s) examined, %d reconstructed as matching, %d total in the "
			     "inactive array."),
			Considered, PlausibleMatches, Self->InactivePlayerArray.Num());

		/*
		 * THE TWO TELLS, AND THEY POINT AT DIFFERENT CAUSES.
		 *
		 * MORE THAN ONE match is H1 — several states carry the same identity, so the matcher ties and
		 * picks arbitrarily. That is the acute failure the save already shows for SunFry, and the line
		 * below names which states are tied, which is the answer to "which one is real".
		 *
		 * ZERO matches with candidates present is the joiner's own identity FIELDS being empty, not the
		 * scorer failing.
		 */
		if (PlausibleMatches > 1)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   ⚠ %d candidate states match this joiner. Vanilla will pick ONE and the "
				     "choice is arbitrary. The states listed above as 'WOULD match' are the duplicates "
				     "— this is the identity-duplication failure, not an identity-loss one."),
				PlausibleMatches);
		}
		else if (PlausibleMatches == 0 && Considered > 0)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM]   ⚠ NOT ONE of %d candidate states matches this joiner on identity fields. "
				     "A new state is the inevitable result. Compare the joiner's offlineId/onlineIds "
				     "above against the candidates' — the field that went missing is the bug."),
				Considered);
		}

		// Deliberately NO Scope.Override and NO Scope.Cancel. Vanilla decides; we only watch.
	};

	auto OnAfterMatch = [](const bool& bClaimed, AFGGameMode* Self, APlayerController* PC)
	{
		if (!FPMDiag::IsOn(FPMDiag::EChannel::CloneSensor)) { return; }
		// Signature is fixed by SML: the return arrives by CONST REFERENCE, not by value, and there is
		// no scope parameter on an _AFTER handler.
		if (!Self || !PC || !Self->HasAuthority()) { return; }

		const AFGPlayerState* Bound = Cast<AFGPlayerState>(PC->PlayerState);

		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   RESULT: vanilla %s. bound state=%s  name='%s'  ownsPawn=%s"),
			bClaimed ? TEXT("CLAIMED an existing state")
			         : TEXT("did NOT claim — a NEW state will be minted"),
			*GetNameSafe(Bound),
			(Bound && !Bound->GetPlayerName().IsEmpty()) ? *Bound->GetPlayerName() : TEXT("(EMPTY)"),
			(Bound && Bound->GetOwnedPawn()) ? TEXT("yes") : TEXT("NO"));
	};

	FPM_SUBSCRIBE_VIRTUAL("clone-sensor", AFGGameMode::FindInactivePlayer, Sample, OnBeforeMatch);
	FPM_SUBSCRIBE_VIRTUAL_AFTER("clone-sensor", AFGGameMode::FindInactivePlayer, Sample, OnAfterMatch);
}
