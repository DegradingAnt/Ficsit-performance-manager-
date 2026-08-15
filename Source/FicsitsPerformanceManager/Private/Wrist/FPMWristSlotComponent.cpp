// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Wrist/FPMWristSlotComponent.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"
#include "Core/FPMMasterSwitch.h"
#include "Wrist/FPMWristItemBase.h"

#include "FGCharacterPlayer.h"

#include "HAL/IConsoleManager.h"
#include "Net/UnrealNetwork.h"

/*
 * ════════════════════════════════════════════════════════════════════════════════════════════════
 * FILE-SCOPE STATE — the registry, the pending-worn map, the live-component roster, the counters.
 *
 * ⚠ NAMES ARE GLOBALLY UNIQUE ON PURPOSE, NOT MERELY PREFIXED FOR TIDINESS. UE builds this module as
 * a UNITY BUILD (FPMFixContract.h's own warning): the .cpp files are concatenated into ONE
 * translation unit, so two anonymous namespaces declaring the same name are one namespace declaring
 * it twice -- `error C2374`. Every symbol below carries the `FPMWrist`/`GFPMWrist` stem for that
 * reason.
 *
 * ⚠ THREADING: game thread only, and that is a claim with a mechanism behind it rather than a hope.
 * The four writers are (1) the `AFGCharacterPlayer::BeginPlay` AFTER-hook, (2) the component's own
 * BeginPlay/EndPlay, (3) Server/Client RPC implementations, (4) console commands and cvar sinks.
 * UE dispatches actor BeginPlay, RPC delivery, console execution and cvar sinks on the game thread;
 * none of this is reachable from a Factory Tick worker the way the RPC gate's census was. No lock.
 * If a future build moves character BeginPlay off the game thread, this comment is the thing that
 * has to change first -- do not quietly add a lock instead.
 * ════════════════════════════════════════════════════════════════════════════════════════════════
 */
namespace
{
	/**
	 * FPM'S OWN CVAR, which is what keeps it residue-free -- the same shape as `FPM.Zipline.Volume`
	 * (FPMZiplineVolume.cpp:24). It is not a vanilla `US_*`-backed setting, it is never written to any
	 * ini, and it is gone when the module unloads.
	 *
	 * This is the WRIST FEATURE switch of design 11.2.6's OFF contract. OFF does not delete the
	 * component and does not touch save state -- it force-holsters and then declines.
	 */
	TAutoConsoleVariable<int32> GFPMCVarWristEnabled(
		TEXT("FPM.Wrist.Enabled"), 1,
		TEXT("1 = the wrist slot accepts equip/deploy. 0 = the OFF contract: any deployed item is "
		     "force-released, the component STAYS on every character and every API entry point still "
		     "answers, and Equip/Deploy return SlotDisabled. Registration is never lost, and no save "
		     "state is touched - OFF releases behaviour, the save keeps the item."),
		ECVF_Default);

	/**
	 * A PER-MACHINE preference, pushed by the owning client to the server. Design 11.2.2's Ruling 11:
	 * LEFT is the default, RIGHT is the player's own choice.
	 *
	 * ⚠ STATED GAP, not a silent substitution. Design line 1074 puts handedness in the SML mod-config
	 * row (`Configs/FicsitsPerformanceManager.cfg`). The SHIPPED HEADER
	 * (`Public/Wrist/FPMWristSlotComponent.h`, Server_SetHandedness's comment) names a cvar
	 * `FPM.Wrist.RightHanded` instead, and this file implements the header. Whoever owns
	 * `FPMSettingsConfig` should add the config row and have it drive THIS cvar; nothing in this file
	 * changes when that lands. Grep receipt for the gap being real, run over the whole module and
	 * excluding this slice's own two directories:
	 *     grep -rn "Handed\|RightHand" --include=*.h --include=*.cpp .   ->  zero hits outside Wrist/
	 */
	TAutoConsoleVariable<int32> GFPMCVarWristRightHanded(
		TEXT("FPM.Wrist.RightHanded"), 0,
		TEXT("0 = wear the wrist item on the LEFT arm (the default), 1 = RIGHT. A per-machine "
		     "preference: the owning client pushes its own value to the server. Changing it while an "
		     "item is DEPLOYED is ignored until release - no mid-flight re-attach."),
		ECVF_Default);

	/**
	 * One catalog entry. Deliberately NOT a `USTRUCT` -- the header's class comment prices the
	 * reflected surface at 2 UCLASS + 1 UINTERFACE + 0 USTRUCT against a measured 0-USTRUCT baseline,
	 * which is why `RegisterWristItem` takes the fields as separate UFUNCTION parameters and this
	 * struct never crosses the reflection boundary.
	 */
	struct FFPMWristRegistration
	{
		FName Owner;
		FName ItemId;
		TSoftClassPtr<AActor> ItemClass;
		FText DisplayName;
		int32 ConsumerMajor = 0;
		int32 ConsumerMinor = 0;
	};

	/**
	 * The registry, as plain file-scope state -- mirroring `FPMHookLedger`'s static-module-state shape
	 * rather than spending a third UCLASS on a subsystem nobody approved (header class comment).
	 *
	 * `TSoftClassPtr` holds a `FSoftObjectPath`, not a live UObject pointer, so this array keeps
	 * nothing alive and needs no `AddReferencedObjects`. That is the reason the API takes a SOFT class
	 * pointer rather than a `UClass*`.
	 */
	TArray<FFPMWristRegistration> GFPMWristRegistry;

	/**
	 * The transient, UNSAVED pending-worn map of design 11.2.3 point 3. Weak on both sides: a
	 * character or item destroyed between the two halves of the handshake must not be kept alive by
	 * this map, and a stale entry must read as gone rather than as a dangling pointer.
	 */
	TMap<TWeakObjectPtr<AFGCharacterPlayer>, TWeakObjectPtr<AFPMWristItemBase>> GFPMWristPendingWorn;

	/** Every live component, so the OFF/ON transition can reach all of them. Weak, same reason. */
	TArray<TWeakObjectPtr<UFPMWristSlotComponent>> GFPMWristLiveComponents;

	// ── COUNTERS ────────────────────────────────────────────────────────────────────────────────
	// Every one of these is answerable to the dead-instrument question "what concrete input makes
	// this non-zero?", and `FPM.Wrist.Report` prints the answer beside the number rather than
	// leaving a reader to assume a zero is health. See LogReport below.
	int32 GFPMWristComponentsAdded = 0;      // an AFGCharacterPlayer spawning on the authority
	int32 GFPMWristComponentsSeenLocal = 0;  // a component BeginPlay-ing here, authority or not
	int32 GFPMWristComponentsSeenRemote = 0; // ...specifically WITHOUT authority: replication witness
	int32 GFPMWristEquips = 0;               // a successful Server_Equip
	int32 GFPMWristUnequips = 0;
	int32 GFPMWristDeploys = 0;
	int32 GFPMWristReleases = 0;
	int32 GFPMWristForceHolsters = 0;        // FPM.Wrist.Enabled 0 or FPM.Enabled 0 while deployed
	int32 GFPMWristHandednessReps = 0;       // OnRep_Handedness on a client: replication witness
	int32 GFPMWristHandednessIgnored = 0;    // a handedness push refused because an item is deployed
	int32 GFPMWristPendingRegistered = 0;    // AFPMWristItemBase::PostLoadGame with a live owner
	int32 GFPMWristPendingCompleted = 0;     // ...and the other half of the handshake arrived
	int32 GFPMWristRefusalCounts[5] = { 0, 0, 0, 0, 0 };

