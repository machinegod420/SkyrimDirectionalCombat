#pragma once
#include "Direction.h"
#include "Utils.h"
#include <unordered_map>
#include "parallel_hashmap/phmap.h"

class AttackHandler
{
public:
	RE::NiPointer<RE::BGSAttackData> PlayerAttackData;
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
	// Attack lockout only. For "may this actor swing right now", use
	// CanInitiateAttack — this one is also read as a strategic signal.
	bool CanAttack(RE::Actor* actor);
	bool CanInitiateAttack(RE::Actor* actor);
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
	// Guard-charge attack speed: Ratio is 0-1 from DirectionHandler's guard
	// charge, cashed in at swing start.
	void GiveChargeSpeedBuff(RE::Actor* actor, float Ratio);
	// Attack-speed penalty for a cut thrown from the same horizontal side as the
	// last one. Its own slot rather than netted into the charge: unrelated
	// mechanic, and it must keep applying when guard charge is switched off.
	// Passing false clears it. Everything here routes through ApplySpeedNet;
	// none of it touches WeaponSpeedMult directly.
	void SetSameSideSpeedPenalty(RE::Actor* actor, bool Penalise);
	
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


	// One contribution: a delta and its own clock. Expiring zeroes the delta so
	// a lapsed slot contributes nothing without needing to be erased.
	struct SpeedMod
	{
		float Delta = 0.f;
		float TimeLeft = 0.f;
		bool Active() const { return TimeLeft > 0.f; }
		float Live() const { return TimeLeft > 0.f ? Delta : 0.f; }
		void Set(float NewDelta, float Duration)
		{
			Delta = (Duration > 0.f) ? NewDelta : 0.f;
			TimeLeft = (Duration > 0.f) ? Duration : 0.f;
		}
		void Tick(float delta)
		{
			if (TimeLeft <= 0.f)
			{
				return;
			}
			TimeLeft -= delta;
			if (TimeLeft <= 0.f)
			{
				Set(0.f, 0.f);
			}
		}
	};
	struct SpeedEntry
	{
		SpeedMod Chain;       // attack chain, 1s
		SpeedMod SmallChain;  // small attack chain, 2s
		SpeedMod Charge;      // guard charge cashed in at swing start
		SpeedMod SameSide;    // negative: combo continued on the same side
		// The one value ever written to WeaponSpeedMult for this actor. Removals
		// use THIS, never a recomputed figure, so the slots above can change
		// underneath without the two drifting apart.
		float appliedDelta = 0.f;
	};
	phmap::flat_hash_map<RE::ActorHandle, SpeedEntry> SpeedMods;
	mutable std::shared_mutex SpeedModsMtx;
	// Recomputes the net from live slots and moves the actor value to it.
	// Caller holds SpeedModsMtx.
	void ApplySpeedNet(RE::Actor* actor, SpeedEntry& Entry);

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