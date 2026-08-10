// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMHudHookGuard.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"

#include "Patching/BlueprintHookManager.h"
#include "Patching/BlueprintHookBlueprint.h"

namespace
{
	/** The vanilla widget whose Construct injection asserts. Matched with Contains: the blueprint class
	 *  is `Widget_PlayerHUD_C`, so an exact compare would never hit. */
	const TCHAR* GFPMHudGuardTargetClass = TEXT("Widget_PlayerHUD");

	/**
	 * ⚠ EXACT MATCH, not Contains — a REFINEMENT over the old implementation.
	 *
	 * The old code used `Def.TargetFunction->GetName().Contains(TEXT("Construct"))`, which also matches
	 * any function whose name merely CONTAINS the word — `ReconstructWidget`, `PostConstruct`, and
	 * anything a mod invents. This guard removes another mod's code, so its predicate must be as narrow
	 * as the evidence: the crash log names `Widget_PlayerHUD_C:Construct` and nothing else.
	 */
	const TCHAR* GFPMHudGuardCrashingFunction = TEXT("Construct");

	/**
	 * Assets known to carry the crashing descriptor. Substring-matched against the hook asset's path.
	 *
	 * ⚠ A LIST, NOT A RULE, AND IT STAYS SHORT. Everything not on it is ALLOWED and merely named in the
	 * log, so a second offender becomes visible without being pre-emptively broken. An entry earns its
	 * place with a crash log behind it, never with a suspicion.
	 */
	const TCHAR* const GFPMHudGuardKnownCrashers[] = { TEXT("KPrivateCodeLib") };

	int32 GFPMHudSeen = 0;
	int32 GFPMHudStripped = 0;
	int32 GFPMHudAllowed = 0;
	int32 GFPMHudCancelled = 0;
}

FFPMHudHookGuard& FFPMHudHookGuard::Get()
{
	static FFPMHudHookGuard Instance;
	return Instance;
}

void FFPMHudHookGuard::GetCounts(int32& OutSeen, int32& OutStripped, int32& OutAllowed, int32& OutCancelled)
{
	OutSeen = GFPMHudSeen;
	OutStripped = GFPMHudStripped;
	OutAllowed = GFPMHudAllowed;
	OutCancelled = GFPMHudCancelled;
}