	/** Last value `IsWristFeatureEnabled()` returned, so a transition can be detected rather than polled. */
	bool GFPMWristLastEnabled = true;
	bool GFPMWristStateSeeded = false;

	/** Set once so a re-Arm cannot register a second master-switch stop hook (there is no unregister). */
	bool GFPMWristStopHookRegistered = false;

	const TCHAR* GFPMWristRefusalText(EFPMWristRefusal Reason)
	{
		switch (Reason)
		{
			case EFPMWristRefusal::None:            return TEXT("None");
			case EFPMWristRefusal::SlotOccupied:    return TEXT("SlotOccupied");
			case EFPMWristRefusal::SlotDisabled:    return TEXT("SlotDisabled");
			case EFPMWristRefusal::VersionMismatch: return TEXT("VersionMismatch");
			case EFPMWristRefusal::NotAWristItem:   return TEXT("NotAWristItem");
		}
		return TEXT("<unknown>");
	}

	void GFPMWristCountRefusal(EFPMWristRefusal Reason)
	{
		// The bound is checked against the array rather than against the enum's last value on
		// purpose: adding a sixth EFPMWristRefusal without widening the array would otherwise write
		// past the end, and this is the one line that would notice.
		const int32 Index = static_cast<int32>(Reason);
		if (Index >= 0 && Index < UE_ARRAY_COUNT(GFPMWristRefusalCounts))
		{
			++GFPMWristRefusalCounts[Index];
		}
	}

	/**
	 * ★ THE CLASSIFIER, ISOLATED IN ONE FUNCTION SO IT CAN BE PROVEN.
	 *
	 * Design 11.2.6, verbatim: registration is ACCEPTED when
	 * `consumer.Major == host.Major AND consumer.Minor <= host.Minor`. A consumer with a NEWER minor
	 * than the host may need members the host lacks, which is why the minor check points that way.
	 *
	 * It is a free function rather than three lines inside `RegisterWristItem` for one reason: a gate
	 * that can only be exercised by a third-party mod nobody has written yet is a dead instrument.
	 * `FPM.Wrist.SelfTest` drives THIS function against a known-positive and two known-negatives at
	 * every Arm(), so the accept/refuse decision is measured on Ant's build without waiting for a
	 * consumer to exist. Same discipline as `FPMCVarWriter::SelfTest` (FPMCVarWriter.cpp:364).
	 */
	bool GFPMWristVersionAccepted(int32 ConsumerMajor, int32 ConsumerMinor)
	{
		return ConsumerMajor == FPM_WRIST_API_MAJOR && ConsumerMinor <= FPM_WRIST_API_MINOR;
	}

	int32 GFPMWristFindRegistration(FName Owner, FName ItemId)
	{
		return GFPMWristRegistry.IndexOfByPredicate(
			[Owner, ItemId](const FFPMWristRegistration& Entry)
			{
				return Entry.Owner == Owner && Entry.ItemId == ItemId;
			});
	}

