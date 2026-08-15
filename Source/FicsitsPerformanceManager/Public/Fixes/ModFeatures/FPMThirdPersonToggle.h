// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Core/FPMFixContract.h"

/**
 * THIRD-PERSON VIEW AS A BUTTON SETTING — m6737334, §6.7.
 *
 * OriginNamed, the same shape as the zipline volume lever: the cause is not a bug, it is that vanilla
 * exposes `AFGCharacterPlayer::ToggleCameraMode` (`FGCharacterPlayer.h:630`, a public, plain `UFUNCTION()`
 * with no Enhanced Input action bound to it anywhere in this project) but ships no dedicated, remappable
 * KEY for it. "No placeable" in the board's own wording rules out the alternative this considered and
 * rejected — an in-world button prop — in favour of an ordinary keybind, which is what this is.
 *
 * ══ THE MECHANISM, PER §6.7 ══
 *
 * Registration binds `AFGCharacterPlayer::OnPlayerInputInitialized`
 * (`DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPlayerInputInitializedDelegate, AFGCharacterPlayer*,
 * UInputComponent*)`, `FGCharacterPlayer.h:45`) — a plain multicast delegate to AddLambda/Remove, not a
 * method to override. It fires on every `SetupPlayerInputComponent`, so re-binding on a respawn or a
 * possession change is free and the same code path handles the first bind. `FPMWristSlotComponent.cpp`
 * already uses this exact delegate for handedness; this is the second consumer, not a new pattern.
 *
 * The bound handler casts the `UInputComponent*` the delegate hands us to `UEnhancedInputComponent` and
 * binds our `IA_ThirdPersonToggle` action (`ETriggerEvent::Started`) to call
 * `Character->ToggleCameraMode()`. It also calls `AddMappingContext` on the local player's
 * `UEnhancedInputLocalPlayerSubsystem` for our `IMC_FPM` context, defensively — belt-and-braces alongside
 * whatever the Game Feature Data scan does, per §6.7's own warning that "a keybind that never appears is a
 * MISSING SCAN RULE before it is a code bug".
 *
 * ══ THE TWO ASSETS THIS FILE CANNOT CREATE, NAMED EXPLICITLY ══
 *
 * §6.7 states the cost directly: "The context/action DATA ASSETS are editor-authored." A `UInputAction`
 * and an `FGInputMappingContext` are binary content, not C++. This fix loads them defensively by path —
 * `/FicsitsPerformanceManager/Input/IA_ThirdPersonToggle` and `/FicsitsPerformanceManager/Input/IMC_FPM` —
 * the same loud-not-crashing shape `FPMSettingsConfig.cpp`'s `ResolveSMLPropertyClass` already uses for a
 * missing SML widget class: a clear, named log line, and the fix simply does not bind until the assets
 * exist. See the findings file for the exact editor steps to create them.
 *
 * ══ SCOPE ══
 *
 * CLIENT ONLY — camera mode is a local rendering concern; a dedicated server has no camera to toggle.
 *
 * ★ EXECUTION-PROVEN VS EXISTENCE-PROVEN, STATED OUT LOUD: `ToggleCameraMode` is confirmed to exist as a
 * public callable UFUNCTION from the header (`FGCharacterPlayer.h:630`). Its `.cpp` in this source tree is
 * an EMPTY STUB (`FGCharacterPlayer.cpp:316`) — this project's vendored FactoryGame source ships headers
 * with stripped bodies; the real implementation lives in the shipped binary this mod loads into, not in
 * this tree. That the symbol exists and links is EXISTENCE-proven. That calling it actually swaps the
 * camera in the live game is NOT proven here — it needs one boot, standing on both legs, pressing the key.
 *
 * ZERO RESIDUE: no cvar, no ini, no SaveGame state. The Enhanced Input binding lives on the character's
 * `InputComponent` and is torn down with it; the mapping-context registration is released in `Disarm()`.
 */
class FFPMThirdPersonToggle final : public IFPMFix
{
public:
	static FFPMThirdPersonToggle& Get();

	virtual const TCHAR* Name() const override { return TEXT("third-person-toggle"); }

	/** Camera mode is a local rendering concern. A dedicated server has no camera to toggle. */
	virtual EFPMFixSide Side() const override { return EFPMFixSide::NeverOnDedicatedServer; }

	/** The cause is named: vanilla exposes ToggleCameraMode but binds no dedicated key to it. */
	virtual EFPMOriginStatus OriginStatus() const override { return EFPMOriginStatus::OriginNamed; }

	virtual FPMDiag::EChannel Channel() const override { return FPMDiag::EChannel::ThirdPersonToggle; }

	virtual void Arm() override;
	virtual void Disarm() override;

	/** `FPM.ThirdPerson.Report` — whether both assets resolved, and whether the bind ever fired. */
	static void ReportNow();
};
