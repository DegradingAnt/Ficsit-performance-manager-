// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Configuration/FPMSettingsConfig.h"

#include "FicsitsPerformanceManager.h"

#include "Configuration/ConfigProperty.h"
#include "Configuration/Properties/ConfigPropertyBool.h"
#include "Configuration/Properties/ConfigPropertyFloat.h"
#include "Configuration/Properties/ConfigPropertyInteger.h"
#include "Configuration/Properties/ConfigPropertySection.h"
#include "Configuration/Properties/WidgetExtension/CP_Float.h"
#include "Configuration/Properties/WidgetExtension/CP_Integer.h"
#include "Configuration/Properties/WidgetExtension/CP_Section.h"

#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "UObject/ConstructorHelpers.h"

#define LOCTEXT_NAMESPACE "FPMSettingsConfig"

namespace
{
	/** Where SML keeps the Blueprint subclasses that can actually draw a row. See the header. */
	const TCHAR* const GFPMSMLPropertyPath = TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/");

	/**
	 * Resolve one of SML's `BP_ConfigProperty*` classes, falling back to the plain C++ class.
	 *
	 * The fallback is deliberately NOT silent: with the C++ class the config still registers, saves and
	 * loads, and the page renders nothing — which looks identical to "the mod has no settings". FPM1
	 * lost a whole boot cycle to that, so the failure is loud and says what the player will see.
	 */
	template <typename TBase>
	UClass* ResolveSMLPropertyClass(const TCHAR* AssetName)
	{
		const FString Path = GFPMSMLPropertyPath + FString(AssetName);
		ConstructorHelpers::FClassFinder<TBase> Finder(*Path);
		if (Finder.Succeeded())
		{
			return Finder.Class;
		}

		UE_LOG(LogFicsitsPerformanceManager, Error,
			TEXT("[FPM] settings: SML widget class '%s' would not load. The config still saves and loads, "
			     "but that part of the page will RENDER NOTHING - which looks exactly like FPM having no "
			     "settings at all."), *Path);
		return TBase::StaticClass();
	}

	/**
	 * ★ THE NAME IS THE BINDING. A row's subobject name is its cvar with dots replaced by underscores,
	 * so `FPM.Upscaler.DLSSPreset` becomes `FPM_Upscaler_DLSSPreset`. There is no mapping table to keep
	 * in step with anything, because there is no mapping.
	 */
	FString SubobjectNameFor(const TCHAR* CVarName)
	{
		return FString(CVarName).Replace(TEXT("."), TEXT("_"));
	}

	FString CVarNameFor(const UObject* Row)
	{
		return Row->GetName().Replace(TEXT("_"), TEXT("."));
	}

	/*
	 * ★ ONE DEFAULT, ONE SITE. Each config row now takes its default FROM ITS OWN CVAR
	 * (IConsoleVariable::GetDefaultValue(), IConsoleManager.h:630 - "the value this CVar was
	 * constructed with") instead of a second hand-typed literal at the AddInt/AddBool/AddFloat call
	 * site below. FPM.Diag.Overlay shipped with the two disagreeing (cvar default 1, row literal
	 * false) because the row's default was hand-typed a second time and drifted from the cvar's own
	 * declaration. This removes the second typing for all six rows, not only the one that had
	 * already drifted, so the same drift cannot happen to any of the other five later.
	 */
	IConsoleVariable* FindCVarOrFatal(const TCHAR* CVarName)
	{
		IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(CVarName);
		if (Var == nullptr)
		{
			/*
			 * ⚠ FATAL, NOT A LOGGED FALLBACK. This runs at CDO construction from a hand-typed cvar name
			 * that some OTHER FPM source file must already have registered - a null here can only mean
			 * a typo here or a cvar renamed out from under the settings page, i.e. a build-time defect,
			 * never something a player's own actions can trigger. checkf was considered and rejected:
			 * DO_CHECK collapses to USE_CHECKS_IN_SHIPPING (engine Build.h:305), which is 0 in the
			 * Shipping config this mod ships as, so checkf would compile to nothing and leave the next
			 * line as an unguarded null-pointer read with no message at all. UE_LOG at Fatal verbosity
			 * is not gated by DO_CHECK - it survives even NO_LOGGING (engine LogMacros.h:147-159) - so
			 * it fails loud with the cvar name in every build configuration, Shipping included.
			 */
			UE_LOG(LogFicsitsPerformanceManager, Fatal,
				TEXT("[FPM] settings: row for '%s' built before its cvar was registered. Typo in the "
				     "row, or the cvar was renamed."), CVarName);
		}
		return Var;
	}