	/** Drops dead weak entries. Called from the few places that walk the roster, never on a tick. */
	void GFPMWristCompactRosters()
	{
		GFPMWristLiveComponents.RemoveAll(
			[](const TWeakObjectPtr<UFPMWristSlotComponent>& Weak) { return !Weak.IsValid(); });

		for (auto It = GFPMWristPendingWorn.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid() || !It.Value().IsValid())
			{
				It.RemoveCurrent();
			}
		}
	}

	UFPMWristSlotComponent* GFPMWristFindComponent(AFGCharacterPlayer* Character)
	{
		return Character != nullptr ? Character->FindComponentByClass<UFPMWristSlotComponent>() : nullptr;
	}

	/**
	 * ★ THE ORDER-INDEPENDENT HALF OF THE PERSISTENCE HANDSHAKE (design 11.2.3 point 3).
	 *
	 * Called from BOTH sides -- `RegisterPendingWorn` (the item arrived) and the component's own
	 * BeginPlay (the component arrived). Whichever runs second finds the other half present and
	 * completes the equip; the first one to run finds nothing and returns, having lost nothing.
	 *
	 * ⚠ IT CHECKS THE OFF CONTRACT FIRST AND LEAVES THE ITEM IN THE MAP WHEN THE FEATURE IS OFF.
	 * Design 11.2.6: with the feature off "the persisted worn item still spawns from the save ... but
	 * the handshake completes only to HOLSTERED-INERT ... Toggling ON completes the equip from the
	 * same pending-worn map." Claiming the item and then dropping it would empty the map and make the
	 * ON transition a no-op -- the save state would be intact but the item would never come back this
	 * session, which is the silent-failure shape this tier exists to catch.
	 */
	void GFPMWristTryCompleteHandshake(AFGCharacterPlayer* Character)
	{
		if (Character == nullptr || !Character->HasAuthority())
		{
			return;   // the save, and therefore the handshake, is the authority's business only
		}

		if (!UFPMWristSlotComponent::IsWristFeatureEnabled())
		{
			return;   // HOLSTERED-INERT; the item stays pending and the ON transition retries
		}

		UFPMWristSlotComponent* Component = GFPMWristFindComponent(Character);
		if (Component == nullptr || Component->GetEquippedItemActor() != nullptr)
		{
			return;   // component half not here yet, or something is already worn
		}

		AFPMWristItemBase* Item = UFPMWristSlotComponent::ClaimPendingWorn(Character);
		if (Item == nullptr)
		{
			return;   // item half not here yet
		}

		++GFPMWristPendingCompleted;

		/*
		 * Calling the Server RPC ON THE AUTHORITY executes its _Implementation directly (UE resolves
		 * the callspace to Local when the owning actor has authority) -- so this reuses every gate in
		 * Server_Equip rather than growing a second, divergent equip path. No RCO, no round trip, and
		 * single player collapses to exactly the same call.
		 */
		Component->Server_Equip(Item);

		if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] wrist-slot: persistence handshake COMPLETED for %s with %s (#%d). The save "
				     "is the authority on what is worn - no config read, no schematic re-validation."),
				*GetNameSafe(Character), *GetNameSafe(Item), GFPMWristPendingCompleted);
		}
	}

	/**
	 * ★ THE OFF/ON TRANSITION. Called from the `FPM.Wrist.Enabled` sink and the master-switch stop
	 * hook. Detects a CHANGE rather than re-broadcasting on every call -- a delegate that fires when
	 * nothing changed teaches a dependent mod to ignore it.
	 */
	void GFPMWristApplyFeatureState(const TCHAR* Reason)
	{
		const bool bEnabled = UFPMWristSlotComponent::IsWristFeatureEnabled();

		if (GFPMWristStateSeeded && bEnabled == GFPMWristLastEnabled)
		{
			return;
		}

		GFPMWristLastEnabled = bEnabled;
		GFPMWristStateSeeded = true;

		GFPMWristCompactRosters();

		for (const TWeakObjectPtr<UFPMWristSlotComponent>& Weak : GFPMWristLiveComponents)
		{
			UFPMWristSlotComponent* Component = Weak.Get();
			if (Component == nullptr) { continue; }

			if (!bEnabled)
			{
				// OFF-means-RELEASED (design 5.1, extended to the API by 11.2.6). Authority-only,
				// because the deploy state is replicated FROM the authority - a client releasing
				// locally would be overwritten on the next update and would look like a stutter.
				Component->ForceHolster(Reason);
			}

			// The delegate fires on EVERY character's own component, client included: a dependent mod
			// reacts instead of polling, and it must be told on both machines.
			Component->OnWristSlotStateChanged.Broadcast(bEnabled);
		}

		if (bEnabled)
		{
			// "Toggling ON completes the equip from the same pending-worn map" - design 11.2.6.
			for (const TWeakObjectPtr<UFPMWristSlotComponent>& Weak : GFPMWristLiveComponents)
			{
				UFPMWristSlotComponent* Component = Weak.Get();
				if (Component == nullptr) { continue; }
				GFPMWristTryCompleteHandshake(Cast<AFGCharacterPlayer>(Component->GetOwner()));
			}
		}

		// Unthrottled and ungated: a feature-state transition is a rare, operator-caused event, and
		// the log line is what lets "it did nothing" be told apart from "it was off".
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] wrist-slot: feature now %s (%s). %d live component(s) notified. The component "
			     "is NOT removed and registration is NOT lost - OFF declines, it never vanishes."),
			bEnabled ? TEXT("ENABLED") : TEXT("DISABLED"), Reason, GFPMWristLiveComponents.Num());
	}

	/**
	 * ★ THE SELF-TEST -- and it exists because of a named failure mode, not for symmetry.
	 *
	 * `GetRegisteredWristItemCount()` and every refusal counter in this file read ZERO on Ant's build
	 * today and will keep reading zero until some mod registers a wrist item. FPM itself ships none
	 * (the grapple is Slice 5, explicitly not built here). A permanently-zero counter is
	 * indistinguishable from a healthy one, which is the exact instrument shape this project has
	 * shipped five times in two days.
	 *
	 * So the version gate is proven the way `FPMCVarWriter::SelfTest` proves the write path: drive it
	 * with a KNOWN-POSITIVE that must be accepted and KNOWN-NEGATIVES that must be refused, and round
	 * trip a real registration through the real registry so the store is proven to store and the
	 * count is proven to move. What would make this report a failure: any change that inverts the
	 * `<=` on the minor, drops the major equality, or makes the registry a write-only sink.
	 */
	bool GFPMWristSelfTest(FOutputDevice* Ar)
	{
		auto Emit = [Ar](const FString& Line)
		{
			if (Ar != nullptr) { Ar->Log(Line); }
		};

		// ── the classifier, both directions ─────────────────────────────────────────────────────
		const bool bPositiveExact = GFPMWristVersionAccepted(FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR);
		const bool bPositiveOlder = GFPMWristVersionAccepted(FPM_WRIST_API_MAJOR, FMath::Max(0, FPM_WRIST_API_MINOR - 1));
		const bool bNegativeMajor = GFPMWristVersionAccepted(FPM_WRIST_API_MAJOR + 1, FPM_WRIST_API_MINOR);
		const bool bNegativeMinor = GFPMWristVersionAccepted(FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR + 1);

		const bool bClassifierOk = bPositiveExact && bPositiveOlder && !bNegativeMajor && !bNegativeMinor;

		// ── the registry, round trip ────────────────────────────────────────────────────────────
		static const FName ProbeOwner(TEXT("FPM.Wrist.SelfTest"));
		static const FName ProbeItem(TEXT("probe"));

		const int32 CountBefore = UFPMWristSlotComponent::GetRegisteredWristItemCount();

		EFPMWristRefusal ProbeRefusal = EFPMWristRefusal::None;
		const bool bProbeAccepted = UFPMWristSlotComponent::RegisterWristItem(
			ProbeOwner, ProbeItem, TSoftClassPtr<AActor>(),
			FText::FromString(TEXT("FPM wrist-slot self-test probe")),
			FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR, ProbeRefusal);

		const int32 CountHeld = UFPMWristSlotComponent::GetRegisteredWristItemCount();
		const bool bProbeStored = GFPMWristFindRegistration(ProbeOwner, ProbeItem) != INDEX_NONE;

		EFPMWristRefusal BadRefusal = EFPMWristRefusal::None;
		const bool bBadAccepted = UFPMWristSlotComponent::RegisterWristItem(
			ProbeOwner, FName(TEXT("probe-from-the-future")), TSoftClassPtr<AActor>(),
			FText::FromString(TEXT("FPM wrist-slot self-test probe (newer minor)")),
			FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR + 1, BadRefusal);

		// Remove the probe. There is deliberately no public unregister on the API surface - the
		// catalog is additive by design - so the self-test reaches the file-scope store directly.
		const int32 ProbeIndex = GFPMWristFindRegistration(ProbeOwner, ProbeItem);
		if (ProbeIndex != INDEX_NONE) { GFPMWristRegistry.RemoveAt(ProbeIndex); }

		const int32 CountAfter = UFPMWristSlotComponent::GetRegisteredWristItemCount();

		const bool bRegistryOk =
			bProbeAccepted && bProbeStored &&
			CountHeld == CountBefore + 1 &&
			CountAfter == CountBefore &&
			!bBadAccepted && BadRefusal == EFPMWristRefusal::VersionMismatch;

		if (bClassifierOk && bRegistryOk)
		{
			const FString Line = FString::Printf(
				TEXT("[FPM] wrist-slot self-test PASSED: version gate accepted {%d,%d} and {%d,%d}, "
				     "refused {%d,%d} and {%d,%d}; registry went %d -> %d -> %d around one probe entry, "
				     "and the refused entry was NOT stored. The accept/refuse decision and the catalog "
				     "both work on this build, with zero third-party consumers present."),
				FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR,
				FPM_WRIST_API_MAJOR, FMath::Max(0, FPM_WRIST_API_MINOR - 1),
				FPM_WRIST_API_MAJOR + 1, FPM_WRIST_API_MINOR,
				FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR + 1,
				CountBefore, CountHeld, CountAfter);

			Emit(Line);
			UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
			return true;
		}

		const FString Line = FString::Printf(
			TEXT("[FPM] wrist-slot self-test FAILED. classifier: exact=%s older=%s newer-major=%s "
			     "newer-minor=%s (want 1 1 0 0). registry: probe-accepted=%s probe-stored=%s "
			     "counts %d/%d/%d (want N/N+1/N) bad-accepted=%s bad-refusal=%s (want 0 / "
			     "VersionMismatch). The public wrist API is UNVERIFIED this boot - do not trust a "
			     "zero refusal count."),
			bPositiveExact ? TEXT("1") : TEXT("0"), bPositiveOlder ? TEXT("1") : TEXT("0"),
			bNegativeMajor ? TEXT("1") : TEXT("0"), bNegativeMinor ? TEXT("1") : TEXT("0"),
			bProbeAccepted ? TEXT("1") : TEXT("0"), bProbeStored ? TEXT("1") : TEXT("0"),
			CountBefore, CountHeld, CountAfter,
			bBadAccepted ? TEXT("1") : TEXT("0"), GFPMWristRefusalText(BadRefusal));

		Emit(Line);
		UE_LOG(LogFicsitsPerformanceManager, Error, TEXT("%s"), *Line);
		return false;
	}

	/**
	 * `FPM.Wrist.Report` -- and it PRINTS ITS OWN COVERAGE beside every number.
	 *
	 * A wrist slot with no registered items can never equip anything, so "0 equips" is the correct
	 * reading of a healthy system, not a fault. Saying only "0" would let silence read as a clean
	 * bill of health, which is the failure this project has already shipped. Every zero below is
	 * printed with the input that would make it non-zero.
	 */
	void GFPMWristLogReport(FOutputDevice* Ar)
	{
		GFPMWristCompactRosters();

		auto Emit = [Ar](const FString& Line)
		{
			if (Ar != nullptr) { Ar->Log(Line); }
			UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
		};

		Emit(FString::Printf(
			TEXT("[FPM] wrist-slot: feature %s (FPM.Wrist.Enabled=%d, FPM.Enabled=%d), API {%d,%d}."),
			UFPMWristSlotComponent::IsWristFeatureEnabled() ? TEXT("ENABLED") : TEXT("DISABLED"),
			GFPMCVarWristEnabled.GetValueOnGameThread(),
			FPMMasterSwitch::IsEnabled() ? 1 : 0,
			FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR));

		Emit(FString::Printf(
			TEXT("[FPM] wrist-slot: %d registered item(s). COVERAGE: with 0 registered items nothing "
			     "CAN be equipped, so a 0 equip count below is the expected reading and not evidence "
			     "of a fault. FPM ships no wrist item of its own - the grapple is Slice 5."),
			GFPMWristRegistry.Num()));

		for (const FFPMWristRegistration& Entry : GFPMWristRegistry)
		{
			Emit(FString::Printf(
				TEXT("[FPM] wrist-slot:   %s / %s  \"%s\"  class=%s  compiled against {%d,%d}"),
				*Entry.Owner.ToString(), *Entry.ItemId.ToString(), *Entry.DisplayName.ToString(),
				*Entry.ItemClass.ToString(), Entry.ConsumerMajor, Entry.ConsumerMinor));
		}

		Emit(FString::Printf(
			TEXT("[FPM] wrist-slot: components - %d added here on the authority, %d seen locally, of "
			     "which %d arrived WITHOUT authority. COVERAGE: that last number is the multiplayer "
			     "replication witness. A client never creates this component itself, so a non-zero "
			     "there is proof that dynamic component replication delivered it; a 0 on a CLIENT with "
			     "a live character means replication did NOT arrive and the slot is dead on that "
			     "machine. On a single-player or server-side report a 0 there is correct."),
			GFPMWristComponentsAdded, GFPMWristComponentsSeenLocal, GFPMWristComponentsSeenRemote));

		Emit(FString::Printf(
			TEXT("[FPM] wrist-slot: actions - %d equip, %d unequip, %d deploy, %d release, "
			     "%d force-holster. Handedness: %d replicated update(s) received, %d push(es) ignored "
			     "because an item was deployed."),
			GFPMWristEquips, GFPMWristUnequips, GFPMWristDeploys, GFPMWristReleases,
			GFPMWristForceHolsters, GFPMWristHandednessReps, GFPMWristHandednessIgnored));

		Emit(FString::Printf(
			TEXT("[FPM] wrist-slot: refusals - SlotOccupied %d, SlotDisabled %d, VersionMismatch %d, "
			     "NotAWristItem %d. COVERAGE: SlotDisabled becomes non-zero the moment anything calls "
			     "Equip/Deploy with FPM.Wrist.Enabled 0; VersionMismatch is exercised every boot by "
			     "FPM.Wrist.SelfTest whether or not a consumer mod exists."),
			GFPMWristRefusalCounts[static_cast<int32>(EFPMWristRefusal::SlotOccupied)],
			GFPMWristRefusalCounts[static_cast<int32>(EFPMWristRefusal::SlotDisabled)],
			GFPMWristRefusalCounts[static_cast<int32>(EFPMWristRefusal::VersionMismatch)],
			GFPMWristRefusalCounts[static_cast<int32>(EFPMWristRefusal::NotAWristItem)]));

		Emit(FString::Printf(
			TEXT("[FPM] wrist-slot: persistence - %d item(s) registered pending-worn from a save, "
			     "%d handshake(s) completed, %d still pending. COVERAGE: all three stay 0 until a save "
			     "actually contains a worn wrist item; a non-zero pending count with 0 completed means "
			     "the component half never arrived, or the feature is OFF and the item is correctly "
			     "sitting HOLSTERED-INERT."),
			GFPMWristPendingRegistered, GFPMWristPendingCompleted, GFPMWristPendingWorn.Num()));
	}

	static FAutoConsoleCommandWithOutputDevice GFPMWristReportCmd(
		TEXT("FPM.Wrist.Report"),
		TEXT("Print the wrist-slot catalog, the live component count, every action and refusal "
		     "counter, and the coverage note that says what each zero means."),
		FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
		{
			GFPMWristLogReport(&Ar);
		}));

	static FAutoConsoleCommandWithOutputDevice GFPMWristSelfTestCmd(
		TEXT("FPM.Wrist.SelfTest"),
		TEXT("Drive the public API's version gate against a known-positive and two known-negatives, "
		     "and round-trip one probe entry through the registry. Proves the accept/refuse decision "
		     "works without waiting for a third-party consumer mod to exist."),
		FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
		{
			GFPMWristSelfTest(&Ar);
		}));
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// THE COMPONENT
// ════════════════════════════════════════════════════════════════════════════════════════════════

