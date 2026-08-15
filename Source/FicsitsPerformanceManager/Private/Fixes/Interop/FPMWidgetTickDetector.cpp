// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMWidgetTickDetector.h"

#include "Blueprint/UserWidget.h"
// UWidget::GetCachedWidget, the per-instance route to the engine's own tick verdict.
#include "Components/Widget.h"
#include "UObject/UObjectIterator.h"
// FAutoConsoleCommand for FPM.Detect.WidgetTick. Declared rather than left to arrive transitively
// through the UMG and Slate headers above. The version before this one did not declare it and did
// compile, so it does arrive from somewhere; WHICH header is unverified, and leaning on an include
// nobody has named is the IWYU anti-pattern.
#include "HAL/IConsoleManager.h"
// SWidget::GetCanTick / SetCanTick, the flag that verdict is written to and read from.
#include "Widgets/SWidget.h"
// SBox, the throwaway probe the boot self-test forces both ways. Slate is already a public dependency
// of this module (Build.cs: "SlateCore", "Slate", "UMG").
#include "Widgets/Layout/SBox.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDetectorRegistry.h"
#include "Core/FPMOverlay.h"

namespace
{
	/** What the engine has decided about one live widget instance. Four states, not two. */
	enum class EFPMWidgetTickVerdict : uint8
	{
		/** The engine computed a tick for this instance. This is the cost the trap is about. */
		Ticking,
		/** `TickFrequency` is Auto, the widget is constructed, and the engine computed no tick. */
		NotTicking,
		/** A human set `TickFrequency` to Never in the class defaults. */
		OptedOut,
		/** No Slate widget yet, so no decision has been made. A coverage hole, not a verdict. */
		NotConstructed,
	};

	/**
	 * ★ THE CLASSIFIER, IN ONE PLACE, so the census and the self-test cannot drift apart. A self-test
	 * that exercises a second copy of the logic proves only that two copies agree.
	 *
	 * The `Never` check comes first and is NOT redundant with the flag read below it: `UpdateCanTick`
	 * leaves the flag false for a `Never` widget, so both routes agree on "not ticking", but only this
	 * branch can say WHY. The count it produces is the receipt for the whole rebuild - it is how many
	 * widgets the old `Auto` reading actually excluded.
	 */
	EFPMWidgetTickVerdict ClassifyWidget(const UUserWidget* Widget)
	{
		if (Widget == nullptr) { return EFPMWidgetTickVerdict::NotConstructed; }

		if (Widget->GetDesiredTickFrequency() == EWidgetTickFrequency::Never)
		{
			return EFPMWidgetTickVerdict::OptedOut;
		}

		const TSharedPtr<SWidget> Slate = Widget->GetCachedWidget();
		if (!Slate.IsValid()) { return EFPMWidgetTickVerdict::NotConstructed; }

		return Slate->GetCanTick() ? EFPMWidgetTickVerdict::Ticking : EFPMWidgetTickVerdict::NotTicking;
	}
}

FFPMWidgetTickDetector& FFPMWidgetTickDetector::Get()
{
	static FFPMWidgetTickDetector Instance;
	return Instance;
}

bool FFPMWidgetTickDetector::SelfTest()
{
	/*
	 * ★ THE SLATE READ PATH, FORCED BOTH WAYS. Deterministic, needs no world, no viewport and no UI,
	 * which is what lets it run at Arm() from StartupModule. The probe is not registered with any
	 * invalidation root, so `SetCanTick` is a plain flag write here (`WidgetProxy.cpp:387` guards the
	 * only side effect on `IsValid(Widget)`).
	 */
	const TSharedRef<SBox> Probe = SNew(SBox);

	Probe->SetCanTick(false);
	const bool bNegativeOk = !Probe->GetCanTick();

	Probe->SetCanTick(true);
	const bool bPositiveOk = Probe->GetCanTick();

	/*
	 * ★ THE CLASSIFIER'S OWN KNOWN-NEGATIVE, and it is the assertion aimed straight at the defect this
	 * file was rebuilt for. The base `UUserWidget` CDO has `TickFrequency == Auto` and no Slate widget.
	 * A classifier that reads the authored enum calls that "ticking"; the correct one calls it
	 * "not constructed", because nothing has decided anything about it.
	 */
	const UUserWidget* BaseCdo = GetDefault<UUserWidget>();
	const bool bClassifierOk = BaseCdo != nullptr
		&& ClassifyWidget(BaseCdo) == EFPMWidgetTickVerdict::NotConstructed;

	const bool bPassed = bNegativeOk && bPositiveOk && bClassifierOk;

	const FString Msg = FString::Printf(
		TEXT("[FPM] detect: widget-tick self-test %s - Slate read path known-negative %s, known-positive "
		     "%s; classifier known-negative %s (the base UUserWidget CDO reads TickFrequency Auto and has "
		     "no Slate widget, so it must classify as not-constructed rather than ticking). COVERAGE: this "
		     "proves the READ PATH and the CLASSIFIER. Whether any real widget ticks is what RunNow() "
		     "measures, once a world and a HUD exist."),
		bPassed ? TEXT("PASSED") : TEXT("FAILED"),
		bNegativeOk   ? TEXT("ok") : TEXT("FAILED"),
		bPositiveOk   ? TEXT("ok") : TEXT("FAILED"),
		bClassifierOk ? TEXT("ok") : TEXT("FAILED"));

	if (bPassed)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Msg);
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("%s"), *Msg);
	}

	return bPassed;
}

