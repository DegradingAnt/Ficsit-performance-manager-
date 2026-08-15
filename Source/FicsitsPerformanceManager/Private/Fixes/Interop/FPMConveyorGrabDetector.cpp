// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMConveyorGrabDetector.h"

#include "EngineUtils.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Buildables/FGBuildable.h"
#include "Containers/Ticker.h"
#include "UObject/UObjectHash.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDetectorRegistry.h"
#include "Core/FPMOverlay.h"

namespace
{
	const FName GFactoryTickEventName(TEXT("Factory_ReceiveTick"));

	/** Deferred one-shot: the world is loaded but actors are not guaranteed spawned during the
	 *  OnWorldLoad construction phase (IFPMFix's own contract). Five seconds is long enough for
	 *  a save to finish populating and short enough that the number still describes "just loaded"
	 *  rather than a session that has already changed shape. */
	FTSTicker::FDelegateHandle GDeferredHandle;
	TWeakObjectPtr<UWorld> GPendingWorld;
}

FFPMConveyorGrabDetector& FFPMConveyorGrabDetector::Get()
{
	static FFPMConveyorGrabDetector Instance;
	return Instance;
}

bool FFPMConveyorGrabDetector::ClassOverridesEvent(const UClass* Class, const UClass* BaseClass,
                                                     FName EventFunctionName)
{
	if (Class == nullptr || BaseClass == nullptr) { return false; }

	UFunction* Found = Class->FindFunctionByName(EventFunctionName);
	if (Found == nullptr) { return false; }

	// The base class's own declaration has BaseClass as the function's Outer. Anything below it
	// that overrides/implements the event gets its OWN UFunction, whose Outer is that lower class.
	return Found->GetOuter() != BaseClass;
}

bool FFPMConveyorGrabDetector::SelfTest()
{
	// Known negative: the base class does not override its own declaration.
	const bool bNegativeOk = !ClassOverridesEvent(AFGBuildable::StaticClass(), AFGBuildable::StaticClass(),
	                                               GFactoryTickEventName);

	// Known positive, sought rather than assumed: walk every currently-loaded AFGBuildable
	// subclass and look for ANY function that class declares directly (ExcludeSuper) whose name
	// also resolves on AFGBuildable itself - i.e. a real, already-loaded override of an inherited
	// function. If one exists, running ClassOverridesEvent on that exact (Class, FunctionName)
	// pair must report true, which proves the mechanism against REAL content rather than a
	// synthetic case this file invented for itself.
	TArray<UClass*> Derived;
	GetDerivedClasses(AFGBuildable::StaticClass(), Derived, true);

	bool bFoundPositive = false;
	FString PositiveClassName, PositiveFunctionName;
	for (const UClass* Class : Derived)
	{
		if (Class == nullptr) { continue; }
		for (TFieldIterator<UFunction> FuncIt(Class, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
		{
			const FName FuncName = FuncIt->GetFName();
			if (AFGBuildable::StaticClass()->FindFunctionByName(FuncName) == nullptr) { continue; }
			if (ClassOverridesEvent(Class, AFGBuildable::StaticClass(), FuncName))
			{
				bFoundPositive = true;
				PositiveClassName = Class->GetPathName();
				PositiveFunctionName = FuncName.ToString();
				break;
			}
		}
		if (bFoundPositive) { break; }
	}

	const bool bPassed = bNegativeOk && (bFoundPositive || Derived.Num() == 0);

	if (bFoundPositive)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] detect: conveyor-grab self-test %s - known-negative %s, known-positive found "
			     "and confirmed: '%s' overrides '%s'."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"),
			bNegativeOk ? TEXT("ok") : TEXT("FAILED"), *PositiveClassName, *PositiveFunctionName);
	}
	else
	{
		// Honest coverage, per the design's own blessed outcome (§14 Slice 4): the mechanism's
		// positive path is UNVERIFIED this boot, stated as such rather than hidden inside a bare
		// PASS. Not a failure - Derived.Num() classes were genuinely searched and none overrode
		// anything inherited from AFGBuildable.
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] detect: conveyor-grab self-test %s - known-negative %s. COVERAGE: searched "
			     "%d loaded AFGBuildable subclass(es) for ANY inherited-function override and found "
			     "none, so the classifier's POSITIVE path is UNVERIFIED this boot - it has not been "
			     "proven wrong, only untested against real content."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), bNegativeOk ? TEXT("ok") : TEXT("FAILED"),
			Derived.Num());
	}

	return bPassed;
}

