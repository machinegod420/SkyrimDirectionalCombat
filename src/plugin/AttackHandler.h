#pragma once
#include "Direction.h"
#include "Utils.h"
#include <unordered_map>
#include "parallel_hashmap/phmap.h"

class AttackHandler
{
public:

	AttackHandler() : FeintFX(nullptr)
	{
		PlayerLockout = false;
		PlayerLockoutTimer = 0.f;
	}
	void Initialize();
	static AttackHandler* GetSingleton()
	{
		static AttackHandler obj;
		return std::addressof(obj);
	}

	bool InChamberWindow(RE::Actor* actor);
	float GetChamberWindowTime(RE::Actor* actor);
	bool CanAttack(RE::Actor* actor);
	bool InFeintWindow(RE::Actor* actor);

	void AddChamberWindow(RE::Actor* actor);
	void AddFeintWindow(RE::Actor* actor);
	void RemoveFeintWindow(RE::Actor* actor);
	const float AttackSpeedMult = 0.1;
	void GiveAttackSpeedBuff(RE::Actor* actor)
	{
		SpeedBuffMtx.lock();
		if (!SpeedBuff.contains(actor->GetHandle()))
		{
			SpeedBuff[actor->GetHandle()] = 2.f;
			actor->AsActorValueOwner()->ModActorValue(RE::ActorValue::kWeaponSpeedMult, AttackSpeedMult);
		}
		SpeedBuffMtx.unlock();
	}
	void AddLockout(RE::Actor* actor);
	void HandleFeint(RE::Actor* actor);
	void HandleFeintChangeDirection(RE::Actor* actor);

	// This is really gross since this class should never have any AI logic
	// But this is the easiest place to put this, as we don't want NPCs to attack in very 
	// repeated succession so they can switch guards and be more intelligent
	// This basically adds a 100ms timeout for NPC attakcs
	void AddNPCSmallLockout(RE::Actor* actor);

	void LockoutPlayer(RE::Actor* actor);
	bool IsPlayerLocked() const
	{
		PlayerMtx.lock_shared();
		bool ret = PlayerLockout;
		PlayerMtx.unlock_shared();
		return ret;
	}
	void Update(float delta);

	void RemoveActor(RE::ActorHandle actor);

	void Cleanup();
private:
	phmap::flat_hash_map<RE::ActorHandle, float> ChamberWindow;
	mutable std::shared_mutex ChamberWindowMtx;

	phmap::flat_hash_map<RE::ActorHandle, float> FeintWindow;
	mutable std::shared_mutex FeintWindowMtx;

	phmap::flat_hash_map<RE::ActorHandle, float> AttackLockout;
	mutable std::shared_mutex AttackLockoutMtx;

	phmap::flat_hash_map<RE::ActorHandle, float> SpeedBuff;
	mutable std::shared_mutex SpeedBuffMtx;

	// Player is special case cause it should be accessed way more
	float PlayerLockoutTimer;
	bool PlayerLockout;
	mutable std::shared_mutex PlayerMtx;

	RE::SpellItem* FeintFX;
	RE::SpellItem* WeaponSpeedBuff;
};