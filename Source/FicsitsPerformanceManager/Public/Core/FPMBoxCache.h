// Copyright 2026 DegradingAnt. Licensed under GPL-3.0.

#pragma once

#include "CoreMinimal.h"
#include "Math/Box.h"

/**
 * A PERSISTED class -> bounding-box table, derived on THIS machine and invalidated by any change to it.
 *
 * WHY IT EXISTS. Ant, 2026-08-08: "why cant we fix the rain issue offline and just save that forever?
 * why does it need to do it live at all?" — then "we could just derive it at runtime ONCE per system
 * when in a loading screen or the benchmark screen. problem solved. also solves adding new mods." — and
 * when I proposed skipping persistence: "just make the cache invalidate on any change or update and
 * remake the cache as needed."
 *
 * The data is per-class CONSTANT: same assets in, same box out, every session forever. Recomputing it
 * every boot is pure waste, and the old fix recomputed it thousands of times per session.
 *
 * ★ WE SHIP THE DERIVER, NOT THE DATA, AND THAT IS WHAT KEEPS US CLEAR OF THE LICENCE QUESTION.
 * The table is measurements of third-party mod geometry. Shipping one would be distributing derived
 * data from assets we do not own. Generating it on the user's machine from assets they already own is
 * not distribution at all — the constraint is satisfied by construction rather than by policy.
 *
 * ⚠ RESIDUE. This writes ONE file under the game's Saved directory. If the mod is uninstalled the file
 * is inert — nothing reads it and it changes no game behaviour — which satisfies the zero-residue law
 * as stated ("if the mod is uninstalled it must do NOTHING"). It is still a file left behind, so it
 * lives in its own clearly-named folder rather than scattered into the game's own configs.
 */
class FICSITSPERFORMANCEMANAGER_API FPMBoxCache
{
public:
	/**
	 * A fingerprint of everything that could change an answer: every enabled plugin's name and version,
	 * the engine changelist, and the game build. Any mod added, removed or updated changes this string,
	 * and so does a game patch — which is exactly Ant's "invalidate on any change or update".
	 *
	 * Cheap to compute (a sorted string plus one hash) and computed once per load.
	 */
	static FString ComputeEnvironmentKey();

	/** Loads the table if its stored key matches Key. A mismatch discards it and reports why. */
	bool Load(const FString& Key);

	/** Writes the table with Key stamped in. */
	bool Save(const FString& Key) const;

	/** nullptr when the class is not in the table. */
	const FBox* Find(FName ClassName) const;

	/** A class that yielded no usable geometry is recorded too — otherwise it is re-derived every load. */
	bool IsKnownGeometryless(FName ClassName) const;

	void Add(FName ClassName, const FBox& Box);
	void AddGeometryless(FName ClassName);

	int32 NumBoxes() const { return Boxes.Num(); }
	int32 NumGeometryless() const { return Geometryless.Num(); }
	bool IsEmpty() const { return Boxes.Num() == 0 && Geometryless.Num() == 0; }

	/** Where the file lives. Exposed so a diagnostic can name it rather than a reader guessing. */
	static FString GetCacheFilePath();

	/** Where builds up to 0.4.1 wrote it - outside the plugin, so it survived uninstall. */
	static FString GetLegacyCacheFilePath();

	/** Remove the pre-0.4.2 cache from outside the plugin. One exact path, never a pattern. */
	static void CleanUpLegacyResidue();

private:
	TMap<FName, FBox> Boxes;
	TSet<FName> Geometryless;
};
