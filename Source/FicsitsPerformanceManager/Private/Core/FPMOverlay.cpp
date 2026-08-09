// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.
//
// ⚠ ROUTE NOTE, CHECKED AGAINST THE COMMUNITY DOCS 2026-08-08.
//
// The documented way to add UI to this game is Widget Blueprint Hooks
// (Development/ModLoader/WidgetBlueprintHooks.adoc) — "add your custom widget into one of the existing
// game widgets", declared via the Game Instance Module's WidgetBlueprintHooks array. SML uses it for the
// Mods button, it integrates at the widget-archetype level, and it is what ANY player-facing FPM surface
// must use.
//
// It cannot serve THIS case, and the reason is structural rather than a preference: it needs a host game
// widget, a cooked .uasset and a live world. A loading screen has none of the three, and the loading
// screen is exactly when Ant wants to see what the mod is doing. Raw Slate into the game viewport needs
// no asset, no world and no host widget.
//
// So this is a DEV INSTRUMENT, not the shipped debug view. When it becomes something a user sees, it
// moves to WidgetBlueprintHooks + a Widget Blueprint.

#include "Core/FPMOverlay.h"

#include "Core/FPMDiag.h"

#include "FicsitsPerformanceManager.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

/*
 * THE KEY IS A CVAR, not a literal, because the right key is Ant's call and not mine — and because a
 * hard-coded one that collides with a mod she installs next week is unfixable without a build.
 *
 * F8 by default: vanilla Satisfactory binds F1-F6 (photo mode, map, codex and friends) and F7 is the
 * common screenshot key, so F8 is the first one likely to be free. If it collides, change this rather
 * than reporting the overlay as broken.
 */
/*
 * ★ HOW FAR DOWN THE PANEL STARTS. Ant, 2026-08-09: *"had to turn off debug ui since it blocked the fps
 * number"* — her hardware overlay (FPS/GPU/CPU) owns the top strip of the screen, and this panel was
 * anchored at y=16 with lines long enough to reach the right-hand side, so the two collided and the only
 * way to read her frame rate was to hide ours.
 *
 * A cvar rather than a number I pick, for the same reason the hotkey is one: the right value depends on
 * HER overlay's height and her resolution, neither of which this code can see, and a hard-coded guess
 * that collides is unfixable without a build. 44 px clears a single-row RTSS-style bar at 1440p.
 *
 * Takes effect on the next attach, so F8 twice after changing it.
 */
static TAutoConsoleVariable<float> CVarOverlayTopMargin(
	TEXT("FPM.Diag.OverlayTop"), 44.f,
	TEXT("Pixels from the top of the viewport to the FPM overlay panel. Raise it to clear a hardware "
	     "monitor overlay along the top edge. Applies on next attach - press the overlay key twice."),
	ECVF_Default);

static TAutoConsoleVariable<FString> CVarOverlayKey(
	TEXT("FPM.Diag.OverlayKey"), TEXT("F8"),
	TEXT("Key that toggles the FPM debug overlay. Any UE key name (F8, F9, Tilde, ScrollLock...). "
	     "Empty string disables the hotkey entirely and leaves only FPM.Diag.Overlay."),
	ECVF_Default);

namespace
{
	/**
	 * ⚠ IT MUST RETURN FALSE FOR EVERYTHING IT DOES NOT OWN. A pre-processor sits ABOVE the game's whole
	 * input stack, so returning true swallows the key before any vanilla or mod binding sees it. Consuming
	 * only our own key is what makes this safe to ship enabled by default.
	 */
	class FFPMOverlayHotkey : public IInputProcessor
	{
	public:
		virtual void Tick(const float, FSlateApplication&, TSharedRef<ICursor>) override {}

