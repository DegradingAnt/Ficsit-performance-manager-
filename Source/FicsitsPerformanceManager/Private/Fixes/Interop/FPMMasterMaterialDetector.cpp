// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Fixes/Interop/FPMMasterMaterialDetector.h"

#include "Resources/FGItemDescriptor.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "UObject/UObjectHash.h"

#include "FicsitsPerformanceManager.h"
#include "Core/FPMDetectorRegistry.h"
#include "Core/FPMOverlay.h"

namespace
{
	const TCHAR* GVanillaPrefix = TEXT("/Game/FactoryGame/");

	/** Every root UMaterial reachable from a loaded item descriptor's conveyor-mesh material
	 *  slots. Populated fresh each run, not cached, so a game patch that changes vanilla's master
	 *  material is picked up the next time this runs rather than quietly compared against a stale
	 *  baseline. */
	void BuildMasterSet(TSet<const UMaterial*>& OutVanillaSet, int32& OutVanillaClassesConsidered)
	{
		OutVanillaSet.Empty();
		OutVanillaClassesConsidered = 0;

		TArray<UClass*> Derived;
		GetDerivedClasses(UFGItemDescriptor::StaticClass(), Derived, true);

		for (UClass* Class : Derived)
		{
			if (Class == nullptr || !Class->GetPathName().StartsWith(GVanillaPrefix)) { continue; }

			const UStaticMesh* Mesh = UFGItemDescriptor::GetItemMesh(Class);
			if (Mesh == nullptr) { continue; }

			++OutVanillaClassesConsidered;
			for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
			{
				if (UMaterialInterface* Iface = Slot.MaterialInterface)
				{
					if (const UMaterial* Root = Iface->GetMaterial())
					{
						OutVanillaSet.Add(Root);
					}
				}
			}
		}
	}
}

FFPMMasterMaterialDetector& FFPMMasterMaterialDetector::Get()
{
	static FFPMMasterMaterialDetector Instance;
	return Instance;
}

bool FFPMMasterMaterialDetector::SelfTest()
{
	TSet<const UMaterial*> VanillaSet;
	int32 VanillaClassesConsidered = 0;
	BuildMasterSet(VanillaSet, VanillaClassesConsidered);

	const bool bSetNonEmpty = VanillaSet.Num() > 0;

	// Known negative: re-derive ONE vanilla item's own root material and confirm it is a member
	// of the set that was built from exactly that population. A set excluding its own source data
	// would be the loudest possible false positive this detector could produce.
	bool bVanillaClassifiesAsMember = false;
	TArray<UClass*> Derived;
	GetDerivedClasses(UFGItemDescriptor::StaticClass(), Derived, true);
	FString ProbedClassName;
	for (UClass* Class : Derived)
	{
		if (Class == nullptr || !Class->GetPathName().StartsWith(GVanillaPrefix)) { continue; }
		const UStaticMesh* Mesh = UFGItemDescriptor::GetItemMesh(Class);
		if (Mesh == nullptr) { continue; }
		bool bAnySlot = false;
		for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			if (UMaterialInterface* Iface = Slot.MaterialInterface)
			{
				bAnySlot = true;
				if (const UMaterial* Root = Iface->GetMaterial())
				{
					bVanillaClassifiesAsMember = VanillaSet.Contains(Root);
				}
			}
		}
		if (bAnySlot) { ProbedClassName = Class->GetPathName(); break; }
	}

	const bool bPassed = bSetNonEmpty && bVanillaClassifiesAsMember;

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: master-material self-test %s - vanilla set built from %d loaded "
		     "/Game/FactoryGame/ item descriptor(s) with %d distinct root material(s); vanilla "
		     "probe '%s' classifies as a set member: %s."),
		bPassed ? TEXT("PASSED") : TEXT("FAILED"), VanillaClassesConsidered, VanillaSet.Num(),
		ProbedClassName.IsEmpty() ? TEXT("<none found>") : *ProbedClassName,
		bVanillaClassifiesAsMember ? TEXT("yes") : TEXT("NO"));

	return bPassed;
}