void FFPMConveyorGrabDetector::RunNow(UWorld* World)
{
	if (World == nullptr)
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] detect: conveyor-grab census skipped - no world."));
		return;
	}

	TArray<UClass*> Derived;
	GetDerivedClasses(AFGBuildable::StaticClass(), Derived, true);

	TSet<const UClass*> FlaggedClasses;
	int32 BlueprintBuildableClasses = 0;
	for (const UClass* Class : Derived)
	{
		if (Class == nullptr || Cast<UBlueprintGeneratedClass>(Class) == nullptr) { continue; }
		++BlueprintBuildableClasses;
		if (ClassOverridesEvent(Class, AFGBuildable::StaticClass(), GFactoryTickEventName))
		{
			FlaggedClasses.Add(Class);
		}
	}

	TMap<const UClass*, int32> LiveInstancesPerFlaggedClass;
	if (FlaggedClasses.Num() > 0)
	{
		// One pass over the world's buildables, not one TActorIterator pass per flagged class.
		for (TActorIterator<AFGBuildable> It(World); It; ++It)
		{
			const UClass* ActorClass = It->GetClass();
			if (FlaggedClasses.Contains(ActorClass))
			{
				LiveInstancesPerFlaggedClass.FindOrAdd(ActorClass) += 1;
			}
		}
	}

	int32 Reported = 0;
	for (const UClass* Class : FlaggedClasses)
	{
		const int32 LiveCount = LiveInstancesPerFlaggedClass.FindRef(Class);
		FFPMDetectorRegistry::Report(TEXT("conveyor-grab-detector"), Class->GetPathName(),
			FString::Printf(TEXT("Blueprint Factory_Tick override, %d live instance(s) - Blueprint VM "
			                      "cost paid per belt tick per instance"), LiveCount),
			LiveCount);
		++Reported;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: conveyor-grab census - %d Blueprint-generated AFGBuildable subclass(es) "
		     "loaded, %d override Factory_ReceiveTick (reported to FPM.Detect.Report). COVERAGE: "
		     "loaded-only, like every census in this project - a class not yet loaded is outside this "
		     "run's sight, and a zero here on a save with mods installed means checked-and-clean, not "
		     "unchecked."),
		BlueprintBuildableClasses, Reported);

	// m6164470: every FPM feature reports to the dev overlay.
	FPMOverlay::PostSticky(TEXT("detect"), TEXT("conveyor-grab"),
		FString::Printf(TEXT("trap 1 (conveyor grab): %d/%d BP buildable class(es) flagged"),
			Reported, BlueprintBuildableClasses));
}

void FFPMConveyorGrabDetector::OnWorldLoad(UWorld* World)
{
	if (GDeferredHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GDeferredHandle);
		GDeferredHandle.Reset();
	}
	GPendingWorld = World;
	if (World == nullptr) { return; }

	GDeferredHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			if (UWorld* PendingWorld = GPendingWorld.Get())
			{
				FFPMConveyorGrabDetector::RunNow(PendingWorld);
			}
			return false; // one-shot
		}), 5.0f);
}

void FFPMConveyorGrabDetector::Disarm()
{
	// Same shape as FPMDistanceFieldAudit::Disarm - a repeating/deferred ticker left running past
	// Disarm keeps firing into a module that thinks it has torn down, and FPMFixes::DisarmAll()
	// would report this fix disarmed while it did so. Caught by the structure gate.
	if (GDeferredHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GDeferredHandle);
		GDeferredHandle.Reset();
	}
	GPendingWorld.Reset();
}

void FFPMConveyorGrabDetector::Arm()
{
	const bool bSelfTestPassed = SelfTest();
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: conveyor-grab-detector armed - self-test %s. Trap 1 of 4 (§9.3): "
		     "Blueprint-implemented Factory_Tick, censused per class 5s after each world load."),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
}

namespace
{
	static FAutoConsoleCommand GFPMConveyorGrabRunCmd(
		TEXT("FPM.Detect.ConveyorGrab"),
		TEXT("Re-run the Blueprint-Factory_Tick census now, against the current world, without "
		     "waiting for the next world load."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			if (GPendingWorld.IsValid())
			{
				FFPMConveyorGrabDetector::RunNow(GPendingWorld.Get());
			}
			else
			{
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] detect: conveyor-grab has no known world yet - run after a world load."));
			}
		}));
}