		virtual bool HandleKeyDownEvent(FSlateApplication&, const FKeyEvent& Event) override
		{
			const FString Wanted = CVarOverlayKey.GetValueOnAnyThread();
			if (Wanted.IsEmpty()) { return false; }

			// Compared by NAME so the cvar can hold any key without a lookup table to fall out of date.
			if (Event.GetKey().GetFName() != FName(*Wanted)) { return false; }

			// Modifiers are deliberately NOT required, and deliberately not REJECTED either: a debug
			// toggle that stops working because Shift happened to be down is a bug report waiting to be
			// filed against the wrong thing.
			FPMOverlay::Get().Toggle();
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] overlay %s by %s"),
				FPMOverlay::Get().IsVisible() ? TEXT("SHOWN") : TEXT("HIDDEN"), *Wanted);
			return true;   // ours, and only ours
		}

		virtual const TCHAR* GetDebugName() const override { return TEXT("FPMOverlayHotkey"); }
	};
}

FPMOverlay& FPMOverlay::Get()
{
	static FPMOverlay Instance;
	return Instance;
}

void FPMOverlay::InstallHotkey()
{
	if (Hotkey.IsValid() || HotkeyRetryHandle.IsValid()) { return; }

	// Slate is usually up by the time a game feature starts, but "usually" is how the residency pin ended
	// up nineteen seconds late today. Try now, retry per frame, stop the moment it takes.
	if (!HotkeyRetry(0.f))
	{
		return;   // registered on the first attempt
	}
	HotkeyRetryHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FPMOverlay::HotkeyRetry), 0.f);
}

bool FPMOverlay::HotkeyRetry(float)
{
	if (Hotkey.IsValid()) { HotkeyRetryHandle.Reset(); return false; }
	if (!FSlateApplication::IsInitialized()) { return true; }   // keep waiting

	Hotkey = MakeShared<FFPMOverlayHotkey>();
	if (!FSlateApplication::Get().RegisterInputPreProcessor(Hotkey))
	{
		// Never fail silently. Without this line a dead hotkey is indistinguishable from a wrong key name,
		// and Ant would reasonably conclude the fourth request was ignored like the first three.
		Hotkey.Reset();
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] overlay hotkey: Slate REFUSED the input pre-processor - no keybind this session. "
			     "FPM.Diag.Overlay still works."));
		HotkeyRetryHandle.Reset();
		return false;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] overlay hotkey armed on '%s' (change with FPM.Diag.OverlayKey, empty to disable)."),
		*CVarOverlayKey.GetValueOnAnyThread());
	HotkeyRetryHandle.Reset();
	return false;
}

void FPMOverlay::Post(const TCHAR* Category, const FString& Line)
{
	/*
	 * LOG WHETHER OR NOT THE PANEL IS UP. A line visible on screen but absent from FactoryGame.log is
	 * one nobody can send us afterwards — the log is the durable half, the panel is the convenient one.
	 *
	 * ⚠ BUT IT HONOURS THE MASTER SWITCH, ADDED 2026-08-08 AFTER REVIEW CALLED IT A BYPASS. Post is
	 * shared by every fix and receives only a category STRING, so it cannot know which channel it
	 * belongs to and must not guess — per-channel gating belongs at the call site, and every current
	 * caller does it. What Post can and must honour is `FPM.Diag 0`, because a master switch that one
	 * function ignores is not a master switch. Without this, "silence everything" left a hole exactly
	 * wide enough for the next caller who forgets to wrap their Post.
	 *
	 * The FEED below is deliberately still written when silenced, so turning diagnostics back on
	 * mid-session shows the history instead of starting blank.
	 */
	if (!FPMDiag::IsSilenced())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s: %s"), Category, *Line);
	}

	FPMOverlay& Self = Get();
	{
		FScopeLock Lock(&Self.LinesLock);
		Self.Lines.Add({ FString(), FString::Printf(TEXT("%-14s %s"), Category, *Line) });
		while (Self.Lines.Num() > MaxLines)
		{
			Self.Lines.RemoveAt(0);
		}
		Self.bDirty = true;
	}
}

