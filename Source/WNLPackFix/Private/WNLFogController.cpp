#include "WNLFogController.h"
#include "WNLPackFix.h"

#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

#include "EngineUtils.h"                              // TActorIterator
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Buildables/FGBuildable.h"

FWNLFogController& FWNLFogController::Get()
{
	static FWNLFogController Instance;
	return Instance;
}

void FWNLFogController::Start()
{
	if (bStarted || IsRunningDedicatedServer())
	{
		return;
	}
	LoadConfig();
	if (!bEnabled)
	{
		UE_LOG(LogWNLPackFix, Display, TEXT("[WNLPackFix] indoor-fog controller disabled via config"));
		return;
	}
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FWNLFogController::Tick), 0.0f); // every frame; work is trivial
	bStarted = true;
	UE_LOG(LogWNLPackFix, Display,
		TEXT("[WNLPackFix] indoor-fog controller armed: adaptive bubble (cap %.0fm, wall-bias %.2f), transition %.1fs"),
		IndoorStartDistance / 100.f, WallBias, TransitionSec);
}

void FWNLFogController::Stop()
{
	if (bStarted)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		bStarted = false;
	}
}

void FWNLFogController::LoadConfig()
{
	const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("Configs"), TEXT("WNLPackFix.cfg"));
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path))
	{
		return; // governor writes the file with defaults; missing = use ours
	}
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return;
	}
	// One parse routine for a "Fog" json object; used for the main file AND the in-game menu below.
	auto ApplyFogObject = [this](const TSharedPtr<FJsonObject>& F)
	{
		F->TryGetBoolField(TEXT("Enabled"), bEnabled);
		F->TryGetNumberField(TEXT("IndoorStartDistance"), IndoorStartDistance);
		F->TryGetNumberField(TEXT("TransitionSec"), TransitionSec);
		F->TryGetNumberField(TEXT("RoofTraceUp"), RoofTraceUp);
		F->TryGetNumberField(TEXT("CheckInterval"), CheckInterval);
		F->TryGetNumberField(TEXT("MinBubble"), MinBubble);
		F->TryGetNumberField(TEXT("WallBias"), WallBias);
		F->TryGetNumberField(TEXT("GrowLerp"), GrowLerp);
		F->TryGetNumberField(TEXT("ShrinkLerp"), ShrinkLerp);
		F->TryGetNumberField(TEXT("SealCountMin"), SealCountMin);
	};
	const TSharedPtr<FJsonObject>* Fog;
	if (Json->TryGetObjectField(TEXT("Fog"), Fog))
	{
		ApplyFogObject(*Fog);
	}
	// In-game config MENU overlay (SML settings page → Configs/WNLPackFix/Menu.cfg, "Fog" section):
	// menu values win over the main file, mirroring the governor's merge. Absent file = nothing to do.
	const FString MenuPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Configs"), TEXT("WNLPackFix"), TEXT("Menu.cfg"));
	FString MenuRaw;
	TSharedPtr<FJsonObject> MenuJson;
	if (FFileHelper::LoadFileToString(MenuRaw, *MenuPath) &&
	    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MenuRaw), MenuJson) && MenuJson.IsValid() &&
	    MenuJson->TryGetObjectField(TEXT("Fog"), Fog))
	{
		ApplyFogObject(*Fog);
	}
	// Clamp so a hand-edit can't invert the fade or ask for a silly bubble.
	IndoorStartDistance = FMath::Clamp(IndoorStartDistance, 0.f, 50000.f);
	TransitionSec       = FMath::Clamp(TransitionSec, 0.25f, 30.f);
	RoofTraceUp         = FMath::Clamp(RoofTraceUp, 200.f, 20000.f);
	CheckInterval       = FMath::Clamp(CheckInterval, 0.05f, 2.f);
	MinBubble           = FMath::Clamp(MinBubble, 0.f, IndoorStartDistance);
	WallBias            = FMath::Clamp(WallBias, 0.5f, 1.f);
	GrowLerp            = FMath::Clamp(GrowLerp, 0.05f, 1.f);
	ShrinkLerp          = FMath::Clamp(ShrinkLerp, 0.05f, 1.f);
	SealCountMin        = FMath::Clamp(SealCountMin, 1, 13);
}

