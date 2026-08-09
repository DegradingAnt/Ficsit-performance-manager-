// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#include "Core/FPMBoxCache.h"

#include "FicsitsPerformanceManager.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	const TCHAR* KeyField = TEXT("environmentKey");
	const TCHAR* BoxesField = TEXT("boxes");
	const TCHAR* GeometrylessField = TEXT("geometryless");
}

FString FPMBoxCache::GetLegacyCacheFilePath()
{
	// Where every build up to 0.4.1 wrote it: under the GAME's Saved directory, OUTSIDE the plugin. That
	// file survives uninstall, which is the residue this fix exists to end. Kept as a named constant so
	// the one-time cleanup below has something exact to delete rather than a pattern to guess with.
	return FPaths::ProjectSavedDir() / TEXT("FicsitsPerformanceManager") / TEXT("DerivedBoxes.json");
}

FString FPMBoxCache::GetCacheFilePath()
{
	/*
	 * ★ INSIDE THE PLUGIN WHEN WE CAN, UNDER Saved/ WHEN WE CANNOT — Ant's ruling, 2026-08-09:
	 * *"would be good to keep it in the mod if possible, if its not we declare it an exception."*
	 *
	 * FOUND BY AUDIT, NOT BY DESIGN. Every build to 0.4.1 wrote 120,681 bytes to
	 * `%LOCALAPPDATA%/FactoryGame/Saved/FicsitsPerformanceManager/DerivedBoxes.json` — outside the plugin
	 * folder, so deleting the mod left it behind. Meanwhile the residue sentinel reported "NOTHING would
	 * remain", because it audits cvars and file residue was a category it could not see. Both halves are
	 * fixed: this chooses a location that uninstalls with the mod, and the sentinel now audits files.
	 *
	 * ⚠ THE PLUGIN DIRECTORY IS NOT RELIABLY WRITABLE AND THAT IS WHY THIS IS A FALLBACK, NOT A MOVE.
	 * It IS writable unelevated on Ant's Steam install (probed 2026-08-09) because Steam's tree grants
	 * Users write access — but `Program Files` is read-only on plenty of machines, and FPM is heading for
	 * a public release. So we ASK the filesystem rather than assume, every boot, and degrade to the old
	 * location when the answer is no. A cache that silently stops persisting would be a slow performance
	 * regression nobody could attribute.
	 *
	 * Also true and worth knowing rather than discovering: the plugin folder is overwritten by a mod
	 * update (`deploy_fpm_client.py` extracts over it, and SMM manages it), so a cache living there is
	 * lost on every update. That costs one re-derive and is the correct trade against permanent residue.
	 */
	static FString Resolved;
	if (!Resolved.IsEmpty()) { return Resolved; }

	if (const TSharedPtr<IPlugin> Self = IPluginManager::Get().FindPlugin(TEXT("FicsitsPerformanceManager")))
	{
		const FString InPlugin = FPaths::ConvertRelativePathToFull(Self->GetBaseDir()) / TEXT("DerivedBoxes.json");

		// The only honest writability test is a write. Permissions, virtualisation and read-only media all
		// lie to every cheaper check.
		const FString Probe = FPaths::GetPath(InPlugin) / TEXT("_fpm-write-probe.tmp");
		if (FFileHelper::SaveStringToFile(TEXT("x"), *Probe))
		{
			IFileManager::Get().Delete(*Probe, /*RequireExists*/ false, /*EvenReadOnly*/ true);
			Resolved = InPlugin;
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] box cache: writing INSIDE the plugin (%s) - it uninstalls with the mod, so it "
				     "is not residue."), *Resolved);
			return Resolved;
		}
	}

	Resolved = GetLegacyCacheFilePath();
	UE_LOG(LogFicsitsPerformanceManager, Warning,
		TEXT("[FPM] box cache: the plugin directory is not writable, so the cache goes to %s - OUTSIDE the "
		     "plugin. ⚠ THAT FILE SURVIVES UNINSTALL. It is a DECLARED named exception, it is listed by "
		     "FPM.Residue, and deleting it by hand is always safe: it is derived data and nothing else."),
		*Resolved);
	return Resolved;
}

