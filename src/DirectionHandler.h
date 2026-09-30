#pragma once

#include "UIMenu.h"
#include "Direction.h"
#include "Utils.h"
#include "CreatureHandler.h"

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
		ComboChain1 = nullptr;
		ComboChain2 = nullptr;
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
	// A perked actor or a creature that attacks on lines. The AI's branch
	// split and the vanilla-attack refusal key on this, not on perks alone.
	bool IsDirectionalOpponent(RE::Actor* actor) const;
	bool HasBlockAngle(RE::Actor* attacker, RE::Actor* target) const;
	void AddDirectional(RE::Actor* actor, RE::TESObjectWEAP* weapon);
	void ApplyModeMarker(RE::Actor* actor);
	// Which attack animation set the actor is in. The only place that mapping
	// lives; pikes ride the battleaxe set, an unarmed or unknown weapon defaults.
	WeaponSet AnimationSet(RE::Actor* actor) const;
	// The line this direction collapses onto in the active mode. For Honor folds
	// TL into TR, KCD folds BR into BL, Normal folds nothing. One definition of
	// "which lines exist" — everything that used to special-case a mode asks here.
	static Directions FoldDirection(Directions dir);
	static bool DirectionEnabled(Directions dir) { return FoldDirection(dir) == dir; }
	// Bitmask over Directions of the lines this mode uses, for the UI.
	static uint8_t EnabledDirections();
	// Diagnostic: the line held before the last switch, and seconds since it.
	Directions GetPreviousDirection(RE::Actor* actor, float& outAgo) const;
	// Diagnostic: the actor the player is locked onto, 0 if none. The HUD only
	// draws that one, so it is the guard the player was actually reading.
	uint32_t GetLockedTargetFormID() const;
	RE::Actor* GetLockedTarget() const;
	// Who a swing is aimed at: the player's lock, else the combat target.
	RE::Actor* GetSwingTarget(RE::Actor* actor) const;
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
		const bool found = Iter != ActiveDirections.end();
		if (found)
		{
			ret = Iter->second;
		}
		ActiveDirectionsMtx.unlock_shared();
		// Creature swings carry a line without an entry here. Asked after the
		// lock is released so the two never nest.
		if (!found)
		{
			CreatureHandler::GetSingleton()->GetAttackLine(actor, ret);
		}
		return ret;
	}
	inline bool HasQueuedDirection(RE::Actor* actor, Directions& OutDirection)
	{
		bool ret = false;
		DirectionTimersMtx.lock_shared();
		auto Iter = DirectionTimers.find(actor->GetHandle());
		if (Iter != DirectionTimers.end())
		{
			OutDirection = Iter->second.dir;
			ret = true;
		}
		DirectionTimersMtx.unlock_shared();
		return ret;
	}
	void RemoveDirectionalPerks(RE::ActorHandle handle);
	void UIDrawAngles(RE::Actor* actor);
	// Distance and only-show-targetted filters for a non-player marker.
	bool ShouldShowMarker(RE::Actor* actor) const;
	bool DetermineMirrored(RE::Actor* actor);
	void AdjustActorScale(RE::Actor* actor);

	void Initialize(TDM_API::IVTDM2 *tdm);
	// Pure capability — does this actor's race/equipment/alive state support
	// directional combat? Independent of distance, lock state, or current
	// target.
	bool CanHaveDirectionalPerks(RE::Actor* actor, RE::TESObjectWEAP*& outWeapon) const;

	bool ShouldHaveDirectionalPerks(RE::Actor* actor, RE::TESObjectWEAP*& outWeapon);
	void UpdateCharacter(RE::Actor* actor, float delta);
	void Update(float delta);
	void SendAnimationEvent(RE::Actor* actor, bool slow);
	void QueueAnimationEvent(RE::Actor* actor);
	// True if a guard transition was queued or blending.
	bool ClearAnimationQueue(RE::Actor* actor);
	void DebuffActor(RE::Actor* actor);
	// ForceComplete grants outright, skipping the direction recording and the
	// distinctness test. Also skips the consume path, so it can never eat a
	// token already held.
	// ExtraSteps credits steps beyond the hit's own, without a direction slot.
	void AddCombo(RE::Actor* actor, bool ForceComplete = false, int ExtraSteps = 0);
	// Ends the actor's combo, as the timeout does: the chain is over.
	void ResetCombo(RE::Actor* actor);
	// The combo rule: every direction in the window must differ.
	// Pointer + count rather than a container so callers can pass a stack
	// buffer — WouldCompleteCombo runs under two locks and must not allocate.
	static constexpr float ComboFlashTime = 0.3f;
	static bool AllDirectionsDistinct(const Directions* Dirs, int Count)
	{
		for (int i = 0; i < Count; ++i)
		{
			for (int j = i + 1; j < Count; ++j)
			{
				if (Dirs[i] == Dirs[j])
				{
					return false;
				}
			}
		}
		return true;
	}
	// Four lines: all distinct. Three: differs from the last, so the finisher keeps a choice.
	static bool RunAdvances(const Directions* Dirs, int Count)
	{
		if (EnabledDirections() == 0xF)
		{
			return AllDirectionsDistinct(Dirs, Count);
		}
		return Count < 2 || Dirs[Count - 1] != Dirs[Count - 2];
	}
	// TR/BR are the right side, TL/BL the left. Used by the same-side combo
	// penalty: a continuation that crosses is the strong one, staying on the
	// same side is the weak one.
	static bool IsLeftSide(Directions Dir)
	{
		return Dir == Directions::TL || Dir == Directions::BL;
	}
	// Direction of the PREVIOUS landed attack. False when there is no history,
	// so the opening attack of a chain is never penalised. Read before AddCombo
	// runs for the current hit, so currentIdx - 1 is the last one recorded.
	inline bool GetLastAttackDirection(RE::Actor* actor, Directions& OutDirection) const
	{
		bool ret = false;
		ComboDatasMtx.lock_shared();
		auto Iter = ComboDatas.find(actor->GetHandle());
		if (Iter != ComboDatas.end() && Iter->second.size > 0)
		{
			const ComboData& Data = Iter->second;
			const int ComboSize = static_cast<int>(Data.lastAttackDirs.size());
			if (ComboSize > 0)
			{
				int LastIdx = Data.currentIdx - 1;
				if (LastIdx < 0)
				{
					LastIdx = ComboSize - 1;
				}
				OutDirection = Data.lastAttackDirs[LastIdx];
				ret = true;
			}
		}
		ComboDatasMtx.unlock_shared();
		return ret;
	}
	// 0-1 flash intensity, decaying after the combo last advanced.
	inline float GetComboFlash(RE::Actor* actor) const
	{
		float ret = 0.f;
		ComboDatasMtx.lock_shared();
		auto Iter = ComboDatas.find(actor->GetHandle());
		if (Iter != ComboDatas.end())
		{
			ret = std::clamp(Iter->second.flashLeft / ComboFlashTime, 0.f, 1.f);
		}
		ComboDatasMtx.unlock_shared();
		return ret;
	}
	// True when dir already appears in the actor's live combo window, so a hit
	// there cannot advance the combo.
	inline bool IsInComboWindow(RE::Actor* actor, Directions dir) const
	{
		bool ret = false;
		ComboDatasMtx.lock_shared();
		auto Iter = ComboDatas.find(actor->GetHandle());
		if (Iter != ComboDatas.end())
		{
			const ComboData& Data = Iter->second;
			const int ComboSize = static_cast<int>(Data.lastAttackDirs.size());
			// Same rule as RunAdvances: on three lines only the last hit blocks.
			const int Count = EnabledDirections() == 0xF ?
				std::min(Data.size, ComboSize) : std::min(Data.size, 1);
			for (int i = 1; i <= Count; ++i)
			{
				if (Data.lastAttackDirs[(Data.currentIdx - i + ComboSize) % ComboSize] == dir)
				{
					ret = true;
					break;
				}
			}
		}
		ComboDatasMtx.unlock_shared();
		return ret;
	}
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
	// How far into a combo the actor is: 0 fresh, 1 when a single landed hit
	// would finish it. Counts the newest hits that are still pairwise distinct,
	// so a repeat adds nothing — its slot has to be overwritten before the
	// window can complete.
	inline float GetComboProgress(RE::Actor* actor) const
	{
		float ret = 0.f;
		ComboDatasMtx.lock_shared();
		auto Iter = ComboDatas.find(actor->GetHandle());
		if (Iter != ComboDatas.end())
		{
			const int ComboSize = static_cast<int>(Iter->second.lastAttackDirs.size());
			if (ComboSize > 1)
			{
				ret = std::clamp(static_cast<float>(ComboRun(Iter->second) + Iter->second.bonus) /
					static_cast<float>(ComboSize - 1), 0.f, 1.f);
			}
		}
		ComboDatasMtx.unlock_shared();
		return ret;
	}
	// Landed hits in the current combo run, a landed power counting two. 0 with no combo.
	inline int GetComboStep(RE::Actor* actor) const
	{
		int ret = 0;
		ComboDatasMtx.lock_shared();
		auto Iter = ComboDatas.find(actor->GetHandle());
		if (Iter != ComboDatas.end())
		{
			ret = ComboRun(Iter->second) + Iter->second.bonus;
		}
		ComboDatasMtx.unlock_shared();
		return ret;
	}
	// True when a landed hit in NextDirection would complete the combo. size
	// saturates at comboSize and only resets on a grant, so a size test alone
	// can't answer this — an actor that has attacked many times sits at the cap
	// while any distinct direction would still finish.
	inline bool WouldCompleteCombo(RE::Actor* actor, Directions NextDirection) const
	{
		bool ret = false;
		ComboDatasMtx.lock_shared();
		auto Iter = ComboDatas.find(actor->GetHandle());
		if (Iter != ComboDatas.end())
		{
			const ComboData& Data = Iter->second;
			// the array is sized to comboSize at creation, so it is the
			// authority — no need to re-derive it from settings here
			const int ComboSize = static_cast<int>(Data.lastAttackDirs.size());
			ret = ComboSize > 0 && ComboRunWith(Data, NextDirection) + Data.bonus >= ComboSize;
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
		ImperfectParryMtx.lock_shared();
		ret = ImperfectParry.contains(actor->GetHandle());
		ImperfectParryMtx.unlock_shared();
		return ret;
	}
	inline bool HasTimedParry(RE::Actor* actor) const;

	void AddTimedParry(RE::Actor* actor);

	// Guard charge ("being set"): wall-clock seconds the actor has held its
	// current guard line without attacking. Accrued in UpdateCharacter, zeroed
	// on any completed direction switch and on taking a clean hit. 
	// used for incentivizing holding a guard
	float GetGuardChargeRatio(RE::Actor* actor) const;
	// returns the 0-1 ratio AND zeroes the clock — call once, at attack start
	float ConsumeGuardCharge(RE::Actor* actor);
	void ResetGuardCharge(RE::Actor* actor);

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

	bool CanSwitch(RE::Actor* actor) const;
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
	// Swaps the direction spells to `dir`.
	void ApplyDirectionSpells(RE::Actor* actor, Directions dir);
private:
	TDM_API::IVTDM2 *TDM;
	void CleanupActor(RE::ActorHandle actor);
	RE::SpellItem* TR;
	RE::SpellItem* TL;
	RE::SpellItem* BL;
	RE::SpellItem* BR;
	
	RE::SpellItem* Unblockable;
	// Animation-condition markers for the non-default direction modes.
	RE::SpellItem* ForHonorMarker = nullptr;
	RE::SpellItem* KCDMarker = nullptr;
	RE::SpellItem* ComboChain1;
	RE::SpellItem* ComboChain2;
	void ApplyComboChain(RE::Actor* actor, int run);
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
		// Seconds of indicator flash left, set when the combo advances.
		float flashLeft = 0.f;
		// circular array
		std::vector<Directions> lastAttackDirs;
		int currentIdx = 0;
		int repeatCount = 0;
		int size = 0;
		float timeLeft = 0.f;
		// Steps credited without a direction slot (a landed power attack counts
		// two). Kept only while each hit extends the run.
		int bonus = 0;
	};
	// Newest hits that are still pairwise distinct: the ones that count toward
	// completing the combo. Caller holds ComboDatasMtx.
	static int ComboRun(const ComboData& Data)
	{
		Directions Run[4];
		const int ComboSize = static_cast<int>(Data.lastAttackDirs.size());
		const int Valid = std::min(Data.size, static_cast<int>(std::size(Run)));
		int Len = 0;
		for (int i = 1; i <= Valid; ++i)
		{
			// newest first: the slot before currentIdx, wrapping
			Run[Len] = Data.lastAttackDirs[(Data.currentIdx - i + ComboSize) % ComboSize];
			if (!RunAdvances(Run, Len + 1))
			{
				break;
			}
			++Len;
		}
		return Len;
	}
	// ComboRun as it would be after one more hit in NextDirection: that hit,
	// then the newest entries left once it overwrites the oldest slot. Stack
	// only, since callers hold ComboDatasMtx and sometimes DifficultyMapMtx.
	static int ComboRunWith(const ComboData& Data, Directions NextDirection)
	{
		Directions Run[4];
		const int ComboSize = static_cast<int>(Data.lastAttackDirs.size());
		const int Valid = std::min(Data.size, std::min(ComboSize - 1, static_cast<int>(std::size(Run)) - 1));
		Run[0] = NextDirection;
		int Len = 1;
		for (int i = 1; i <= Valid; ++i)
		{
			Run[Len] = Data.lastAttackDirs[(Data.currentIdx - i + ComboSize) % ComboSize];
			if (!RunAdvances(Run, Len + 1))
			{
				break;
			}
			++Len;
		}
		return Len;
	}

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
	// Diagnostic only: the line each actor held before its last switch, and when
	// that switch landed, so a block that reads wrong can be reconstructed.
	struct SwitchRecord
	{
		Directions previous = Directions::TR;
		std::chrono::steady_clock::time_point at{};
	};
	phmap::flat_hash_map<RE::ActorHandle, SwitchRecord> SwitchHistory;
	mutable std::shared_mutex SwitchHistoryMtx;
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

	// Seconds held on the current guard line without attacking (see
	// GetGuardChargeRatio). Pruned alongside TimedParry in Update.
	phmap::flat_hash_map<RE::ActorHandle, float> GuardCharge;
	mutable std::shared_mutex GuardChargeMtx;

	phmap::flat_hash_set<RE::ActorHandle> ToAdd;
	mutable std::shared_mutex ToAddMtx;

	phmap::flat_hash_set<RE::ActorHandle> ToRemove;
	mutable std::shared_mutex ToRemoveMtx;

};