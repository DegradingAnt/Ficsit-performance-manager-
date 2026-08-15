// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/ModFeatures/FPMReflexMode.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMDiag.h"

#include "HAL/IConsoleManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	/*
	 * ★ THE MODE VALUES, FROM THE BANKED SPEC RATHER THAN FROM MEMORY (FPG-REBUILD-SPEC.md:76):
	 *   0  off
	 *   1  eLowLatency  — helps when GPU-bound, costs <=4% FPS, near-free otherwise. The recommendation.
	 *   2  Boost        — "can cost FPS/power", and the spec's words are "never Boost as baseline".
	 */
	TAutoConsoleVariable<int32> CVarReflexMode(
		TEXT("FPM.Reflex.Mode"), 1,
		TEXT("NVIDIA Reflex mode. 0 = leave the game's setting alone, 1 = low latency (the banked "
		     "recommendation: helps when GPU-bound, costs <=4% FPS, near-free otherwise), 2 = Boost "
		     "(CAN COST FPS AND POWER - the spec says never as a baseline; opt in deliberately). "
		     "This fix is off by default until one boot proves it can reach Reflex at all."),
		ECVF_Default);

	constexpr int32 GFPMReflexModeMax = 2;

	/*
	 * ★★★ VERIFIED FROM THE SHIPPED BINARY, 2026-08-11 — the spec flagged these UNVERIFIED and Ant said
	 * "check the dumps for how to do it". UTF-16 extraction from
	 * FactoryGame/Plugins/StreamlineCore/Binaries/Win64/FactoryGameSteam-StreamlineCore-Win64-Shipping.dll
	 * returns the names WITH their help text:
	 *
	 *     "Enable Streamline Reflex extension. (default = 0)"          t.Streamline.Reflex.Enable
	 *     "Streamline Reflex mode (default = 1)  1: low latency  2: low latency with boost"
	 *                                                                 t.Streamline.Reflex.Mode
	 *     "Enable Streamline Reflex extension when other SL features need it. (default = 1)"
	 *                                                                 t.Streamline.Reflex.Auto
	 *     "Controls whether Streamline Reflex handles frame rate limiting instead of the engine
	 *      (default = true)"                                          t.Streamline.Reflex.HandleMaxTickRate
	 *
	 * ⚠⚠ AND THE EXTRACTION CAUGHT A REAL DEFECT IN THE FIRST VERSION OF THIS FILE.
	 *
	 * `Reflex.Enable` DEFAULTS TO 0. Reflex is OFF in this game. The first version set only `.Mode`,
	 * which would have written a mode onto a DISABLED extension and then reported success forever —
	 * the dead-instrument shape, in the fix whose own header warns about it. Setting the mode is not
	 * enabling the feature, and nothing short of reading the help text would have said so.
	 *
	 * `.Mode`'s own default is already 1, so the mode was never the interesting half.
	 */
	const TCHAR* GFPMReflexEnableCVar = TEXT("t.Streamline.Reflex.Enable");
	const TCHAR* GFPMReflexModeCVar   = TEXT("t.Streamline.Reflex.Mode");

	/** Where the Streamline Reflex blueprint library lives, if the plugin is mounted. */
	const TCHAR* GFPMReflexClassPath = TEXT("/Script/StreamlineReflex.StreamlineLibraryReflex");
	const TCHAR* GFPMReflexFunctionName = TEXT("SetReflexMode");

	IConsoleVariable* FindCVar(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name);
	}

	/**
	 * Call `UStreamlineLibraryReflex::SetReflexMode(Mode)` through the reflection system.
	 *
	 * ★ THE PARAMETER IS WRITTEN BY INTROSPECTION, NOT BY ASSUMING ITS TYPE. `SetReflexMode` takes an
	 * enum, and whether that enum is byte-backed or int-backed decides the size of the parameter frame.
	 * Guessing wrong writes past the end of a stack buffer or silently passes garbage — so the first
	 * property of the UFunction is located and its OWN size and setter are used.
	 *
	 * @return true only if the function was found AND invoked.
	 */
	bool CallReflexLibrary(int32 Mode, FString& OutDetail)
	{
		UClass* LibClass = FindObject<UClass>(nullptr, GFPMReflexClassPath);
		if (LibClass == nullptr)
		{
			OutDetail = TEXT("StreamlineLibraryReflex class not found - the plugin is not mounted here");
			return false;
		}

		UFunction* Fn = LibClass->FindFunctionByName(FName(GFPMReflexFunctionName));
		if (Fn == nullptr)
		{
			OutDetail = FString::Printf(
				TEXT("found %s but it has no %s - the API moved"), GFPMReflexClassPath, GFPMReflexFunctionName);
			return false;
		}

		// The parameter frame, zeroed. ParmsSize is the authority on how big it must be.
		TArray<uint8> Frame;
		Frame.SetNumZeroed(FMath::Max<int32>(Fn->ParmsSize, 1));

		/*
		 * Find the FIRST input parameter and write the mode into it using its own property interface.
		 * FNumericProperty covers both the byte-backed and int-backed enum cases, and an FEnumProperty
		 * exposes its underlying numeric property for exactly this purpose.
		 */
		FProperty* Param = nullptr;
		for (TFieldIterator<FProperty> It(Fn); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			if (!(It->PropertyFlags & CPF_ReturnParm))
			{
				Param = *It;
				break;
			}
		}

		if (Param == nullptr)
		{
			OutDetail = TEXT("SetReflexMode takes no parameter - the API moved");
			return false;
		}

		void* ValuePtr = Param->ContainerPtrToValuePtr<void>(Frame.GetData());

		if (FEnumProperty* AsEnum = CastField<FEnumProperty>(Param))
		{
			AsEnum->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, static_cast<int64>(Mode));
		}
		else if (FNumericProperty* AsNumeric = CastField<FNumericProperty>(Param))
		{
			AsNumeric->SetIntPropertyValue(ValuePtr, static_cast<int64>(Mode));
		}
		else
		{
			OutDetail = FString::Printf(
				TEXT("SetReflexMode's parameter is a %s, which this fix does not know how to write"),
				*Param->GetClass()->GetName());
			return false;
		}

		// A BlueprintCallable static runs on the class default object.
		LibClass->GetDefaultObject()->ProcessEvent(Fn, Frame.GetData());

		OutDetail = FString::Printf(TEXT("%s::%s via reflection (param %s)"),
			GFPMReflexClassPath, GFPMReflexFunctionName, *Param->GetClass()->GetName());
		return true;
	}
}

