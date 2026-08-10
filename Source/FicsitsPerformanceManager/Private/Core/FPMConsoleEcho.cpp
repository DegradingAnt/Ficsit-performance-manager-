// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMConsoleEcho.h"

#include "Misc/OutputDeviceRedirector.h"

FPMScopedConsoleEcho::FPMScopedConsoleEcho(FOutputDevice* InAr)
{
	/*
	 * GLog is the process-wide redirector; adding a device makes it forward every subsequent line there
	 * too. Guarded on both pointers because a console command can legitimately be invoked with no device
	 * (the internal callers), and because GLog is null in some very early and very late phases.
	 */
	if (InAr != nullptr && GLog != nullptr)
	{
		Attached = InAr;
		GLog->AddOutputDevice(Attached);
	}
}

FPMScopedConsoleEcho::~FPMScopedConsoleEcho()
{
	/*
	 * ⚠ REMOVAL IS NOT OPTIONAL AND IT IS THE WHOLE REASON THIS IS RAII. The console's output device
	 * lives only for the duration of the command call. Leaving it registered on GLog would leave the
	 * redirector holding a dangling pointer and writing into it on the next log line from any thread —
	 * a use-after-free whose symptom would be a crash somewhere else entirely, long after the command.
	 */
	if (Attached != nullptr && GLog != nullptr)
	{
		GLog->RemoveOutputDevice(Attached);
		Attached = nullptr;
	}
}