UFPMWristSlotComponent::UFPMWristSlotComponent()
{
	// Nothing here polls. The slot reacts to equip/deploy calls and to replication; a tick would be
	// pure cost with no work to do.
	PrimaryComponentTick.bCanEverTick = false;

	/*
	 * ⚠ SetIsReplicatedByDefault, NOT SetIsReplicated, AND THE DIFFERENCE IS AN ENSURE.
	 * [MEASURED - engine source, this task]
	 * `UActorComponent::SetIsReplicated` opens with
	 * `ensureMsgf(!NeedsInitialization(), TEXT("SetIsReplicatedByDefault is preferred during Component
	 * Construction."))` (ActorComponent.cpp:2837), and `SetIsReplicatedByDefault` is the API written
	 * for exactly this moment (ActorComponent.cpp:3240-3253). Because `bReplicates` is true before
	 * `AddOwnedComponent` runs, the owner puts this component into `ReplicatedComponents` on the spot
	 * (Actor.cpp:3766-3770) and `ReadyForReplication` is called lazily at replication time
	 * (ActorReplication.cpp:667, :980). So a component created at RUNTIME, after the character has
	 * already begun play, still replicates - which is the whole premise of the add-hook below.
	 */
	SetIsReplicatedByDefault(true);
}

void UFPMWristSlotComponent::BeginPlay()
{
	Super::BeginPlay();

	GFPMWristLiveComponents.AddUnique(this);
	++GFPMWristComponentsSeenLocal;

	const AActor* OwnerActor = GetOwner();
	const bool bAuthority = OwnerActor != nullptr && OwnerActor->HasAuthority();

	if (!bAuthority)
	{
		/*
		 * ★ THE REPLICATION WITNESS, and it is the one instrument in this file that can prove the
		 * multiplayer path end to end.
		 *
		 * A client NEVER creates this component: the add-hook is guarded on HasAuthority(). So this
		 * component existing on a machine without authority can only mean the server's component
		 * replicated down. That makes a NON-ZERO here positive proof, and a ZERO on a client with a
		 * live character a real, reportable failure rather than an absence of news. B18 reads exactly
		 * this line on the two-client rig.
		 */
		++GFPMWristComponentsSeenRemote;

		if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] wrist-slot: component arrived on a NON-AUTHORITY machine for %s (#%d). "
				     "This machine never creates one, so this line is proof that dynamic component "
				     "replication delivered it."),
				*GetNameSafe(OwnerActor), GFPMWristComponentsSeenRemote);
		}
	}

	AFGCharacterPlayer* Character = Cast<AFGCharacterPlayer>(GetOwner());

	/*
	 * THE OWNING CLIENT PUSHES ITS OWN HANDEDNESS as soon as its copy of the component exists.
	 *
	 * The header ties `Server_SetHandedness` to `FFPMWristSlotHook`'s `OnPlayerInputInitialized`
	 * handler, and that binding is there in `Arm()`. This second push is not a duplicate of it, it
	 * covers a real ordering hole: input initialisation can complete BEFORE this component has
	 * replicated down, and in that case the input-init handler finds no component and the server
	 * never learns the local preference. Both pushes are idempotent - the server ignores a value
	 * that is already set - so running both costs one refused RPC at worst.
	 *
	 * In single player the authority IS the locally-controlled client, so this is a direct local
	 * call and no round trip happens.
	 */
	if (Character != nullptr && Character->IsLocallyControlled())
	{
		Server_SetHandedness(GFPMCVarWristRightHanded.GetValueOnGameThread() != 0);
	}

	// The component half of design 11.2.3's handshake. Deliberately here rather than inside the
	// add-hook: at this point the component is fully registered and BeginPlay-ed, so an equip that
	// runs now cannot race its own registration.
	GFPMWristTryCompleteHandshake(Character);
}

void UFPMWristSlotComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GFPMWristLiveComponents.RemoveAll(
		[this](const TWeakObjectPtr<UFPMWristSlotComponent>& Weak) { return Weak.Get() == this; });

	/*
	 * ⚠ NO FORCED UNEQUIP AND NO DESTRUCTION OF THE ITEM HERE, AND THAT IS DELIBERATE.
	 * The worn item is world content that persists in the save (design 11.2.3, Ruling 21). Tearing it
	 * down when a character streams out or the world ends would be FPM deleting the player's item -
	 * the failure the persistence design exists to prevent. The component goes; the item does not.
	 */
	Super::EndPlay(EndPlayReason);
}

void UFPMWristSlotComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	/*
	 * All three are UNCONDITIONAL, not COND_OwnerOnly. Design 11.2.2 wants the wrist item visible on
	 * other players' characters, which means every client needs the equipped actor, the deployed
	 * state and the side. `bRightHanded` in particular decides which arm a watching player sees the
	 * item on, so restricting it to the owner would attach it to the wrong arm for everyone else.
	 */
	DOREPLIFETIME(UFPMWristSlotComponent, mEquippedItemActor);
	DOREPLIFETIME(UFPMWristSlotComponent, bDeployed);
	DOREPLIFETIME(UFPMWristSlotComponent, bRightHanded);
}

IFPMWristItem* UFPMWristSlotComponent::GetEquippedInterface() const
{
	/*
	 * ⚠ THIS RETURNS nullptr FOR A PURE-BLUEPRINT WRIST ITEM, AND CALLERS MUST KNOW THAT.
	 * `Cast<IFPMWristItem>` walks native interfaces only; a Blueprint class that implements the
	 * UINTERFACE has no native interface pointer to hand back. The interface is `Blueprintable`
	 * precisely so a third-party item may be pure Blueprint, so this accessor is a convenience for
	 * C++ consumers and NOT the route this file uses internally - every call site below goes through
	 * `ImplementsInterface` + the generated `Execute_` wrappers, which work for both kinds.
	 */
	return mEquippedItemActor != nullptr ? Cast<IFPMWristItem>(mEquippedItemActor.Get()) : nullptr;
}

void UFPMWristSlotComponent::OnRep_Handedness()
{
	++GFPMWristHandednessReps;

	if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] wrist-slot: handedness replicated to %s on %s (#%d). Second replication "
			     "witness: this only fires on a machine that received the update from the authority."),
			bRightHanded ? TEXT("RIGHT") : TEXT("LEFT"), *GetNameSafe(GetOwner()),
			GFPMWristHandednessReps);
	}

	/*
	 * No local re-attach here. Attachment is decided on the authority and reaches this machine
	 * through the item actor's own `AttachmentReplication` [MEASURED - Actor.cpp:1998 replicates
	 * AttachmentReplication whenever the root component does not, independently of movement
	 * replication, so AFPMWristItemBase's SetReplicatingMovement(false) does not suppress it].
	 * Re-attaching locally would fight that and show as a one-frame snap.
	 */
}

// ── CLIENT-TO-SERVER ACTIONS ────────────────────────────────────────────────────────────────────

void UFPMWristSlotComponent::Server_Equip_Implementation(AActor* ItemActor)
{
	/*
	 * ⚠ AUTHORITY IS RE-CHECKED HERE EVEN THOUGH THIS IS A Server RPC. In single player the RPC is a
	 * direct local call, and `GFPMWristTryCompleteHandshake` calls it directly too - so "it arrived,
	 * therefore we are the server" is not true of every path into this function. This is the
	 * per-call authority question the fix contract insists stays at the call site.
	 */
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority())
	{
		return;
	}

	if (!IsWristFeatureEnabled())
	{
		GFPMWristCountRefusal(EFPMWristRefusal::SlotDisabled);
		Client_OnActionRefused(TEXT("Equip"), EFPMWristRefusal::SlotDisabled);
		return;
	}

	if (ItemActor == nullptr || !ItemActor->GetClass()->ImplementsInterface(UFPMWristItem::StaticClass()))
	{
		GFPMWristCountRefusal(EFPMWristRefusal::NotAWristItem);
		Client_OnActionRefused(TEXT("Equip"), EFPMWristRefusal::NotAWristItem);

		// Believed unreachable through the public API - but per the project's evidence discipline an
		// unreachable path gets a line rather than silence, unthrottled, because every occurrence is
		// a finding.
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] wrist-slot: Server_Equip refused - %s does not implement IFPMWristItem."),
			*GetNameSafe(ItemActor));
		return;
	}

	if (mEquippedItemActor != nullptr)
	{
		/*
		 * NO SILENT UNEQUIP OF A THIRD PARTY'S ITEM - design 11.2.5's table: "The UI offers a swap.
		 * No silent unequip ... the swap is user-initiated." The refusal is typed so a UI can offer
		 * exactly that instead of guessing why nothing happened.
		 */
		GFPMWristCountRefusal(EFPMWristRefusal::SlotOccupied);
		Client_OnActionRefused(TEXT("Equip"), EFPMWristRefusal::SlotOccupied);
		return;
	}

	AFGCharacterPlayer* Character = Cast<AFGCharacterPlayer>(GetOwner());

	mEquippedItemActor = ItemActor;
	++GFPMWristEquips;

	// The item's own Equip is BOOKKEEPING, never a second gate - the interface header says so, and
	// this function has already made every decision.
	IFPMWristItem::Execute_WristEquip(ItemActor, Character);

	if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] wrist-slot: %s equipped %s (id=%s) on the %s arm."),
			*GetNameSafe(Character), *GetNameSafe(ItemActor),
			*IFPMWristItem::Execute_GetWristItemId(ItemActor).ToString(),
			bRightHanded ? TEXT("RIGHT") : TEXT("LEFT"));
	}
}

