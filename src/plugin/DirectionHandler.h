#pragma once

#include "UIMenu.h"
#include "Direction.h"
#include "Utils.h"

#include <shared_mutex>
#include <unordered_set>
#include "parallel_hashmap/phmap.h"

#include "3rdparty/TrueDirectionalMovementAPI.h"

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
		BattleaxeKeyword = nullptr;
		PikeKeyword = nullptr;
		PikeKeyword2 = nullptr;
	}
	static DirectionHandler* GetSingleton()
	{
		static DirectionHandler obj;
		return std::addressof(obj);
	}

	bool HasDirectionalPerks(RE::Actor* actor) const;
	bool HasBlockAngle(RE::Actor* attacker, RE::Actor* target) const;
	void AddDirectional(RE::Actor* actor, RE::TESObjectWEAP* weapon);
	void SwitchDirectionLeft(RE::Actor* actor, bool ChangeQueued);
	void SwitchDirectionUp(RE::Actor* actor, bool ChangeQueued);
	void SwitchDirectionDown(RE::Actor* actor, bool ChangeQueued);
	void SwitchDirectionRight(RE::Actor* actor, bool ChangeQueued);
	// force == override direction and timeLeft value
	// overwrite == overide timeLeft value
	void WantToSwitchTo(RE::Actor* actor, Directions dir, bool force = false, bool overwrite = true, bool lock = false);
	RE::SpellItem* DirectionToPerk(Directions dir) const;
	RE::SpellItem* GetDirectionalPerk(RE::Actor* actor) const;
	Directions PerkToDirection(RE::SpellItem* perk) const;
	inline Directions GetCurrentDirection(RE::Actor* actor) const
	{
		Directions ret = Directions::TR;
		ActiveDirectionsMtx.lock_shared();
		auto Iter = ActiveDirections.find(actor->GetHandle());
		if (Iter != ActiveDirections.end())
		{
			ret = Iter->second;
		}
		ActiveDirectionsMtx.unlock_shared();
		return ret;
	}
	inline bool HasQueuedDirection(RE::Actor* actor, Directions& OutDirection)
	{
		bool ret = false;
		DirectionTimersMtx.lock_shared();
		if (DirectionTimers.contains(actor->GetHandle()))
		{
			OutDirection = DirectionTimers.at(actor->GetHandle()).dir;
			ret = true;
		}
		DirectionTimersMtx.unlock_shared();
		return ret;
	}
	void RemoveDirectionalPerks(RE::ActorHandle handle);
	void UIDrawAngles(RE::Actor* actor);
	bool DetermineMirrored(RE::Actor* actor);
	void AdjustActorScale(RE::Actor* actor);

	void Initialize(TDM_API::IVTDM2 *tdm);
	void UpdateCharacter(RE::Actor* actor, float delta);
	void Update(float delta);
	void SendAnimationEvent(RE::Actor* actor, bool slow);
	void QueueAnimationEvent(RE::Actor* actor);
	void ClearAnimationQueue(RE::Actor* actor);
	void DebuffActor(RE::Actor* actor);
	void AddCombo(RE::Actor* actor);
	inline unsigned GetRepeatCount(RE::Actor* actor) const
	{
		unsigned ret = 0;
		ComboDatasMtx.lock_shared();
		auto Iter = ComboDatas.find(actor->GetHandle());
		if (Iter != ComboDatas.end())
		{
			ret = Iter->second.repeatCount;
		}
		ComboDatasMtx.unlock_shared();
		return ret;
	}
	inline bool IsUnblockable(RE::Actor* actor) const
	{
		bool ret = false;
		UnblockableActorsMtx.lock_shared();
		ret = UnblockableActors.contains(actor->GetHandle());
		UnblockableActorsMtx.unlock_shared();
		return ret;
	}
	inline bool HasImperfectParry(RE::Actor* actor) const
	{
		bool ret = false;
		UnblockableActorsMtx.lock_shared();
		ret = ImperfectParry.contains(actor->GetHandle());
		UnblockableActorsMtx.unlock_shared();
		return ret;
	}
	inline bool HasTimedParry(RE::Actor* actor) const;

	void AddTimedParry(RE::Actor* actor);

	static Directions GetCounterDirection(Directions Direction)
	{
		switch (Direction)
		{
		case Directions::TR:
			return Directions::TL;
			break;
		case Directions::TL:
			return Directions::TR;
			break;
		case Directions::BR:
			return Directions::BL;
			break;
		default:
			return Directions::BR;
			break;
		}
	}

	bool CanSwitch(RE::Actor* actor);
	void Cleanup();

	inline void StartedAttackWindow(RE::Actor* actor)
	{
		//SendAnimationEvent(actor);
		InAttackWinMtx.lock();
		InAttackWin.insert(actor->GetHandle());
		InAttackWinMtx.unlock();
	}

	inline bool InAttackWindow(RE::Actor* actor)
	{
		std::shared_lock lock(InAttackWinMtx);
		return InAttackWin.contains(actor->GetHandle());
	}

	inline void EndedAttackWindow(RE::Actor* actor)
	{
		//SendAnimationEvent(actor);
		InAttackWinMtx.lock();
		InAttackWin.erase(actor->GetHandle());
		InAttackWinMtx.unlock();
	}

	// DO NOT CALL THIS PUBLICALLY UNLESS IN VERY SPECIFIC SITUATIONS
	void SwitchDirectionSynchronous(RE::Actor* actor, Directions dir, bool wasBlocking);