void FPMOverlay::PostSticky(const TCHAR* Category, const TCHAR* Key, const FString& Line)
{
	/*
	 * THE LOG STILL GETS EVERY UPDATE. Only the PANEL row is replaced -- the log keeps the full series,
	 * because a gauge's history is exactly what you want when reading a session back afterwards, and it
	 * is the half nobody can reconstruct later. Same master-switch rule as Post: a switch one function
	 * ignores is not a switch.
	 */
	if (!FPMDiag::IsSilenced())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s: %s"), Category, *Line);
	}

	// An empty key would match every other empty-keyed EVENT row and start overwriting the history.
	// Treat it as the caller meaning Post, rather than silently corrupting the feed.
	if (!Key || !*Key)
	{
		FPMOverlay& Fallback = Get();
		FScopeLock Lock(&Fallback.LinesLock);
		Fallback.Lines.Add({ FString(), FString::Printf(TEXT("%-14s %s"), Category, *Line) });
		while (Fallback.Lines.Num() > MaxLines) { Fallback.Lines.RemoveAt(0); }
		Fallback.bDirty = true;
		return;
	}

	FPMOverlay& Self = Get();
	{
		FScopeLock Lock(&Self.LinesLock);
		const FString Text = FString::Printf(TEXT("%-14s %s"), Category, *Line);
		// Rewritten IN PLACE so the row keeps the position it first took. A gauge that jumps to the
		// bottom on every update makes the panel reshuffle under a screenshot, which is the one thing
		// this overlay exists to be good at.
		FRow* Existing = Self.Lines.FindByPredicate([Key](const FRow& R) { return R.Key == Key; });
		if (Existing)
		{
			Existing->Text = Text;
		}
		else
		{
			Self.Lines.Add({ FString(Key), Text });
			/*
			 * ⚠ EVICT AN EVENT, NEVER A GAUGE. Review finding on my own change, before it shipped: the
			 * plain RemoveAt(0) used by Post would happily drop a STICKY row once eighteen events piled
			 * up behind it — and then the hitch summary would silently vanish from the panel and
			 * reappear at the bottom sixty seconds later. That is precisely the "prints forever, scrolls
			 * the useful line away" behaviour this whole change exists to end, so shipping it would have
			 * fixed the symptom and kept the disease.
			 *
			 * Gauges are few (two today: `running` and `world load`) and are the rows worth keeping. If
			 * every row is somehow a gauge there is nothing to evict and the panel simply grows past
			 * MaxLines — bounded in practice by the number of distinct keys, which is code, not input.
			 */
			while (Self.Lines.Num() > MaxLines)
			{
				const int32 Oldest = Self.Lines.IndexOfByPredicate([](const FRow& R) { return R.Key.IsEmpty(); });
				if (Oldest == INDEX_NONE) { break; }
				Self.Lines.RemoveAt(Oldest);
			}
		}
		Self.bDirty = true;
	}
}

void FPMOverlay::Clear()
{
	FPMOverlay& Self = Get();
	{
		FScopeLock Lock(&Self.LinesLock);
		Self.Lines.Reset();
		Self.bDirty = true;
	}
	// Logged rather than silent: a cleared panel and a panel that never received anything look identical
	// on screen, and the log is what tells the two apart when reading a session back.
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] overlay feed cleared (the log above is untouched and still holds every line)."));
}

static FAutoConsoleCommand GFPMOverlayClear(
	TEXT("FPM.Diag.Clear"),
	TEXT("Clear the FPM debug overlay feed. Screen only - the log keeps every line."),
	FConsoleCommandDelegate::CreateStatic(&FPMOverlay::Clear));

void FPMOverlay::SetVisible(bool bInVisible)
{
	bVisible = bInVisible;

	if (bVisible && !TickHandle.IsValid())
	{
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FPMOverlay::Tick), 0.f);
	}

	if (!bVisible && Root.IsValid() && GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->RemoveViewportWidgetContent(Root.ToSharedRef());
		Root.Reset();
		TextBlock.Reset();
	}
}

