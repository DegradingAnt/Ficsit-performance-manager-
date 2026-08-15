// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/ModFeatures/FPMThirdPersonToggle.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMConsoleEcho.h"
#include "Core/FPMDiag.h"

#include "FGCharacterPlayer.h"
#include "Input/FGInputMappingContext.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"

#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	/*
	 * §6.7's own content mount: /FicsitsPerformanceManager/. Both are top-level assets, so the object
	 * path repeats the asset name after the dot (package.objectname), the standard LoadObject shape.
	 */
	const TCHAR* const GFPMInputActionPath =
		TEXT("/FicsitsPerformanceManager/Input/IA_ThirdPersonToggle.IA_ThirdPersonToggle");
	const TCHAR* const GFPMInputContextPath =
		TEXT("/FicsitsPerformanceManager/Input/IMC_FPM.IMC_FPM");

	TStrongObjectPtr<UInputAction> GFPMToggleAction;
	TStrongObjectPtr<UFGInputMappingContext> GFPMMappingContext;
	bool bGFPMAssetsAttempted = false;

	FDelegateHandle GFPMInputInitHandle;
	int32 GFPMToggleFires = 0;

	/*
	 * ★ LOUD, NOT CRASHING — the same shape FPMSettingsConfig.cpp's ResolveSMLPropertyClass already uses
	 * for a missing SML widget class. Both assets are editor-authored (§6.7) and simply do not exist
	 * until Ant creates them; that is an expected pre-editor state, not a corrupt install, so this reports
	 * it once and moves on rather than treating it as fatal.
	 */
	bool ResolveInputAssets()
	{
		if (bGFPMAssetsAttempted)
		{
			return GFPMToggleAction.IsValid() && GFPMMappingContext.IsValid();
		}
		bGFPMAssetsAttempted = true;

		UInputAction* Action = LoadObject<UInputAction>(nullptr, GFPMInputActionPath);
		UFGInputMappingContext* Context = LoadObject<UFGInputMappingContext>(nullptr, GFPMInputContextPath);

		if (Action == nullptr || Context == nullptr)
		{
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] third-person toggle: input asset(s) missing - action %s, context %s. The "
				     "keybind will not appear in the Options menu until both exist. See the Slice 5 "
				     "findings for the editor steps to create them."),
				Action ? TEXT("OK") : GFPMInputActionPath,
				Context ? TEXT("OK") : GFPMInputContextPath);
			return false;
		}

		GFPMToggleAction.Reset(Action);
		GFPMMappingContext.Reset(Context);
		return true;
	}
}

FFPMThirdPersonToggle& FFPMThirdPersonToggle::Get()
{
	static FFPMThirdPersonToggle Instance;
	return Instance;
}

void FFPMThirdPersonToggle::Arm()
{
	/*
	 * The same delegate FPMWristSlotComponent.cpp already binds for handedness — a plain multicast the
	 * game publishes on every SetupPlayerInputComponent, so this is idempotent by construction and needs
	 * no guard against double-firing.
	 */
	GFPMInputInitHandle = AFGCharacterPlayer::OnPlayerInputInitialized.AddLambda(
		[](AFGCharacterPlayer* Character, UInputComponent* InputComponent)
		{
			if (Character == nullptr || !Character->IsLocallyControlled()) { return; }
			if (!ResolveInputAssets()) { return; }

			UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent);
			if (EIC == nullptr)
			{
				UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::ThirdPersonToggle), LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] third-person toggle: InputComponent is not Enhanced Input - cannot bind."));
				return;
			}

			/*
			 * Belt-and-braces alongside the Game Feature Data scan §6.7 warns about ("a keybind that
			 * never appears is a MISSING SCAN RULE before it is a code bug") - registering the context
			 * here does not depend on the scan having picked it up, only on the asset having loaded.
			 */
			if (APlayerController* PC = Character->GetController<APlayerController>())
			{
				if (ULocalPlayer* LP = PC->GetLocalPlayer())
				{
					if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
						LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
					{
						Subsystem->AddMappingContext(GFPMMappingContext.Get(), 0);
					}
				}
			}

			EIC->BindActionInstanceLambda(GFPMToggleAction.Get(), ETriggerEvent::Started,
				[Character](const FInputActionInstance&)
				{
					if (Character == nullptr) { return; }
					++GFPMToggleFires;

					// EXISTENCE-proven, not EXECUTION-proven — see the header. The symbol resolves and
					// links against the shipped game; whether it actually swaps the camera needs a boot.
					Character->ToggleCameraMode();

					UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::ThirdPersonToggle), LogFicsitsPerformanceManager, Display,
						TEXT("[FPM] third-person toggle: fired (%d total)"), GFPMToggleFires);
				});

			UE_CLOG(FPMDiag::IsOn(FPMDiag::EChannel::ThirdPersonToggle), LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] third-person toggle: bound on input init."));
		});

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] third-person toggle ARMED - no hook. Binds IA_ThirdPersonToggle to "
		     "AFGCharacterPlayer::ToggleCameraMode() on every input init. FPM.ThirdPerson.Report says "
		     "whether the two input assets resolved."));
}

void FFPMThirdPersonToggle::Disarm()
{
	if (GFPMInputInitHandle.IsValid())
	{
		AFGCharacterPlayer::OnPlayerInputInitialized.Remove(GFPMInputInitHandle);
		GFPMInputInitHandle.Reset();
	}
	GFPMToggleAction.Reset();
	GFPMMappingContext.Reset();
	bGFPMAssetsAttempted = false;
}

void FFPMThirdPersonToggle::ReportNow()
{
	const bool bResolved = GFPMToggleAction.IsValid() && GFPMMappingContext.IsValid();
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] third-person toggle: assets resolved=%s (action %s, context %s) * fired %d time(s)"),
		bResolved ? TEXT("yes") : TEXT("no"),
		GFPMToggleAction.IsValid() ? TEXT("OK") : TEXT("MISSING"),
		GFPMMappingContext.IsValid() ? TEXT("OK") : TEXT("MISSING"),
		GFPMToggleFires);

	UE_CLOG(!bResolved, LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM]   create the missing asset(s) under /FicsitsPerformanceManager/Input/ - see the "
		     "Slice 5 findings file for the exact editor steps."));
}

static FAutoConsoleCommandWithOutputDevice GFPMThirdPersonReportCmd(
	TEXT("FPM.ThirdPerson.Report"),
	TEXT("Third-person toggle: whether both input assets resolved, and how many times it fired."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& Ar)
	{
		FPMScopedConsoleEcho Echo(&Ar);
		FFPMThirdPersonToggle::ReportNow();
	}));