	int32 CompiledIntDefault(const TCHAR* CVarName)
	{
		return FCString::Atoi(*FindCVarOrFatal(CVarName)->GetDefaultValue());
	}

	bool CompiledBoolDefault(const TCHAR* CVarName)
	{
		return FindCVarOrFatal(CVarName)->GetDefaultValue().ToBool();
	}

	float CompiledFloatDefault(const TCHAR* CVarName)
	{
		return FCString::Atof(*FindCVarOrFatal(CVarName)->GetDefaultValue());
	}
}

UFPMSettingsConfig::UFPMSettingsConfig()
{
	/*
	 * ⚠ EMPTY CATEGORY IS LOAD-BEARING. The Mods menu looks a config up by mod reference with a blank
	 * category; a non-empty one renders an empty page while registration, save and load all keep
	 * working. FPM1 recorded this as boot-test bug 2.
	 */
	ConfigId.ModReference   = TEXT("FicsitsPerformanceManager");
	ConfigId.ConfigCategory = FString();

	DisplayName = LOCTEXT("ModDisplayName", "Ficsit's Performance Manager");
	Description = LOCTEXT("ModDescription",
		"Fixes for standing bugs in the game and in mod interactions, plus a few levers the game does "
		"not expose. Everything here writes to a console variable and nothing else - no game setting is "
		"modified, and uninstalling removes this file with it.");

	UClass* const ClsSection = ResolveSMLPropertyClass<UConfigPropertySection>(TEXT("BP_ConfigPropertySection"));
	UClass* const ClsInt     = ResolveSMLPropertyClass<UConfigPropertyInteger>(TEXT("BP_ConfigPropertyInteger"));
	UClass* const ClsBool    = ResolveSMLPropertyClass<UConfigPropertyBool>   (TEXT("BP_ConfigPropertyBool"));
	UClass* const ClsFloat   = ResolveSMLPropertyClass<UConfigPropertyFloat>  (TEXT("BP_ConfigPropertyFloat"));

	RootSection = static_cast<UConfigPropertySection*>(CreateDefaultSubobject(
		TEXT("RootSection"), UConfigPropertySection::StaticClass(), ClsSection, true, false));

	/*
	 * Sections carry no usable default for these — the C++ stub leaves them zero-initialised and the
	 * Blueprint CDO's values are not ours to rely on — so they are set outright.
	 */
	/*
	 * ⚠ THE LAYOUT FIELDS LIVE ON UCP_Section, NOT ON UConfigPropertySection - and getting that wrong
	 * is what the first build of this file did. The base class carries only SectionProperties; the
	 * widget-extension subclass adds WidgetType and HasHeader
	 * (Properties/WidgetExtension/CP_Section.h:17-21). FPM1's code casts to UCP_Section for exactly
	 * this reason and I copied the field names without noticing the cast, which is the "read the
	 * declaration, not the usage" lesson twice in one day.
	 *
	 * The cast can fail if SML's BP class did not resolve - the section still works, it just uses the
	 * Blueprint CDO's layout instead of ours.
	 */
	if (UCP_Section* Root = Cast<UCP_Section>(RootSection))
	{
		Root->WidgetType = ECP_SectionWidgetType::CPS_Vertical;
		Root->HasHeader  = false;   // the mod page already draws the title
	}

	auto AddSection = [this, ClsSection](const TCHAR* Key, const FText& Display) -> UConfigPropertySection*
	{
		UConfigPropertySection* S = static_cast<UConfigPropertySection*>(CreateDefaultSubobject(
			*FString::Printf(TEXT("Sec_%s"), Key), UConfigPropertySection::StaticClass(), ClsSection, true, false));
		S->DisplayName = Display;
		if (UCP_Section* W = Cast<UCP_Section>(S))
		{
			W->WidgetType = ECP_SectionWidgetType::CPS_Vertical;
			W->HasHeader   = true;
			/*
			 * ⚠ HasHeader ALONE DRAWS AN EMPTY BAR. The widget renders HeaderText (CP_Section.h:23), a
			 * field separate from the base UConfigProperty::DisplayName set above - setting only
			 * DisplayName left every section (Upscaler, Weather, Diagnostics) with a blank header.
			 */
			W->HeaderText  = Display;
		}
		if (UConfigPropertySection* Root = Cast<UConfigPropertySection>(RootSection))
		{
			Root->SectionProperties.Add(Key, S);
		}
		return S;
	};

	/*
	 * One helper per type. Each takes THE CVAR NAME and derives the subobject name from it, which is
	 * what makes the binding unbreakable — see SubobjectNameFor.
	 */
	/*
	 * ★★★ THE BINDING THAT WAS MISSING, AND ITS ABSENCE MADE THIS WHOLE PAGE A DEAD INSTRUMENT.
	 *
	 * Found by a vox-review pass on 2026-08-11, minutes after shipping: `SyncAllToCVars` was written,
	 * documented and never CALLED. Every row would have rendered, saved to the .cfg and reached no cvar
	 * at all - a settings page where nothing does anything, and nothing in a build or a structure gate
	 * can see that.
	 *
	 * ⚠ BINDING ON THE CDO IS CORRECT HERE, which is unusual enough to state. SML does not instantiate
	 * the configuration: `UConfigManager::RegisterModConfiguration` takes
	 * `Configuration.GetDefaultObject()->RootSection` as the LIVE value
	 * (ConfigManager.cpp:280) and then loads the file into it (:287). The CDO's subobjects ARE the
	 * player's settings, so a constructor-time bind is binding the real thing.
	 */
	auto BindRow = [this](UConfigProperty* P)
	{
		/*
		 * RequestSync, NOT SyncAllToCVars DIRECTLY. Binding straight to SyncAllToCVars was the shape
		 * that shipped 2026-08-15 and it under-counted its own cost: SML fires this delegate once PER
		 * ROW (MarkDirty(), ConfigProperty.cpp:18-27), and a Section's ResetToDefault cascades a reset to
		 * every child, each of which calls its own MarkDirty (ConfigPropertySection.cpp:102-118) - so a
		 * reset touching this page's whole tree fires it six times, not once. SyncAllToCVars walks ALL of
		 * BoundRows on every call, so that meant six full-table re-syncs instead of one. RequestSync
		 * coalesces the burst into a single deferred SyncAllToCVars() - see its header comment.
		 */
		P->OnPropertyValueChanged.AddDynamic(this, &UFPMSettingsConfig::RequestSync);
		BoundRows.Add(P);
	};

	auto AddInt = [this, ClsInt, &BindRow](UConfigPropertySection* Sec, const TCHAR* CVarName,
	                             const FText& Display, const FText& Tip,
	                             int32 Min, int32 Max)
	{
		const FString SubName = SubobjectNameFor(CVarName);
		const int32 Default = CompiledIntDefault(CVarName);
		UConfigPropertyInteger* P = static_cast<UConfigPropertyInteger*>(CreateDefaultSubobject(
			*SubName, UConfigPropertyInteger::StaticClass(), ClsInt, true, false));
		P->DisplayName   = Display;
		P->Tooltip       = Tip;
		P->DefaultValue  = Default;
		P->Value         = Default;

		/*
		 * Min/Max and the widget choice live on UCP_Integer, the widget-extension subclass
		 * (CP_Integer.h:22-28), not on UConfigPropertyInteger.
		 *
		 * ⚠ SPINBOX, NOT CPI_Enum, AND THAT IS A DELIBERATE HOLD. CP_Integer.h:11 offers
		 * "CPI_Enum - A DropDown list of Enum Field Names", which would be far better for the DLSS
		 * preset than a number line where 8 and 9 are invalid. But the DLSS values are NON-CONTIGUOUS
		 * (0, 10, 11), and nothing in the header says whether the widget stores the enum's VALUE or its
		 * INDEX. If it stores the index, a non-contiguous enum silently writes the wrong number - a
		 * control that looks right and does something else. Settle that with one look at
		 * UCP_Integer::GetEnumNames' consumer before switching.
		 */
		if (UCP_Integer* W = Cast<UCP_Integer>(P))
		{
			W->WidgetType = ECP_IntegerWidgetType::CPI_Spinbox;
			W->MinValue   = Min;
			W->MaxValue   = Max;
		}
		Sec->SectionProperties.Add(SubName, P);
		BindRow(P);
	};

	auto AddBool = [this, ClsBool, &BindRow](UConfigPropertySection* Sec, const TCHAR* CVarName,
	                               const FText& Display, const FText& Tip)
	{
		const FString SubName = SubobjectNameFor(CVarName);
		const bool Default = CompiledBoolDefault(CVarName);
		UConfigPropertyBool* P = static_cast<UConfigPropertyBool*>(CreateDefaultSubobject(
			*SubName, UConfigPropertyBool::StaticClass(), ClsBool, true, false));
		P->DisplayName  = Display;
		P->Tooltip      = Tip;
		P->DefaultValue = Default;
		P->Value        = Default;
		Sec->SectionProperties.Add(SubName, P);
		BindRow(P);
	};

	auto AddFloat = [this, ClsFloat, &BindRow](UConfigPropertySection* Sec, const TCHAR* CVarName,
	                                           const FText& Display, const FText& Tip,
	                                           float Min, float Max)
	{
		const FString SubName = SubobjectNameFor(CVarName);
		const float Default = CompiledFloatDefault(CVarName);
		UConfigPropertyFloat* P = static_cast<UConfigPropertyFloat*>(CreateDefaultSubobject(
			*SubName, UConfigPropertyFloat::StaticClass(), ClsFloat, true, false));
		P->DisplayName  = Display;
		P->Tooltip      = Tip;
		P->DefaultValue = Default;
		P->Value        = Default;
		if (UCP_Float* W = Cast<UCP_Float>(P))
		{
			W->WidgetType = ECP_FloatWidgetType::CPF_Slider;
			W->MinValue   = Min;
			W->MaxValue   = Max;
		}
		Sec->SectionProperties.Add(SubName, P);
		BindRow(P);
	};

	// ── UPSCALER ────────────────────────────────────────────────────────────────────────────────────
	UConfigPropertySection* Upscaler = AddSection(TEXT("Upscaler"),
		LOCTEXT("SecUpscaler", "Upscaler"));

	AddInt(Upscaler, TEXT("FPM.Upscaler.DLSSPreset"),
		LOCTEXT("DLSSPreset", "DLSS preset"),
		LOCTEXT("DLSSPresetTip",
			"0 leaves the game's own choice alone - it asks for Preset C, the old model, which smears "
			"things that move across other surfaces. 10 and 11 are the newer transformer presets J and "
			"K. Which of the two looks better is a matter of taste; try both. Only affects DLSS."),
		0, 11);

	AddInt(Upscaler, TEXT("FPM.Reflex.Mode"),
		LOCTEXT("ReflexMode", "NVIDIA Reflex"),
		LOCTEXT("ReflexModeTip",
			"0 leaves it alone, 1 is low latency, 2 adds Boost. The game ships Reflex switched off. "
			"1 costs up to 4% of your frame rate when the graphics card is the bottleneck and is close "
			"to free otherwise. 2 can cost frames AND power, so it is not a sensible default."),
		0, 2);

	AddFloat(Upscaler, TEXT("FPM.Sharpness.Amount"),
		LOCTEXT("Sharpness", "Sharpening"),
		LOCTEXT("SharpnessTip",
			"0 leaves the game's own sharpening alone. The control used depends on which upscaler you "
			"are running - FSR has its own, and TSR or no upscaler uses the tonemapper's. DLSS and XeSS "
			"sharpen inside their own pass, so this does nothing for them and says so in the log rather "
			"than pretending."),
		0.f, 2.f);

	// ── WEATHER ─────────────────────────────────────────────────────────────────────────────────────
	UConfigPropertySection* Weather = AddSection(TEXT("Weather"),
		LOCTEXT("SecWeather", "Weather and particles"));

	AddBool(Weather, TEXT("FPM.Weather.Gate"),
		LOCTEXT("WeatherGate", "Quieten weather indoors"),
		LOCTEXT("WeatherGateTip",
			"Scales rain and wind particles down while you are inside a sealed room. It is not "
			"collision - three of the game's five weather systems ship with no collision at all - so "
			"this is the cheap half of the problem, not the whole of it."));

	// ── DIAGNOSTICS ─────────────────────────────────────────────────────────────────────────────────
	UConfigPropertySection* Diag = AddSection(TEXT("Diagnostics"),
		LOCTEXT("SecDiag", "Diagnostics"));

	AddInt(Diag, TEXT("FPM.Diag"),
		LOCTEXT("DiagMaster", "Log detail"),
		LOCTEXT("DiagMasterTip",
			"-1 leaves each area at its own setting, 0 silences everything FPM prints, 1 is normal and "
			"2 is verbose. This only changes what is written to the log - it never changes what the mod "
			"does. If you are sending a log to someone, leave it at -1."),
		-1, 2);

	AddBool(Diag, TEXT("FPM.Diag.Overlay"),
		LOCTEXT("Overlay", "On-screen readout"),
		LOCTEXT("OverlayTip",
			"The developer feed in the corner. F8 toggles it in game."));
}