UExponentialHeightFogComponent* FWNLFogController::FindFog(UWorld* World)
{
	if (FogComp.IsValid())
	{
		return FogComp.Get();
	}
	// Cache miss = first acquisition OR a world change invalidated the old component. Reset all
	// per-world state so a stale baseline/bubble from the PREVIOUS world is never written into a
	// fresh one (review finding): with bControlling false and CurrentStart 0, the same tick
	// re-adopts the new world's real StartDistance and early-returns without writing.
	BaselineStart = -1.f;
	bControlling  = false;
	SmoothedNear  = -1.f;
	CurrentStart  = 0.f;
	TargetStart   = 0.f;
	for (TActorIterator<AExponentialHeightFog> It(World); It; ++It)
	{
		if (UExponentialHeightFogComponent* Comp = It->GetComponent())
		{
			FogComp = Comp;
			return Comp;
		}
	}
	return nullptr;
}

// Weighted set of sample directions, sky-biased (fog leaks from the open sky above, so an
// unblocked upward direction matters most). Fully-blocked-by-buildables in every direction = 1.
namespace
{
	struct FSampleRay { FVector Dir; float Weight; };

	const TArray<FSampleRay>& EnclosureRays()
	{
		static TArray<FSampleRay> Rays;
		if (Rays.Num() == 0)
		{
			Rays.Add({ FVector(0, 0, 1), 3.f }); // straight up — the roof
			const int32 Az = 6;
			const float Hi = FMath::DegreesToRadians(60.f); // near-vertical ring
			const float Lo = FMath::DegreesToRadians(28.f); // near-horizon ring (open walls escape here)
			for (int32 i = 0; i < Az; ++i)
			{
				const float A = (2.f * PI * i) / Az;
				Rays.Add({ FVector(FMath::Cos(A) * FMath::Cos(Hi), FMath::Sin(A) * FMath::Cos(Hi), FMath::Sin(Hi)), 2.f });
				Rays.Add({ FVector(FMath::Cos(A) * FMath::Cos(Lo), FMath::Sin(A) * FMath::Cos(Lo), FMath::Sin(Lo)), 1.f });
			}
		}
		return Rays;
	}
}

FWNLFogController::FEnclosure FWNLFogController::ComputeEnclosure(UWorld* World)
{
	FEnclosure E;
	APlayerController* PC = GEngine ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return E; // no body (menu/spectator) -> fully open, fog stays normal
	}
	// StartDistance is CAMERA-relative, so measure the room from the camera too (matches the lever).
	const FVector Start = (PC->PlayerCameraManager)
		? PC->PlayerCameraManager->GetCameraLocation()
		: Pawn->GetActorLocation() + FVector(0, 0, 50.f);
	// Reach far enough to actually measure a big hall (the old 40 m cap couldn't size a large room).
	const float MaxTrace = FMath::Max(RoofTraceUp, IndoorStartDistance);
	FCollisionQueryParams Params(FName(TEXT("WNLFogEnclosure")), /*bTraceComplex*/ false, Pawn);

	float Blocked = 0.f, Total = 0.f;
	TArray<float, TInlineAllocator<13>> Seals; // sealing-ray hit distances (cm)
	for (const FSampleRay& Ray : EnclosureRays())
	{
		Total += Ray.Weight;
		FHitResult Hit;
		// A hit only counts as "sealing" if it's a player buildable — terrain/cave ceilings are
		// AActor/landscape, not AFGBuildable, so natural cover keeps its fog. Glass walls/windows
		// ARE buildables, so they seal (a sealed greenhouse has still, clear interior air); god-rays
		// still pour through them because we never touch volumetric fog.
		if (World->LineTraceSingleByChannel(Hit, Start, Start + Ray.Dir * MaxTrace, ECC_WorldStatic, Params)
			&& Hit.GetActor() && Hit.GetActor()->IsA<AFGBuildable>())
		{
			Blocked += Ray.Weight;
			Seals.Add(Hit.Distance);
		}
	}
	E.Fraction  = (Total > 0.f) ? (Blocked / Total) : 0.f;
	E.SealCount = Seals.Num();
	if (E.SealCount > 0)
	{
		Seals.Sort();
		// Drop the single nearest reading as a likely outlier (a pillar/rail right by the camera)
		// once we have enough seals; otherwise take the nearest we have.
		E.NearSeal = (E.SealCount >= 3) ? Seals[1] : Seals[0];
	}
	return E;
}

