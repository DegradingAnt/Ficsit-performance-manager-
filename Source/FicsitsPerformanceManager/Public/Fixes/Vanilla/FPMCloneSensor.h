// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * THE CLONE SENSOR — log-only. It changes nothing and decides nothing.
 *
 * THE BUG, MEASURED FROM THE LIVE SERVER SAVE (2026-08-08): the save holds 31 player states for 2
 * players, carrying only 2 distinct identities across 3 stamps. Two DIFFERENT state objects carry
 * SunFry's identity, so the vanilla matcher has two perfect candidates and picks arbitrarily — that is
 * "sometimes i get sunfrys items", and it is why she has it worse than Ant, who has one state. The
 * other 28 states carry no identity at all, and empty matches empty perfectly.
 *
 * WHAT IS STILL UNKNOWN, AND WHY THIS EXISTS: which of SunFry's two states is the real one. Both blocks
 * are 32,423 bytes and identical, so nothing measured statically separates them, and PICKING WRONG
 * STRANDS HER INVENTORY. One login with this sensor names the winner directly.
 *
 * IT ALSO DISCRIMINATES THE TWO CAUSE HYPOTHESES with no further code: H1 is MergeIdentities stamping
 * the identity on each claim so several states tie; H2 is SubstractIdentity stripping losers to an
 * empty OfflineId so two empty strings score perfectly. The per-candidate line prints the fields that
 * tell them apart.
 *
 * THE CARRIED IMPROVEMENT OVER THE OLD MOD'S SENSOR: this prints each candidate's OBJECT NAME
 * (BP_PlayerState_C_2147415417 and so on). The old one printed only the player name, which cannot be
 * tied back to the save bytes — and the save bytes are where the two duplicate states were found. The
 * object name is the join key between the log and the measurement.
 *
 * WHY A SENSOR AND NOT THE FIX. The repair is self-healing once it is right, because MergeIdentities
 * stamps the joiner's identity onto the state it claims — FPM only has to be right once per player.
 * But a wrong selection binds a player to SOMEONE ELSE'S character, which is exactly as unrecoverable
 * as losing one. Measure first.
 *
 * ⚠ AND THE OBVIOUS SHORTCUT IS THE ONE THING FORBIDDEN. CSS's own PurgeInactiveClientsFromSave deletes
 * every uncontrolled AFGCharacterPlayer — every offline player's body — because no server-side
 * predicate separates "orphan clone" from "logged out five minutes ago". That is why the clone killer
 * costs people their inventories, Ant included. FPM NEVER DESTROYS PLAYER CHARACTERS: not behind a
 * config flag, not behind a chat command. The eventual fix STEERS THE MATCH and never touches a body.
 *
 * SIDE: AFGGameMode only exists where there is authority, so this cannot run on a joining client. It
 * still checks HasAuthority() at the call site, per the project's standing rule.
 *
 * COST: once per join. Not on any per-frame or per-item path.
 */
class FFPMCloneSensor final : public IFPMFix
{
public:
	static FFPMCloneSensor& Get();

	virtual const TCHAR* Name() const override { return TEXT("clone-sensor"); }
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/** UnknownCause: it is the origin-naming instrument for the clone family, so its own status is the honest one until the
	 * family's mechanism is receipted. 2026-08-09 advanced it -- the inactive array holds the SAME object
	 * twice (equal uid) and every matching candidate is identity-identical -- but WHY duplicates accrue is
	 * still unnamed. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::CloneSensor; }
	virtual void Arm() override;

	/**
	 * Removes all 2 hooks.
	 *
	 * ⚠ Without this, `FPMFixes::DisarmAll()` reports this fix disarmed while its handler keeps
	 * running. Near-harmless at process exit, which is where DisarmAll was called from until P4.2
	 * shipped the master OFF switch (`FPM.Enabled 0`, `FPMMasterSwitch.cpp`) - that is why the
	 * omission survived that long. DisarmAll now also runs mid-session from that switch, which is
	 * exactly why this override has to be correct.
	 */
	virtual void Disarm() override;

private:
	/** Handle from Arm(), so Disarm() removes exactly this handler. */
	FDelegateHandle FindInactiveBeforeHandle;
	/** Handle from Arm(), so Disarm() removes exactly this handler. */
	FDelegateHandle FindInactiveAfterHandle;
};
