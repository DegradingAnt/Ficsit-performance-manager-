// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMBootProbes.h"

#include "FicsitsPerformanceManager.h"

#include "FGCharacterPlayer.h"
#include "FGTimeSubsystem.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

namespace
{
	/** Same duplicated-emit discipline `FPMCVarProbe.cpp`'s `FPMProbeEmit` uses: Ar reaches the console
	 *  Ant is looking at, UE_LOG reaches the file an agent reads afterwards. Kept local rather than
	 *  shared because it is four lines and a shared header for four lines is its own kind of drift risk. */
	void FPMBootProbeEmit(FOutputDevice* Ar, const FString& Line)
	{
		if (Ar != nullptr) { Ar->Log(Line); }
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s"), *Line);
	}
}

void FPMBootProbes::ReportTimeOfDay(UWorld* World, FOutputDevice* Ar)
{
	FPMBootProbeEmit(Ar, TEXT("---- FPM.Probe.TimeOfDay (B9: does FGTimeOfDaySubsystem expose a "
	                          "non-cheat pin API?) ----"));

	/*
	 * ★ THE EXISTENCE HALF IS ALREADY SETTLED FROM SOURCE, STATED HERE SO THE LOG CARRIES THE ANSWER
	 * BESIDE THE READ, NOT ONLY IN A DESIGN DOC. `SetDaySeconds(float)` (FGTimeSubsystem.h:48) and
	 * `SetTimeSpeedMultiplier(float)` (:128) are both plain `public:` C++ members, never
	 * `UFUNCTION(exec, CheatBoard, ...)` the way UFGCheatManager's day/night controls are. Nothing
	 * below calls either — this command reads only.
	 */
	FPMBootProbeEmit(Ar, TEXT("  source read: AFGTimeOfDaySubsystem::SetDaySeconds / SetTimeSpeedMultiplier "
	                          "are public, non-cheat C++ members (FGTimeSubsystem.h:48,128) - a pin API "
	                          "EXISTS. Not called here; this command is read-only by design."));

	if (World == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ** no UWorld - run this from an active session, not the main menu. **"));
		return;
	}

	AFGTimeOfDaySubsystem* TimeOfDay = AFGTimeOfDaySubsystem::Get(World);
	if (TimeOfDay == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ** AFGTimeOfDaySubsystem::Get() returned null on a real world - the "
		                          "subsystem is not up yet (too early in load) or this build moved it. "
		                          "Coverage: this probe can only report what it can reach; a null here is "
		                          "a finding, not a silent skip. Re-run once fully loaded in. **"));
		return;
	}

	FPMBootProbeEmit(Ar, FString::Printf(
		TEXT("  reachable: day %d, %.1fh (%.0fs into the day), day length %.1fmin, night length %.1fmin."),
		TimeOfDay->GetPassedDays(), TimeOfDay->GetTimeOfDayHours(), TimeOfDay->GetDaySeconds(),
		TimeOfDay->GetDayLength(), TimeOfDay->GetNightLength()));
	FPMBootProbeEmit(Ar, TEXT("  => B9 answered: subsystem reachable, pin API exists and is public. "
	                          "Verifying the PIN itself (call SetDaySeconds, confirm the clock holds) is "
	                          "follow-up work for whoever builds the lever, not this read-only probe."));
}