bool FWNLFogController::Tick(float DeltaTime)
{
	UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
	if (!World || !World->IsGameWorld() || DeltaTime <= 0.f || DeltaTime > 1.f)
	{
		return true;
	}

	// Re-evaluate enclosure at a coarse cadence (cheap: ~13 short traces, 4x/sec — this is how we
	// avoid a per-frame recheck cost); the fade itself runs every frame.
	const double Now = FPlatformTime::Seconds();
	if (Now - LastCheck >= CheckInterval)
	{
		LastCheck = Now;
		// Until we've captured the game's baseline StartDistance, hold at 0 so we never yank fog
		// before we know the intended value.
		const float Base = (BaselineStart >= 0.f) ? BaselineStart : 0.f;
		const FEnclosure E = ComputeEnclosure(World);
		if (E.SealCount < SealCountMin || E.NearSeal < 0.f)
		{
			// Not enough sealing to call this an interior → let the game own the fog (no bubble).
			SmoothedNear = -1.f;
			TargetStart  = Base;
		}
		else
		{
			// Size the bubble to the NEAREST WALL (capped) so height-fog resumes right at the shell
			// and the view out a door/window keeps its outdoor fog. Asymmetric smoothing: shrink
			// fast (a doorway must not briefly de-fog the outside), grow slow (don't pulse bigger on
			// a lucky far reading).
			const float Raw = FMath::Clamp(E.NearSeal * WallBias, MinBubble, IndoorStartDistance);
			if (SmoothedNear < 0.f)
			{
				SmoothedNear = Raw; // first capture — no ramp
			}
			else
			{
				const float A = (Raw < SmoothedNear) ? ShrinkLerp : GrowLerp;
				SmoothedNear = FMath::Lerp(SmoothedNear, Raw, A);
			}
			// Holes still leak: partial enclosure scales the bubble down toward the baseline.
			TargetStart = FMath::Lerp(Base, SmoothedNear, E.Fraction);
		}
		// Observability for the boot test: how sealed, how big the room read, where the bubble is heading.
		UE_LOG(LogWNLPackFix, Verbose,
			TEXT("[WNLPackFix] fog: seals %d, nearWall %.0fcm, frac %.2f -> bubble %.0fcm"),
			E.SealCount, E.NearSeal, E.Fraction, TargetStart);
	}

	// Long, frame-rate-independent fade toward the target (constant cm/sec so the full range takes
	// ~TransitionSec regardless of framerate).
	const float Speed = FMath::Max(IndoorStartDistance, 1.f) / FMath::Max(TransitionSec, 0.01f);
	CurrentStart = FMath::FInterpConstantTo(CurrentStart, TargetStart, DeltaTime, Speed);

	UExponentialHeightFogComponent* Fog = FindFog(World);
	if (!Fog)
	{
		return true;
	}

	// While hands-off, re-adopt whatever StartDistance the game currently has as our baseline (it
	// rarely changes it, but this keeps us riding the game's intent instead of a stale capture).
	if (!bControlling)
	{
		BaselineStart = Fog->StartDistance;
	}
	const float Base = (BaselineStart >= 0.f) ? BaselineStart : 0.f;

	// "Controlling" = our bubble is meaningfully bigger than the game's baseline.
	const bool bWantControl = CurrentStart > Base + 1.f;
	if (!bWantControl && !bControlling)
	{
		return true; // fully outdoors and hands-off: let the game own its fog entirely
	}

	if (bWantControl)
	{
		Fog->SetStartDistance(CurrentStart);
		bControlling = true;
	}
	else
	{
		// Faded all the way back: restore the game's baseline once and hand control back clean.
		Fog->SetStartDistance(Base);
		CurrentStart = Base;
		SmoothedNear = -1.f; // re-capture the room size fresh on the next entry
		bControlling = false;
	}
	return true;
}
