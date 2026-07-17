#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class FJsonObject; // config json (menu merge parameter)

/**
 * WNL perf governor — client-only.
 *
 * Lever 1 (RESOLUTION, v0.9.6): the ENGINE's native dynamic resolution (r.DynamicRes.OperationMode=2),
 *   set up once in ApplyDynamicResolution(). The engine varies render resolution flash-free (built-in
 *   MinResolutionChangePeriod debounce + hysteresis) and feeds whichever upscaler the user runs
 *   (DLSS/FSR/TSR — UE5.1+ routes automatically); UseCPUTimeLogic/UseGameThreadCriticalPath keep it from
 *   dropping res futilely when CPU-bound. ReadDynamicResPct() feeds the live fraction to the stage
 *   engine's floor gate. (This replaced a manual r.ScreenPercentage slew controller that glitched TSR
 *   into geometry flashing — the engine now IGNORES manual r.ScreenPercentage once dyn-res is on.)
 * Lever 1b (CPU RELIEF, v0.9.6): the CPU-bound lever — resolution/GPU-quality only help GPU-bound frames.
 *   UpdateCpuRelief() ramps a smooth intensity [0..1] as FPS approaches a hard CpuFloorFPS (60) floor,
 *   fast-up/slow-down, and scales CPU-cheapening levers (FG building cull → foliage/grass cull → skeletal
 *   LOD) by it, from captured baselines. It MAY dip below the user's quality baseline to hold the floor,
 *   restores on headroom, and no-ops while GPU-bound.
 * Lever 2 (the STAGE ENGINE, v0.9): one QualityStage integer spanning -4..+6 over the
 *   declarative tables in WNLQualityStages.h. Stage 0 = the user's own settings (vanilla
 *   perf guaranteed). Positive stages are cumulative bonuses up to beyond-Cinematic,
 *   promoted one per PromoteDwellSec ONLY at ~full res with measured GPU headroom, each
 *   promote verified for VerifyWindowSec (a burn demotes + learns the stage's real cost +
 *   starts a growing cooldown). Bonuses shed within seconds under load — before resolution
 *   is meaningfully spent. Negative stages are the emergency cut ladder (contact shadows →
 *   fog coarsen → sparse-probe filtered GI → GI tier LAST), engaged only at the res floor
 *   while GPU-bound, restored LIFO the moment headroom returns or the frame goes CPU-bound.
 *
 * Cap-aware budget: the frame budget follows the LIVE frame cap (t.MaxFPS) and a
 *   learned vsync plateau, so sitting at 60 fps on a 60 Hz display reads as "at the
 *   cap" (healthy), not as overload — otherwise a capped machine would strip every
 *   uplift forever (review finding).
 * User-settings adoption: menu changes can't reach our SetByConsole cvars directly,
 *   so the governor polls GameUserSettings and adopts USER changes (frame limit,
 *   resolution scale) as new baselines, writing them through itself.
 *
 * Frame generation is NEVER touched (explicitly unwanted — works poorly in Satisfactory).
 *
 * Behavior targets (Ant, 2026-07-16): hold >90 FPS, 120 cap (monitor rate), tune later.
 * All knobs live in FactoryGame/Configs/WNLPackFix.cfg (JSON, created with defaults on
 * first run) so tuning needs no rebuild.
 */
class FWNLPerfGovernor
{
public:
	static FWNLPerfGovernor& Get();

	/** Idempotent; no-op on dedicated servers and in the editor. */
	void Start();
	void Stop();