void FFPMMasterMaterialDetector::RunNow()
{
	TSet<const UMaterial*> VanillaSet;
	int32 VanillaClassesConsidered = 0;
	BuildMasterSet(VanillaSet, VanillaClassesConsidered);

	if (VanillaSet.Num() == 0)
	{
		// Honest coverage rather than a silent zero: with no vanilla masters resolved, NOTHING
		// could ever be flagged, so a "0 offenders" result below would be meaningless.
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] detect: master-material census SKIPPED - 0 root materials resolved from "
			     "%d /Game/FactoryGame/ item descriptor(s). The comparison set is empty, so no "
			     "verdict this run would be trustworthy; retry after more content has loaded."),
			VanillaClassesConsidered);
		FPMOverlay::PostSticky(TEXT("detect"), TEXT("master-material"),
			TEXT("trap 2 (master material): SKIPPED, 0 vanilla masters resolved"));
		return;
	}

	TArray<UClass*> Derived;
	GetDerivedClasses(UFGItemDescriptor::StaticClass(), Derived, true);

	int32 ClassesConsidered = 0;
	int32 Flagged = 0;
	for (UClass* Class : Derived)
	{
		if (Class == nullptr) { continue; }
		const UStaticMesh* Mesh = UFGItemDescriptor::GetItemMesh(Class);
		if (Mesh == nullptr) { continue; }
		++ClassesConsidered;

		TArray<FString> OffendingMaterials;
		for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			UMaterialInterface* Iface = Slot.MaterialInterface;
			if (Iface == nullptr) { continue; }
			const UMaterial* Root = Iface->GetMaterial();
			if (Root == nullptr || !VanillaSet.Contains(Root))
			{
				OffendingMaterials.Add(Root ? Root->GetName() : TEXT("<unresolved>"));
			}
		}

		if (OffendingMaterials.Num() > 0)
		{
			++Flagged;
			FFPMDetectorRegistry::Report(TEXT("master-material-detector"), Class->GetPathName(),
				FString::Printf(TEXT("belt mesh '%s' uses %d material(s) not in the vanilla master "
				                      "set: %s. Loses CSS's at-scale conveyor rendering optimisation"),
					*Mesh->GetName(), OffendingMaterials.Num(), *FString::Join(OffendingMaterials, TEXT(", "))),
				OffendingMaterials.Num());
		}
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: master-material census - %d loaded item descriptor(s) with a conveyor "
		     "mesh considered against %d vanilla master material(s), %d flagged (reported to "
		     "FPM.Detect.Report). COVERAGE: loaded-only - a full scan would force-load content "
		     "purely to check it, which is the sync-load cost this mod exists to avoid."),
		ClassesConsidered, VanillaSet.Num(), Flagged);

	// m6164470: every FPM feature reports to the dev overlay.
	FPMOverlay::PostSticky(TEXT("detect"), TEXT("master-material"),
		FString::Printf(TEXT("trap 2 (master material): %d/%d item descriptor(s) flagged"),
			Flagged, ClassesConsidered));
}

void FFPMMasterMaterialDetector::OnWorldLoad(UWorld* World)
{
	if (World != nullptr)
	{
		RunNow();
	}
}

void FFPMMasterMaterialDetector::Arm()
{
	const bool bSelfTestPassed = SelfTest();
	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] detect: master-material-detector armed - self-test %s. Trap 2 of 4 (section "
		     "9.3): belt-mesh materials outside the vanilla master set, censused each world load."),
		bSelfTestPassed ? TEXT("PASSED") : TEXT("FAILED"));
}

namespace
{
	static FAutoConsoleCommand GFPMMasterMaterialRunCmd(
		TEXT("FPM.Detect.MasterMaterial"),
		TEXT("Re-run the belt-mesh master-material census now, against currently loaded item "
		     "descriptors."),
		FConsoleCommandDelegate::CreateStatic([]() { FFPMMasterMaterialDetector::RunNow(); }));
}