void FPMOverlay::EnsureAttached()
{
	// The viewport may not exist yet — during early load it genuinely does not. Retry each tick rather
	// than failing once and going quiet, which is how an overlay silently never appears.
	if (Root.IsValid() || !GEngine || !GEngine->GameViewport) { return; }

	/*
	 * ★ WRAP AT THE VIEWPORT EDGE INSTEAD OF RUNNING OFF IT. Ant, 2026-08-09: *"it also cuts off to the
	 * right side of the screen. better if it just went down a row instead"*.
	 *
	 * The hitch summary is one long line by design — every figure carries its denominator, which is the
	 * whole reason this instrument is trusted — so it reliably overruns 1440p and the session totals at
	 * the END of the line were the part being lost. Silently truncating the tail of a measurement is the
	 * worst possible place to truncate.
	 *
	 * A LAMBDA ATTRIBUTE, not a constant: Slate re-evaluates it, so an alt-tab, a resolution change or a
	 * DPI change re-wraps without rebuilding the widget. A width baked in at attach time would be wrong
	 * from the first resize and would look like a rendering bug rather than a stale number.
	 *
	 * Slate works in DPI-scaled units while the viewport reports pixels, so the scale has to be divided
	 * out or the wrap point lands off-screen on any non-100% display.
	 */
	auto WrapWidth = TAttribute<float>::CreateLambda([]() -> float
	{
		constexpr float Fallback = 1400.f;   // sane at 1080p if the viewport cannot be measured
		constexpr float Chrome   = 52.f;     // 16 left offset + 20 border padding + 16 right gutter
		if (!GEngine || !GEngine->GameViewport) { return Fallback; }
		FVector2D ViewportSize(ForceInitToZero);
		GEngine->GameViewport->GetViewportSize(ViewportSize);
		const float DPI = GEngine->GameViewport->GetDPIScale();
		if (ViewportSize.X <= 0.f || DPI <= 0.f) { return Fallback; }
		// Floor rather than clamp-to-fallback: on a genuinely tiny viewport, wrapping hard is correct
		// and still readable, whereas falling back to 1400 would run off the edge again.
		return FMath::Max(320.f, static_cast<float>(ViewportSize.X) / DPI - Chrome);
	});

	SAssignNew(TextBlock, STextBlock)
		.Font(FCoreStyle::GetDefaultFontStyle("Mono", 11))
		.ColorAndOpacity(FLinearColor(0.85f, 0.95f, 1.0f))
		.WrapTextAt(WrapWidth)
		.Text(FText::FromString(TEXT("[FPM] overlay attached")));

	/*
	 * ⚠⚠ HitTestInvisible IS NOT OPTIONAL — WITHOUT IT THIS WIDGET EATS EVERY CLICK IN THE GAME.
	 *
	 * Found the hard way on the 2026-08-08 boot: Ant could not click anything in the main menu.
	 * AddViewportWidgetContent gives the widget a FULL-VIEWPORT slot, and SBorder is hit-testable by
	 * default — so a panel that LOOKS like a small box in the corner is actually an invisible sheet
	 * across the entire screen, absorbing all mouse input. The visible bounds are not the input bounds.
	 *
	 * HitTestInvisible applies to this widget AND all its children, which is what a read-only overlay
	 * wants: it can never take focus, never block a button, never steal a drag. A diagnostic that can
	 * interfere with the thing it observes is not a diagnostic.
	 */
	/*
	 * ⚠ AND THE SECOND HALF OF THE SAME MISTAKE: THE PANEL MUST SIZE ITSELF, NOT THE VIEWPORT.
	 *
	 * Ant, same boot: "the entire screen was very dark". An SBorder placed directly in the viewport slot
	 * FILLS that slot and paints its background across the whole screen — HAlign/VAlign only positioned
	 * the CONTENT inside it, not the border. So the 72%-black background became a full-screen tint, and
	 * the same full-screen extent is what swallowed the clicks.
	 *
	 * SConstraintCanvas with AutoSize gives the border a slot that is exactly as big as its text, anchored
	 * to the top-left. Now the visible bounds and the actual bounds are the same thing — which is the
	 * property that was missing, and the reason one mistake produced two unrelated-looking symptoms.
	 */
	Root = SNew(SConstraintCanvas)
		.Visibility(EVisibility::HitTestInvisible)
		+ SConstraintCanvas::Slot()
			.Anchors(FAnchors(0.f, 0.f, 0.f, 0.f))   // top-left of the viewport
			// Y comes from FPM.Diag.OverlayTop so a hardware monitor along the top edge can be cleared
			// without a build. Clamped at 0: a negative offset puts the panel off-screen, and a debug
			// surface you cannot see reads exactly like a broken one.
			.Offset(FMargin(16.f, FMath::Max(0.f, CVarOverlayTopMargin.GetValueOnAnyThread()), 0.f, 0.f))
			.AutoSize(true)                          // shrink to the content, not the screen
			.Alignment(FVector2D(0.f, 0.f))
			[
				SNew(SBorder)
				.Visibility(EVisibility::HitTestInvisible)
				.BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.72f))
				.Padding(FMargin(10.f, 8.f))
				[
					TextBlock.ToSharedRef()
				]
			];

	// ZOrder above the loading screen. This is the whole point of the widget.
	GEngine->GameViewport->AddViewportWidgetContent(Root.ToSharedRef(), 1000);

	// Honours the master switch: FPM.Diag.Overlay can now detach and reattach the panel mid-session,
	// so an ungated line here would print on every toggle.
	if (!FPMDiag::IsSilenced())
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] overlay: attached to the game viewport"));
	}
}