	// --- v0.9.8 in-game config (console commands + SML menu overlay; public: called from the module
	//     and the file-scope console-command lambdas) ---
	/** `WNLPackFix.Status` — print the live governor state (vendor/upscaler, res %, stage, CPU relief). */
	void PrintStatus() const;
	/** `WNLPackFix.Set <Key> <Value>` — persist any top-level WNLPackFix.cfg key and live-apply the safe
	 *  ones (Enabled, TargetFPS, floors, stage caps, CpuRelief). Unknown keys are rejected (typo guard). */
	void SetConfigKey(const FString& Key, const FString& Value);
	/** Merge the in-game config MENU's values (SML page → Configs/WNLPackFix/Menu.cfg) over the main
	 *  config json before parsing. EVERY governor key is menu-exposed; menu values win. */
	void MergeMenuConfig(TSharedPtr<FJsonObject>& MainJson);

private:
	bool Tick(float DeltaTime);
	void LoadOrCreateConfig();

	/** v0.9.6: hand resolution scaling to the ENGINE's native dynamic-resolution system
	 *  (r.DynamicRes.OperationMode=2). It debounces changes (MinResolutionChangePeriod) and drives
	 *  whichever upscaler the user has active (DLSS/FSR/TSR), so it is flash-free — unlike the old
	 *  manual r.ScreenPercentage poking (which the engine now silently ignores once dyn-res is on, and
	 *  which bypassed the upscaler's hysteresis -> geometry flashing). Set once at post-settle. */
	void ApplyDynamicResolution();
	/** Read the engine's LIVE dynamic-resolution fraction (0..100) for the stage engine's floor gate. */
	float ReadDynamicResPct() const;

	/** v0.9.8: SMOOTH CPU-relief controller. Resolution/quality only help GPU-bound frames; this is the
	 *  CPU-bound lever. Driven by REAL thread time (GGameThreadTime/GRenderThreadTime — what "stat unit"
	 *  shows; FPS only as fallback): relief starts as the CPU nears the 90fps soft target's budget and is
	 *  full at the 75fps hard floor's budget. One intensity [0..1] drives a VISIBILITY-ORDERED ladder —
	 *  invisible levers first (conveyor item rate/draw radius, far skeletal LODs), moderate next (building
	 *  cull, effects density), and the clearly-visible foliage cull LAST and SHALLOW (emergency band only).
	 *  MAY go below the user's quality baseline to hold the floor; restores fully on headroom. */
	void UpdateCpuRelief(float FrameMs, float GameThreadMs, float RenderThreadMs,
	                     bool bGpuBound, bool bGpuKnown, float DT);

	FTSTicker::FDelegateHandle TickHandle;

	/** One-time post-settle: detect the GPU/upscaler, guard RR off, and apply the baseline-free set. */
	void ApplyPostSettleGraphics();


	// --- stage engine (v0.9) ---
	/** Snapshot the live value of every cvar the stage tables touch — the stage-0 truth every
	 *  policy computes from and every revert restores to. Runs once, after the baseline-free set. */
	void CaptureBaselines();
	/** Recompute the full effective cvar map for NewStage (baseline overlaid with cumulative stage
	 *  tables), write only the diffs, log one line. Idempotent; ApplyStage(0) = exact revert. */
	void ApplyStage(int32 NewStage, const TCHAR* Reason);
	float StageCostMs(int32 BonusStage) const; // learned cost if measured, else the table estimate

	enum class EGpuVendor : uint8 { Unknown, NVIDIA, AMD, Intel, Other };
	static const TCHAR* VendorName(EGpuVendor V);

	// The temporal upscaler the user actually has LIVE. We tune on the active upscaler, not the vendor:
	// SF 1.2.3.1 exposes TSR/DLSS/XeSS + FSR (FSR 4 on supported GPUs, per the build-495413 changelog), so
	// "Vendor==AMD ⇒ FSR" is still wrong — an AMD user may be on TSR, XeSS, OR FSR; detect the LIVE one.
	// DLSS/XeSS/FSR each condition their own image (skip the tonemapper sharpen); DLSS reconstructs cleanest
	// at low res (keeps the lower floor).
	enum class EUpscaler : uint8 { None, TSR, DLSS, XeSS, FSR };
	static const TCHAR* UpscalerName(EUpscaler U);
	EUpscaler DetectActiveUpscaler() const;   // reads live cvars at post-settle

