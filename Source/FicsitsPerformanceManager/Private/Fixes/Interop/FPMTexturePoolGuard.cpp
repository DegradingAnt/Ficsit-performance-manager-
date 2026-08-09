// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMTexturePoolGuard.h"

#include "FicsitsPerformanceManager.h"

#include "Core/FPMCVarWriter.h"
#include "Core/FPMDiag.h"
#include "UI/FPMChatRelay.h"

#include "HAL/IConsoleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "RHI.h"
#include "RHIStats.h"

namespace
{
	/*
	 * THE TUNING, with each number's reason. These were derived in the old mod across VRAM sizes from
	 * 2 GB to 48 GB; the arithmetic is carried forward, the never-implemented parts are not.
	 */

	/** Nanite's floor, subtracted BEFORE textures. It fails globally at 85% occupancy; textures fail
	 *  gradually and locally. That asymmetry is the whole reason for the ordering. */
	constexpr int64 NaniteFloorMB = 512;

	/** Left unclaimed for Cartograph. Ant 2026-07-19 "give cartograph one gb and the rest to the game",
	 *  raised 2026-07-20: "give the mod 2 gb or vram / and the rest of the vram goes to the game". */
	constexpr int64 CartographReserveMB = 2048;

	/** Render targets and the post chain. Scales with RESOLUTION rather than VRAM, so it is a share with
	 *  a floor and a cap rather than a flat subtraction. */
	constexpr float RendererFraction = 0.15f;
	constexpr int64 RendererFloorMB  = 1024;
	constexpr int64 RendererCapMB    = 2048;

	/** Deliberately unclaimed: other mods, driver overhead, and the possibility that we are wrong. */
	constexpr float HeadroomFraction = 0.20f;

	/** Below this the game's own value is the safer answer — the renderer working set is a far larger
	 *  SHARE of a small card, so a proportional claim risks evicting the buffers the reserves protect. */
	constexpr int64 MinCardMB = 6000;

	/** Epic's own top tier (sg.TextureQuality=3) is 1000 MB. We never write less than the game would. */
	constexpr int64 VanillaTopTierMB = 1000;

	/*
	 * ⚠ NO POOL WRITE FOR THIS LONG AFTER ARMING. A pool resize is a reallocation, i.e. a hitch. Doing
	 * it during the level-load and PSO-compile storm is the one moment it is guaranteed to be felt.
	 * Measured 2026-08-09: hand-typing the raise mid-session produced a 336 ms hitch — that artefact is
	 * exactly what this window exists to avoid, and it is why the console measurement is the WORST case
	 * for hitching rather than the shipped behaviour.
	 */
	constexpr double SettleSeconds = 45.0;

	/** Watchdog cadence. It exists for ONE job: repairing the scalability pass's clobber. */
	constexpr double PollSeconds = 30.0;

	int32 GRaises            = 0;
	int32 GClobbersRepaired  = 0;
	int32 GLastTargetMB      = 0;

	const TCHAR* const PoolCVar = TEXT("r.Streaming.PoolSize");
	const FName        GPoolOwner(TEXT("texture-pool"));
}

int64 FPMTexturePool::QueryTotalVramMB()
{
	/*
	 * RHI, not DXGI. The old implementation carried a whole cached IDXGIAdapter3 path to read Budget and
	 * CurrentUsage — but the static model needs neither, and NOT needing them is precisely what stops the
	 * answer from moving. `DedicatedVideoMemory` is total, static, and cross-platform, which also means
	 * this file compiles for the Linux server target without a PLATFORM_WINDOWS fence around half of it.
	 */
	FTextureMemoryStats Stats;
	RHIGetTextureMemoryStats(Stats);
	return Stats.DedicatedVideoMemory / (1024 * 1024);
}

bool FPMTexturePool::DetectCartograph()
{
	return IPluginManager::Get().FindEnabledPlugin(TEXT("Cartograph")).IsValid();
}

