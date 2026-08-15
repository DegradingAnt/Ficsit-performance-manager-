// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMAudioVoiceDetector.h"

#include "AkAudioDevice.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDetectorRegistry.h"
#include "Core/FPMOverlay.h"

FFPMAudioVoiceDetector& FFPMAudioVoiceDetector::Get()
{
	static FFPMAudioVoiceDetector Instance;
	return Instance;
}

bool FFPMAudioVoiceDetector::SelfTest()
{
	const bool bDeviceReachable = FAkAudioDevice::Get() != nullptr;

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: audio-voice self-test - FAkAudioDevice::Get() %s. This proves the probe "
		     "runs on real ground; it does NOT prove a voice-count API exists, because none was "
		     "found (class comment) - that absence is reported by RunNow(), not by this check."),
		bDeviceReachable ? TEXT("reachable") : TEXT("NOT reachable (no Wwise device this early - "
		                                             "expected before the audio engine finishes init)"));

	return bDeviceReachable;
}

void FFPMAudioVoiceDetector::RunNow()
{
	const bool bDeviceReachable = FAkAudioDevice::Get() != nullptr;

	// No cost-attribution row is reported to FFPMDetectorRegistry - a detector whose trap cannot
	// fire on this build must not appear in the per-mod table, because a table entry implies a
	// finding and there is none. The registry's own coverage line (FPM.Detect.Report) already
	// names every detector that never reported.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: audio-voice - COVERAGE: the AK voice-count API is NOT PRESENT in this "
		     "game's shipped Wwise integration (searched the entire Plugins/Wwise tree - Public, "
		     "Private, ThirdParty - for GetNumActiveVoices/ActiveVoiceCount; zero hits). This is a "
		     "structural absence, not a bug in this detector, and it is checked fresh every run in "
		     "case a future Wwise update adds the surface. Wwise device %s (voices could not be "
		     "counted either way)."),
		bDeviceReachable ? TEXT("is reachable") : TEXT("is NOT reachable"));

	// m6164470: every FPM feature reports to the dev overlay, including one whose trap cannot
	// fire - "no voice count API on this build" is the finding, and it must be visible without
	// anyone typing FPM.Detect.AudioVoice.
	FPMOverlay::PostSticky(TEXT("detect"), TEXT("audio-voice"),
		FString::Printf(TEXT("audio voice starvation: NO voice-count API on this build (device %s)"),
			bDeviceReachable ? TEXT("reachable") : TEXT("unreachable")));
}

void FFPMAudioVoiceDetector::Arm()
{
	const bool bSelfTestPassed = SelfTest();
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: audio-voice-detector armed - self-test (device reachability) %s. §9.3's "
		     "audio-voice-starvation trap: the voice-count API does not exist on this build; "
		     "FPM.Detect.AudioVoice prints why, every time, rather than staying silent about it."),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
	RunNow();
}

namespace
{
	static FAutoConsoleCommand GFPMAudioVoiceRunCmd(
		TEXT("FPM.Detect.AudioVoice"),
		TEXT("Re-probe the AK voice-count surface and print its coverage line."),
		FConsoleCommandDelegate::CreateStatic([]() { FFPMAudioVoiceDetector::RunNow(); }));
}
