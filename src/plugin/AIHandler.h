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
	void TryAttack(RE::Actor* actor);

	bool CanAct(RE::Actor* actor) const;
	void DidAct(RE::Actor* actor);
	bool ShouldAttack(RE::Actor* actor, RE::Actor* target);
	Difficulty CalcAndInsertDifficulty(RE::Actor* actor);


	void SignalGoodThing(RE::Actor* actor, Directions attackedDir);
	void SignalBadThing(RE::Actor* actor, Directions attackedDir);

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
		// if there is an obvious pattern, become way harder
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
	};

	RE::NiPointer<RE::BGSAttackData> FindActorAttackData(RE::Actor* actor);

	std::unordered_map<RE::ActorHandle,Action> ActionQueue;
	std::unordered_map<RE::ActorHandle, float> UpdateTimer;
	std::unordered_map<RE::ActorHandle, AIDifficulty> DifficultyMap;

	std::unordered_map<Difficulty, float> DifficultyUpdateTimer;
	std::unordered_map<Difficulty, float> DifficultyActionTimer;
};