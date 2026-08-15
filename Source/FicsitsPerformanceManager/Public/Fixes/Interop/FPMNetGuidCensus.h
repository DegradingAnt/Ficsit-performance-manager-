// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * NET-GUID CENSUS, plus FPM.Net.Report. Two net INSTRUMENTS, no behaviour change anywhere.
 *
 * THE RULE FIRST, because the file name invites the wrong reading. "NO NETWORK ACTIVITY IN FPM, EVER"
 * bans transports FPM opens ITSELF. Nothing in this file opens one. It reads counters the engine
 * already keeps, and counts calls the engine already makes on the game's OWN replication, which is the
 * game's transport and not ours.
 *
 * ===== WHY IT EXISTS =====
 *
 * Survey of 2026-08-15 over the 12 Linux dedicated-server logs (87.8 MB, 2026-08-06 to 08-07, two
 * players): 33,390 LogNetPackageMap warnings, 4.7% of all 706,398 server log lines, peaking at 349 in
 * a single second at 2026.08.06-21.40.55. Two families:
 *   A. 24,462  UPackageMapClient::InternalLoadObject "Unable to resolve default guid from client"
 *              (PackageMapClient.cpp:1210), each preceded by a global StaticFindObject at :1199.
 *   B.  8,928  FNetGUIDCache::SupportsObject "... NOT Supported" (PackageMapClient.cpp:3137), each
 *              building a full object path just to print the warning.
 * Both are server-side game-thread work on the DatHost box. The COUNTS are facts. The per-event
 * millisecond cost is a HYPOTHESIS, because nothing has ever profiled that machine.
 *
 * That corpus is nine days old and the tree has moved since. This census exists to answer ONE question
 * from live data: is family B still happening, and on which classes.
 *
 * ===== WHAT IT COVERS, AND WHAT IT CANNOT =====
 *
 * IT COVERS FAMILY B ONLY: 8,928 of 33,390, i.e. 27% of the measured signal. It prints that ratio in
 * its own report, because an instrument that stays quiet about its blind half reads as a clean bill of
 * health for the whole.
 *
 * !! FAMILY A IS NOT HOOKED, AND THE REASON IS AN ABI HAZARD RATHER THAN AN OVERSIGHT.
 * The natural target is UPackageMapClient::InternalLoadObject (ENGINE_API, PackageMapClient.h:592),
 * which returns FNetworkGUID BY VALUE. FNetworkGUID holds one uint32 (NetworkGuid.h:11-18), so the
 * MSVC x64 ABI returns it in a register. SML chooses its trampoline from std::is_class<ReturnType>
 * ALONE (NativeHookManager.h:458-460) and, for any class type, selects ApplyCallUserTypeByValue, which
 * reinterpret_casts the real function to a hidden-out-pointer signature (NativeHookManager.h:405-410).
 * For a register-returned struct that is the wrong calling convention. It would compile clean and
 * corrupt the NetGUID path at runtime, on the dedicated server, inside the network stack, which is the
 * hardest place this project has to debug. So family A stays unmeasured until either SML gains an
 * ABI-correct path or a scalar-returning choke point is found. Recorded here so the gap is a decision
 * on the record, not a hole a later reader mistakes for coverage.
 *
 * ===== WHY THIS HOOK TARGET IS SAFE =====
 *
 * FNetGUIDCache::SupportsObject (ENGINE_API, PackageMapClient.h:221) returns bool, a scalar, so it
 * takes SML ApplyCallScalar and the hazard above cannot apply. It is const and sits on a plain
 * non-UObject class; both are supported, and FPM already hooks a non-UObject class exactly this way
 * (FPMJoinVersionEcho.cpp:123, on FSMLNetworkManager). It has three callers, all in the
 * object-reference serialization path: PackageMapClient.cpp:3167, :3227 and DataChannel.cpp:5621.
 *
 * ===== OFF BY DEFAULT, ON PRECEDENT =====
 *
 * Those three callers still make this the path every replicated object REFERENCE passes through. FPM
 * has already paid for a census-shaped detour on a universal path once: no-owner-rpc-gate held one on
 * UNetDriver::ProcessRemoteFunction, measured zero suppressions on both of her machines, and cost her
 * hoverpack audio (FPMNoOwnerRpcGate.h:74-97). Repeating that shape by default would be the same
 * mistake under a different function name.
 *
 * So this is DefaultArmed() == false. FPM.Fix.NetGuidCensus 1 arms it, the census names offenders
 * within seconds, and FPM.Fix.NetGuidCensus 0 removes the detour again. A capability one cvar away is
 * not a missing capability.
 *
 * ===== DEAD-INSTRUMENT CHECK, ANSWERED =====
 *
 * What concrete input makes this report a non-zero? Any object reference serialized for an object that
 * is neither IsFullNameStableForNetworking() nor IsSupportedForNetworking(). The 2026-08-07 corpus
 * names four such classes on her own server: LightweightCollisionComponent 3463, StaticMeshComponent
 * 3462, FGColoredInstanceMeshProxy 1298, SceneComponent 642. So a KNOWN POSITIVE exists and is
 * reachable on demand: build or dismantle near any lightweight buildable.
 * What makes it a FAILURE rather than a zero? The report distinguishes "never armed" from "armed and
 * saw nothing", because those are different answers and a bare 0 hides which one you are holding.
 *
 * HYPOTHESISED MECHANISM, receipt attached, NOT proven end to end. FHitResult::NetSerialize serializes
 * its Component pointer (HitResult.cpp:120), and ULightweightCollisionComponent is created at runtime
 * by AAbstractInstanceManager (AbstractInstanceManager.h:205, :251) so it can never be net-addressable.
 * Confirming that from play data is this census's job. Nothing here ACTS on the hypothesis: nulling the
 * component before the send would be bit-equivalent on the wire (GetOrAssignNetGUID returns a default
 * GUID for an unsupported object, PackageMapClient.cpp:3167-3172) but it would change what a client
 * sends a server, and that class of change is deliberately not in this file.
 *
 * !! IT MUST NEVER SUPPRESS THE ENGINE WARNING. The logging law is fix-at-cause. This census only ADDS
 * a bounded set of named lines; the engine keeps saying everything it said before.
 */