void UFPMSettingsConfig::SyncAllToCVars()
{
	IConsoleManager& Console = IConsoleManager::Get();

	int32 Written = 0, Missing = 0, Mismatched = 0;

	for (UConfigProperty* Row : BoundRows)
	{
		if (Row == nullptr) { continue; }

		const FString CVarName = CVarNameFor(Row);
		IConsoleVariable* Var = Console.FindConsoleVariable(*CVarName);

		/*
		 * ⚠ A MISSING CVAR IS A LOUD FAILURE, NOT A SKIP. The row's name IS the cvar name, so this can
		 * only mean a typo in the row or a cvar that was renamed out from under it - and in both cases
		 * the player has a control that silently does nothing. That is the exact shape this page's
		 * design was chosen to prevent, so it must never pass quietly.
		 */
		if (Var == nullptr)
		{
			++Missing;
			UE_LOG(LogFicsitsPerformanceManager, Error,
				TEXT("[FPM] settings: row '%s' maps to cvar '%s', which does not exist. That control does "
				     "NOTHING. Either the row is misnamed or the cvar was renamed."),
				*Row->GetName(), *CVarName);
			continue;
		}

		int32 Want = 0;
		if (const UConfigPropertyInteger* AsInt = Cast<UConfigPropertyInteger>(Row))
		{
			Want = AsInt->Value;
		}
		else if (const UConfigPropertyBool* AsBool = Cast<UConfigPropertyBool>(Row))
		{
			Want = AsBool->Value ? 1 : 0;
		}
		else if (const UConfigPropertyFloat* AsFloat = Cast<UConfigPropertyFloat>(Row))
		{
			/*
			 * ⚠ FLOATS GO THROUGH SetFloat, NOT THROUGH THE int32 PATH. An earlier version of this page
			 * had no float branch at all, so a float row would have fallen through to `continue` and
			 * silently controlled nothing - which is why FPM.Sharpness.Amount was deliberately kept OFF
			 * the page until this existed rather than shipped as an Int row that truncates 0.5 to 0.
			 */
			Var->Set(AsFloat->Value, ECVF_SetByCode);
			++Written;

			if (!FMath::IsNearlyEqual(Var->GetFloat(), AsFloat->Value, KINDA_SMALL_NUMBER))
			{
				++Mismatched;
				UE_LOG(LogFicsitsPerformanceManager, Warning,
					TEXT("[FPM] settings: wrote %.3f to '%s' and read back %.3f. Something outranks this "
					     "write."), AsFloat->Value, *CVarName, Var->GetFloat());
			}
			continue;
		}
		else
		{
			continue;   // no other row types on this page yet
		}

		/*
		 * ECVF_SetByCode, NOT the writer's tagged hold. These are the PLAYER'S choices, expressed
		 * through FPM's own cvars, and the fixes that consume them do their own holding of GAME cvars
		 * through FPMCVarWriter. Writing our own settings through the release-tracked path would put
		 * the player's preferences into the ledger that ReleaseAll empties.
		 */
		Var->Set(Want, ECVF_SetByCode);
		++Written;

		/*
		 * ★ READ IT BACK. Several of the values these rows ultimately drive are owned by FactoryGame,
		 * and a page that reports success while the write was refused is worth nothing. This checks the
		 * FPM cvar itself, which is the part this page is responsible for.
		 */
		if (Var->GetInt() != Want)
		{
			++Mismatched;
			UE_LOG(LogFicsitsPerformanceManager, Warning,
				TEXT("[FPM] settings: wrote %d to '%s' and read back %d. Something outranks this write."),
				Want, *CVarName, Var->GetInt());
		}
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] settings applied: %d row(s) written, %d missing cvar(s), %d that did not stick."),
		Written, Missing, Mismatched);
}

