// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMMasterSwitch.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMCVarWriter.h"
#include "Core/FPMFixContract.h"

#include "HAL/IConsoleManager.h"

namespace
{
	static TAutoConsoleVariable<int32> CVarFPMEnabled(
		TEXT("FPM.Enabled"), 1,
		TEXT("Master switch. 1 = on (default), 0 = off.\n"
		     "OFF disarms every fix AND RELEASES every console variable FPM wrote, restoring each one's "
		     "prior value and prior SetBy priority - it does not merely stop writing new ones.\n"
		     "ON re-arms. \xE2\x9A\xA0 A fix whose work happens at world load comes back armed but INERT until "
		     "the next world load; re-arming does not replay one."),
		ECVF_Default);

	/**
	 * What the switch last ACTED on.
	 *
	 * ⚠ NOT a cache of the cvar — a record of the last transition performed. The sink fires on every
	 * `set`, including one that writes the value it already had, and acting on that would call
	 * `DisarmAll` twice in a row. The second call would find an empty armed list and report a disarm
	 * that disarmed nothing, which is a false line in a log people read to decide whether FPM is
	 * running.
	 */
	bool bGFPMLastAppliedEnabled = true;

	bool bGFPMSwitchInstalled = false;

	void ApplyMasterSwitch()
	{
		const bool bWanted = CVarFPMEnabled.GetValueOnGameThread() != 0;
		if (bWanted == bGFPMLastAppliedEnabled)
		{
			return;
		}
		bGFPMLastAppliedEnabled = bWanted;

		if (!bWanted)
		{
			/*
			 * ★ ORDER MATTERS AND THIS IS THE ORDER.
			 *
			 * Disarm FIRST, release SECOND. A fix that re-asserts a console variable from a hook — the
			 * glass quality hold is exactly that, it re-applies on a settings apply — would otherwise
			 * write its value straight back in the window between the release and its own teardown, and
			 * the release would silently accomplish nothing on precisely the levers that fight hardest
			 * to stay set.
			 */
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] MASTER SWITCH: OFF. Disarming every fix, then releasing every console "
				     "variable FPM wrote."));

			FPMFixes::DisarmAll();
			FPMCVarWriter::Get().ReleaseAll(TEXT("FPM.Enabled 0"));

			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] MASTER SWITCH: OFF complete. FPM.Enabled 1 turns it back on. \xE2\x9A\xA0 Fixes "
				     "whose work happens at world load will come back armed but inert until the next "
				     "one."));
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] MASTER SWITCH: ON. Re-arming."));

			FPMFixes::RearmAll();
		}
	}
}

void FPMMasterSwitch::Install()
{
	if (bGFPMSwitchInstalled) { return; }
	bGFPMSwitchInstalled = true;

	/*
	 * ⚠ SEEDED FROM THE LIVE CVAR RATHER THAN ASSUMED TRUE.
	 *
	 * `Install()` runs at the end of StartupModule, by which point everything has already armed — so the
	 * real state is "on". But the cvar can be set from the command line or an ini BEFORE this point, and
	 * seeding a hardcoded `true` against a cvar that already reads 0 would leave the switch believing
	 * FPM is on while the user asked for off, and the first `set` back to 0 would be swallowed as a
	 * no-op transition.
	 */
	bGFPMLastAppliedEnabled = true;
	CVarFPMEnabled.AsVariable()->SetOnChangedCallback(
		FConsoleVariableDelegate::CreateStatic([](IConsoleVariable*) { ApplyMasterSwitch(); }));

	// If the launch environment already asked for OFF, honour it now rather than at the first toggle.
	ApplyMasterSwitch();

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] master switch installed: FPM.Enabled %d. OFF disarms every fix and RELEASES every "
		     "console variable FPM wrote - it does not just stop writing."),
		CVarFPMEnabled.GetValueOnGameThread());
}

bool FPMMasterSwitch::IsEnabled()
{
	// The live cvar, never a cached copy: a caller asking "is FPM on" during a transition must not be
	// told what the switch last ACTED on.
	return CVarFPMEnabled.GetValueOnGameThread() != 0;
}