FFPMPoolDecision FPMTexturePool::ComputePoolMB(int64 VramMB, bool bCartographPresent)
{
	FFPMPoolDecision D;
	D.bCartograph = bCartographPresent;

	if (VramMB <= 0)
	{
		D.StandDownReason = TEXT("the RHI has not reported a VRAM size yet");
		return D;
	}

	if (VramMB < MinCardMB)
	{
		D.StandDownReason = TEXT("card is below the guard tier, so the game's own value is the safer one");
		return D;
	}

	// ---- PRIORITY ORDER. Textures are LAST, by Ant's Nanite ruling. -------------------------------
	D.NaniteMB      = static_cast<int32>(NaniteFloorMB);
	D.CartographMB  = static_cast<int32>(bCartographPresent ? CartographReserveMB : 0);
	D.RendererMB    = static_cast<int32>(FMath::Clamp<int64>(
		static_cast<int64>(VramMB * RendererFraction), RendererFloorMB, RendererCapMB));
	D.HeadroomMB    = static_cast<int32>(VramMB * HeadroomFraction);

	const int64 Ceiling = VramMB - D.NaniteMB - D.CartographMB - D.RendererMB - D.HeadroomMB;
	D.CeilingMB = static_cast<int32>(FMath::Max<int64>(Ceiling, 0));

	/*
	 * ★ THE LINE THE OLD VERSION NEVER WROTE.
	 *
	 * The old code computed this same Ceiling, then assigned `Out.TargetMB = Claim` where `Claim` was
	 * declared 0 and never touched — so the guard returned 0 forever, its driver read 0 as "stand down",
	 * and it removed its own ticker 45 seconds into every boot. THIS assignment is the repair.
	 *
	 * Why it is safe to claim the ceiling now, when Ant's rule says "we dont ship beyond vanilla if its
	 * not benched on that hardware": her rule's condition is a MEASUREMENT, and the measurement exists as
	 * of 2026-08-09 — 57→92 FPS and 1% low 46→69 on a 16303 MB card. The rule was never "never claim",
	 * it was "never claim unmeasured". Below MinCardMB, where nothing has been measured, we still
	 * stand down.
	 */
	if (Ceiling < VanillaTopTierMB)
	{
		D.StandDownReason = TEXT("after the higher-priority reserves there is less left than vanilla "
		                         "already gives textures, and writing a smaller pool than vanilla would "
		                         "be a regression wearing our name");
		return D;
	}

	D.TargetMB = D.CeilingMB;
	return D;
}

FFPMTexturePoolGuard& FFPMTexturePoolGuard::Get()
{
	static FFPMTexturePoolGuard Instance;
	return Instance;
}

void FFPMTexturePoolGuard::GetCounts(int32& OutRaises, int32& OutClobbersRepaired, int32& OutLastTargetMB)
{
	OutRaises           = GRaises;
	OutClobbersRepaired = GClobbersRepaired;
	OutLastTargetMB     = GLastTargetMB;
}

void FFPMTexturePoolGuard::Arm()
{
	if (TickHandle.IsValid()) { return; }

	StartTime = FApp::GetCurrentTime();
	LastPoll  = StartTime;

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FFPMTexturePoolGuard::Tick), 1.0f);

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] texture-pool guard armed: first decision in %.0fs, watchdog every %.0fs."),
		SettleSeconds, PollSeconds);
}

void FFPMTexturePoolGuard::Disarm()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	// The writer owns the release. Our hold goes back through the ledger like every other, so uninstall
	// leaves the pool exactly as the game had it.
	FPMCVarWriter::Get().ReleaseOwner(GPoolOwner);
}

