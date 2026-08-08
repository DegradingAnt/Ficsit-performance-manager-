// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/**
 * FPM OVERLAY — a generic on-screen feed of what the mod is doing. Deliberately ugly, deliberately
 * readable.
 *
 * Ant, 2026-08-08: "i want UI to show when and what the rain fix thing is doing so i can see that its
 * working. we should have a generic UI overlay for the loading screen or something that shows what the
 * game/our mod is doing so we can debug easier."
 *
 * ★ WHY SLATE AND NOT A UUserWidget, CARRIED FROM THE OLD MOD'S OVERLAY ALONG WITH THE REASON. A
 * UUserWidget needs a Widget Blueprint asset, a cook step and a live HUD to attach to — none of which
 * exist while a loading screen is up, which is precisely when Ant wants to see this. Raw C++ Slate goes
 * straight into the game viewport, needs no asset and no world, and therefore survives exactly the
 * moment a UMG panel cannot.
 *
 * WHAT CHANGED ON CARRY: the old one was hardwired to the governor and the benchmark — its ComposeText()
 * read their accessors directly, so it could only ever show those two things. This is a FEED: anything
 * posts a line, the panel shows the last N. That is the "generic" half of what she asked for, and it is
 * the difference between a governor readout and a debug surface.
 *
 * VIEWER ONLY. It reads nothing and steers nothing — posting is one-way. A diagnostic that can change
 * what it measures is not a diagnostic.
 *
 * SCREENSHOT-FIRST, because that is how it gets used: monospace so columns line up, flat dark panel so
 * the text survives compression, nothing abbreviated. Styled to be legible at 50% in a chat window, not
 * to be pretty. It is explicitly NOT the shipped debug view — it is the thing that makes the next boot
 * test legible, and that later gets replaced rather than grown.
 */
class FICSITSPERFORMANCEMANAGER_API FPMOverlay
{
public:
	static FPMOverlay& Get();

	/**
	 * Post one line. Safe from before a viewport exists (it is buffered) and safe to call often.
	 *
	 * ALSO LOGS, ALWAYS. The screen and the log must never disagree — a line visible in game but absent
	 * from FactoryGame.log is one nobody can send us afterwards, and the log is what survives the
	 * session.
	 */
	static void Post(const TCHAR* Category, const FString& Line);

	/** Show/hide. Safe before a viewport exists; it attaches as soon as one appears. */
	void SetVisible(bool bInVisible);
	void Toggle() { SetVisible(!bVisible); }
	bool IsVisible() const { return bVisible; }

	/** Drops the widget and stops ticking, so nothing outlives the viewport. */
	void Shutdown();

private:
	bool Tick(float DeltaSeconds);
	void EnsureAttached();
	FString ComposeText() const;

	TArray<FString> Lines;
	mutable FCriticalSection LinesLock;

	TSharedPtr<class SWidget> Root;
	TSharedPtr<class STextBlock> TextBlock;
	FTSTicker::FDelegateHandle TickHandle;

	bool bVisible = false;
	bool bDirty = false;

	/** Enough to read a sweep's story, few enough to stay legible in a screenshot. */
	static constexpr int32 MaxLines = 18;

	/** 4 Hz. Faster shows no new information and spends frame time in a mod whose job is not to. */
	static constexpr float RefreshSeconds = 0.25f;
	float SinceRefresh = 0.f;
};