void FFPMHudHookGuard::Arm()
{
	auto OnRegisterHook = [](auto& Scope, UBlueprintHookManager* Mgr, UGameInstance* GameInstance,
	                         UHookBlueprintGeneratedClass* HookClass)
	{
		if (!HookClass) { return; }

		// Does this registration touch the player HUD at all? Almost none do; leave those untouched.
		bool bTargetsPlayerHUD = false;
		FString TargetDesc;
		for (const FBlueprintHookDefinition& Def : HookClass->HookDescriptors)
		{
			// A null TargetFunction is the "Failed to find any hook targets" case — a DEAD definition.
			if (!Def.TargetFunction) { continue; }

			const UClass* Owner = Def.TargetFunction->GetOuterUClass();
			if (Owner && Owner->GetName().Contains(GFPMHudGuardTargetClass))
			{
				bTargetsPlayerHUD = true;
				TargetDesc = FString::Printf(TEXT("%s::%s"), *Owner->GetName(), *Def.TargetFunction->GetName());
				break;
			}
		}
		if (!bTargetsPlayerHUD) { return; }

		++GFPMHudSeen;

		const FString HookPath = HookClass->GetPathName();
		bool bKnownCrasher = false;
		for (const TCHAR* Needle : GFPMHudGuardKnownCrashers)
		{
			if (HookPath.Contains(Needle)) { bKnownCrasher = true; break; }
		}

		if (!bKnownCrasher)
		{
			/*
			 * NOT blocked, and NAMED. If this one turns out to crash too, the log already says who — and
			 * an author reading their own log can see that we looked and deliberately left them alone.
			 * This line is the difference between "a mod's UI vanished" and "we can find out why".
			 */
			++GFPMHudAllowed;
			if (FPMDiag::IsOn(FPMDiag::EChannel::HudGuard))
			{
				UE_LOG(LogFicsitsPerformanceManager, Display,
					TEXT("[FPM] hud guard: ALLOWING %s - it injects into %s, the same place a known crash "
					     "comes from, but it is not on the known-crashing list so it runs untouched. If a "
					     "crash appears in ExecuteUbergraph_Widget_PlayerHUD, this line names the candidate."),
					*HookPath, *TargetDesc);
			}
			return;
		}

		/*
		 * ★ STRIP THE DESCRIPTOR, NOT THE ASSET. The crash is one injection — into Construct — and a hook
		 * asset carries many descriptors. Cancelling the asset took out the HUD contribution of the whole
		 * KAPI/KBFL/KUI family and Ant felt it: "some modded UI doesnt close on escape and parts of their
		 * UI dosnt exist."
		 *
		 * Reverse iteration because we remove as we go.
		 */
		int32 Removed = 0;
		FString RemovedDesc;
		for (int32 i = HookClass->HookDescriptors.Num() - 1; i >= 0; --i)
		{
			const FBlueprintHookDefinition& Def = HookClass->HookDescriptors[i];
			if (!Def.TargetFunction) { continue; }

			const UClass* Owner = Def.TargetFunction->GetOuterUClass();
			const bool bIsTheCrashingOne = Owner
				&& Owner->GetName().Contains(GFPMHudGuardTargetClass)
				// Exact, not Contains — see the constant's comment.
				&& Def.TargetFunction->GetName() == GFPMHudGuardCrashingFunction;

			if (bIsTheCrashingOne)
			{
				RemovedDesc = FString::Printf(TEXT("%s::%s"), *Owner->GetName(), *Def.TargetFunction->GetName());
				HookClass->HookDescriptors.RemoveAt(i);
				++Removed;
			}
		}

		if (Removed > 0 && HookClass->HookDescriptors.Num() > 0)
		{
			GFPMHudStripped += Removed;
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] hud guard: %s - removed %d hook definition(s) targeting %s, and let the rest "
				     "install (%d remain). ONLY the injection into Construct is dropped: that is the one "
				     "that asserts on every death and vehicle exit (ExecuteUbergraph_Widget_PlayerHUD "
				     "offset 0x10000000C). This mod's other HUD hooks run normally."),
				*HookPath, Removed, *RemovedDesc, HookClass->HookDescriptors.Num());
			return;   // registration proceeds, minus the crashing descriptor
		}

		/*
		 * ⚠ NOTHING LEFT TO REGISTER, OR NOTHING ISOLATED. Cancel rather than let an empty registration
		 * through — that would be a silent no-op wearing the appearance of success.
		 *
		 * This branch is the one that costs another mod its overlay, so it is counted separately and
		 * should read as ZERO in a healthy session. A non-zero cancelled count means the strip is no
		 * longer working and somebody is losing UI again.
		 */
		++GFPMHudCancelled;
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] hud guard: CANCELLING %s - it injects into %s, and injected code there asserts on "
			     "every death/vehicle-exit. %s Disable FPM's hud guard to restore the overlay, and the "
			     "crash with it."),
			*HookPath, *TargetDesc,
			Removed > 0
				? TEXT("Stripping the crashing definition left the asset with no hooks at all, so the whole "
				       "registration is cancelled rather than installing an empty one.")
				: TEXT("The crashing definition could not be isolated, so the whole asset is cancelled - its "
				       "HUD overlay is disabled and everything else it does still works."));
		Scope.Cancel();
	};

	RegisterBlueprintHookHandle = FPM_SUBSCRIBE("hud-hook-guard", UBlueprintHookManager::RegisterBlueprintHook, OnRegisterHook);

	/*
	 * ⚠ THE OLD MOD'S SECOND HOOK IS DELIBERATELY NOT CARRIED. It also subscribed to
	 * `UOverlay::AddChildToOverlay` purely to census which widgets get added to a HUD overlay. That is a
	 * DIAGNOSTIC, not the guard, and design P3.5 says "rebuild NARROW". It put a hook into a UMG function
	 * on every widget add, for information we do not currently need. If the census is ever wanted it can
	 * come back as its own log-only fix with its own channel — not smuggled inside a crash guard.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] hud guard ARMED - strips ONLY blueprint-hook descriptors targeting "
		     "Widget_PlayerHUD::Construct, and only from assets on the known-crashing list. Everything "
		     "else that hooks the HUD is allowed through and NAMED in the log. Two earlier versions of "
		     "this guard were too broad and cost Ant real mod UI; this one removes one descriptor and "
		     "lets the asset install."));
}

void FFPMHudHookGuard::Disarm()
{
	/*
	 * UNSUBSCRIBE_METHOD is correct for a _VIRTUAL subscribe: both drive the same
	 * HookInvoker<decltype(&M), &M>, and RemoveHandler clears the BEFORE and AFTER maps
	 * alike, uninstalling the detour once both are empty (NativeHookManager.h:359-378).
	 *
	 * ⚠ Guarded on IsValid() because the editor path installs nothing and returns an
	 * invalid handle; RemoveHandler would then walk maps SML never allocated.
	 */
	if (RegisterBlueprintHookHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(UBlueprintHookManager::RegisterBlueprintHook, RegisterBlueprintHookHandle);
		RegisterBlueprintHookHandle.Reset();
	}
}
