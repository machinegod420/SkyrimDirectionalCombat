#pragma once
#include "Direction.h"
#include "Utils.h"
#include "parallel_hashmap/phmap.h"

#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Creatures attack on a line but hold no guard, never block on a line, and
// never run the mod's AI. Their state lives here rather than in
// DirectionHandler's direction map so HasDirectionalPerks stays false for them.
class CreatureHandler
{
public:
	static CreatureHandler* GetSingleton()
	{
		static CreatureHandler obj;
		return std::addressof(obj);
	}
	// Classifies every race once. Data load, after HumanoidUndeadRaces has
	// moved draugr onto the humanoid graph.
	void Initialize();
	// Per frame for every non-perked actor in range: assigns a line when a
	// swing starts, keeps it as the creature's guard between swings, and
	// draws the marker while it fights.
	void UpdateActor(RE::Actor* actor);
	void Update(float delta);
	// The creature's current or last line. False only before its first swing
	// of a fight. This is what the AI reads as its guard.
	bool GetAttackLine(RE::Actor* actor, Directions& outDir) const;
	// True only while a swing with an assigned line is in progress. The hit
	// hooks key on this, so a bash or a hit between swings stays vanilla.
	bool IsDirectionalAttacker(RE::Actor* actor) const;
	// True when the race does not run the humanoid behavior graph — the
	// property the mod's guard animations actually depend on. Decides both
	// who gets a guard and who gets attack lines. Read-only after
	// Initialize, any thread.
	bool IsCreatureGraph(RE::TESRace* race) const;
	// A creature that attacks on lines right now: the feature is on and this
	// is a non-player creature-graph actor. The AI runs its full directional
	// branch against one.
	bool IsCreatureOpponent(RE::Actor* actor) const;
	// Height relative to the player; 1 is player-sized.
	static float SizeRatio(RE::Actor* actor);
	// Weapon-weight equivalent of the creature, from its size alone.
	static float SizeWeight(RE::Actor* actor);
private:
	// The path test behind IsCreatureGraph, run once per race by Initialize.
	static bool GraphIsCreature(RE::TESRace* race);
	Directions LineForAttack(RE::Actor* actor, RE::BGSAttackData* attackData);
	// The two lines this creature attacks on, by size. outPair[0] is the left.
	static void PairFor(RE::Actor* actor, Directions outPair[2]);
	// The one place to change when a better size measure turns up.
	static float BodyHeight(RE::Actor* actor);
	// Position of the attack in the race's sorted attack list: the side when
	// the name carries no left/right, so a pair still alternates across attacks.
	static size_t AttackIndex(RE::TESRace* race, RE::BGSAttackData* attackData);
	void LogRaceOnce(RE::Actor* actor);

	struct AttackLine
	{
		Directions line;
		// The swing this line was assigned for. A chained swing never leaves
		// the attack state, so this is what tells one swing from the next.
		RE::BGSAttackData* attack;
	};
	phmap::flat_hash_map<RE::ActorHandle, AttackLine> AttackLines;
	mutable std::shared_mutex AttackLinesMtx;
	// Filled at data load, never written after: no lock.
	phmap::flat_hash_set<RE::TESRace*> CreatureRaces;
	std::unordered_set<RE::TESRace*> LoggedRaces;
	std::mutex LoggedRacesMtx;
};
