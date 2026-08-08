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

#include "FicsitsPerformanceManager.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

FPMOverlay& FPMOverlay::Get()
{
	static FPMOverlay Instance;
	return Instance;
}

void FPMOverlay::Post(const TCHAR* Category, const FString& Line)
{
	// ALWAYS LOG, whether or not the panel is up. A line visible on screen but absent from
	// FactoryGame.log is one nobody can send us afterwards.
	UE_LOG(LogFicsitsPerformanceManager, Display, TEXT("[FPM] %s: %s"), Category, *Line);

	FPMOverlay& Self = Get();
	{
		FScopeLock Lock(&Self.LinesLock);
		Self.Lines.Add(FString::Printf(TEXT("%-14s %s"), Category, *Line));
		while (Self.Lines.Num() > MaxLines)
		{
			Self.Lines.RemoveAt(0);
		}
		Self.bDirty = true;
	}
}

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

	SAssignNew(TextBlock, STextBlock)
		.Font(FCoreStyle::GetDefaultFontStyle("Mono", 11))
		.ColorAndOpacity(FLinearColor(0.85f, 0.95f, 1.0f))
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
			.Offset(FMargin(16.f, 16.f, 0.f, 0.f))
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

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] overlay: attached to the game viewport"));
}

FString FPMOverlay::ComposeText() const
{
	FScopeLock Lock(&LinesLock);
	if (Lines.Num() == 0) { return TEXT("[FPM] no activity yet"); }
	return FString::Join(Lines, TEXT("\n"));
}

bool FPMOverlay::Tick(float DeltaSeconds)
{
	if (!bVisible) { return true; }

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
}