void FFPMWidgetTickDetector::RunNow()
{
	TMap<const UClass*, int32> TickingInstancesPerClass;
	int32 InstancesConsidered  = 0;
	int32 InstancesOptedOut    = 0;
	int32 InstancesNotBuilt    = 0;
	int32 InstancesNotTicking  = 0;

	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		UUserWidget* Widget = *It;
		if (Widget == nullptr || Widget->IsTemplate() || Widget->HasAnyFlags(RF_ClassDefaultObject))
		{
			continue; // CDOs and archetypes are not live cost, only real instances are
		}
		++InstancesConsidered;

		switch (ClassifyWidget(Widget))
		{
		case EFPMWidgetTickVerdict::Ticking:
			TickingInstancesPerClass.FindOrAdd(Widget->GetClass()) += 1;
			break;
		case EFPMWidgetTickVerdict::OptedOut:
			++InstancesOptedOut;
			break;
		case EFPMWidgetTickVerdict::NotConstructed:
			++InstancesNotBuilt;
			break;
		case EFPMWidgetTickVerdict::NotTicking:
			++InstancesNotTicking;
			break;
		}
	}

	int32 FlaggedClasses   = 0;
	int32 FlaggedInstances = 0;
	for (const TPair<const UClass*, int32>& Entry : TickingInstancesPerClass)
	{
		FFPMDetectorRegistry::Report(TEXT("widget-tick-detector"), Entry.Key->GetPathName(),
			FString::Printf(TEXT("%d live instance(s) the engine has computed a per-frame tick for "
			                      "(SWidget NeedsTick, set by UUserWidget::UpdateCanTick). The cause is one "
			                      "of six: a Blueprint Tick event, a property binding, an active animation, "
			                      "a queued animation transition, a latent action, or a native C++ base. The "
			                      "flag alone cannot say which"), Entry.Value),
			Entry.Value);
		++FlaggedClasses;
		FlaggedInstances += Entry.Value;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: widget-tick census - %d live UUserWidget instance(s) considered: %d TICKING "
		     "across %d class(es) (reported to FPM.Detect.Report), %d constructed and not ticking, %d opted "
		     "out with TickFrequency Never, %d not constructed yet. COVERAGE: the last group is a hole, not "
		     "a verdict - those widgets have no Slate widget, so the engine has decided nothing about them. "
		     "And this is this run's own moment only: a widget created after this call is invisible to it "
		     "until the next run."),
		InstancesConsidered, FlaggedInstances, FlaggedClasses, InstancesNotTicking, InstancesOptedOut,
		InstancesNotBuilt);

	// m6164470: every FPM feature reports to the dev overlay.
	FPMOverlay::PostSticky(TEXT("detect"), TEXT("widget-tick"),
		FString::Printf(TEXT("trap 3 (widget tick): %d ticking instance(s) across %d class(es) of %d live "
		                     "(%d idle, %d opted out, %d not built)"),
			FlaggedInstances, FlaggedClasses, InstancesConsidered, InstancesNotTicking, InstancesOptedOut,
			InstancesNotBuilt));
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
		TEXT("[FPM] detect: widget-tick-detector armed - self-test %s. Trap 3 of 4 (§9.3): live widgets the "
		     "engine has computed a tick for, censused each world load."),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
}

namespace
{
	static FAutoConsoleCommand GFPMWidgetTickRunCmd(
		TEXT("FPM.Detect.WidgetTick"),
		TEXT("Re-run the widget tick census now, against whatever widgets currently exist."),
		FConsoleCommandDelegate::CreateStatic([]() { FFPMWidgetTickDetector::RunNow(); }));
}