	// --- config (defaults; overridden by WNLPackFix.cfg) ---
	bool  bEnabled          = true;
	float TargetFPS         = 90.f;   // SOFT target (Ant 2026-07-17): dyn-res + promotes steer to this;
	                                  // the HARD floor (CpuFloorFPS=75) is where cuts/relief go maximal
	float MaxFPS            = 120.f;  // monitor rate — no point rendering past it
	float CapFPS            = 120.f;  // t.MaxFPS cap (only applied if the game is uncapped)
	float MinScreenPct      = 58.f;   // res floor. v0.9: 58 (DLSS-Balanced anchor) — the v0.8.3 boot
	                                  // proved 50 (Performance-grade input at 4K) reads as global
	                                  // shimmer/softness; the user's own setting was 60.
	float MaxScreenPct      = 100.f;  // DLAA-grade input when headroom allows
	// (v0.9.6: SlewDown/SlewUpPctPerSec removed — the manual r.ScreenPercentage controller they drove is
	//  gone; the engine's native dynamic res owns resolution smoothing now.)
	// Stage-engine tuning (v0.9). Boot-test law: user settings = stage-0 baseline; quality is a
	// measured-headroom bonus, cuts are a floor-only emergency (36ms-GPU-at-50%-res lesson).
	int32 MaxBonusStage     = 6;      // bonus ceiling (+6 = beyond-Cinematic); auto-lowered to 4 on AMD
	int32 MaxCutStage       = 4;      // cut floor (-4 = GI tier down); 0 disables cuts entirely
	float NudgeAfterSec     = 5.f;    // sustained overload at floor before each cut rung (v0.9.6: fast-down)
	float RestoreAfterSec   = 8.f;    // recovery hold before each LIFO cut restore (slow-up: no oscillation)
	float PromoteDwellSec   = 30.f;   // continuous headroom required before each bonus promote (slow-up)
	float DemoteDwellSec    = 1.5f;   // load hold before a bonus is shed (v0.9.6: FAST — defend the target)
	float PromoteGpuFrac    = 0.80f;  // promote only while GPU time < this fraction of budget
	float PromoteHeadroomFactor = 1.5f; // and headroom > factor x next stage's (learned) cost
	float VerifyWindowSec   = 8.f;    // post-promote watch window; over budget inside it = burn
	float PromoteCooldownSec = 120.f; // first burn cooldown per stage (doubles, capped below)
	float PromoteCooldownMaxSec = 600.f;

	// --- baseline-free set (applied post-settle; measured free or VRAM-only) ---
	bool  bVSMStability     = true;   // VSM page pool + cache + panning bias (anti-shimmer, no ms cost)
	bool  bReflectionDenoise = true;  // Lumen reflection screen-space reconstruction (measured no-op-to-free)
	bool  bContactShadows   = true;   // static-geometry grounding (cheap); belt items + foliage suppressed separately
	float ContactShadowLength = 0.035f; // screen-space; sun's baked length may be 0 so we force it
	bool  bNaniteSharpen    = true;   // r.Nanite.MaxPixelsPerEdge tweak (1.0 default = no-op unless tuned)
	float NanitePixelsPerEdge = 1.0f; // 1.0 = default; <1 sharper geometry, >1 cheaper
	// --- v0.9.7 Lumen free-win levers (pure compute-scheduling, no image change) ---
	bool  bLumenPerfLevers  = true;   // reflection trace-compaction + radiance-cache trace-tile sort
	bool  bDontLimitOnBattery = true; // r.DontLimitOnBattery=1 — trivial anti-throttle (free on laptops)
	int32 MaxFrameQueue     = 1;      // RHI.MaximumFrameLatency PROBE — cvar is ABSENT on 1.2.3.1
	                                  // (no log line either boot); kept in case CSS registers it.
	                                  // The real latency mechanism is Reflex below. 0 = don't touch