FString FPMOverlay::ComposeText() const
{
	FScopeLock Lock(&LinesLock);
	if (Lines.Num() == 0) { return TEXT("[FPM] no activity yet"); }
	TArray<FString> Text;
	Text.Reserve(Lines.Num());
	for (const FRow& R : Lines) { Text.Add(R.Text); }
	return FString::Join(Text, TEXT("\n"));
}

bool FPMOverlay::Tick(float DeltaSeconds)
{
	if (!bVisible) { return true; }

	/*
	 * FPM.Diag.Overlay 0 detaches the panel without tearing the feed down: Post() keeps recording, so
	 * turning it back on mid-session shows the history rather than starting blank. Ant asked for a
	 * keybind; until the Game Instance Module's keybind registry is wired up, this is the switch.
	 */
	if (!FPMDiag::IsOn(FPMDiag::EChannel::Overlay))
	{
		if (Root.IsValid() && GEngine && GEngine->GameViewport)
		{
			GEngine->GameViewport->RemoveViewportWidgetContent(Root.ToSharedRef());
			Root.Reset();
			TextBlock.Reset();
		}
		return true;
	}

	EnsureAttached();

	SinceRefresh += DeltaSeconds;
	if (SinceRefresh < RefreshSeconds) { return true; }
	SinceRefresh = 0.f;

	if (bDirty && TextBlock.IsValid())
	{
		TextBlock->SetText(FText::FromString(ComposeText()));
		bDirty = false;
	}
	return true; // keep ticking
}

void FPMOverlay::Shutdown()
{
	SetVisible(false);
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	// The pre-processor sits in Slate's own list. Left behind, it would keep receiving every keystroke in
	// the process after this module's code is gone -- which is a crash, not a leak.
	if (HotkeyRetryHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HotkeyRetryHandle);
		HotkeyRetryHandle.Reset();
	}
	if (Hotkey.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(Hotkey);
		}
		Hotkey.Reset();
	}
}
