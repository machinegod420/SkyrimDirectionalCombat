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
	void ApplyBlockDamage(RE::Actor* actor, RE::HitData &hitData);
	void CauseStagger(RE::Actor* actor, RE::Actor* heading, float magnitude = 0.f, bool force = false);
	void CauseRecoil(RE::Actor* actor);
	void GiveHyperarmor(RE::Actor* actor);
	inline bool HasHyperarmor(RE::Actor* actor)
	{
		return HyperArmorTimer.contains(actor->GetHandle());
	}
	void HandleBlock(RE::Actor* attacker, RE::Actor* target);
	bool HandleMasterstrike(RE::Actor* attacker, RE::Actor* target);
	
	void Cleanup()
	{
		StaggerTimer.clear();
		HyperArmorTimer.clear();
	}
private:
	RE::BGSKeyword* NPCKeyword;
	phmap::parallel_flat_hash_map<RE::ActorHandle, float> StaggerTimer;
	
	phmap::parallel_flat_hash_map<RE::ActorHandle, float> HyperArmorTimer;
};