void UFPMSettingsConfig::RequestSync()
{
	/*
	 * ⚠ COALESCE ON THE WAY IN, NOT BY QUIETING THE LOG ON THE WAY OUT. The bug this exists to fix was
	 * never the log line - it was SyncAllToCVars() itself running once per row in a same-frame burst,
	 * each run re-writing and re-verifying every one of BoundRows. De-duplicating the log message would
	 * have hidden that the work was still happening N times; this stops the work from happening N times,
	 * so the log stays an honest count of how many full syncs actually ran.
	 *
	 * `bSyncPending` absorbs every firing that lands before the deferred call runs, so a six-row burst
	 * schedules exactly one tick, not six. Cleared BEFORE calling SyncAllToCVars(), not after, so a row
	 * that changes again while this pass is running schedules its own follow-up instead of the flag
	 * getting stuck true forever.
	 */
	if (bSyncPending) { return; }
	bSyncPending = true;

	TWeakObjectPtr<UFPMSettingsConfig> WeakThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float) -> bool
	{
		if (UFPMSettingsConfig* Self = WeakThis.Get())
		{
			Self->bSyncPending = false;
			Self->SyncAllToCVars();
		}
		return false;   // one-shot: coalesce, don't repeat
	}), 0.0f);   // next tick - explicit, not relied on as a default
}

#undef LOCTEXT_NAMESPACE
