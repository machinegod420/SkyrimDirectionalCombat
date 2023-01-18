#pragma once
#include "Direction.h"
#include "Utils.h"
#include <unordered_map>
#include "parallel_hashmap/phmap.h"

class AttackHandler
{
public:
	static AttackHandler* GetSingleton()
	{
		static AttackHandler obj;
		return std::addressof(obj);
	}

	bool InChamberWindow(RE::Actor* actor);
	bool CanAttack(RE::Actor* actor);

	void AddChamberWindow(RE::Actor* actor);

	void AddLockout(RE::Actor* actor);
	void HandleFeint(RE::Actor* actor);

	// This is really gross since this class should never have any AI logic
	// But this is the easiest place to put this, as we don't want NPCs to attack in very 
	// repeated succession so they can switch guards and be more intelligent
	// This basically adds a 100ms timeout for NPC attakcs
	void AddNPCSmallLockout(RE::Actor* actor);

	void LockoutPlayer();
	bool IsPlayerLocked()
	{
		return PlayerLockout;
	}
	void Update(float delta);
private:
	phmap::parallel_flat_hash_map<RE::ActorHandle, float> ChamberWindow;
	phmap::parallel_flat_hash_map<RE::ActorHandle, float> AttackLockout;
	

	// Player is special case cause it should be accessed way more
	float PlayerLockoutTimer;
	bool PlayerLockout;
};