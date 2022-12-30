#pragma once

#include "UIMenu.h"
#include "Direction.h"
#include "Utils.h"

#include <shared_mutex>
#include <unordered_set>


class DirectionHandler
{
public:

	DirectionHandler()
	{
		TR = nullptr;
		TL = nullptr;
		BL = nullptr;
		BR = nullptr;
		Debuff = nullptr;
		Unblockable = nullptr;
		NPCKeyword = nullptr;
		ParryVFX = nullptr;
	}
	static DirectionHandler* GetSingleton()
	{
		static DirectionHandler obj;
		return std::addressof(obj);
	}

	bool HasDirectionalPerks(RE::Actor* actor) const;
	bool HasBlockAngle(RE::Actor* attacker, RE::Actor* target);
	void AddDirectional(RE::Actor* actor);
	void SwitchDirectionLeft(RE::Actor* actor);
	void SwitchDirectionUp(RE::Actor* actor);
	void SwitchDirectionDown(RE::Actor* actor);
	void SwitchDirectionRight(RE::Actor* actor);
	void SwitchToNewDirection(RE::Actor* attacker, RE::Actor* target);
	void WantToSwitchTo(RE::Actor* actor, Directions dir, bool force = false);
	RE::BGSPerk* DirectionToPerk(Directions dir) const;
	RE::BGSPerk* GetDirectionalPerk(RE::Actor* actor) const;
	Directions PerkToDirection(RE::BGSPerk* perk) const;
	void RemoveDirectionalPerks(RE::Actor* actor);
	void UIDrawAngles(RE::Actor* actor);
	bool DetermineMirrored(RE::Actor* actor);

	void Initialize();
	void UpdateCharacter(RE::Actor* actor, float delta);
	void Update(float delta);
	void SendAnimationEvent(RE::Actor* actor);
	void DebuffActor(RE::Actor* actor);
	void AddCombo(RE::Actor* actor);
	inline unsigned GetRepeatCount(RE::Actor* actor)
	{
		auto Iter = ComboDatas.find(actor->GetHandle());
		if (Iter != ComboDatas.end())
		{
			return Iter->second.repeatCount;
		}
		return 0;
	}
	inline bool IsUnblockable(RE::Actor* actor)
	{
		return actor->HasPerk(Unblockable);
	}

	void Cleanup();

	void StartedAttackWindow(RE::Actor* actor)
	{
		InAttackWin.insert(actor->GetHandle());
	}

	void EndedAttackWindow(RE::Actor* actor)
	{
		InAttackWin.erase(actor->GetHandle());
	}
private:
	void CleanupActor(RE::Actor* actor);
	RE::BGSPerk* TR;
	RE::BGSPerk* TL;
	RE::BGSPerk* BL;
	RE::BGSPerk* BR;
	
	RE::BGSPerk* Unblockable;
	RE::BGSPerk* Debuff;
	RE::TESObjectACTI* ParryVFX;
	RE::BGSKeyword* NPCKeyword;

	// is mapping really better?
	// 5 compares versus hashing?
	//std::map<RE::BGSPerk*, Directions> PerkToDirection;
	//std::map<Directions, RE::BGSPerk*> DirectionToPerk;
	 

	bool CanSwitch(RE::Actor* actor);
	void SwitchDirectionSynchronous(RE::Actor* actor, Directions dir);
	struct DirectionSwitch
	{
		Directions dir;
		float timeLeft;
	};
	// The direction switches are queued as we don't want instant guard switches
	std::unordered_map<RE::ActorHandle, DirectionSwitch> DirectionTimers;

	// The transition is slower than the actual guard break time since it looks better,
	// so we need to queue the forceidle events as skyrim does not allow blending multiple animations
	// during blending another animation transition
	std::unordered_map<RE::ActorHandle, std::queue<float>> AnimationTimer;

	struct ComboData
	{
		// circular array
		std::vector<Directions> lastAttackDirs;
		int currentIdx;
		int repeatCount;
		int size;
		float timeLeft;
	};

	// Metadata to handle combos and punishing repeated attacks
	std::unordered_map<RE::ActorHandle, ComboData> ComboDatas;

	// To switch directions after the hitframe but still in attack state for fluid animations
	// set uses hash so is fast?
	std::unordered_set<RE::ActorHandle> InAttackWin;
};