void UFPMWristSlotComponent::Server_Unequip_Implementation()
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority())
	{
		return;
	}

	AActor* Item = mEquippedItemActor;
	if (Item == nullptr)
	{
		return;   // nothing worn: not a refusal, there is simply nothing to undo
	}

	/*
	 * ONE ORDER, ALWAYS - design 11.2.5: "The rope force-releases first, with momentum preserved
	 * (identical to a release input), then the item unequips." Unequipping a deployed item without
	 * releasing it first would strand whatever the item is holding.
	 *
	 * ⚠ NOTE THE ORDER IS NOT GATED ON `IsWristFeatureEnabled()`. Unequip must work while the feature
	 * is OFF, or turning the feature off would trap an item on the player forever.
	 */
	if (bDeployed)
	{
		bDeployed = false;
		++GFPMWristReleases;
		IFPMWristItem::Execute_WristRelease(Item);
	}

	mEquippedItemActor = nullptr;
	++GFPMWristUnequips;
	IFPMWristItem::Execute_WristUnequip(Item);

	if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] wrist-slot: %s unequipped %s. The ITEM is untouched - it is world content and "
			     "persists in the save."),
			*GetNameSafe(GetOwner()), *GetNameSafe(Item));
	}
}

void UFPMWristSlotComponent::Server_Deploy_Implementation()
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority())
	{
		return;
	}

	if (!IsWristFeatureEnabled())
	{
		GFPMWristCountRefusal(EFPMWristRefusal::SlotDisabled);
		Client_OnActionRefused(TEXT("Deploy"), EFPMWristRefusal::SlotDisabled);
		return;
	}

	AActor* Item = mEquippedItemActor;
	if (Item == nullptr || bDeployed)
	{
		return;   // nothing worn, or already deployed: idempotent, not a refusal
	}

	// The item may decline (a cooldown, for instance). The component does NOT second-guess a
	// refusal - the interface header's own contract.
	if (!IFPMWristItem::Execute_WristDeploy(Item))
	{
		if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] wrist-slot: %s declined its own deploy on %s. Not overridden - the item "
				     "owns that decision."),
				*GetNameSafe(Item), *GetNameSafe(GetOwner()));
		}
		return;
	}

	bDeployed = true;
	++GFPMWristDeploys;
}

void UFPMWristSlotComponent::Server_Release_Implementation()
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority())
	{
		return;
	}

	AActor* Item = mEquippedItemActor;
	if (Item == nullptr || !bDeployed)
	{
		return;
	}

	// Release cannot be refused by the item - interface contract - and is deliberately NOT gated on
	// the feature switch, because OFF-means-RELEASED needs this path to work while the feature is off.
	bDeployed = false;
	++GFPMWristReleases;
	IFPMWristItem::Execute_WristRelease(Item);
}

void UFPMWristSlotComponent::Server_SetHandedness_Implementation(bool bNewRightHanded)
{
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority())
	{
		return;
	}

	if (bNewRightHanded == bRightHanded)
	{
		return;   // no change: silence, or the two idempotent pushes would fill a log
	}

	if (bDeployed)
	{
		/*
		 * NO MID-FLIGHT RE-ATTACH - design 11.2.2: "the deployed rope keeps its current arm until
		 * release; the new side applies on the next deployment." The header says this is enforced
		 * here by ignoring the change while deployed, and it is: the value is DROPPED, not queued.
		 * The client's cvar keeps its new value, so the next push after release lands it.
		 */
		++GFPMWristHandednessIgnored;

		if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] wrist-slot: handedness change to %s IGNORED on %s - an item is deployed. "
				     "It applies on the next deployment (#%d)."),
				bNewRightHanded ? TEXT("RIGHT") : TEXT("LEFT"), *GetNameSafe(GetOwner()),
				GFPMWristHandednessIgnored);
		}
		return;
	}

	bRightHanded = bNewRightHanded;

	/*
	 * ⚠ OnRep_ DOES NOT FIRE ON THE AUTHORITY. Any authority-side reaction has to be written here as
	 * well, and forgetting that is one of the standard ways a replicated property "works in single
	 * player and not on a server". Here the reaction is the re-attach below plus the counter.
	 */
	AActor* Item = mEquippedItemActor;
	if (Item != nullptr)
	{
		/*
		 * RE-ATTACH THE HOLSTERED ITEM AT THE NEW SOCKET, and the route is a judgement call worth
		 * stating. `IFPMWristItem` has no re-attach entry point, so the choices were: call
		 * Unequip+Equip (which logs a misleading "unequipped" and momentarily empties the slot), add
		 * a member to the interface (a Minor API bump for a case the interface already covers), or
		 * re-run Equip. Re-running Equip is chosen: its contract is explicitly "bookkeeping (attach,
		 * remember the owner), never a second gate", so it is idempotent by design and re-resolves
		 * `GetWristAttachSocket(bRightHanded)` against the new value. The slot is never empty at any
		 * point.
		 */
		IFPMWristItem::Execute_WristEquip(Item, Cast<AFGCharacterPlayer>(GetOwner()));
	}

	if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] wrist-slot: handedness for %s is now %s%s."),
			*GetNameSafe(GetOwner()), bNewRightHanded ? TEXT("RIGHT") : TEXT("LEFT"),
			Item != nullptr ? TEXT(" (worn item re-attached at the new socket)") : TEXT(""));
	}
}

void UFPMWristSlotComponent::Client_OnActionRefused_Implementation(FName Action, EFPMWristRefusal Reason)
{
	// The acting client's own UI surface. Logged so a refusal that a UI swallows is still readable in
	// FactoryGame.log afterwards - a refusal nobody can see is how "it did nothing" becomes a bug
	// report with no evidence in it.
	if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] wrist-slot: %s refused - %s."),
			*Action.ToString(), GFPMWristRefusalText(Reason));
	}
}

void UFPMWristSlotComponent::ForceHolster(const TCHAR* Reason)
{
	/*
	 * ★ THE OFF CONTRACT'S TEETH (design 11.2.6, and 5.1's OFF-means-RELEASED extended to the API).
	 *
	 * IT HOLSTERS, IT DOES NOT UNEQUIP, and the header's own idempotency clause is what settles that:
	 * "a no-op on an already-holstered component". Unequipping would remove the item from the slot
	 * while the save still says it is worn, so an ON toggle would have to re-derive worn state from
	 * somewhere - and OFF never deletes save state (11.2.6: "OFF releases BEHAVIOR, the save keeps
	 * the ITEM").
	 */
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority())
	{
		return;   // deploy state is replicated FROM the authority; a client release would be reverted
	}

	if (!bDeployed)
	{
		return;   // idempotent
	}

	bDeployed = false;
	++GFPMWristReleases;
	++GFPMWristForceHolsters;

	if (AActor* Item = mEquippedItemActor)
	{
		IFPMWristItem::Execute_WristRelease(Item);
	}

	// Unthrottled: this only happens when Ant turns something off, and it is the line that explains
	// why a deployed item let go on its own.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] wrist-slot: force-holstered %s (%s). The item stays equipped and stays in the "
		     "save; only the deployed state was released."),
		*GetNameSafe(GetOwner()), Reason != nullptr ? Reason : TEXT("no reason given"));
}

// ── THE PUBLIC, STATIC API ──────────────────────────────────────────────────────────────────────

