#include "WNLQualityStages.h"

// Value provenance: [ours] = our own boot-measured set · [def] = the engine default the value
// moves from. Every tier placement was validated on our own hardware (see the boot-test log).
// Feature-group tags (v0.9.3, NOT citations): [denoise] = software-Lumen denoiser levers · [nanite].
// The tables are data, not logic — the governor's stage engine owns policies, gates, and reverts.
// POLICY RULE: a bonus lever must never lower a user's own better baseline. higher-is-better cvars
// use MaxOf; lower-is-better cvars (MaxPixelsPerEdge, ViewMeshLODBias, GridPixelSize) use MinOf.

const TArray<FWNLQualityStage>& WNLGetBonusStages()
{
	static const TArray<FWNLQualityStage> Stages = {
		{ +1, TEXT("Lumen filtering + reflection richness"), 0.5f, {
			// Cheapest GI win: filter harder instead of tracing more.
			{ TEXT("r.Lumen.ScreenProbeGather.SpatialFilterNumPasses"), 4 },
			{ TEXT("r.Lumen.ScreenProbeGather.TemporalFilterProbes"), 1 },
			{ TEXT("r.Lumen.RadianceCache.SpatialFilterProbes"), 1 },
			{ TEXT("r.Lumen.Reflections.SampleSceneColorAtHit"), 1 },  // richest reflection realism per ms
			{ TEXT("r.Lumen.Reflections.RadianceCache"), 1 },
			// [denoise] vendor-neutral software-Lumen denoise = our answer to "ray reconstruction".
			// RR itself is inert on software Lumen (no HWRT hit-data) and stays guarded off; these are
			// SPATIAL passes only — the RR-style clean-up on BOTH NVIDIA and AMD. Temporal-accumulation
			// denoisers are deliberately HELD across the whole engine (they worsen the conveyor-shimmer
			// finding [ours]) — none appear in any bonus tier.
			{ TEXT("r.Lumen.Reflections.BilateralFilter.NumSamples"), 6, EWNLLeverPolicy::MaxOf },                    // def 4
			{ TEXT("r.Lumen.Reflections.BilateralFilter.KernelRadius"), 10.f, EWNLLeverPolicy::MaxOf, true },         // def 8
			// [nanite] FREE: CSS ships a 50MB Nanite pool (10x below default), so the engine's hidden
			// quality auto-downscale fires early in big factories. These two just move its thresholds.
			{ TEXT("r.Nanite.Streaming.QualityScale.MaxPoolPercentage"), 92.f, EWNLLeverPolicy::MaxOf, true }, // def 85
			{ TEXT("r.Nanite.Streaming.QualityScale.MinQuality"), 0.6f, EWNLLeverPolicy::MaxOf, true },        // def 0.3
		}},
		{ +2, TEXT("Translucency + radiosity polish"), 0.8f, {
			{ TEXT("r.Lumen.TranslucencyReflections.FrontLayer.Enable"), 1 }, // glass/water reflections
			{ TEXT("r.Lumen.TranslucencyVolume.SpatialFilter.NumPasses"), 3 },
			{ TEXT("r.LumenScene.Radiosity.Temporal.MaxFramesAccumulated"), 32 },
			{ TEXT("pool.light.relevancyMultiplier"), 10 },                   // Satisfactory light pool
			// [denoise] the closest software-Lumen equivalent to what RR does on hardware Lumen: the
			// screen-space BRDF-reweighting reconstruction pass. SPATIAL, no ghosting. (The diffuse-GI
			// temporal-accumulation bump is HELD with the rest — see +1 — pending a ghosting boot-test.)
			{ TEXT("r.Lumen.Reflections.ScreenSpaceReconstruction.NumSamples"), 7, EWNLLeverPolicy::MaxOf },   // def 5
			// [nanite] restore imposters (CSS disables them). The atlas is already baked into every
			// cooked Nanite mesh, so this is a pure runtime toggle (zero recook) that kills the
			// distant-cluster popping on the horizon.
			{ TEXT("r.Nanite.Streaming.Imposters"), 1, EWNLLeverPolicy::MaxOf },              // def 1, CSS forces 0
			{ TEXT("r.Nanite.Streaming.NumInitialImposters"), 2048, EWNLLeverPolicy::MaxOf }, // def 2048, CSS forces 0
		}},
		{ +3, TEXT("Trace reach + surface cache"), 1.5f, {
			// Long-range GI carries the dramatic-vista look; we start conservatively at half scale.
			{ TEXT("r.Lumen.TraceDistanceScale"), 2.f, EWNLLeverPolicy::Absolute, true },
			{ TEXT("r.Lumen.TraceMeshSDFs"), 1 },
			{ TEXT("r.Lumen.TraceMeshSDFs.Allow"), 1 },
			{ TEXT("r.LumenScene.SurfaceCache.CardTexelDensityScale"), 800.f, EWNLLeverPolicy::Absolute, true, 11500 },
			{ TEXT("r.LumenScene.GlobalSDF.Resolution"), 256 },
			// MinOf: LOWER GridPixelSize = sharper. Never coarsen a user whose baseline is already
			// <6 (UE Cinematic FogQuality ships 4). 4 was artifact-prone for us so 6 is our ceiling.
			{ TEXT("r.VolumetricFog.GridPixelSize"), 6, EWNLLeverPolicy::MinOf },
			// [nanite] grow the 50MB streaming pool (10x below engine default) so geometry stays sharp
			// while streaming catches up after fast movement through dense areas. Direct VRAM cost ->
			// gate to 12GB+ cards (the dynamic-VRAM system owns the texture pool separately).
			{ TEXT("r.Nanite.Streaming.StreamingPoolSize"), 128, EWNLLeverPolicy::MaxOf, false, 11500 }, // MB; def 512, CSS 50 — MaxOf so a user at 512 keeps it
			{ TEXT("r.Nanite.Streaming.MaxPendingPages"), 256, EWNLLeverPolicy::MaxOf },        // def 128
			{ TEXT("r.Nanite.Streaming.MaxPageInstallsPerFrame"), 256, EWNLLeverPolicy::MaxOf },// def 128
		}},
		{ +4, TEXT("Shadow + world detail sharpen"), 1.5f, {
			// BaseDelta so the sharpen is RELATIVE to the user's real ShadowQuality — never deeper
			// than -0.5 below their baseline (conveyor-shimmer finding [ours]).
			{ TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectional"), -0.5f, EWNLLeverPolicy::BaseDelta, true },
			{ TEXT("r.Shadow.Virtual.SMRT.RayCountDirectional"), 16 },
			{ TEXT("r.Shadow.Virtual.SMRT.SamplesPerRayDirectional"), 8 },
			{ TEXT("r.Shadow.Virtual.OnePassProjection"), 1 },
			{ TEXT("r.Shadow.Virtual.OnePassProjection.MaxLightsPerPixel"), 8 },
			{ TEXT("pool.light.shadowquality"), 4 },                          // night-cliff lever
			{ TEXT("foliage.LODDistanceScale"), 1.5f, EWNLLeverPolicy::MaxOf, true }, // trees hold 3D LODs longer
			// [nanite] the single biggest Nanite sharpness lever: smaller target triangle edge =
			// crisper cluster silhouettes scene-wide (pipes/foundations/conveyors). The engine's own
			// PixelsPerEdgeScaling auto-relief keeps it inside the raster budget under load.
			{ TEXT("r.Nanite.MaxPixelsPerEdge"), 0.8f, EWNLLeverPolicy::MinOf, true },     // def 1.0; MinOf = never coarsen a user who set it lower
			{ TEXT("r.Nanite.ViewMeshLODBias.Offset"), -0.5f, EWNLLeverPolicy::MinOf, true }, // finer LOD than TSR picks; engine floor -2.0; MinOf keeps a user's finer bias
		}},
		{ +5, TEXT("Cinematic GI"), 2.0f, {
			// MaxOf: never lowers a user who already runs Cinematic.
			{ TEXT("sg.GlobalIlluminationQuality"), 4, EWNLLeverPolicy::MaxOf },
			// NOTE: the Nanite cluster-cap CORRECTNESS fix (MaxVisibleClusters/MaxCandidateClusters/
			// MaxNodes) used to live here but was moved to the baseline post-settle pass — it prevents
			// DROPPED geometry in dense factories, so it must NOT be gated behind headroom the governor
			// sheds under exactly that load (review finding). See ApplyPostSettleGraphics.
		}},
		{ +6, TEXT("Beyond-Cinematic"), 3.0f, {
			{ TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor"), 4 },        // Cine=8 → 2x probe density [ours]
			{ TEXT("r.Lumen.ScreenProbeGather.TracingOctahedronResolution"), 16 },
			{ TEXT("r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget"), 512 },
			{ TEXT("r.Lumen.TraceDistanceScale"), 4.f, EWNLLeverPolicy::Absolute, true }, // overrides +3's 2
			{ TEXT("r.Lumen.TraceMeshSDFs.TraceDistance"), 250.f, EWNLLeverPolicy::Absolute, true },
			{ TEXT("r.LumenScene.SurfaceCache.CardTexelDensityScale"), 2000.f, EWNLLeverPolicy::Absolute, true, 15500 },
			{ TEXT("r.VolumetricFog.GridSizeZ"), 192 },
			{ TEXT("r.VolumetricFog.HistoryMissSupersampleCount"), 16 },
			// GridPixelSize deliberately stays 6 (from +3): 4 produced grid artifacts in steamy
			// interiors on the 2026-07-16 boot — fails match-reality.
			// [nanite] push edge/LOD sharpness past +4 at the top tier. MinOf keeps a user's own
			// sharper value; recompute-from-baseline means this cleanly supersedes +4's 0.8/-0.5.
			{ TEXT("r.Nanite.MaxPixelsPerEdge"), 0.6f, EWNLLeverPolicy::MinOf, true },      // finer than +4's 0.8
			{ TEXT("r.Nanite.ViewMeshLODBias.Offset"), -1.0f, EWNLLeverPolicy::MinOf, true },// finer than +4's -0.5
			// [denoise] Epic-flagged Experimental GI spatial kernel widen — top tier only, max headroom.
			{ TEXT("r.Lumen.ScreenProbeGather.SpatialFilterHalfKernelSize"), 2, EWNLLeverPolicy::MaxOf }, // def 1 (Experimental)
		}},
	};
	return Stages;
}

const TArray<FWNLQualityStage>& WNLGetCutStages()
{
	static const TArray<FWNLQualityStage> Stages = {
		{ -1, TEXT("contact shadows off"), 0.3f, {
			{ TEXT("r.ContactShadows"), 0 },
		}},
		{ -2, TEXT("volumetric fog coarsened"), 0.5f, {
			// MaxOf: coarser-or-equal to the user's own grid — a cut must never IMPROVE a value.
			{ TEXT("r.VolumetricFog.GridPixelSize"), 16, EWNLLeverPolicy::MaxOf },
		}},
		{ -3, TEXT("sparse-probe filtered GI"), 1.0f, {
			// Sparse probes + heavy filtering IS the graceful floor look (still coherent GI, much cheaper).
			{ TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor"), 2.f, EWNLLeverPolicy::BaseScale, false, 0, 8.f, 32.f },
			{ TEXT("r.Lumen.ScreenProbeGather.SpatialFilterNumPasses"), 4 },
			{ TEXT("r.Lumen.ScreenProbeGather.TemporalFilterProbes"), 1 },
		}},
		{ -4, TEXT("GI tier down (last resort)"), 1.5f, {
			{ TEXT("sg.GlobalIlluminationQuality"), -1.f, EWNLLeverPolicy::BaseDelta, false, 0, 0.f, 4.f },
		}},
	};
	return Stages;
}
