// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMDiag.h"

#include "FicsitsPerformanceManager.h"

#include "HAL/IConsoleManager.h"

/*
 * THE MASTER SWITCH. -1 means "defer to the per-channel value", which is the default so that turning a
 * single channel up does not require touching this one. Set it to 0 to silence everything at once —
 * the case that matters when reading a log for something else.
 */
static TAutoConsoleVariable<int32> CVarDiagMaster(
	TEXT("FPM.Diag"), -1,
	TEXT("Master FPM diagnostics level. -1 = per-channel (default), 0 = silence ALL, 1 = on, 2 = verbose. "
	     "Never changes what a fix DOES, only what it prints."),
	ECVF_Default);

/*
 * ONE CVAR PER CHANNEL, IN THE SAME ORDER AS FPMDiag::EChannel. The order is load-bearing — the lookup
 * below indexes this table by the enum — so a new channel goes in BOTH places or the compile-time size
 * check underneath fires.
 */
static TAutoConsoleVariable<int32> CVarDiagSchematic(
	TEXT("FPM.Diag.Schematic"), 1,
	TEXT("Schematic access probe. 0 = silent, 1 = anomalies + throttled totals, 2 = every call."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagHologram(
	TEXT("FPM.Diag.Hologram"), 1,
	TEXT("Replicated build-preview repair. 0 = silent, 1 = throttled totals + every unrepairable class, "
	     "2 = every hologram seen."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagInventory(
	TEXT("FPM.Diag.Inventory"), 1,
	TEXT("Inventory init repair. 0 = silent, 1 = throttled totals + every refusal, 2 = every component."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagClone(
	TEXT("FPM.Diag.Clone"), 1,
	TEXT("Join-time player-state clone sensor. 0 = silent, 1 = per-join summary, 2 = every candidate."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagHitch(
	TEXT("FPM.Diag.Hitch"), 1,
	TEXT("Frame-time hitch meter. 0 = silent, 1 = every hitch + the periodic summary, 2 = also names the "
	     "packages that were loading when it hit. Level 2 costs a string copy per async load - it is for a "
	     "deliberate boot, not for playing."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarDiagOverlay(
	TEXT("FPM.Diag.Overlay"), 1,
	TEXT("The on-screen dev feed. 0 = hide it, 1 = show it. Ant asked for a keybind; until the Game "
	     "Instance Module's keybind registry is wired up, this is the switch."),
	ECVF_Default);

namespace
{
	TAutoConsoleVariable<int32>* const GChannelCVars[] = {
		&CVarDiagSchematic,
		&CVarDiagHologram,
		&CVarDiagInventory,
		&CVarDiagClone,
		&CVarDiagHitch,
		&CVarDiagOverlay,
	};

	// The table and the enum must not drift. This is the whole reason the indexing is safe.
	static_assert(UE_ARRAY_COUNT(GChannelCVars) == static_cast<int32>(FPMDiag::EChannel::Count),
		"FPMDiag::EChannel and GChannelCVars are out of sync - add the new channel to BOTH.");

	const TCHAR* ChannelName(FPMDiag::EChannel Channel)
	{
		switch (Channel)
		{
		case FPMDiag::EChannel::SchematicProbe: return TEXT("FPM.Diag.Schematic");
		case FPMDiag::EChannel::HologramNet:    return TEXT("FPM.Diag.Hologram");
		case FPMDiag::EChannel::InventoryInit:  return TEXT("FPM.Diag.Inventory");
		case FPMDiag::EChannel::CloneSensor:    return TEXT("FPM.Diag.Clone");
		case FPMDiag::EChannel::Hitch:          return TEXT("FPM.Diag.Hitch");
		case FPMDiag::EChannel::Overlay:        return TEXT("FPM.Diag.Overlay");
		default:                                return TEXT("<unknown>");
		}
	}
}

int32 FPMDiag::LevelOf(EChannel Channel)
{
	const int32 Master = CVarDiagMaster.GetValueOnAnyThread();
	if (Master >= 0)
	{
		return Master;   // master wins in BOTH directions: 0 silences, 2 turns everything up
	}

	const int32 Index = static_cast<int32>(Channel);
	if (Index < 0 || Index >= UE_ARRAY_COUNT(GChannelCVars))
	{
		return 0;        // fail QUIET, not fail loud: an unknown channel must not spam
	}
	return GChannelCVars[Index]->GetValueOnAnyThread();
}

bool FPMDiag::IsOn(EChannel Channel, int32 Level)
{
	return LevelOf(Channel) >= Level;
}

bool FPMDiag::IsSilenced()
{
	// Explicitly 0, not merely "<= 0" — the default is -1 and that means "defer to the channel", which
	// is the opposite of silence. Conflating the two would silence everything by default.
	return CVarDiagMaster.GetValueOnAnyThread() == 0;
}

void FPMDiag::LogAll()
{
	const int32 Master = CVarDiagMaster.GetValueOnAnyThread();
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] diagnostics — FPM.Diag = %d (%s)"),
		Master, Master < 0 ? TEXT("per-channel") : TEXT("OVERRIDING every channel"));

	for (int32 i = 0; i < static_cast<int32>(EChannel::Count); ++i)
	{
		const EChannel Ch = static_cast<EChannel>(i);
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM]   %-22s = %d   (effective %d)"),
			ChannelName(Ch), GChannelCVars[i]->GetValueOnAnyThread(), LevelOf(Ch));
	}
}

/*
 * `FPM.Diag.List` — because a switch you cannot read the state of is a switch you end up guessing at,
 * and this project has burned boots on exactly that shape of guess.
 */
static FAutoConsoleCommand GDiagListCmd(
	TEXT("FPM.Diag.List"),
	TEXT("Print every FPM diagnostic channel and its effective level."),
	FConsoleCommandDelegate::CreateStatic(&FPMDiag::LogAll));
