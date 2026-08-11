// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/ModFeatures/FPMReflexMode.h"

#include "FicsitsPerformanceManager.h"
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
	 * The cvar names the spec explicitly flags as UNVERIFIED. They are TRIED, never assumed: if
	 * FindConsoleVariable returns null the name does not exist on this build and the reflection route
	 * takes over. Ordered most-specific first.
	 */
	const TCHAR* GFPMReflexModeCVarCandidates[] =
	{
		TEXT("t.Streamline.Reflex.Mode"),
		TEXT("t.Streamline.Reflex.Enable"),
	};

	/** Where the Streamline Reflex blueprint library lives, if the plugin is mounted. */
	const TCHAR* GFPMReflexClassPath = TEXT("/Script/StreamlineReflex.StreamlineLibraryReflex");
	const TCHAR* GFPMReflexFunctionName = TEXT("SetReflexMode");

	IConsoleVariable* FindReflexCVar(FString& OutName)
	{
		for (const TCHAR* Candidate : GFPMReflexModeCVarCandidates)
		{
			if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Candidate))
			{
				OutName = Candidate;
				return V;
			}
		}
		return nullptr;
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
		     "a baseline. ⚠ Neither the cvar names nor the Streamline headers are verified on this "
		     "build, so this fix DISCOVERS its route and FPM.Reflex.Report names which one it found."));
}

void FFPMReflexMode::Disarm()
{
	/*
	 * ⚠ NOTHING IS PUT BACK, AND THAT IS STATED RATHER THAN SILENT.
	 *
	 * There is no "previous Reflex mode" to restore: the game does not expose what it had, and reading
	 * the cvar back would read OUR value if the cvar route was the one that worked. Writing 0 on disarm
	 * would not be a restore either — it would be turning Reflex OFF for a player whose game may have
	 * had it on before FPM ever loaded, which is a change disguised as an undo.
	 *
	 * So disarm stops FUTURE writes only. Zero residue is untouched: this writes no file and no ini, and
	 * the cvar route goes through the console variable the plugin itself owns.
	 */
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
	FString CVarName;
	if (IConsoleVariable* V = FindReflexCVar(CVarName))
	{
		V->Set(Want, ECVF_SetByCode);
		AppliedMode = Want;
		RouteFound = FString::Printf(TEXT("cvar %s"), *CVarName);

		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] reflex: set mode %d via %s at %s. ⚠ This CONFIRMS a cvar name the banked spec "
			     "listed as unverified - worth writing back into FPG-REBUILD-SPEC.md:76."),
			Want, *CVarName, Moment);
		return;
	}

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

static FAutoConsoleCommand GFPMReflexReportCmd(
	TEXT("FPM.Reflex.Report"),
	TEXT("Reflex: the mode requested, the mode applied, and WHICH route reached the plugin."),
	FConsoleCommandDelegate::CreateStatic(&FFPMReflexMode::ReportNow));
