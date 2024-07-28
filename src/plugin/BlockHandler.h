#pragma once

#include <unordered_map>
#include "Direction.h"
#include "Utils.h"
#include "parallel_hashmap/phmap.h"

class BlockHandler
{
public:
	BlockHandler();
	void Initialize();
	static BlockHandler* GetSingleton()
	{
		static BlockHandler obj;
		return std::addressof(obj);
	}
	void Update(float delta);

	void ApplyBlockDamage(RE::Actor* target, RE::Actor* attacker, RE::HitData &hitData);
	void CauseStagger(RE::Actor* actor, RE::Actor* heading, float magnitude = 0.f, bool force = false);
	void CauseRecoil(RE::Actor* actor) const;
	void GiveHyperarmor(RE::Actor* actor, RE::Actor* attacker);
	inline bool HasHyperarmor(RE::Actor* actor, RE::Actor* attacker) const
	{
		bool ret = false;
		HyperArmorTimerMtx.lock_shared();
		// if the attacker is NOT the same as the original attacker that gave the hyperarmor, count as having hyperarmor
		if (HyperArmorTimer.contains(actor->GetHandle()))
		{
			if (HyperArmorTimer.at(actor->GetHandle()).Target != attacker->GetHandle())
			{
				ret = true;
			}
		}
		HyperArmorTimerMtx.unlock_shared();
		return ret;
	}
	void HandleBlock(RE::Actor* attacker, RE::Actor* target);
	bool HandleMasterstrike(RE::Actor* attacker, RE::Actor* target);

	void AddNewAttacker(RE::Actor* actor, RE::Actor* attacker);
	int GetNumberAttackers(RE::Actor* actor) const;

	void ParriedAttacker(RE::Actor* actor, RE::Actor* attacker);
	

	void RemoveActor(RE::ActorHandle actor);
	
	void Cleanup()
	{
		StaggerTimerMtx.lock();
		StaggerTimer.clear();
		StaggerTimerMtx.unlock();

		HyperArmorTimerMtx.lock();
		HyperArmorTimer.clear();
		HyperArmorTimerMtx.unlock();
	}
private:
	RE::BGSKeyword* NPCKeyword;
	RE::SpellItem* MultiAttackerFX;

	phmap::flat_hash_map<RE::ActorHandle, float> StaggerTimer;
	mutable std::shared_mutex StaggerTimerMtx;
	
	struct HyperArmorData
	{
		RE::ActorHandle Target;
		float TimeLeft = 0;
	};
	phmap::flat_hash_map<RE::ActorHandle, HyperArmorData> HyperArmorTimer;
	mutable std::shared_mutex HyperArmorTimerMtx;

	struct MultipleAttackers
	{
		// must NOT be current target
		phmap::flat_hash_set<RE::ActorHandle> attackers;
		// time before we remove an attacker
		float timeLeft;
	};
	phmap::flat_hash_map<RE::ActorHandle, MultipleAttackers> AttackersMap;
	mutable std::shared_mutex AttackersMapMtx;

	struct LastParried
	{
		RE::ActorHandle lastParried;
		float timeLeft;
	};
	phmap::flat_hash_map<RE::ActorHandle, LastParried> LastParriedMap;
	mutable std::shared_mutex LastParriedMapMtx;
};