	// --- v0.9 baseline-free additions (incl. Reflex; all null-guarded, logged was→new) ---
	int32 ReflexMode        = 1;      // t.Streamline.Reflex.Mode: 0 off / 1 low-latency / 2 +boost.
	                                  // NVIDIA-gated; Enable=1 is the master switch (shipped default 0).
	                                  // Mode 2 pins GPU clocks and costs fps when GPU-bound — opt-in.
	float FSRSharpness      = 0.5f;   // r.FidelityFX.FSR.Sharpness when FSR is live (0.5 = moderate RCAS)
	bool  bAsyncTick        = true;   // game-thread relief: async tick dispatch/cleanup + FX batching
	int32 GrassTickInterval = 10;     // grass refresh every N frames — CPU relief, near-free
	float GrassDensityScale = 1.0f;   // static opt-in only (mid-session change = rebuild pop); 1 = untouched
	int32 StreamingPoolMB   = 0;      // texture pool: 0 = auto by VRAM (>=16GB: 6144, >=12GB: 4096), >0 explicit
	bool  bIncrementalGC    = true;   // spread GC reachability/gather over frames (experimental engine flag — gated for bisectability)

	// --- v0.8 vendor-adaptive (the shared drop-in must work on Ant's NVIDIA AND the friend's AMD) ---
	bool  bVendorAdaptive   = true;   // master switch for the per-GPU branch
	float AmdMinScreenPct   = 62.f;   // higher dyn-res floor for FSR/TSR/None (they blur worse than DLSS at low res)
	float TsrXessSharpen    = 0.8f;   // r.Tonemapper.Sharpen on non-DLSS paths (0 = off); never stacked on DLSS
	bool  bForceUpscalerIfNone = true; // force TSR (AMD/Other) when the user has no temporal upscaler active
	// --- v0.9.7 vendor upscaler auto-select + AMD/Intel guards (vendor bug audit 2026-07-17) ---
	bool  bUpscalerAutoSelect  = true; // branch tuning on the ACTIVE upscaler (DLSS/XeSS/FSR/TSR), not the vendor
	bool  bAmdAntiFlicker      = true; // AMD: r.TSR.ShadingRejection.Flickering=1 + TAA→TSR (RDNA Lumen flicker)
	float AmdDFShadowCullTile  = 400.f;// AMD: r.DFShadowCullTileWorldSize (terrain/DF-shadow flicker; SF-verified)
	int32 IntelArcMaxBonusStage  = 2;  // Arc dGPU bonus ceiling (Lumen+Nanite+XeSS DX12 crash class on 5.6)
	int32 IntelIGpuMaxBonusStage = 0;  // Iris/UHD iGPU: no bonus stages
	float IntelIGpuMinScreenPct  = 50.f;// iGPU: let dyn-res drop HARD to hold FPS (opposite of the dGPU floor)
	bool  bAssertRayReconstructionOff = true; // RR is inert on software Lumen here → hard-guard DenoiserMode=0
	float GpuBoundFraction  = 0.85f;  // GPU time ≥ this fraction of frame time = GPU-bound (cuts allowed)
	float VramFloorMB       = 1500.f; // keep this much VRAM FREE: below it, don't promote (dynamic — reacts
	                                  // to the Claude app / other apps holding VRAM)
	float PoolBudgetFraction = 0.4f;  // streaming pool = this fraction of the GRANTED VRAM budget (capped at
	                                  // the card tier). Off BUDGET not free-headroom → no self-feedback loop.
	float LadderGraceSec    = 45.f;   // no stage PROMOTES or CUTS for this long after the graphics pass /
	                                  // each baseline re-capture (the join / PSO-compile storm); LIFO
	                                  // restores are exempt so a cut can always recover