void FPMBoxCache::CleanUpLegacyResidue()
{
	/*
	 * ONE-TIME CLEANUP of the file older builds left behind. Only ever the EXACT path we wrote ourselves,
	 * inside a directory carrying the mod's own name, containing nothing but derived bounding boxes.
	 *
	 * ⚠ Deliberately narrow, because deletion is the one verb this project treats as dangerous. It is not
	 * a pattern, not a sweep and not recursive; it removes one known path and then the directory only if
	 * that left it empty. If anything else is in there, it stays and is reported.
	 */
	const FString Legacy = GetLegacyCacheFilePath();
	if (Legacy == GetCacheFilePath()) { return; }              // Saved/ IS the live location; nothing to clean
	if (!IFileManager::Get().FileExists(*Legacy)) { return; }

	if (IFileManager::Get().Delete(*Legacy, /*RequireExists*/ false, /*EvenReadOnly*/ true))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] box cache: removed the pre-0.4.2 cache left outside the plugin at %s. It was ours, "
			     "it was derived data, and it would have survived uninstall."), *Legacy);

		const FString Dir = FPaths::GetPath(Legacy);
		TArray<FString> Remaining;
		IFileManager::Get().FindFiles(Remaining, *(Dir / TEXT("*")), true, true);
		if (Remaining.Num() == 0)
		{
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ false);
		}
		else
		{
			UE_LOG(LogFicsitsPerformanceManager, Display,
				TEXT("[FPM] box cache: %s still holds %d other item(s), so the folder stays. Nothing that "
				     "is not ours gets deleted."), *Dir, Remaining.Num());
		}
	}
	else
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] box cache: could not remove the legacy cache at %s. It is harmless derived data and "
			     "safe to delete by hand."), *Legacy);
	}
}

FString FPMBoxCache::ComputeEnvironmentKey()
{
	/*
	 * EVERY ENABLED PLUGIN'S NAME AND VERSION, SORTED, PLUS THE ENGINE AND GAME BUILD.
	 *
	 * Sorted because IPluginManager's order is not guaranteed stable, and an unstable key would
	 * invalidate the cache on every boot — which looks exactly like a working cache that never hits.
	 * Version is included as well as name so UPDATING a mod invalidates, not just adding one; that is
	 * the half of "any change or update" a name-only key would silently miss.
	 */
	TArray<FString> Parts;
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		/*
		 * ★ OUR OWN VERSION IS EXCLUDED — 2026-08-09, and it is a measured fix rather than a tidy-up.
		 *
		 * The boxes are derived from OTHER mods' and vanilla's geometry. FPM's version number has no
		 * bearing on what they come out as. The one thing that DOES — the derivation logic itself — is
		 * already covered by the deriver version below, which exists for exactly that and was added on
		 * 2026-08-08 when this same gap was caught once already.
		 *
		 * Leaving our own name@version in the key meant EVERY bump invalidated the whole cache. Observed
		 * across three bumps in one morning; the 0.4.1 boot logged
		 * `cache MISS->rebuilt | 3678 classes examined | 0 from cache` and re-derived 573 classes whose
		 * geometry had not changed. During iteration that is a cache that never hits — the precise failure
		 * the Sort() below is commented as preventing, arriving through a different door.
		 */
		if (Plugin->GetName() == TEXT("FicsitsPerformanceManager")) { continue; }

		Parts.Add(FString::Printf(TEXT("%s@%s"), *Plugin->GetName(), *Plugin->GetDescriptor().VersionName));
	}
	Parts.Sort();

	/*
	 * ★ THE DERIVER VERSION — BUMP THIS WHENEVER THE DERIVATION LOGIC CHANGES.
	 *
	 * Caught 2026-08-08 before it shipped: the key covered the mod set, the game build and the plugin
	 * VersionName, but NOT the code that produces the boxes. Adding Ant's collision filter to the
	 * component path changed every box the component source yields — and because VersionName was still
	 * 0.0.1, the key matched, the cache HIT, and the OLD UNFILTERED boxes would have been replayed
	 * forever. The fix would have been invisible and we would have concluded the filter did nothing.
	 *
	 * A cache keyed on inputs but not on the FUNCTION is a cache that lies after a code change.
	 *   1 = original (instance-data + unfiltered component walk)
	 *   2 = collision filter applied to BOTH sources
	 */
	Parts.Add(TEXT("deriver@2"));

	Parts.Add(FString::Printf(TEXT("engine@%s"), *FEngineVersion::Current().ToString()));
	// NO leading '*' — GetBuildVersion() already returns const TCHAR*, and dereferencing it hands %s a
	// single character. UE's checked-printf caught it; without that check the key would have been
	// silently wrong and the cache would have thrashed or, worse, matched across different builds.
	Parts.Add(FString::Printf(TEXT("build@%s"), FApp::GetBuildVersion()));

	const FString Combined = FString::Join(Parts, TEXT("|"));
	return FMD5::HashAnsiString(*Combined);
}

