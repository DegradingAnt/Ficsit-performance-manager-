// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMWidgetTickDetector.h"

#include "Blueprint/UserWidget.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDetectorRegistry.h"
#include "Core/FPMOverlay.h"

FFPMWidgetTickDetector& FFPMWidgetTickDetector::Get()
{
	static FFPMWidgetTickDetector Instance;
	return Instance;
}

bool FFPMWidgetTickDetector::SelfTest()
{
	const UUserWidget* BaseCdo = GetDefault<UUserWidget>();
	const bool bNegativeOk = BaseCdo != nullptr
		&& BaseCdo->GetDesiredTickFrequency() == EWidgetTickFrequency::Never;

	TArray<UClass*> Derived;
	GetDerivedClasses(UUserWidget::StaticClass(), Derived, true);

	bool bFoundPositive = false;
	FString PositiveClassName;
	for (const UClass* Class : Derived)
	{
		if (Class == nullptr) { continue; }
		const UUserWidget* Cdo = Class->GetDefaultObject<UUserWidget>();
		if (Cdo != nullptr && Cdo->GetDesiredTickFrequency() == EWidgetTickFrequency::Auto)
		{
			bFoundPositive = true;
			PositiveClassName = Class->GetPathName();
			break;
		}
	}

	const bool bPassed = bNegativeOk && (bFoundPositive || Derived.Num() == 0);

	if (bFoundPositive)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] detect: widget-tick self-test %s - base UUserWidget CDO reads Never "
			     "(known-negative %s), known-positive found: '%s' reads Auto."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), bNegativeOk ? TEXT("ok") : TEXT("FAILED"),
			*PositiveClassName);
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] detect: widget-tick self-test %s - base UUserWidget CDO reads Never "
			     "(known-negative %s). COVERAGE: %d UUserWidget subclass(es) loaded at self-test "
			     "time, none read Auto - the read path's POSITIVE case is UNVERIFIED this boot "
			     "(expected before any UI exists; Arm() runs from StartupModule). RunNow() after a "
			     "world load, once the HUD is up, is where a real positive is expected."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), bNegativeOk ? TEXT("ok") : TEXT("FAILED"),
			Derived.Num());
	}

	return bPassed;
}

void FFPMWidgetTickDetector::RunNow()
{
	TMap<const UClass*, int32> AutoInstancesPerClass;
	int32 InstancesConsidered = 0;

	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		UUserWidget* Widget = *It;
		if (Widget == nullptr || Widget->IsTemplate() || Widget->HasAnyFlags(RF_ClassDefaultObject))
		{
			continue; // CDOs and archetypes are not live cost, only real instances are
		}
		++InstancesConsidered;
		if (Widget->GetDesiredTickFrequency() == EWidgetTickFrequency::Auto)
		{
			AutoInstancesPerClass.FindOrAdd(Widget->GetClass()) += 1;
		}
	}

	int32 FlaggedClasses = 0;
	int32 FlaggedInstances = 0;
	for (const TPair<const UClass*, int32>& Entry : AutoInstancesPerClass)
	{
		FFPMDetectorRegistry::Report(TEXT("widget-tick-detector"), Entry.Key->GetPathName(),
			FString::Printf(TEXT("%d live instance(s) with EWidgetTickFrequency::Auto - Blueprint tick "
			                      "event, a property binding, an animation, or a native-inherited tick; "
			                      "the enum alone cannot say which"), Entry.Value),
			Entry.Value);
		++FlaggedClasses;
		FlaggedInstances += Entry.Value;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: widget-tick census - %d live UUserWidget instance(s) considered, %d "
		     "across %d class(es) read Auto (reported to FPM.Detect.Report). COVERAGE: this run's "
		     "own moment only - a widget created after this call is invisible to it until the next "
		     "run."),
		InstancesConsidered, FlaggedInstances, FlaggedClasses);

	// m6164470: every FPM feature reports to the dev overlay.
	FPMOverlay::PostSticky(TEXT("detect"), TEXT("widget-tick"),
		FString::Printf(TEXT("trap 3 (widget tick): %d Auto instance(s) across %d class(es) of %d"),
			FlaggedInstances, FlaggedClasses, InstancesConsidered));
}

void FFPMWidgetTickDetector::OnWorldLoad(UWorld* World)
{
	if (World != nullptr)
	{
		RunNow();
	}
}

void FFPMWidgetTickDetector::Arm()
{
	const bool bSelfTestPassed = SelfTest();
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: widget-tick-detector armed - self-test %s. Trap 3 of 4 (§9.3): live "
		     "widgets reading EWidgetTickFrequency::Auto, censused each world load."),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
}

namespace
{
	static FAutoConsoleCommand GFPMWidgetTickRunCmd(
		TEXT("FPM.Detect.WidgetTick"),
		TEXT("Re-run the widget tick census now, against whatever widgets currently exist."),
		FConsoleCommandDelegate::CreateStatic([]() { FFPMWidgetTickDetector::RunNow(); }));
}