FFPMReflexMode& FFPMReflexMode::Get()
{
	static FFPMReflexMode Instance;
	return Instance;
}

void FFPMReflexMode::Arm()
{
	CVarReflexMode.AsVariable()->SetOnChangedCallback(
		FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*)
		{
			FFPMReflexMode::Get().ApplyFromCVar(TEXT("cvar changed"));
		}));

	// Ungated by the diag channel: the stated Arm()-line exception.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] reflex mode ARMED - no hook. FPM1 shipped this as Gfx_ReflexMode and the rewrite "
		     "dropped it. Mode 1 (low latency) is the banked recommendation: helps when GPU-bound, "
		     "costs <=4%% FPS, near-free otherwise. Mode 2 (Boost) can COST FPS and power and is never "
		     "a baseline. ⚠ THE GAME SHIPS t.Streamline.Reflex.Enable AT 0 - Reflex is OFF by default "
		     "here, verified from the StreamlineCore DLL's own help text - so this sets Enable AND Mode. "
		     "FPM.Reflex.Report names which route reached the plugin."));
}

void FFPMReflexMode::Disarm()
{
	/*
	 * ⚠ NOTHING IS PUT BACK, AND THAT IS STATED RATHER THAN SILENT.
	 *
	 * ★ THE CVAR ROUTE NOW RESTORES PROPERLY, because the values it overwrote were captured BEFORE the
	 * first write. An earlier version of this comment argued a restore was impossible - that was true
	 * only while the fix did not bother to remember, which is not the same thing.
	 *
	 * ⚠ THE REFLECTION ROUTE STILL CANNOT BE UNDONE, and that is stated rather than hidden.
	 * `SetReflexMode` is a setter with no getter beside it, so there is nothing to read back before
	 * calling it. Disarm therefore stops FUTURE writes on that path and says so.
	 *
	 * Zero residue is untouched either way: this writes no file and no ini.
	 */
	static const FName Owner(TEXT("reflex-mode"));

	// The writer captured the prior value/SetBy at the first Hold; Release restores both through the
	// engine's own tagged-history mechanism. Safe to call when nothing is held - it says so and no-ops.
	const bool bReleasedEnable = FPMCVarWriter::Get().Release(Owner, GFPMReflexEnableCVar);
	const bool bReleasedMode   = FPMCVarWriter::Get().Release(Owner, GFPMReflexModeCVar);
	if (bReleasedEnable || bReleasedMode)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] reflex: released %s and %s back to their prior value/SetBy."),
			GFPMReflexEnableCVar, GFPMReflexModeCVar);
	}

	AppliedMode = -1;
}

