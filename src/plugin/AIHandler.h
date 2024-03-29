#pragma once

#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include "Direction.h"
#include "Utils.h"
#include "parallel_hashmap/phmap.h"

class AIHandler
{
public:
	enum class Difficulty : uint16_t
	{
		Uninitialized = 0,
		VeryEasy,
		Easy,
		Normal,
		Hard,
		VeryHard,
		Legendary
	};
	void InitializeValues();
	AIHandler()
	{
		EnableRaceKeyword = nullptr;
	}
	enum class Actions
	{
		None,
		Riposte,
		Block,
		ProBlock,
		StartFeint,
		EndFeint,
		Bash
	};

	static AIHandler* GetSingleton()
	{
		static AIHandler obj;
		return std::addressof(obj);
	}
	bool RaceForcedDirectionalCombat(RE::Actor* actor)
	{
		// can cache value in <race,bool> dictionary
		return (actor->GetRace()->HasKeyword(EnableRaceKeyword));
	}
	void Update(float delta);
	void RunActor(RE::Actor* actor, float delta);
	// called from other thread
	void TryRiposte(RE::Actor* actor, RE::Actor* attacker);
	void TryBlock(RE::Actor* actor, RE::Actor* attacker);
	bool ShouldAttack(RE::Actor* actor, RE::Actor* target);
	void SignalGoodThing(RE::Actor* actor, Directions attackedDir);
	void SignalBadThing(RE::Actor* actor, Directions attackedDir);
	void SwitchTarget(RE::Actor* actor, RE::Actor* newTarget);

	void DidAttack(RE::Actor* actor);
	//

	void SwitchToNewDirection(RE::Actor* actor, RE::Actor* target);
	void TryAttack(RE::Actor* actor);
	

	
	bool CanAct(RE::Actor* actor) const;


	// WARNING - lower level function do not lock
	Difficulty CalcAndInsertDifficulty(RE::Actor* actor);
	void DirectionMatchTarget(RE::Actor* actor, RE::Actor* target, bool force);
	inline void IncreaseBlockChance(RE::Actor* actor, Directions dir, int percent, int modifier);
	void ReduceDifficulty(RE::Actor* actor);
	void ResetDifficulty(RE::Actor* actor);
	void SwitchToNextAttack(RE::Actor* actor);

	// Load attackdata so our attackstart event is processed correctly as an attack
	void LoadCachedAttack(RE::Actor* actor);
	//



	// if something stopped combat, make sure they get removed from the map
	void RemoveActor(RE::ActorHandle actor)
	{
		DifficultyActionTimerMtx.lock();
		DifficultyMap.erase(actor);
		DifficultyActionTimerMtx.unlock();

		UpdateTimerMtx.lock();
		UpdateTimer.erase(actor);
		UpdateTimerMtx.unlock();

		ActionQueueMtx.lock();
		ActionQueue.erase(actor);
		ActionQueueMtx.unlock();
	}



	void Cleanup();
private:
	RE::BGSKeyword* EnableRaceKeyword;

	void AddAction(RE::Actor* actor, Actions toDo, Directions attackedDir = Directions::TR, bool force = false);
	float CalcUpdateTimer(RE::Actor* actor);
	float CalcActionTimer(RE::Actor* actor);
	void DidAct(RE::Actor* actor);

	struct Action
	{
		Actions toDo;
		float timeLeft;
		// for pro block
		Directions targetDir;
	};

	struct AIDifficulty
	{
		Difficulty difficulty;
		// this exists because depending on the circumstances of the fight, we may want to make the AI 
		// more challenging or less challenging based on its mistake ratio
		// Increment we do a successful action (landing an attack, blocking)
		// Decrement if we fail an action (missing a block, attack was blocked)
		float mistakeRatio = 0.f;
		// keep a list of the last 5 directions attacked by 
		// if there is an obvious pattern, become harder or easier
		// todo: circular array
		std::vector<Directions> lastDirectionsEncountered;

		// which direction we last saw, we try to match that
		Directions lastDirectionTracked;
		// cache the basic attack data for unblockable/riposte funcitionality
		RE::NiPointer<RE::BGSAttackData> cachedBasicAttackData = nullptr;

		// this stores the amount of time since the target last switched guards
		// if the target does not switch enough, then we can change guards to try to snipe
		float targetSwitchTimer = 0.f;
		Directions targetLastDir;
		RE::ActorHandle currentTarget;

		// percent chance it may switch to a specific direction to emulate anticipation of attacks
		phmap::flat_hash_map<Directions, int> directionChangeChance;

		// AI should attack in specific patterns, just like people do
		// todo: use uint16_t, every 2 bits represents a direction for fast access and generation of patterns
		// the actual random generation for this would be difficult as you would want a spread of bits versus true random
		std::vector<Directions> attackPattern;
		unsigned currentAttackIdx = 0u;
	};

	RE::NiPointer<RE::BGSAttackData> FindActorAttackData(RE::Actor* actor);

	phmap::flat_hash_map<RE::ActorHandle,Action> ActionQueue;
	mutable std::shared_mutex ActionQueueMtx;

	phmap::flat_hash_map<RE::ActorHandle, float> UpdateTimer;
	mutable std::shared_mutex UpdateTimerMtx;

	phmap::flat_hash_map<RE::ActorHandle, AIDifficulty> DifficultyMap;
	mutable std::shared_mutex DifficultyMapMtx;

	phmap::flat_hash_map<Difficulty, float> DifficultyUpdateTimer;
	mutable std::shared_mutex DifficultyUpdateTimerMtx;

	phmap::flat_hash_map<Difficulty, float> DifficultyActionTimer; 
	mutable std::shared_mutex DifficultyActionTimerMtx;

};