	// --- v0.9.8 CPU-relief controller (the CPU-bound lever; res/quality only help GPU-bound frames).
	//     Signal = REAL thread time (game/render thread ms); ladder = visibility-ordered bands:
	//     A invisible (conveyor rate/radius, skel LOD) → B moderate (building cull, effects) →
	//     C emergency (foliage, shallow). Ant: keep graphics good as long as possible. ---
	bool  bCpuRelief        = true;   // master switch for the smooth CPU-relief controller
	float CpuFloorFPS       = 75.f;   // HARD floor to defend — relief maximal at/below this (Ant: 75)
	float CpuComfortFPS     = 90.f;   // no CPU throttle while the CPU can make this (the soft target)
	float CpuReliefUpPerSec = 3.0f;   // FAST ramp-up (defend the floor: intensity climbs quickly under load)
	float CpuReliefDnPerSec = 0.4f;   // SLOW ramp-down (gentle restore, no oscillation / pop-in flicker)
	int32 ConveyorItemFreqMin = 24;   // band A: FG.ConveyorItemFrequency at full band (60 = game default)
	float ConveyorDrawDistMin = 40000.f; // band A: CSS.Conveyor.MaxDrawDistance at full band (100000 default)
	float CpuSkelLODMax     = 2.f;    // band A: r.SkeletalMeshLODBias at full band (far creatures coarsen)
	float CpuBuildCullMin   = 0.65f;  // band B: FG building cull modifier at full band (1.0 = game default)
	int32 NiagaraQualityMin = 2;      // band B: fx.Niagara.QualityLevel at full band (3 = Ultra default)
	float CpuFoliageCullMin = 0.85f;  // band C EMERGENCY, SHALLOW: v0.9.7's 0.6 across the whole range
	                                  // read as "foliage looks terrible" (Ant boot-test) — now last + least
	// (physics-wait lever deliberately omitted — sim-lag risk; every lever here is visual-only)

	// --- runtime state ---
	float ConfiguredMinScreenPct = 58.f; // fixed config floor; effective floor never ratchets below it
	float SmoothedFrameMs   = 0.f;
	float CurrentPct        = 0.f;    // 0 = not yet initialized from cvar
	double StartTime        = 0.0;    // governor arm time (anchors the post-settle pass)

	// --- stage-engine state (v0.9) ---
	int32 QualityStage      = 0;      // -MaxCutStage .. +MaxBonusStage; 0 = user baseline
	double StageTimer       = 0.0;    // dwell anchor for the currently-pending transition
	int32 PendingKind       = 0;      // which transition owns StageTimer (resets the dwell on change)
	double RecaptureAt      = 0.0;    // re-capture baselines at this time after a user settings change (0 = none)
	double VerifyUntil      = 0.0;    // post-promote verify window end (0 = not verifying)
	int32 VerifyingStage    = 0;      // the bonus stage under verification
	float PreVerifyGpuMs    = 0.f;    // GPU ms just before the verified promote (learns the cost)
	float LearnedCostMs[8]  = {};     // measured engage cost per bonus stage (index 1..6; 0 = unmeasured)
	double CooldownUntil[8] = {};     // burned-stage re-promote gate per bonus stage
	float BurnCooldownSec[8] = {};    // per-stage growing cooldown (0 = fresh)
	TMap<FString, float> Baselines;   // live stage-0 value of every staged cvar (captured post-settle)
	TMap<FString, float> LastWritten; // what WE last wrote per staged cvar (diff-writing)
	TMap<FString, bool>  FloatFlags;  // write-as-float per staged cvar (from the lever tables)
	bool  bBaselinesCaptured = false;
	int64 VramMB            = 0;      // TOTAL dedicated VRAM (card-capability gate for VRAM-heavy stage levers)
	int64 BudgetVramMB      = -1;    // OS-GRANTED VRAM budget (shrinks under other apps; sizes the pool WITHOUT self-feedback)
	int64 FreeVramMB        = -1;    // budget - our usage (headroom right now; gates promotes); -1 = unknown
	double LastVramPoll     = 0.0;    // last VRAM poll (2s cadence)
	int32 CurStreamingPoolMB = 0;     // streaming pool we last set (so dynamic resizes only when it MOVES)

