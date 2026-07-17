#include "WNLModConfiguration.h"
#include "Configuration/Properties/ConfigPropertySection.h"
#include "Configuration/Properties/ConfigPropertyBool.h"
#include "Configuration/Properties/ConfigPropertyFloat.h"
#include "Configuration/Properties/ConfigPropertyInteger.h"

// Localization (docs: user-facing text must be localizable; keys auto-derived from EN text)
#define LOCTEXT_NAMESPACE "WNLPackFix"

/*
 * The FULL in-game config menu: EVERY governor + fog knob, grouped into collapsible sections.
 *
 * CONTRACT with the governor (see FWNLPerfGovernor::MergeMenuConfig): each property's MAP KEY is the
 * EXACT main-config json key. SML saves each UI section as a nested json object; the governor hoists
 * section children one level and overlays them onto the main config (menu wins), so the two stay in
 * sync by construction — add a knob here with the right key and it just works. "Fog" is the exception:
 * it stays nested (the fog controller reads that section itself, from both files).
 *
 * Subobject NAMES carry a section prefix (Gen_, Res_, ...) because CreateDefaultSubobject names must
 * be unique across the whole CDO — two sections may both want an "Enabled" key.
 */
namespace
{
	template <typename T>
	T* Prop(UObject* Outer, const TCHAR* SubName, const FText& Display, const FText& Tip)
	{
		T* P = Outer->CreateDefaultSubobject<T>(SubName);
		P->DisplayName = Display;
		P->Tooltip     = Tip;
		return P;
	}
	UConfigPropertyBool* BoolProp(UObject* O, const TCHAR* Sub, const FText& D, const FText& T, bool Def)
	{
		UConfigPropertyBool* P = Prop<UConfigPropertyBool>(O, Sub, D, T);
		P->DefaultValue = Def; P->Value = Def; return P;
	}
	UConfigPropertyFloat* FloatProp(UObject* O, const TCHAR* Sub, const FText& D, const FText& T, float Def)
	{
		UConfigPropertyFloat* P = Prop<UConfigPropertyFloat>(O, Sub, D, T);
		P->DefaultValue = Def; P->Value = Def; return P;
	}
	UConfigPropertyInteger* IntProp(UObject* O, const TCHAR* Sub, const FText& D, const FText& T, int32 Def)
	{
		UConfigPropertyInteger* P = Prop<UConfigPropertyInteger>(O, Sub, D, T);
		P->DefaultValue = Def; P->Value = Def; return P;
	}
}

