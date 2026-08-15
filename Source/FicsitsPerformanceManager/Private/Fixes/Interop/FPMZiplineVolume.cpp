// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMZiplineVolume.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDiag.h"
#include "Core/FPMHookLedger.h"

#include "Equipment/FGEquipmentZipline.h"
#include "AkGameplayStatics.h"

#include "HAL/IConsoleManager.h"

namespace
{
	/**
	 * FPM'S OWN CVAR, and that is what makes it residue-free.
	 *
	 * It is not `US_ZiplineVolume` (FGGameUserSettings-backed, re-applied every boot with or without the
	 * mod — permanent residue) and it is not written to any ini. We declare it, we read it, and it is
	 * gone when the module unloads. Phase 4's settings surface will drive this value; nothing in this
	 * file changes when it does.
	 */
	TAutoConsoleVariable<float> CVarZiplineVolume(
		TEXT("FPM.Zipline.Volume"), 1.0f,
		TEXT("Zipline output-bus volume, 0.0 to 1.0. 1.0 = vanilla and writes nothing on a fresh "
		     "session. Applied when the zipline is EQUIPPED - the game ships no zipline RTPC, so a "
		     "change mid-ride lands on the next equip."),
		ECVF_Default);

	/**
	 * ★ THREADING CONTRACT FOR THESE COUNTERS, VERIFIED AGAINST THE HEADERS, NOT GUESSED.
	 *
	 * Plain int32 and bool here, not std::atomic like the sibling FPMWwiseServerGate.cpp counter. That
	 * is correct because AFGEquipmentZipline::Equip only ever runs on the game thread. The call chain
	 * is fixed by the engine, not by this mod. AFGCharacterPlayer::EquipEquipment is a plain
	 * BlueprintCallable function documented "must be called on the owning client"
	 * (FGCharacterPlayer.h:503-505), and AFGCharacterPlayer::Server_EquipEquipment is a Reliable Server
	 * RPC (FGCharacterPlayer.h:1550-1552). Unreal Engine dispatches both BlueprintCallable gameplay
	 * calls and Server RPCs on the game thread. No async task and no worker thread reach this path. If
	 * a future FactoryGame build ever moves equip handling off the game thread, update this comment
	 * alongside the counters below.
	 */
	int32 GFPMZipEquips = 0;
	int32 GFPMZipWrites = 0;

	/**
	 * ★ THE BUG FIX, AND IT IS THE WHOLE REASON THIS IS NOT A COPY.
	 *
	 * Once we have written a non-vanilla volume to an actor's output bus, that value STAYS on the bus.
	 * The old implementation returned early whenever the configured value was >= 0.999, so setting the
	 * slider back to 1.0 wrote nothing and the bus kept the quiet value — vanilla became unreachable
	 * until a restart. Remembering that we have written means we can write 1.0 deliberately and undo
	 * ourselves.
	 */
	bool bGFPMZipHasWritten = false;

	/** Last value we logged, so a constantly-equipped zipline does not become the noisiest line in the log. */
	float GFPMZipLastLogged = -1.f;
}

FFPMZiplineVolume& FFPMZiplineVolume::Get()
{
	static FFPMZiplineVolume Instance;
	return Instance;
}

void FFPMZiplineVolume::GetCounts(int32& OutEquips, int32& OutWrites)
{
	OutEquips = GFPMZipEquips;
	OutWrites = GFPMZipWrites;
}