void FPMBootProbes::ReportSockets(UWorld* World, FOutputDevice* Ar)
{
	FPMBootProbeEmit(Ar, TEXT("---- FPM.Probe.Sockets (B11: which sockets does the vanilla player skeleton "
	                          "expose per forearm/hand, both sides?) ----"));

	if (World == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ** no UWorld - run this from an active session, not the main menu. **"));
		return;
	}

	APlayerController* PC = GEngine ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
	if (Character == nullptr)
	{
		FPMBootProbeEmit(Ar, TEXT("  ** no local AFGCharacterPlayer pawn - not spawned in yet (main menu, "
		                          "loading screen, mid-respawn). Coverage: this probe needs a live "
		                          "character to read a skeleton off of; that is the whole precondition, "
		                          "stated rather than a silent empty result. Re-run once spawned in. **"));
		return;
	}

	/*
	 * ★ THE TARGETED CHECK, NOT JUST A DUMP. FPMWristItemBase.h:118,121 already ships a guess -
	 * "hand_lSocket" / "hand_rSocket" - and its own comment says B11 has never measured it against the
	 * real skeleton. So the useful answer is PASS/FAIL on the name the shipped code already depends on,
	 * on both meshes a wrist item could plausibly attach to - printed beside the full list so a wrong
	 * guess can be corrected from this one command instead of costing a second boot.
	 */
	const FName CandidateLeft(TEXT("hand_lSocket"));
	const FName CandidateRight(TEXT("hand_rSocket"));

	auto ReportMesh = [Ar](const TCHAR* Label, USkeletalMeshComponent* Mesh)
	{
		if (Mesh == nullptr)
		{
			FPMBootProbeEmit(Ar, FString::Printf(TEXT("  %s: not present on this character."), Label));
			return;
		}

		const bool bLeft = Mesh->DoesSocketExist(FName(TEXT("hand_lSocket")));
		const bool bRight = Mesh->DoesSocketExist(FName(TEXT("hand_rSocket")));
		FPMBootProbeEmit(Ar, FString::Printf(
			TEXT("  %s: FPMWristItemBase's guess - hand_lSocket %s, hand_rSocket %s."),
			Label, bLeft ? TEXT("FOUND") : TEXT("NOT FOUND"), bRight ? TEXT("FOUND") : TEXT("NOT FOUND")));

		TArray<FName> All = Mesh->GetAllSocketNames();
		All.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
		FPMBootProbeEmit(Ar, FString::Printf(TEXT("  %s: %d socket(s) total - %s"),
			Label, All.Num(),
			All.Num() > 0 ? *FString::JoinBy(All, TEXT(", "), [](const FName& N) { return N.ToString(); })
			              : TEXT("(none - this mesh has no sockets, which is itself a finding)")));
	};

	ReportMesh(TEXT("first-person (GetMesh1P)"), Character->GetMesh1P());
	ReportMesh(TEXT("third-person (GetMesh3P)"), Character->GetMesh3P());

	FPMBootProbeEmit(Ar, TEXT("  => B11 answered for this skeleton build: read the FOUND/NOT FOUND lines "
	                          "above. A NOT FOUND on either candidate means FPMWristItemBase.h's "
	                          "mSocketLeft/mSocketRight defaults need updating to a real name from the "
	                          "full list beside it - same boot, no second session needed."));
}

/*
 * `FPM.Probe.TimeOfDay` and `FPM.Probe.Sockets` — plain read-only console commands, same registration
 * shape `FPM.Crates.Report` uses (`FAutoConsoleCommandWithWorldArgsAndOutputDevice`) so the delegate is
 * handed a `UWorld*` without either probe having to find one itself.
 */
static FAutoConsoleCommandWithWorldArgsAndOutputDevice GFPMDiagTimeOfDayCmd(
	TEXT("FPM.Probe.TimeOfDay"),
	TEXT("B9: report whether AFGTimeOfDaySubsystem is reachable and confirm its pin API (SetDaySeconds / "
	     "SetTimeSpeedMultiplier) is public and non-cheat. Read-only - calls no setter."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
		{
			FPMBootProbes::ReportTimeOfDay(World, &Ar);
		}));

static FAutoConsoleCommandWithWorldArgsAndOutputDevice GFPMDiagSocketsCmd(
	TEXT("FPM.Probe.Sockets"),
	TEXT("B11: enumerate socket names on the local player's first- and third-person mesh, and check "
	     "FPMWristItemBase's hand_lSocket/hand_rSocket guess against both. Requires a spawned character."),
	FConsoleCommandWithWorldArgsAndOutputDeviceDelegate::CreateStatic(
		[](const TArray<FString>& /*Args*/, UWorld* World, FOutputDevice& Ar)
		{
			FPMBootProbes::ReportSockets(World, &Ar);
		}));
