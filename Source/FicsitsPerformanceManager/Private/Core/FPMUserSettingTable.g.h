// GENERATED FILE - DO NOT EDIT BY HAND.
//
// Regenerate:  40-TOOLS/satisfactory/extract_user_settings.ps1
// Source    :  the FModel export of the vanilla settings tree.
//
// Every entry is the StrId of a UFGUserSetting whose UseCVar is true, which IS the cvar name the
// game manages for it (FGUserSetting.h:183-189 - "manage and if needed create a cvar for this
// setting based on StrId"). These are the cvars FGGameUserSettings serialises on every save, so a
// value FPM holds on one at save time becomes the player's PERMANENT setting.
//
// SCOPE, STATED SO IT CANNOT BE MISREAD: VANILLA ONLY. Mods register their own settings and this
// export contains none of them, so this table can never be the whole answer. It is the FALLBACK;
// the runtime read of UFGGameUserSettings::GetAllUserSettingsMap() is the PRIMARY and it sees mod
// settings too. Anything that falls back to this table must SAY SO in the log when it does.

#pragma once

namespace FPMUserSettingTable
{
	/** 67 vanilla cvar-backed user settings, sorted. COMPARE CASE-INSENSITIVELY: the
	 *  assets are not consistently cased - r.screenpercentage here vs the engine's r.ScreenPercentage. */
	inline constexpr const TCHAR* GDerivedUSBackedCVars[] =
	{
		TEXT("CSS.Conveyor.MaxDrawDistance"),                  // US_ConveyorItemRenderDistance
		TEXT("css.EnableSSR"),                                 // US_ScreenspaceReflections
		TEXT("FG.AimAssistStrength"),                          // US_AimAssistStrength
		TEXT("FG.ArachnophobiaMode"),                          // US_ArachnophobiaMode
		TEXT("FG.AutosaveInterval"),                           // US_AutosaveInterval
		TEXT("FG.ConveyorItemFrequency"),                      // US_ConveyorItemFrequency
		TEXT("FG.DSAutoPause"),                                // US_DSAutoPause
		TEXT("FG.DSAutoSaveOnDisconnect"),                     // US_DSAutoSaveOnDisconnect
		TEXT("FG.EnableSeasonalEvents"),                       // US_EnableSeasonalEvents
		TEXT("FG.EventOverride"),                              // US_EventOverride
		TEXT("FG.FirstPersonFOVModifier"),                     // US_FirstPersonFOVModifier
		TEXT("FG.FlySpeedMultiplier"),                         // US_FlySpeedMultiplier
		TEXT("FG.ForceGamepadType"),                           // US_ForceGamepadType
		TEXT("FG.FrameGeneration"),                            // US_FrameGen
		TEXT("FG.GamepadDeadzone"),                            // US_GamepadDeadzoneCamera
		TEXT("FG.GamepadDeadzoneMovement"),                    // US_GamepadDeadzoneMovement
		TEXT("FG.GamepadDisconnectSwitchEnabled"),             // US_GamepadDisconnectSwitch
		TEXT("FG.GamepadLookSensitivityX"),                    // US_GamepadLookSensitivityX
		TEXT("FG.GamepadLookSensitivityY"),                    // US_GamepadLookSensitivityY
		TEXT("FG.GamepadMoveSensitivityRight"),                // US_GamepadMoveSensitivityRight
		TEXT("FG.GamepadMoveSensitivityX"),                    // US_GamepadMoveSensitivityForward
		TEXT("FG.GamepadRumbleEnabled"),                       // US_GamepadRumbleEnabled
		TEXT("FG.GamepadRumbleStrength"),                      // US_GamepadRumbleStrength
		TEXT("FG.GamepadSpeakerEnabled"),                      // US_GamepadSpeakerEnabled
		TEXT("FG.GamepadSpeakerVolume"),                       // US_GamepadSpeakerVolume
		TEXT("FG.HoldToCrouch"),                               // US_HoldToCrouch
		TEXT("FG.HoldToSnap"),                                 // US_HoldToSnap
		TEXT("FG.HoldToSprint"),                               // US_HoldToSprint
		TEXT("FG.HoldZipline"),                                // US_HoldZipline
		TEXT("FG.InputMode"),                                  // US_InputMode
		TEXT("FG.NetworkQuality"),                             // US_NetworkQuality
		TEXT("FG.RememberSnapping"),                           // US_RememberSnapping
		TEXT("FG.SendGameplayData"),                           // US_SendGameplayData
		TEXT("FG.ServerRestartTimeSlot"),                      // US_ServerRestartTimeSlot
		TEXT("FG.SubtitleFontSize"),                           // US_SubtitleFontSize
		TEXT("FG.ToggleTooltipStaysOpen"),                     // US_ToggleTooltipStaysOpen
		TEXT("FG.ToggleToOpenInventoryContext"),               // US_ToggleToOpenInventoryContext
		TEXT("FG.UpScalingMethod"),                            // US_UpScalingMethod
		TEXT("FG.VehiclePathRenderDistance"),                  // US_VehiclePathRenderDistance
		TEXT("FGAudio.AudioMeter.AttenuationStrength"),        // US_VoiceChatDetection_Strength
		TEXT("FGAudio.AudioMeter.IncludeMicToDeviceList"),     // US_VoiceChatDetection_MicInput
		TEXT("FGAudio.AudioMeter.ProcessSelector"),            // US_VoiceChatDetection
		TEXT("foliage.DitheredLOD"),                           // US_EnableLODDithering
		TEXT("r.AntiAliasingMethod"),                          // US_AntiAliasMethod
		TEXT("r.ContactShadows"),                              // US_EnableContactShadows
		TEXT("r.Fog.Density"),                                 // US_FogDensity
		TEXT("r.FullScreenMode"),                              // US_FullScreen
		TEXT("r.HZBOcclusion"),                                // US_HierarchicalZ-BufferOcclusion
		TEXT("r.screenpercentage"),                            // US_ScreenPercentage
		TEXT("r.TonemapperGamma"),                             // US_Gamma
		TEXT("r.VolumetricCloud"),                             // US_VolumetricClouds
		TEXT("r.VSync"),                                       // US_VSync
		TEXT("sg.AntiAliasingQuality"),                        // US_AntiAliasing
		TEXT("sg.CloudQuality"),                               // US_CloudQuality
		TEXT("sg.DistanceFieldTraceDistance"),                 // US_ShadowTraceDistance
		TEXT("sg.EffectsQuality"),                             // US_VFXQuality
		TEXT("sg.FoliageLoadDistance"),                        // US_FoliageLoadDistance
		TEXT("sg.FoliageQuality"),                             // US_FoliageQuality
		TEXT("sg.GlobalIlluminationQuality"),                  // US_Globalillumination
		TEXT("sg.PoolLightQuality"),                           // US_LightQuality
		TEXT("sg.PostProcessQuality"),                         // US_PostProcessing
		TEXT("sg.ShadowQuality"),                              // US_ShadowQuality
		TEXT("sg.TextureQuality"),                             // US_TextureQuality
		TEXT("sg.TSRPreset"),                                  // US_TSRPreset
		TEXT("sg.ViewDistanceQuality"),                        // US_ViewDistance
		TEXT("t.CameraShakeIntensityMultiplier"),              // US_CameraShakeIntensity
		TEXT("t.MaxFPS"),                                      // US_MaxFPS
	};
}