bool FPMBoxCache::Load(const FString& Key)
{
	Boxes.Reset();
	Geometryless.Reset();

	const FString Path = GetCacheFilePath();
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path))
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] box cache: none yet at %s — will derive and write one"), *Path);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		// Corrupt beats stale only if we SAY so. A silently-discarded cache is indistinguishable from
		// one that never hits.
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] box cache: %s is not valid JSON — discarding and re-deriving"), *Path);
		return false;
	}

	const FString StoredKey = Root->GetStringField(KeyField);
	if (StoredKey != Key)
	{
		UE_LOG(LogFicsitsPerformanceManager, Display,
			TEXT("[FPM] box cache: INVALIDATED — the mod set, game build or FPM version changed since it "
			     "was written. Re-deriving."));
		return false;
	}

	const TSharedPtr<FJsonObject>* BoxObj = nullptr;
	if (Root->TryGetObjectField(BoxesField, BoxObj) && BoxObj)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*BoxObj)->Values)
		{
			const TArray<TSharedPtr<FJsonValue>>* Six = nullptr;
			if (!Pair.Value->TryGetArray(Six) || !Six || Six->Num() != 6) { continue; }

			const FBox Box(
				FVector((*Six)[0]->AsNumber(), (*Six)[1]->AsNumber(), (*Six)[2]->AsNumber()),
				FVector((*Six)[3]->AsNumber(), (*Six)[4]->AsNumber(), (*Six)[5]->AsNumber()));
			Boxes.Add(FName(*Pair.Key), Box);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* GeoArr = nullptr;
	if (Root->TryGetArrayField(GeometrylessField, GeoArr) && GeoArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *GeoArr)
		{
			Geometryless.Add(FName(*V->AsString()));
		}
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] box cache: HIT — %d box(es) and %d geometryless class(es) loaded, no derivation needed"),
		Boxes.Num(), Geometryless.Num());
	return true;
}

bool FPMBoxCache::Save(const FString& Key) const
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(KeyField, Key);

	const TSharedRef<FJsonObject> BoxObj = MakeShared<FJsonObject>();
	for (const TPair<FName, FBox>& Pair : Boxes)
	{
		TArray<TSharedPtr<FJsonValue>> Six;
		Six.Add(MakeShared<FJsonValueNumber>(Pair.Value.Min.X));
		Six.Add(MakeShared<FJsonValueNumber>(Pair.Value.Min.Y));
		Six.Add(MakeShared<FJsonValueNumber>(Pair.Value.Min.Z));
		Six.Add(MakeShared<FJsonValueNumber>(Pair.Value.Max.X));
		Six.Add(MakeShared<FJsonValueNumber>(Pair.Value.Max.Y));
		Six.Add(MakeShared<FJsonValueNumber>(Pair.Value.Max.Z));
		BoxObj->SetArrayField(Pair.Key.ToString(), Six);
	}
	Root->SetObjectField(BoxesField, BoxObj);

	TArray<TSharedPtr<FJsonValue>> GeoArr;
	for (const FName& Name : Geometryless)
	{
		GeoArr.Add(MakeShared<FJsonValueString>(Name.ToString()));
	}
	Root->SetArrayField(GeometrylessField, GeoArr);

	FString Out;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		UE_LOG(LogFicsitsPerformanceManager, Warning, TEXT("[FPM] box cache: serialise FAILED, not written"));
		return false;
	}

	const FString Path = GetCacheFilePath();
	if (!FFileHelper::SaveStringToFile(Out, *Path))
	{
		// Not fatal — the sweep already applied its results this session. But say it, or a cache that
		// silently never persists looks identical to one that is working.
		UE_LOG(LogFicsitsPerformanceManager, Warning,
			TEXT("[FPM] box cache: could NOT write %s — this session is fine, but the work will be "
			     "repeated next boot"), *Path);
		return false;
	}

	UE_LOG(LogFicsitsPerformanceManager, Display,
		TEXT("[FPM] box cache: wrote %d box(es) and %d geometryless class(es) to %s"),
		Boxes.Num(), Geometryless.Num(), *Path);
	return true;
}

const FBox* FPMBoxCache::Find(FName ClassName) const
{
	return Boxes.Find(ClassName);
}

bool FPMBoxCache::IsKnownGeometryless(FName ClassName) const
{
	return Geometryless.Contains(ClassName);
}

void FPMBoxCache::Add(FName ClassName, const FBox& Box)
{
	Boxes.Add(ClassName, Box);
}

void FPMBoxCache::AddGeometryless(FName ClassName)
{
	Geometryless.Add(ClassName);
}