UWNLModConfiguration::UWNLModConfiguration()
{
	// Category "Menu" → saved to Configs/WNLPackFix/Menu.cfg — its OWN file, so SML's schema-save can
	// never rewrite/trim the main Configs/WNLPackFix.cfg (which also holds values this menu might not
	// know yet after an update).
	ConfigId.ModReference   = TEXT("WNLPackFix");
	ConfigId.ConfigCategory = TEXT("Menu");
	DisplayName = LOCTEXT("ModDisplayName", "WNL PackFix");
	Description = LOCTEXT("ModDescription",
		"Performance governor: your settings are the baseline, quality above them is earned from measured "
		"GPU headroom, and frame-rate is defended CPU-first (invisible savings before visible ones) so "
		"graphics stay untouched as long as possible.");

	RootSection = CreateDefaultSubobject<UConfigPropertySection>(TEXT("RootSection"));
	auto AddSection = [this](const TCHAR* Key, const FText& Display) -> UConfigPropertySection*
	{
		UConfigPropertySection* S = CreateDefaultSubobject<UConfigPropertySection>(*FString::Printf(TEXT("Sec_%s"), Key));
		S->DisplayName = Display;
		RootSection->SectionProperties.Add(Key, S);
		return S;
	};

	// ---------------- General ----------------
	{
		UConfigPropertySection* S = AddSection(TEXT("General"), LOCTEXT("SecGeneral", "General"));
		S->SectionProperties.Add(TEXT("Enabled"), BoolProp(this, TEXT("Gen_Enabled"),
			LOCTEXT("Enablegovernor", "Enable governor"),
			LOCTEXT("Masterswitchforthewholeperforman", "Master switch for the whole performance governor."), true));
		S->SectionProperties.Add(TEXT("TargetFPS"), FloatProp(this, TEXT("Gen_TargetFPS"),
			LOCTEXT("SofttargetFPS", "Soft target FPS"),
			LOCTEXT("Steeredtowithdynamicresolutional", "Steered to with dynamic resolution alone; quality is never cut above this."), 90.f));
		S->SectionProperties.Add(TEXT("CpuFloorFPS"), FloatProp(this, TEXT("Gen_CpuFloorFPS"),
			LOCTEXT("HardfloorFPS", "Hard floor FPS"),
			LOCTEXT("DefendedatallcostsbelowthisCPUre", "Defended at all costs: below this, CPU relief maxes out and emergency quality cuts may engage."), 75.f));
		S->SectionProperties.Add(TEXT("CapFPS"), FloatProp(this, TEXT("Gen_CapFPS"),
			LOCTEXT("FPScap", "FPS cap"),
			LOCTEXT("tMaxFPScapappliedonlyifthegameis", "t.MaxFPS cap applied only if the game is uncapped. 0 = leave uncapped."), 120.f));
	}
	// ---------------- Resolution & upscaler ----------------
	{
		UConfigPropertySection* S = AddSection(TEXT("Resolution"), LOCTEXT("SecResolution", "Resolution & Upscaler"));
		S->SectionProperties.Add(TEXT("MinScreenPct"), FloatProp(this, TEXT("Res_MinScreenPct"),
			LOCTEXT("Resolutionfloor", "Resolution floor %"),
			LOCTEXT("Lowestinternalresolutiondynamicr", "Lowest internal resolution dynamic-res may reach; your upscaler reconstructs from this."), 58.f));
		S->SectionProperties.Add(TEXT("MaxScreenPct"), FloatProp(this, TEXT("Res_MaxScreenPct"),
			LOCTEXT("Resolutionceiling", "Resolution ceiling %"),
			LOCTEXT("Highestinternalresolutionwhenhea", "Highest internal resolution when headroom allows (100 = DLAA-grade input)."), 100.f));
		S->SectionProperties.Add(TEXT("UpscalerAutoSelect"), BoolProp(this, TEXT("Res_UpscalerAutoSelect"),
			LOCTEXT("Autodetectupscaler", "Auto-detect upscaler"),
			LOCTEXT("Tuneforwhicheverupscalerisactual", "Tune for whichever upscaler is actually live (DLSS/XeSS/FSR/TSR) instead of guessing by GPU vendor."), true));
		S->SectionProperties.Add(TEXT("ForceUpscalerIfNone"), BoolProp(this, TEXT("Res_ForceUpscalerIfNone"),
			LOCTEXT("ForceTSRifnoupscaler", "Force TSR if no upscaler"),
			LOCTEXT("Dynamicresolutionneedsatemporalu", "Dynamic resolution needs a temporal upscaler; if none is active, enable TSR."), true));
		S->SectionProperties.Add(TEXT("TsrXessSharpen"), FloatProp(this, TEXT("Res_TsrXessSharpen"),
			LOCTEXT("TSRsharpen", "TSR sharpen"),
			LOCTEXT("TonemappersharpenonTSRnoupscaler", "Tonemapper sharpen on TSR/no-upscaler paths (never stacked on DLSS/XeSS/FSR). 0 = off."), 0.8f));
		S->SectionProperties.Add(TEXT("FSRSharpness"), FloatProp(this, TEXT("Res_FSRSharpness"),
			LOCTEXT("FSRsharpness", "FSR sharpness"),
			LOCTEXT("FSRsownRCASsharpenerappliedonlyw", "FSR's own RCAS sharpener, applied only when FSR is the live upscaler."), 0.5f));
	}
	// ---------------- Quality stages ----------------
	{
		UConfigPropertySection* S = AddSection(TEXT("QualityStages"), LOCTEXT("SecQualityStages", "Quality Stages"));
		S->SectionProperties.Add(TEXT("MaxBonusStage"), IntProp(this, TEXT("Stg_MaxBonusStage"),
			LOCTEXT("Maxqualitybonus06", "Max quality bonus (0-6)"),
			LOCTEXT("BonustiersthegovernormayADDabove", "Bonus tiers the governor may ADD above your settings from measured headroom. 0 = never."), 6));
		S->SectionProperties.Add(TEXT("MaxCutStage"), IntProp(this, TEXT("Stg_MaxCutStage"),
			LOCTEXT("Maxemergencycuts04", "Max emergency cuts (0-4)"),
			LOCTEXT("TiersthegovernormayCUTbelowyours", "Tiers the governor may CUT below your settings in a sustained below-floor emergency. 0 = never touch my settings."), 4));
		S->SectionProperties.Add(TEXT("PromoteDwellSec"), FloatProp(this, TEXT("Stg_PromoteDwellSec"),
			LOCTEXT("Promotedwells", "Promote dwell (s)"),
			LOCTEXT("Continuousheadroomrequiredbefore", "Continuous headroom required before each bonus tier engages (slow-up)."), 30.f));
		S->SectionProperties.Add(TEXT("DemoteDwellSec"), FloatProp(this, TEXT("Stg_DemoteDwellSec"),
			LOCTEXT("Demotedwells", "Demote dwell (s)"),
			LOCTEXT("Loadholdbeforeabonustierisshedfa", "Load hold before a bonus tier is shed (fast-down defends the target)."), 1.5f));
		S->SectionProperties.Add(TEXT("NudgeAfterSec"), FloatProp(this, TEXT("Stg_NudgeAfterSec"),
			LOCTEXT("Cutafters", "Cut after (s)"),
			LOCTEXT("Sustainedoverloadattheresolution", "Sustained overload at the resolution floor before each emergency cut."), 5.f));
		S->SectionProperties.Add(TEXT("RestoreAfterSec"), FloatProp(this, TEXT("Stg_RestoreAfterSec"),
			LOCTEXT("Restoreafters", "Restore after (s)"),
			LOCTEXT("Recoveryholdbeforeeachcutisresto", "Recovery hold before each cut is restored (last-in-first-out)."), 8.f));
		S->SectionProperties.Add(TEXT("PromoteGpuFrac"), FloatProp(this, TEXT("Stg_PromoteGpuFrac"),
			LOCTEXT("PromoteGPUfraction", "Promote GPU fraction"),
			LOCTEXT("PromoteonlywhileGPUtimeisunderth", "Promote only while GPU time is under this fraction of the frame budget."), 0.8f));
		S->SectionProperties.Add(TEXT("PromoteHeadroomFactor"), FloatProp(this, TEXT("Stg_PromoteHeadroomFactor"),
			LOCTEXT("Promoteheadroomfactor", "Promote headroom factor"),
			LOCTEXT("Headroommustexceedthenexttiersme", "Headroom must exceed the next tier's measured cost times this factor."), 1.5f));
		S->SectionProperties.Add(TEXT("VerifyWindowSec"), FloatProp(this, TEXT("Stg_VerifyWindowSec"),
			LOCTEXT("Verifywindows", "Verify window (s)"),
			LOCTEXT("Apromotedtiermustproveitfitswith", "A promoted tier must prove it fits within this window or it is rolled back and put on cooldown."), 8.f));
		S->SectionProperties.Add(TEXT("PromoteCooldownSec"), FloatProp(this, TEXT("Stg_PromoteCooldownSec"),
			LOCTEXT("Promotecooldowns", "Promote cooldown (s)"),
			LOCTEXT("Firstcooldownafterafailedpromote", "First cooldown after a failed promote; doubles each failure."), 120.f));
		S->SectionProperties.Add(TEXT("PromoteCooldownMaxSec"), FloatProp(this, TEXT("Stg_PromoteCooldownMaxSec"),
			LOCTEXT("Promotecooldownmaxs", "Promote cooldown max (s)"),
			LOCTEXT("Upperboundforthedoublingpromotec", "Upper bound for the doubling promote cooldown."), 600.f));
		S->SectionProperties.Add(TEXT("GpuBoundFraction"), FloatProp(this, TEXT("Stg_GpuBoundFraction"),
			LOCTEXT("GPUboundthreshold", "GPU-bound threshold"),
			LOCTEXT("GPUtimeabovethisfractionofframet", "GPU time above this fraction of frame time counts as GPU-bound (enables res/quality action)."), 0.85f));
		S->SectionProperties.Add(TEXT("LadderGraceSec"), FloatProp(this, TEXT("Stg_LadderGraceSec"),
			LOCTEXT("Startupgraces", "Startup grace (s)"),
			LOCTEXT("Nostagereliefdecisionsduringthep", "No stage/relief decisions during the post-launch shader-compile storm."), 45.f));
	}
	// ---------------- CPU relief ----------------
	{
		UConfigPropertySection* S = AddSection(TEXT("CPU"), LOCTEXT("SecCPU", "CPU Relief"));
		S->SectionProperties.Add(TEXT("CpuRelief"), BoolProp(this, TEXT("Cpu_CpuRelief"),
			LOCTEXT("CPUrelief", "CPU relief"),
			LOCTEXT("WhentheCPUlimitsframeratecheapen", "When the CPU limits frame-rate, cheapen barely-visible things first (belt item updates, far LODs); visible things only in a real emergency."), true));
		S->SectionProperties.Add(TEXT("CpuComfortFPS"), FloatProp(this, TEXT("Cpu_CpuComfortFPS"),
			LOCTEXT("ReliefstartsbelowFPS", "Relief starts below FPS"),
			LOCTEXT("NoCPUthrottlewhiletheCPUcanstill", "No CPU throttle while the CPU can still make this frame-rate."), 90.f));
		S->SectionProperties.Add(TEXT("ConveyorItemFreqMin"), IntProp(this, TEXT("Cpu_ConveyorItemFreqMin"),
			LOCTEXT("BeltitemratefloorHz", "Belt item rate floor (Hz)"),
			LOCTEXT("Lowestconveyoritemvisualupdatera", "Lowest conveyor item visual update rate under full relief (game default 60). Barely visible."), 24));
		S->SectionProperties.Add(TEXT("ConveyorDrawDistMin"), FloatProp(this, TEXT("Cpu_ConveyorDrawDistMin"),
			LOCTEXT("Beltitemdrawradiusfloor", "Belt item draw radius floor"),
			LOCTEXT("ShortestdistancecmbeltITEMSrende", "Shortest distance (cm) belt ITEMS render under full relief; the belts themselves always render."), 40000.f));
		S->SectionProperties.Add(TEXT("CpuSkelLODMax"), FloatProp(this, TEXT("Cpu_CpuSkelLODMax"),
			LOCTEXT("CreatureLODbiasmax", "Creature LOD bias max"),
			LOCTEXT("Howmuchfarcreaturesplayersmaycoa", "How much far creatures/players may coarsen under relief (0 = never)."), 2.f));
		S->SectionProperties.Add(TEXT("CpuBuildCullMin"), FloatProp(this, TEXT("Cpu_CpuBuildCullMin"),
			LOCTEXT("Buildingcullfloor", "Building cull floor"),
			LOCTEXT("Distantbuildingdetailculldistanc", "Distant building detail cull-distance multiplier under strong relief (1 = untouched)."), 0.65f));
		S->SectionProperties.Add(TEXT("NiagaraQualityMin"), IntProp(this, TEXT("Cpu_NiagaraQualityMin"),
			LOCTEXT("Effectsqualityfloor", "Effects quality floor"),
			LOCTEXT("Lowestparticleeffectsqualityleve", "Lowest particle/effects quality level under strong relief (3 = Ultra)."), 2));
		S->SectionProperties.Add(TEXT("CpuFoliageCullMin"), FloatProp(this, TEXT("Cpu_CpuFoliageCullMin"),
			LOCTEXT("Foliagecullflooremergency", "Foliage cull floor (emergency)"),
			LOCTEXT("Foliagegrassdrawdistancemultipli", "Foliage/grass draw-distance multiplier at FULL emergency only — the most visible lever, so it moves last and least."), 0.85f));
	}
	// ---------------- Graphics extras ----------------
	{
		UConfigPropertySection* S = AddSection(TEXT("Graphics"), LOCTEXT("SecGraphics", "Graphics Extras"));
		S->SectionProperties.Add(TEXT("VSMStability"), BoolProp(this, TEXT("Gfx_VSMStability"),
			LOCTEXT("Shadowmapstability", "Shadow-map stability"),
			LOCTEXT("Virtualshadowmapcachetuningagain", "Virtual shadow map cache tuning against shimmer; effectively free."), true));
		S->SectionProperties.Add(TEXT("ReflectionDenoise"), BoolProp(this, TEXT("Gfx_ReflectionDenoise"),
			LOCTEXT("Reflectiondenoise", "Reflection denoise"),
			LOCTEXT("Lumenreflectionreconstructionfor", "Lumen reflection reconstruction for cleaner metal/floors; measured free."), true));
		S->SectionProperties.Add(TEXT("ContactShadows"), BoolProp(this, TEXT("Gfx_ContactShadows"),
			LOCTEXT("Contactshadows", "Contact shadows"),
			LOCTEXT("Groundingshadowsonstaticgeometry", "Grounding shadows on static geometry (belt items + foliage stay excluded to avoid shimmer)."), true));
		S->SectionProperties.Add(TEXT("ContactShadowLength"), FloatProp(this, TEXT("Gfx_ContactShadowLength"),
			LOCTEXT("Contactshadowlength", "Contact shadow length"),
			LOCTEXT("Screenspacecontactshadowlength01", "Screen-space contact shadow length; >0.1 causes streaking."), 0.035f));
		S->SectionProperties.Add(TEXT("NaniteSharpen"), BoolProp(this, TEXT("Gfx_NaniteSharpen"),
			LOCTEXT("Nanitesharpen", "Nanite sharpen"),
			LOCTEXT("FinerNanitegeometryLODskippedonI", "Finer Nanite geometry LOD (skipped on Intel — slow base-pass on this engine version)."), true));
		S->SectionProperties.Add(TEXT("NanitePixelsPerEdge"), FloatProp(this, TEXT("Gfx_NanitePixelsPerEdge"),
			LOCTEXT("Nanitepixelsperedge", "Nanite pixels per edge"),
			LOCTEXT("10enginedefaultlowersharpergeome", "1.0 = engine default; lower = sharper geometry, higher = cheaper."), 1.f));
		S->SectionProperties.Add(TEXT("LumenPerfLevers"), BoolProp(this, TEXT("Gfx_LumenPerfLevers"),
			LOCTEXT("Lumenschedulingwins", "Lumen scheduling wins"),
			LOCTEXT("Freecomputeschedulingspeedupsfor", "Free compute-scheduling speedups for Lumen reflections (no image change)."), true));
		S->SectionProperties.Add(TEXT("DontLimitOnBattery"), BoolProp(this, TEXT("Gfx_DontLimitOnBattery"),
			LOCTEXT("Nobatterythrottle", "No battery throttle"),
			LOCTEXT("Dontdownclockrenderingonbatteryl", "Don't down-clock rendering on battery (laptops)."), true));
		S->SectionProperties.Add(TEXT("MaxFrameQueue"), IntProp(this, TEXT("Gfx_MaxFrameQueue"),
			LOCTEXT("Maxqueuedframes", "Max queued frames"),
			LOCTEXT("Framequeuedepthprobelowerlessinp", "Frame queue depth probe (lower = less input latency). 0 = don't touch."), 1));
		S->SectionProperties.Add(TEXT("ReflexMode"), IntProp(this, TEXT("Gfx_ReflexMode"),
			LOCTEXT("NVIDIAReflex012", "NVIDIA Reflex (0/1/2)"),
			LOCTEXT("0off1lowlatency2boostboostpinsGP", "0 off, 1 low-latency, 2 +boost (boost pins GPU clocks and can cost fps when GPU-bound)."), 1));
		S->SectionProperties.Add(TEXT("AsyncTick"), BoolProp(this, TEXT("Gfx_AsyncTick"),
			LOCTEXT("Asynctickrelief", "Async tick relief"),
			LOCTEXT("Gamethreadreliefviaasynctickdisp", "Game-thread relief via async tick dispatch/cleanup and FX batching."), true));
		S->SectionProperties.Add(TEXT("GrassTickInterval"), IntProp(this, TEXT("Gfx_GrassTickInterval"),
			LOCTEXT("Grasstickinterval", "Grass tick interval"),
			LOCTEXT("GrassrefresheveryNframesCPUrelie", "Grass refresh every N frames — CPU relief, near-free. 0 = engine default."), 10));
		S->SectionProperties.Add(TEXT("GrassDensityScale"), FloatProp(this, TEXT("Gfx_GrassDensityScale"),
			LOCTEXT("Grassdensity", "Grass density"),
			LOCTEXT("Staticgrassdensitymultiplierappl", "Static grass density multiplier (applied at launch; mid-session change pops)."), 1.f));
		S->SectionProperties.Add(TEXT("IncrementalGC"), BoolProp(this, TEXT("Gfx_IncrementalGC"),
			LOCTEXT("Incrementalgarbagecollection", "Incremental garbage collection"),
			LOCTEXT("SpreadGCworkoverframestoavoidhit", "Spread GC work over frames to avoid hitches. Turn off only when isolating a GC-related problem."), true));
	}
	// ---------------- Vendor & memory ----------------
	{
		UConfigPropertySection* S = AddSection(TEXT("Vendor"), LOCTEXT("SecVendor", "Vendor & Memory"));
		S->SectionProperties.Add(TEXT("VendorAdaptive"), BoolProp(this, TEXT("Ven_VendorAdaptive"),
			LOCTEXT("Vendoradaptivetuning", "Vendor-adaptive tuning"),
			LOCTEXT("PerGPUguardsAMDantiflickerIntelA", "Per-GPU guards: AMD anti-flicker, Intel Arc/iGPU caps, DLSS floor handling."), true));
		S->SectionProperties.Add(TEXT("AmdMinScreenPct"), FloatProp(this, TEXT("Ven_AmdMinScreenPct"),
			LOCTEXT("NonDLSSresolutionfloor", "Non-DLSS resolution floor %"),
			LOCTEXT("HigherdynamicresfloorforTSRXeSSF", "Higher dynamic-res floor for TSR/XeSS/FSR (they blur more than DLSS at low res)."), 62.f));
		S->SectionProperties.Add(TEXT("AmdAntiFlicker"), BoolProp(this, TEXT("Ven_AmdAntiFlicker"),
			LOCTEXT("AMDantiflicker", "AMD anti-flicker"),
			LOCTEXT("RDNALumentemporalflickerguardsTS", "RDNA Lumen temporal flicker guards (TSR rejection + TAA upgrade + DF-shadow tile fix)."), true));
		S->SectionProperties.Add(TEXT("AmdDFShadowCullTile"), FloatProp(this, TEXT("Ven_AmdDFShadowCullTile"),
			LOCTEXT("AMDDFshadowtilesize", "AMD DF-shadow tile size"),
			LOCTEXT("Distancefieldshadowculltileterra", "Distance-field shadow cull tile (terrain shadow flicker fix). 0 = skip."), 400.f));
		S->SectionProperties.Add(TEXT("IntelArcMaxBonusStage"), IntProp(this, TEXT("Ven_IntelArcMaxBonusStage"),
			LOCTEXT("IntelArcbonuscap", "Intel Arc bonus cap"),
			LOCTEXT("BonustierceilingonArcdGPUsDX12Lu", "Bonus-tier ceiling on Arc dGPUs (DX12 Lumen+Nanite crash class)."), 2));
		S->SectionProperties.Add(TEXT("IntelIGpuMaxBonusStage"), IntProp(this, TEXT("Ven_IntelIGpuMaxBonusStage"),
			LOCTEXT("InteliGPUbonuscap", "Intel iGPU bonus cap"),
			LOCTEXT("BonustierceilingonintegratedInte", "Bonus-tier ceiling on integrated Intel GPUs."), 0));
		S->SectionProperties.Add(TEXT("IntelIGpuMinScreenPct"), FloatProp(this, TEXT("Ven_IntelIGpuMinScreenPct"),
			LOCTEXT("InteliGPUresolutionfloor", "Intel iGPU resolution floor %"),
			LOCTEXT("iGPUsdropresolutionhardtoholdfra", "iGPUs drop resolution hard to hold frame-rate (the opposite of the discrete floor)."), 50.f));
		S->SectionProperties.Add(TEXT("AssertRayReconstructionOff"), BoolProp(this, TEXT("Ven_AssertRayReconstructionOff"),
			LOCTEXT("KeepRayReconstructionoff", "Keep Ray Reconstruction off"),
			LOCTEXT("RRisinertonsoftwareLumenhardguar", "RR is inert on software Lumen; hard-guard it off so nothing flips it."), true));
		S->SectionProperties.Add(TEXT("StreamingPoolMB"), IntProp(this, TEXT("Ven_StreamingPoolMB"),
			LOCTEXT("TexturepoolMB", "Texture pool (MB)"),
			LOCTEXT("0automaticfromrealVRAMbudgetreco", "0 = automatic from real VRAM budget (recommended); >0 pins an explicit pool size."), 0));
		S->SectionProperties.Add(TEXT("VramFloorMB"), FloatProp(this, TEXT("Ven_VramFloorMB"),
			LOCTEXT("VRAMfloorMB", "VRAM floor (MB)"),
			LOCTEXT("NeverpromotequalitywhenfreeVRAMi", "Never promote quality when free VRAM is below this."), 1500.f));
		S->SectionProperties.Add(TEXT("PoolBudgetFraction"), FloatProp(this, TEXT("Ven_PoolBudgetFraction"),
			LOCTEXT("Poolbudgetfraction", "Pool budget fraction"),
			LOCTEXT("FractionofthefreeVRAMbudgettheau", "Fraction of the free VRAM budget the automatic texture pool may take."), 0.4f));
	}
	// ---------------- Indoor fog (nested — the fog controller reads this section) ----------------
	{
		UConfigPropertySection* S = AddSection(TEXT("Fog"), LOCTEXT("SecFog", "Indoor Fog"));
		S->SectionProperties.Add(TEXT("Enabled"), BoolProp(this, TEXT("Fog_Enabled"),
			LOCTEXT("Adaptiveindoorfog", "Adaptive indoor fog"),
			LOCTEXT("Pullthefogstartdistanceinwhenyou", "Pull the fog start distance in when you're inside a sealed factory (kills interior haze)."), true));
		S->SectionProperties.Add(TEXT("IndoorStartDistance"), FloatProp(this, TEXT("Fog_IndoorStartDistance"),
			LOCTEXT("Indoorfogstartcm", "Indoor fog start (cm)"),
			LOCTEXT("Fogstartdistancewhileindoors", "Fog start distance while indoors."), 12000.f));
		S->SectionProperties.Add(TEXT("TransitionSec"), FloatProp(this, TEXT("Fog_TransitionSec"),
			LOCTEXT("Transitions", "Transition (s)"),
			LOCTEXT("Fadetimebetweenindooroutdoorfogs", "Fade time between indoor/outdoor fog states."), 4.f));
		S->SectionProperties.Add(TEXT("RoofTraceUp"), FloatProp(this, TEXT("Fog_RoofTraceUp"),
			LOCTEXT("Rooftraceheightcm", "Roof trace height (cm)"),
			LOCTEXT("Howhightolookforaroofwhendecidin", "How high to look for a roof when deciding you're indoors."), 4000.f));
		S->SectionProperties.Add(TEXT("CheckInterval"), FloatProp(this, TEXT("Fog_CheckInterval"),
			LOCTEXT("Checkintervals", "Check interval (s)"),
			LOCTEXT("Howoftentheindoortestruns", "How often the indoor test runs."), 0.25f));
		S->SectionProperties.Add(TEXT("MinBubble"), FloatProp(this, TEXT("Fog_MinBubble"),
			LOCTEXT("Minclearbubblecm", "Min clear bubble (cm)"),
			LOCTEXT("Fogneverstartscloserthanthis", "Fog never starts closer than this."), 200.f));
		S->SectionProperties.Add(TEXT("WallBias"), FloatProp(this, TEXT("Fog_WallBias"),
			LOCTEXT("Wallbias", "Wall bias"),
			LOCTEXT("Howstronglynearbywallscounttowar", "How strongly nearby walls count toward 'sealed'."), 0.9f));
		S->SectionProperties.Add(TEXT("GrowLerp"), FloatProp(this, TEXT("Fog_GrowLerp"),
			LOCTEXT("Growlerp", "Grow lerp"),
			LOCTEXT("Smoothingwhentheclearbubblegrows", "Smoothing when the clear bubble grows."), 0.2f));
		S->SectionProperties.Add(TEXT("ShrinkLerp"), FloatProp(this, TEXT("Fog_ShrinkLerp"),
			LOCTEXT("Shrinklerp", "Shrink lerp"),
			LOCTEXT("Smoothingwhentheclearbubbleshrin", "Smoothing when the clear bubble shrinks (faster, so fog doesn't lag behind walls)."), 0.6f));
		S->SectionProperties.Add(TEXT("SealCountMin"), IntProp(this, TEXT("Fog_SealCountMin"),
			LOCTEXT("Sealcountmin", "Seal count min"),
			LOCTEXT("Howmanyofthesurroundprobesmusthi", "How many of the surround probes must hit geometry to count as sealed (1-13)."), 3));
	}
}

#undef LOCTEXT_NAMESPACE