bool UFPMWristSlotComponent::RegisterWristItem(FName OwnerModReference, FName ItemId,
	TSoftClassPtr<AActor> ItemClass, const FText& DisplayName, int32 ConsumerMajor, int32 ConsumerMinor,
	EFPMWristRefusal& OutRefusal)
{
	OutRefusal = EFPMWristRefusal::None;

	if (!GFPMWristVersionAccepted(ConsumerMajor, ConsumerMinor))
	{
		OutRefusal = EFPMWristRefusal::VersionMismatch;
		GFPMWristCountRefusal(EFPMWristRefusal::VersionMismatch);

		/*
		 * ⚠ REFUSED LOUDLY, AT REGISTRATION, AND NEVER GATED BEHIND A DIAG CHANNEL.
		 * Design 11.2.6: "refused at registration with one log line naming both pairs - refused
		 * loudly at registration, never discovered as a crash later." A silenced version refusal
		 * would send the consumer's author hunting a null pointer weeks later.
		 */
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] wrist-slot: REFUSED registration of '%s' by '%s' - it was compiled against "
			     "wrist API {%d,%d} and this FPM hosts {%d,%d}. Accepted when major matches exactly "
			     "and consumer minor <= host minor."),
			*ItemId.ToString(), *OwnerModReference.ToString(),
			ConsumerMajor, ConsumerMinor, FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR);
		return false;
	}

	FFPMWristRegistration Entry;
	Entry.Owner = OwnerModReference;
	Entry.ItemId = ItemId;
	Entry.ItemClass = ItemClass;
	Entry.DisplayName = DisplayName;
	Entry.ConsumerMajor = ConsumerMajor;
	Entry.ConsumerMinor = ConsumerMinor;

	// Duplicate {Owner, ItemId} REPLACES its own entry. Two different owners can never collide,
	// because the owner is half the key - design 11.2.6.
	const int32 Existing = GFPMWristFindRegistration(OwnerModReference, ItemId);
	const bool bReplaced = Existing != INDEX_NONE;
	if (bReplaced)
	{
		GFPMWristRegistry[Existing] = MoveTemp(Entry);
	}
	else
	{
		GFPMWristRegistry.Add(MoveTemp(Entry));
	}

	/*
	 * REGISTRATION SUCCEEDS EVEN WHILE THE FEATURE IS OFF - design 11.2.6's OFF contract: "a mod that
	 * registers while the feature is off is not lost." Only Equip/Deploy decline.
	 */
	if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] wrist-slot: %s wrist item '%s' from '%s' (compiled against {%d,%d}). %d item(s) "
			     "registered. Feature is currently %s - registration is retained either way."),
			bReplaced ? TEXT("re-registered") : TEXT("registered"),
			*ItemId.ToString(), *OwnerModReference.ToString(), ConsumerMajor, ConsumerMinor,
			GFPMWristRegistry.Num(),
			IsWristFeatureEnabled() ? TEXT("ENABLED") : TEXT("DISABLED"));
	}

	return true;
}

void UFPMWristSlotComponent::GetWristApiVersion(int32& OutMajor, int32& OutMinor)
{
	OutMajor = FPM_WRIST_API_MAJOR;
	OutMinor = FPM_WRIST_API_MINOR;
}

bool UFPMWristSlotComponent::IsWristFeatureEnabled()
{
	// BOTH gates, and the master switch is checked first because it is the one that outranks
	// everything else (FPMMasterSwitch.h:77).
	return FPMMasterSwitch::IsEnabled() && GFPMCVarWristEnabled.GetValueOnGameThread() != 0;
}

int32 UFPMWristSlotComponent::GetRegisteredWristItemCount()
{
	return GFPMWristRegistry.Num();
}

// ── THE PERSISTENCE HANDSHAKE ───────────────────────────────────────────────────────────────────

void UFPMWristSlotComponent::RegisterPendingWorn(AFGCharacterPlayer* Character, AFPMWristItemBase* Item)
{
	if (Character == nullptr || Item == nullptr)
	{
		return;
	}

	GFPMWristPendingWorn.Add(Character, Item);
	++GFPMWristPendingRegistered;

	if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] wrist-slot: %s registered pending-worn under %s (#%d)."),
			*GetNameSafe(Item), *GetNameSafe(Character), GFPMWristPendingRegistered);
	}

	// The item may be the SECOND half to arrive - the component's BeginPlay may already have run and
	// found nothing. Try to complete from this side too; the helper is a no-op when the other half is
	// missing, and it leaves the entry in place when the feature is OFF.
	GFPMWristTryCompleteHandshake(Character);
}

AFPMWristItemBase* UFPMWristSlotComponent::ClaimPendingWorn(AFGCharacterPlayer* Character)
{
	if (Character == nullptr)
	{
		return nullptr;
	}

	TWeakObjectPtr<AFPMWristItemBase> Found;
	if (!GFPMWristPendingWorn.RemoveAndCopyValue(Character, Found))
	{
		return nullptr;
	}

	// A weak pointer that has gone stale between the two halves reads as "no pending item", which is
	// the correct answer - not a dangling pointer, and not a crash on a character that was destroyed
	// mid-load.
	return Found.Get();
}

// ════════════════════════════════════════════════════════════════════════════════════════════════
// THE ADD-HOOK
// ════════════════════════════════════════════════════════════════════════════════════════════════

FFPMWristSlotHook& FFPMWristSlotHook::Get()
{
	static FFPMWristSlotHook Instance;
	return Instance;
}

FPMDiag::EChannel FFPMWristSlotHook::Channel() const
{
	return FPMDiag::EChannel::WristSlot;
}