class FFPMNetGuidCensus final : public IFPMFix
{
public:
	static FFPMNetGuidCensus& Get();

	virtual const TCHAR* Name() const override { return TEXT("net-guid-census"); }

	/**
	 * Any, and specifically INCLUDING the dedicated server, which is where all 33,390 warnings were
	 * measured. SupportsObject runs on both sides so a client reading is meaningful too, but the
	 * server is the machine this was built for.
	 */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::Any; }

	/**
	 * UnknownCause, matching the other probes. The mechanism above carries a receipt but has not been
	 * proven end to end, and this census IS the diagnostic that value obliges a fix to carry.
	 */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::UnknownCause; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::NetGuidCensus; }

	virtual void Arm() override;

	/** Removes the detour. Without this, DisarmAll and FPM.Fix.NetGuidCensus 0 would report it gone
	 *  while the handler kept running on every object-reference serialization. */
	virtual void Disarm() override;

	/** See OFF BY DEFAULT above. The same ruling no-owner-rpc-gate got, applied before the cost is
	 *  paid rather than after. */
	virtual bool DefaultArmed() const override { return false; }

	/** Rejections counted this session, distinct classes the bounded census has named, and whether
	 *  that census hit its cap. */
	static void GetCounts(uint64& OutRejectedTotal, int32& OutSeenClasses, bool& OutCensusSaturated);

	/** FPM.NetGuidCensus.Report prints these, plus the coverage ratio. */
	static void LogReport(class FOutputDevice* Ar = nullptr);

private:
	/** Handle from Arm(), so Disarm() removes exactly this handler. */
	FDelegateHandle SupportsObjectHandle;
};