private:
	TDM_API::IVTDM2 *TDM;
	void CleanupActor(RE::ActorHandle actor);
	RE::SpellItem* TR;
	RE::SpellItem* TL;
	RE::SpellItem* BL;
	RE::SpellItem* BR;
	
	RE::SpellItem* Unblockable;
	RE::BGSPerk* Debuff;
	RE::TESObjectACTI* ParryVFX;
	RE::BGSKeyword* NPCKeyword;

	// This is so incredibly specific i hate having it
	RE::BGSKeyword* BattleaxeKeyword;
	RE::BGSKeyword* PikeKeyword;
	RE::BGSKeyword* PikeKeyword2;

	// is mapping really better?
	// 5 compares versus hashing?
	//std::map<RE::BGSPerk*, Directions> PerkToDirection;
	//std::map<Directions, RE::BGSPerk*> DirectionToPerk;
	 

	
	struct DirectionSwitch
	{
		// for buffering
		bool wasBlocking = false;
		Directions dir;
		bool locked = false;
		float timeLeft = 0.f;
	};
	// The direction switches are queued as we don't want instant guard switches
	phmap::flat_hash_map<RE::ActorHandle, DirectionSwitch> DirectionTimers;
	mutable std::shared_mutex DirectionTimersMtx;

	// The transition is slower than the actual guard break time since it looks better,
	// so we need to queue the forceidle events as skyrim does not allow blending multiple animations
	// during blending another animation transition
	struct AnimationEvent
	{
		float timeLeft = 0.f;
		bool slow = false;
	};
	phmap::flat_hash_map<RE::ActorHandle, std::vector<AnimationEvent>> AnimationTimer;
	mutable std::shared_mutex AnimationTimerMtx;

	struct ComboData
	{
		// circular array
		std::vector<Directions> lastAttackDirs;
		int currentIdx = 0;
		int repeatCount = 0;
		int size = 0;
		float timeLeft = 0.f;
	};

	// Metadata to handle combos and punishing repeated attacks
	phmap::flat_hash_map<RE::ActorHandle, ComboData> ComboDatas;
	mutable std::shared_mutex ComboDatasMtx;

	// To switch directions after the hitframe but still in attack state for fluid animations
	// set uses hash so is fast?
	// <ActorHandle, Last attack was power attack> for tracking attack chains
	phmap::flat_hash_set<RE::ActorHandle> InAttackWin;
	mutable std::shared_mutex InAttackWinMtx;

	// Have to record directions here
	// Because of the way skyrim handles spells, we need these to be totally synchronous
	phmap::flat_hash_map<RE::ActorHandle, Directions> ActiveDirections;
	mutable std::shared_mutex ActiveDirectionsMtx;

	// Set to determine who is unblockable
	phmap::flat_hash_set<RE::ActorHandle> UnblockableActors;
	mutable std::shared_mutex UnblockableActorsMtx;

	// Determine who has an imperfect parry
	phmap::flat_hash_set<RE::ActorHandle> ImperfectParry;
	mutable std::shared_mutex ImperfectParryMtx;

	// Determine who has a timed parry
	phmap::flat_hash_map<RE::ActorHandle, float> TimedParry;
	mutable std::shared_mutex TimedParryMtx;

	phmap::flat_hash_set<RE::ActorHandle> ToAdd;
	mutable std::shared_mutex ToAddMtx;

	phmap::flat_hash_set<RE::ActorHandle> ToRemove;
	mutable std::shared_mutex ToRemoveMtx;

};