	// --- cap-aware budget + user-settings adoption (review findings) ---
	float VsyncPlateauMs    = 0.f;    // learned frame-time plateau under vsync (~refresh interval; 0 = unlearned)
	double LastUserPoll     = 0.0;    // last GameUserSettings poll time (1s cadence)
	float LastSeenUserFPSLimit = -1.f; // user's frame-limit setting at last poll (-1 = uncaptured)
	float LastSeenUserResScale = -1.f; // user's resolution-scale setting at last poll (-1 = uncaptured)
	int32 LastSeenUserGIQuality = -1;  // user's GI-quality menu setting (sg.GI is a STAGED lever, -1 = uncaptured)

	// --- GPU-bound detection (boot-test finding 2026-07-16: the server world is CPU/game-thread
	//     bound ~55fps; wall-time-only steering stripped every uplift for ZERO fps gain) ---
	float SmoothedGpuMs     = 0.f;    // EMA of the GPU frame time (0 = no GPU timestamps available)

	// --- v0.9.8 CPU-relief runtime state (last-applied values so levers only re-write when they MOVE) ---
	float CpuReliefIntensity = 0.f;   // 0 = no throttle .. 1 = full relief (smoothed, fast-up/slow-down)
	float SmoothedCpuMs      = 0.f;   // smoothed max(game-thread, render-thread) ms — the REAL CPU signal
	float CurCpuBuildCull    = 1.f;   // last-applied FG building cull modifier (re-applied on world change)
	float CurCpuFoliageCull  = 1.f;   // last-applied foliage.CullDistanceScale
	float CurCpuGrassCull    = 1.f;   // last-applied grass.CullDistanceScale (own baseline)
	bool  bMenuResFloor      = false; // player set MinScreenPct in the in-game menu → vendor floors defer
	int32 CurCpuSkelLOD      = 0;     // last-applied r.SkeletalMeshLODBias from the relief controller
	int32 CurConveyorFreq    = 60;    // last-applied FG.ConveyorItemFrequency
	float CurConveyorDist    = 100000.f; // last-applied CSS.Conveyor.MaxDrawDistance
	int32 CurNiagaraQuality  = 3;     // last-applied fx.Niagara.QualityLevel
	// Pre-relief baselines captured once so intensity-0 restores to the user's TRUE values, not hardcoded:
	bool  bCpuBaselinesCaptured = false;
	float BaseCpuFoliageCull = 1.f;   // live foliage.CullDistanceScale before relief first touched it
	float BaseCpuGrassCull   = 1.f;   // live grass.CullDistanceScale (SEPARATE baseline — review finding)
	int32 BaseCpuSkelLOD     = 0;     // live r.SkeletalMeshLODBias before relief first touched it
	int32 BaseConveyorFreq   = 60;    // live FG.ConveyorItemFrequency before relief (user's setting)
	float BaseConveyorDist   = 100000.f; // live CSS.Conveyor.MaxDrawDistance before relief (user's setting)
	int32 BaseNiagaraQuality = 3;     // live fx.Niagara.QualityLevel before relief (user's setting)
	void* LastCpuWorld       = nullptr; // world the building-cull modifier was last pushed to (re-apply on change)
	bool  bDynResApplied     = false; // native dynamic-resolution set up once at post-settle

	// --- v0.8 runtime state ---
	bool  bGraphicsApplied  = false;  // one-time post-settle graphics/vendor pass done
	double GraphicsAppliedTime = 0.0; // when the pass ran (anchors the ladder grace period)
	EGpuVendor Vendor       = EGpuVendor::Unknown;
	bool  bDlssKnobsPresent = false;  // r.NGX.* resolve → treat as the DLSS/NVIDIA path (skip tonemapper sharpen)
	EUpscaler ActiveUpscaler = EUpscaler::None; // live upscaler detected at post-settle (drives vendor tuning)
	bool  bStarted          = false;
};