void FFPMWristSlotHook::Arm()
{
	AFGCharacterPlayer* Sample = GetMutableDefault<AFGCharacterPlayer>();

	/*
	 * ⚠ AN "AFTER" HANDLER TAKES NO SCOPE. SML's AddHandlerAfter wants `TFunction<void(C*)>` exactly
	 * (NativeHookManager.h:525) - vanilla has already run, so there is no call left to cancel. Writing
	 * `auto& Scope` here is a compile error, which is the right place to find out.
	 *
	 * WHY BeginPlay AND NOT PossessedBy (both verified virtual overrides, FGCharacterPlayer.h:377 and
	 * :391): BeginPlay fires exactly once per character actor, so the add is idempotent by
	 * construction, and it fires for EVERY character - PossessedBy would miss the persisted bodies of
	 * offline players and would fire repeatedly across possession changes. Design 11.2.2 rejects
	 * PossessedBy with those reasons.
	 */
	auto OnCharacterBeginPlay = [](AFGCharacterPlayer* Self)
	{
		if (Self == nullptr)
		{
			return;
		}

		/*
		 * ⚠ AUTHORITY ONLY, AND THIS GUARD IS THE WHOLE MULTIPLAYER DESIGN IN ONE LINE.
		 * The server creates the component and standard component replication delivers it to clients
		 * [MEASURED - DataChannel.cpp:5064 creates unknown subobjects client-side from the content
		 * block]. If the client created its own instead, it would be a DIFFERENT object with no
		 * NetGUID relationship to the server's, no replicated properties would ever arrive, and every
		 * Server RPC would be sent on an object the server has never heard of. That failure is
		 * completely silent in single player, which is exactly why the guard is here and not
		 * anywhere later.
		 */
		if (!Self->HasAuthority())
		{
			return;
		}

		// Idempotent by construction via BeginPlay, but a cheap belt-and-braces check: a second
		// component would give the character two slots and split the replicated state in half.
		if (Self->FindComponentByClass<UFPMWristSlotComponent>() != nullptr)
		{
			return;
		}

		UFPMWristSlotComponent* Component = NewObject<UFPMWristSlotComponent>(Self);
		if (Component == nullptr)
		{
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] wrist-slot: NewObject returned null for %s - this character has NO wrist "
				     "slot this session."),
				*GetNameSafe(Self));
			return;
		}

		Component->RegisterComponent();
		++GFPMWristComponentsAdded;

		if (FPMDiag::IsOn(FPMDiag::EChannel::WristSlot))
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] wrist-slot: added a slot component to %s on the authority (#%d). Clients "
				     "receive it by component replication; they never create one."),
				*GetNameSafe(Self), GFPMWristComponentsAdded);
		}
	};

	BeginPlayHandle = FPM_SUBSCRIBE_VIRTUAL_AFTER("wrist-slot", AFGCharacterPlayer::BeginPlay, Sample, OnCharacterBeginPlay);

	/*
	 * ★ THE INPUT-INIT BINDING IS NOT A LEDGER HOOK, AND THAT IS CORRECT RATHER THAN AN OVERSIGHT.
	 *
	 * `AFGCharacterPlayer::OnPlayerInputInitialized` is a plain static multicast delegate the GAME
	 * publishes for exactly this purpose - `DECLARE_MULTICAST_DELEGATE_TwoParams(...,
	 * AFGCharacterPlayer*, UInputComponent*)` at FGCharacterPlayer.h:45, declared at :1391 [MEASURED,
	 * direct header read]. It is not a native detour, so there is nothing for FPMHookLedger to
	 * record: the ledger's job is to inventory the hooks FPM INSTALLS over the game's code, and
	 * listing a public delegate subscription there would misreport FPM's detour surface.
	 */
	InputInitHandle = AFGCharacterPlayer::OnPlayerInputInitialized.AddLambda(
		[](AFGCharacterPlayer* Character, UInputComponent* /*InputComponent*/)
		{
			if (Character == nullptr || !Character->IsLocallyControlled())
			{
				return;
			}

			// The owning client pushes ITS OWN local preference. If the component has not replicated
			// down yet, the component's own BeginPlay does the same push when it arrives - the two
			// cover each other's ordering hole and the server ignores a value already set.
			if (UFPMWristSlotComponent* Component = Character->FindComponentByClass<UFPMWristSlotComponent>())
			{
				Component->Server_SetHandedness(GFPMCVarWristRightHanded.GetValueOnGameThread() != 0);
			}
		});

	/*
	 * THE TWO OFF ROUTES. Both end in the same transition function, so the OFF contract cannot be
	 * half-implemented on one route and not the other.
	 */
	if (IConsoleVariable* EnabledVar = GFPMCVarWristEnabled.AsVariable())
	{
		EnabledVar->SetOnChangedCallback(FConsoleVariableDelegate::CreateLambda(
			[](IConsoleVariable*) { GFPMWristApplyFeatureState(TEXT("FPM.Wrist.Enabled")); }));
	}

	if (!GFPMWristStopHookRegistered)
	{
		/*
		 * ⚠ REGISTERED FROM Arm(), NOT FROM A FILE-SCOPE STATIC INITIALIZER, and the ordering is
		 * checked rather than assumed: `FPMMasterSwitch::Install()` is called AFTER every fix has
		 * armed (FicsitsPerformanceManager.cpp, the P4.2 comment above the Install call), and the OFF
		 * branch cannot be reached before the cvar it hangs on exists. The guard flag is here because
		 * `RegisterStopHook` has no unregister, so a re-Arm after a master-switch ON must not stack a
		 * second copy.
		 */
		FPMMasterSwitch::RegisterStopHook([]() { GFPMWristApplyFeatureState(TEXT("FPM.Enabled 0")); });
		GFPMWristStopHookRegistered = true;
	}

	// The local handedness preference, pushed to the server when it changes rather than at the next
	// equip - otherwise a settings change would look like it did nothing until a relog.
	if (IConsoleVariable* HandVar = GFPMCVarWristRightHanded.AsVariable())
	{
		HandVar->SetOnChangedCallback(FConsoleVariableDelegate::CreateLambda(
			[](IConsoleVariable* Var)
			{
				const bool bWantRight = Var->GetInt() != 0;
				GFPMWristCompactRosters();
				for (const TWeakObjectPtr<UFPMWristSlotComponent>& Weak : GFPMWristLiveComponents)
				{
					UFPMWristSlotComponent* Component = Weak.Get();
					if (Component == nullptr) { continue; }

					const AFGCharacterPlayer* Character = Cast<AFGCharacterPlayer>(Component->GetOwner());
					if (Character != nullptr && Character->IsLocallyControlled())
					{
						Component->Server_SetHandedness(bWantRight);
					}
				}
			}));
	}

	// Seed the transition detector so the first real toggle is seen as a CHANGE.
	GFPMWristLastEnabled = UFPMWristSlotComponent::IsWristFeatureEnabled();
	GFPMWristStateSeeded = true;

	// The API is verified at every arm, not only when somebody types the command. Same discipline as
	// FPMCVarWriter::SelfTest at the top of StartupModule.
	const bool bSelfTestPassed = GFPMWristSelfTest(nullptr);

	/*
	 * THE ARMED LINE - printed regardless of diagnostic level, per FPMDiag.h's one stated exception.
	 * It is the line that tells "armed and saw nothing" apart from "never armed".
	 */
	if (BeginPlayHandle.IsValid())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] wrist slot ARMED - a UFPMWristSlotComponent is added to every AFGCharacterPlayer "
			     "on the authority and replicates to clients. Public API {%d,%d}, self-test %s. "
			     "FPM.Wrist.Enabled toggles the OFF contract, FPM.Wrist.RightHanded picks the arm, "
			     "FPM.Wrist.Report prints the catalog and its coverage. FPM ships no wrist item of its "
			     "own yet, so an empty catalog here is expected."),
			FPM_WRIST_API_MAJOR, FPM_WRIST_API_MINOR,
			bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] wrist slot NOT armed - the hook install FAILED on AFGCharacterPlayer::BeginPlay. "
			     "No character will get a wrist slot this session, the public API will answer with an "
			     "empty registry, and any persisted worn item will stay pending forever."));
	}
}

void FFPMWristSlotHook::Disarm()
{
	/*
	 * ⚠ DISARM REMOVES THE HOOKS AND LEAVES EVERY EXISTING COMPONENT ALONE. Design 11.2.6: "The
	 * component's LIFETIME is bound to FPM-installed, not to any feature toggle ... The only state in
	 * which the component is truly absent is FPM uninstalled." Destroying components here would make
	 * `FPM.Fix.WristSlot 0` a different, harsher thing than `FPM.Wrist.Enabled 0`, and would drop
	 * third-party references that the OFF contract promises stay live.
	 *
	 * Guarded on IsValid() because the editor path installs nothing and returns an invalid handle;
	 * RemoveHandler would then walk maps SML never allocated.
	 */
	if (BeginPlayHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(AFGCharacterPlayer::BeginPlay, BeginPlayHandle);
		BeginPlayHandle.Reset();
	}

	if (InputInitHandle.IsValid())
	{
		AFGCharacterPlayer::OnPlayerInputInitialized.Remove(InputInitHandle);
		InputInitHandle.Reset();
	}

	// The sinks would otherwise keep firing into a disarmed fix - and a handedness push from a
	// disarmed slot is exactly the kind of half-off state that makes a toggle untrustworthy.
	if (IConsoleVariable* EnabledVar = GFPMCVarWristEnabled.AsVariable())
	{
		EnabledVar->SetOnChangedCallback(FConsoleVariableDelegate());
	}
	if (IConsoleVariable* HandVar = GFPMCVarWristRightHanded.AsVariable())
	{
		HandVar->SetOnChangedCallback(FConsoleVariableDelegate());
	}

	// The master-switch stop hook cannot be unregistered (no API for it). It stays, and it is safe:
	// GFPMWristApplyFeatureState walks a roster of live components and does nothing when it is empty.
}