bool FFPMTexturePoolGuard::Tick(float /*DeltaTime*/)
{
	if (bStoodDown) { return false; }   // latched: stop ticking, decision already stated

	const double Now = FApp::GetCurrentTime();
	if (Now - StartTime < SettleSeconds) { return true; }
	if (Now - LastPoll  < PollSeconds)   { return true; }
	LastPoll = Now;

	if (!bDetected)
	{
		bDetected   = true;
		bCartograph = FPMTexturePool::DetectCartograph();
	}

	const int64 VramMB = FPMTexturePool::QueryTotalVramMB();
	const FFPMPoolDecision D = FPMTexturePool::ComputePoolMB(VramMB, bCartograph);

	if (D.StandDownReason)
	{
		/*
		 * ★ THE STAND-DOWN RULE. The old guard logged "card below the guard tier" on a 16 GB card — a
		 * cause it had not tested — and that single false line is why nobody noticed it was inert for
		 * months. This prints the ACTUAL failing predicate and EVERY input behind it, so the next reader
		 * can check the arithmetic instead of trusting the verdict.
		 */
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] texture-pool guard STANDS DOWN: %s. Inputs: card=%lld MB, tier=%lld MB, "
			     "nanite=%d, cartograph=%d (%s), renderer=%d, headroom=%d, ceiling=%d, vanilla-floor=%lld."),
			D.StandDownReason, VramMB, MinCardMB, D.NaniteMB, D.CartographMB,
			D.bCartograph ? TEXT("present") : TEXT("absent"),
			D.RendererMB, D.HeadroomMB, D.CeilingMB, VanillaTopTierMB);

		// Latched, and it is a DECISION rather than an early return that happens to stop the ticker.
		bStoodDown = true;
		return false;
	}

	IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(PoolCVar);
	if (!Var)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] texture-pool guard: '%s' does not exist on this build. Remedy: the cvar was "
			     "renamed or removed upstream; re-derive the lever before trusting this guard again."),
			PoolCVar);
		bStoodDown = true;
		return false;
	}

	const int32 Live = Var->GetInt();

	/*
	 * MONOTONIC: only ever raise. If anything has set the pool HIGHER than our claim it stays — we never
	 * write a smaller number than what is live. The one repeat customer is the game's scalability pass
	 * re-applying its own value when the player touches texture settings, and repairing that clobber is
	 * this watchdog's entire job.
	 */
	if (Live >= D.TargetMB)
	{
		return true;   // nothing to do; keep watching for the clobber
	}

	const bool bFirst = (GRaises == 0);

	// Through the WRITER, not a raw Set: the hold lands in the ledger, appears in FPM.Support, and is
	// released on uninstall like everything else. r.Streaming.PoolSize is not US_*-backed (checked
	// against the derived table), so clause 6 does not apply to it.
	const bool bHeld = FPMCVarWriter::Get().Hold(
		GPoolOwner, PoolCVar, *FString::FromInt(D.TargetMB),
		TEXT("texture-pool guard: card-sized streaming pool"));

	if (!bHeld)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] texture-pool guard: the writer REFUSED '%s'. The refusal reason is logged above "
			     "this line. Standing down rather than retrying every %.0fs."), PoolCVar, PollSeconds);
		bStoodDown = true;
		return false;
	}

	++GRaises;
	if (!bFirst) { ++GClobbersRepaired; }
	GLastTargetMB = D.TargetMB;

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] texture-pool %d -> %d MB%s. card=%lld, reserved: nanite %d + cartograph %d (%s) + "
		     "renderer %d + headroom %d."),
		Live, D.TargetMB,
		bFirst ? TEXT("") : TEXT("  [REPAIRED a scalability clobber - the menu had shrunk it]"),
		VramMB, D.NaniteMB, D.CartographMB, D.bCartograph ? TEXT("present") : TEXT("absent"),
		D.RendererMB, D.HeadroomMB);

	/*
	 * ONE line to chat, on the FIRST raise only. Ant, 2026-08-09: "a player wont check logs for stuff,
	 * only devs do" — and this is a change she can SEE, so she should be told it happened rather than
	 * left to wonder why the game suddenly looks better. Clobber repairs stay log-only: they are routine
	 * and repeating them would be the noise the relay's flood cap exists to prevent.
	 */
	if (bFirst)
	{
		FPMChatf(TEXT("[FPM] Texture pool raised %d -> %d MB to match your card. Sharper textures, fewer stalls."),
			Live, D.TargetMB);
	}

	return true;
}
