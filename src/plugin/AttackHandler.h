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

	void AddFeintQueue(RE::Actor* actor);
	bool InFeintQueue(RE::Actor* actor)
	{
		std::shared_lock lock(FeintQueueMtx);
		return FeintQueue.contains(actor->GetHandle());
	}
	
	void GiveAttackSpeedBuff(RE::Actor* actor);
	void GiveSmallAttackSpeedBuff(RE::Actor* actor);
	void RemoveSmallAttackSpeedBuff(RE::Actor* actor);
	
	void AddLockout(RE::Actor* actor);
	void HandleFeint(RE::Actor* actor);
	void HandleFeintChangeDirection(RE::Actor* actor);

	void DoAttack(RE::Actor* actor);
	void DoPowerAttack(RE::Actor* actor);

	void RemoveLockout(RE::Actor* actor);

	void Update(float delta);

	void RemoveActor(RE::ActorHandle actor);

	void Cleanup();

	void AddAttackChain(RE::Actor* actor, bool DidPowerAttack)
	{
		std::unique_lock lock(AttackChainMtx);
		AttackChains[actor->GetHandle()].PreviousAttackWasPower = DidPowerAttack;
		AttackChains[actor->GetHandle()].timeLeft = 1.f;

	}

	bool InAttackChain(RE::Actor* actor, bool& OutDidPowerAttack)
	{
		std::shared_lock lock(AttackChainMtx);
		auto Iter = AttackChains.find(actor->GetHandle());
		if (Iter != AttackChains.end())
		{
			OutDidPowerAttack = Iter->second.PreviousAttackWasPower;
			return true;
		}
		return false;
	}

	void RemoveAttackChain(RE::Actor* actor)
	{
		std::unique_lock lock(AttackChainMtx);
		AttackChains.erase(actor->GetHandle());
	}
private:
	phmap::flat_hash_map<RE::ActorHandle, float> ChamberWindow;
	mutable std::shared_mutex ChamberWindowMtx;

	phmap::flat_hash_map<RE::ActorHandle, float> FeintWindow;
	mutable std::shared_mutex FeintWindowMtx;

	phmap::flat_hash_map<RE::ActorHandle, float> FeintQueue;
	mutable std::shared_mutex FeintQueueMtx;

	phmap::flat_hash_map<RE::ActorHandle, float> AttackLockout;
	mutable std::shared_mutex AttackLockoutMtx;

	phmap::flat_hash_map<RE::ActorHandle, float> SpeedBuff;
	mutable std::shared_mutex SpeedBuffMtx;

	phmap::flat_hash_map<RE::ActorHandle, float> SmallSpeedBuff;
	mutable std::shared_mutex SmallSpeedBuffMtx;

	struct AttackChain
	{
		bool PreviousAttackWasPower;
		float timeLeft;
	};
	phmap::flat_hash_map<RE::ActorHandle, AttackChain> AttackChains;
	mutable std::shared_mutex AttackChainMtx;

	RE::SpellItem* FeintFX;
	RE::SpellItem* WeaponSpeedBuff;

	RE::BGSAction* ActionAttack;
	RE::BGSAction* ActionPowerAttack;
};