void FFPMZiplineVolume::Arm()
{
	/*
	 * SUBSCRIBE_METHOD_VIRTUAL because Equip IS virtual — `FGEquipmentZipline.h`:
	 *   virtual void Equip(class AFGCharacterPlayer* character) override
	 * Read from the real header rather than remembered: a mismatched descriptor binds the wrong overload
	 * silently. Equip is also a large function (full equipment setup), so it is a safe funchook target —
	 * the v0.3.0 corruption came from patching a tiny one.
	 */
	AFGEquipmentZipline* Sample = GetMutableDefault<AFGEquipmentZipline>();

	auto OnEquip = [](auto& Scope, AFGEquipmentZipline* Self, AFGCharacterPlayer* Character)
	{
		if (!Self) { return; }

		++GFPMZipEquips;

		const float Wanted = FMath::Clamp(CVarZiplineVolume.GetValueOnGameThread(), 0.f, 1.f);
		const bool bVanilla = Wanted >= 0.999f;

		/*
		 * ⚠ THE CONDITION THE OLD VERSION GOT WRONG.
		 *
		 * Skip ONLY when we are at vanilla AND have never written. If we have written before, we must
		 * write again even at 1.0 — otherwise our own earlier value is stuck on the bus and "set it back
		 * to normal" silently does nothing. Writing 1.0 is how we undo ourselves.
		 */
		if (bVanilla && !bGFPMZipHasWritten) { return; }

		UAkGameplayStatics::SetOutputBusVolume(Wanted, Self);
		++GFPMZipWrites;
		bGFPMZipHasWritten = true;

		// Once per VALUE, not once per equip. Ziplines are equipped constantly.
		if (!FMath::IsNearlyEqual(GFPMZipLastLogged, Wanted, 0.001f)
			&& FPMDiag::IsOn(FPMDiag::EChannel::Zipline))
		{
			GFPMZipLastLogged = Wanted;
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] zipline: output volume %.2f (1.00 = vanilla) after %d equip(s). Applied on "
				     "equip; a change mid-ride lands on the next equip - the game ships no zipline RTPC "
				     "to drive it live."),
				Wanted, GFPMZipEquips);
		}
	};

	EquipHandle = FPM_SUBSCRIBE_VIRTUAL("zipline-volume", AFGEquipmentZipline::Equip, Sample, OnEquip);

	if (EquipHandle.IsValid())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] zipline volume ARMED - FPM.Zipline.Volume (default 1.0 = vanilla, writes nothing "
			     "until changed). Set per-actor on equip via the Wwise output bus; vanilla ships no "
			     "per-zipline slider and no RTPC. Returning the value to 1.0 now genuinely restores vanilla, "
			     "which the old implementation could not do."));
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] zipline volume NOT armed - hook install FAILED on AFGEquipmentZipline::Equip. "
			     "FPM.Zipline.Volume will NOT apply this session."));
	}
}

void FFPMZiplineVolume::Disarm()
{
	/*
	 * UNSUBSCRIBE_METHOD is correct for a _VIRTUAL subscribe: both drive the same
	 * HookInvoker<decltype(&M), &M>, and RemoveHandler clears the BEFORE and AFTER maps
	 * alike, uninstalling the detour once both are empty (NativeHookManager.h:359-378).
	 *
	 * ⚠ Guarded on IsValid() because the editor path installs nothing and returns an
	 * invalid handle; RemoveHandler would then walk maps SML never allocated.
	 */
	if (EquipHandle.IsValid())
	{
		UNSUBSCRIBE_METHOD(AFGEquipmentZipline::Equip, EquipHandle);
		EquipHandle.Reset();
	}
}

void FFPMZiplineVolume::LogReport(FOutputDevice* Ar)
{
	int32 Equips = 0, Writes = 0;
	GetCounts(Equips, Writes);

	const FString Line = FString::Printf(
		TEXT("[FPM] zipline volume: %d equip(s) seen · %d write(s) issued. A zero write count with a "
		     "non-zero equip count means the lever is sitting at vanilla - which is the correct default, "
		     "not a fault."),
		Equips, Writes);

	if (Ar != nullptr)
	{
		Ar->Log(Line);
	}
	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("%s"), *Line);
}

/*
 * `FPM.Zipline.Report` — takes the output device so it prints in the console she is looking at as well
 * as the log. A Display-level UE_LOG alone does not echo to the in-game console, and a command that
 * answers somewhere the operator is not looking reads as a broken command.
 */
static FAutoConsoleCommandWithOutputDevice GFPMZiplineReportCmd(
	TEXT("FPM.Zipline.Report"),
	TEXT("Print how many zipline equips this fix has seen, and how many non-vanilla volume writes it "
	     "has issued, this session."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FFPMZiplineVolume::LogReport(&Ar);
	}));
