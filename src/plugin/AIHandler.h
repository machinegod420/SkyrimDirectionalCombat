#pragma once

#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include "Direction.h"
#include "Utils.h"

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
		Feint,
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
	void TryRiposte(RE::Actor* actor);
	void TryBlock(RE::Actor* actor, RE::Actor* attacker);
	void DirectionMatchTarget(RE::Actor* actor, RE::Actor* target, bool force);
	void SwitchToNewDirection(RE::Actor* actor, RE::Actor* target);
	void TryAttack(RE::Actor* actor);
	
	void SwitchToNextAttack(RE::Actor* actor);

	bool CanAct(RE::Actor* actor) const;
	bool ShouldAttack(RE::Actor* actor, RE::Actor* target);
	void DidAttack(RE::Actor* actor);
	Difficulty CalcAndInsertDifficulty(RE::Actor* actor);


	void SignalGoodThing(RE::Actor* actor, Directions attackedDir);
	void SignalBadThing(RE::Actor* actor, Directions attackedDir);
	void IncreaseBlockChance(RE::Actor* actor, Directions dir, int percent, int modifier);

	// if something stopped combat, make sure they get removed from the map
	void RemoveActor(RE::Actor* actor)
	{
		DifficultyMap.erase(actor->GetHandle());
		UpdateTimer.erase(actor->GetHandle());
		ActionQueue.erase(actor->GetHandle());
	}

	// Load attackdata so our attackstart event is processed correctly as an attack
	void LoadCachedAttack(RE::Actor* actor);

	void Cleanup();
private:
	RE::BGSKeyword* EnableRaceKeyword;

	void AddAction(RE::Actor* actor, Actions toDo, bool force = false);
	float CalcUpdateTimer(RE::Actor* actor);
	float CalcActionTimer(RE::Actor* actor);
	void DidAct(RE::Actor* actor);

	struct Action
	{
		Actions toDo;
		float timeLeft;
		// for pro block
		Directions toMatch;
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
		std::unordered_map<Directions, int> directionChangeChance;

		// AI should attack in specific patterns, just like people do
		// todo: use uint16_t, every 2 bits represents a direction for fast access and generation of patterns
		// the actual random generation for this would be difficult as you would want a spread of bits versus true random
		std::vector<Directions> attackPattern;
		unsigned currentAttackIdx = 0u;
	};

	RE::NiPointer<RE::BGSAttackData> FindActorAttackData(RE::Actor* actor);

	std::unordered_map<RE::ActorHandle,Action> ActionQueue;
	std::unordered_map<RE::ActorHandle, float> UpdateTimer;
	std::unordered_map<RE::ActorHandle, AIDifficulty> DifficultyMap;

	std::unordered_map<Difficulty, float> DifficultyUpdateTimer;
	std::unordered_map<Difficulty, float> DifficultyActionTimer;
};