void FFPMReflexMode::OnWorldLoad(UWorld* /*World*/)
{
	/*
	 * Streamline initialises well after module startup — Ant's log has
	 * `FStreamlineMaxTickRateHandler::Initialize` at 10:42:13, about eleven seconds after the module
	 * loaded. Applying at Arm() would therefore look for a class that does not exist yet and record a
	 * permanent "not found". World load is the first moment the answer can be right.
	 */
	ApplyFromCVar(TEXT("world load"));
}

void FFPMReflexMode::ApplyFromCVar(const TCHAR* Moment)
{
	const int32 Want = CVarReflexMode.GetValueOnGameThread();

	if (Want < 0 || Want > GFPMReflexModeMax)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] reflex: REFUSING FPM.Reflex.Mode=%d. Valid values are 0 (hands off), 1 (low "
			     "latency) and 2 (Boost, which can cost FPS)."), Want);
		return;
	}

	if (Want == 0)
	{
		UE_CLOG(AppliedMode > 0 && FPMDiag::IsOn(FPMDiag::EChannel::Reflex),
			LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] reflex: FPM.Reflex.Mode is 0 - leaving the game's own Reflex setting alone from "
			     "%s onward. What was already applied is NOT undone; see Disarm's note for why."), Moment);
		AppliedMode = 0;
		return;
	}

	if (AppliedMode == Want)
	{
		return;   // already applied; a world reload re-running this is the common case and is not news
	}

	/*
	 * ★ ROUTE 1: THE CVAR, TRIED RATHER THAN ASSUMED. The spec flags these names UNVERIFIED, so its
	 * existence is the test. If it is here, it is the cheaper and more transparent route — the player
	 * can see and change it themselves.
	 */
	IConsoleVariable* EnableVar = FindCVar(GFPMReflexEnableCVar);
	IConsoleVariable* ModeVar   = FindCVar(GFPMReflexModeCVar);

	if (EnableVar != nullptr && ModeVar != nullptr)
	{
		/*
		 * ★ ENABLE FIRST, THEN MODE, AND BOTH ARE REQUIRED. `.Enable` ships at 0 — writing only the mode
		 * configures a switched-off extension. Order matters only for readability here (neither takes
		 * effect until the next frame), but writing one without the other is the actual bug.
		 */
		/*
		 * ★ THE WRITER CAPTURES THE PRIOR STATE, not this fix. D3: a hand-rolled capture here risked
		 * recording our OWN earlier write instead of the player's on a re-hold - the exact bug R33
		 * killed elsewhere. FPMCVarWriter::Hold captures on first hold and keeps that baseline across
		 * every re-hold.
		 */
		static const FName Owner(TEXT("reflex-mode"));
		const bool bEnableHeld = FPMCVarWriter::Get().Hold(
			Owner, GFPMReflexEnableCVar, TEXT("1"),
			TEXT("reflex mode: .Enable ships at 0, this is the required first half of the pair"));
		const bool bModeHeld = FPMCVarWriter::Get().Hold(
			Owner, GFPMReflexModeCVar, *FString::FromInt(Want),
			TEXT("reflex mode: the player's requested latency mode"));

		if (bEnableHeld && bModeHeld)
		{
			AppliedMode = Want;
			RouteFound = FString::Printf(TEXT("cvars %s=1 + %s=%d"),
				GFPMReflexEnableCVar, GFPMReflexModeCVar, Want);

			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] reflex: ENABLED the extension and set mode %d at %s. Both writes are "
				     "required - %s ships at 0, so a mode-only write would have configured a disabled "
				     "feature and reported success."), Want, Moment, GFPMReflexEnableCVar);
			return;
		}

		// One or both holds were REFUSED (the writer already logged why - Vet()'s clauses). Do not
		// report success for a write that did not fully happen; release whichever half succeeded so
		// Reflex is not left half-enabled, then fall through to the reflection route below.
		FPMCVarWriter::Get().Release(Owner, GFPMReflexEnableCVar);
		FPMCVarWriter::Get().Release(Owner, GFPMReflexModeCVar);
	}

	UE_CLOG(EnableVar != nullptr || ModeVar != nullptr, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM] reflex: found only ONE of the two required cvars (%s=%s, %s=%s). Refusing to write "
		     "half of it - enabling without a mode, or a mode without enabling, is worse than not "
		     "trying. Falling through to the reflection route."),
		GFPMReflexEnableCVar, EnableVar ? TEXT("present") : TEXT("ABSENT"),
		GFPMReflexModeCVar,   ModeVar   ? TEXT("present") : TEXT("ABSENT"));

	// ROUTE 2: reflection on the plugin's own blueprint library.
	FString Detail;
	if (CallReflexLibrary(Want, Detail))
	{
		AppliedMode = Want;
		RouteFound = Detail;

		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] reflex: set mode %d at %s using %s. The cvar names in the spec do NOT exist on "
			     "this build, which settles that question."), Want, Moment, *Detail);
		return;
	}

	/*
	 * ⚠ NEITHER ROUTE EXISTS. Ungated by the diag channel, because this is the case where the fix is
	 * doing nothing at all while being armed - the exact state that must never be quiet.
	 */
	RouteFound = FString::Printf(TEXT("NONE (%s)"), *Detail);
	UE_LOG(LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM] reflex: COULD NOT REACH REFLEX at %s. No known cvar exists and %s. This fix is armed "
		     "and doing NOTHING - do not read its silence as success. Her log shows the plugin mounted "
		     "and lowLatencyAvailable=1, so the capability is there and only this handle on it is wrong."),
		Moment, *Detail);
}

void FFPMReflexMode::ReportNow()
{
	FFPMReflexMode& Self = Get();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] reflex: requested mode %d | applied %d | route: %s"),
		CVarReflexMode.GetValueOnGameThread(), Self.AppliedMode,
		Self.RouteFound.IsEmpty() ? TEXT("not looked yet (needs a world load)") : *Self.RouteFound);

	/*
	 * State what a reader cannot otherwise know: Reflex being SET is not Reflex being EFFECTIVE. The
	 * plugin decides that, and it reports its own availability in the log at startup.
	 */
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM]   ⚠ Setting the mode is not proof it took. Streamline logs its own capability at "
		     "startup - grep 'lowLatencyAvailable' in the log. On Ant's 2026-08-11 client that read 1. "
		     "And mind t.Streamline.Reflex.HandleMaxTickRate: a fight with a frame limiter shows up as "
		     "judder, not as an error."));
}

static FAutoConsoleCommandWithOutputDevice GFPMReflexReportCmd(
	TEXT("FPM.Reflex.Report"),
	TEXT("Reflex: the mode requested, the mode applied, and WHICH route reached the plugin."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMReflexMode::ReportNow();
	}));
