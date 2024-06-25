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

	void RemoveLockout(RE::Actor* actor);

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

	RE::SpellItem* FeintFX;
	RE::SpellItem* WeaponSpeedBuff;
};