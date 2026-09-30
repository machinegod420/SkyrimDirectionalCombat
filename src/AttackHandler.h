#pragma once
#include "Direction.h"
#include "Utils.h"
#include <unordered_map>
#include "parallel_hashmap/phmap.h"

// Attack stamina cost multiplier by consecutive same-direction count.
inline constexpr float RepeatCostMult[4] = { 1.f, 1.4f, 2.f, 2.7f };

class AttackHandler
{
public:
	// THIS IS A MEGA ATTRIBUTES TABLE FOR ALL DIRECTIONS
	// THIS LETS US TUNE PROPERTIES OF EACH LINE IN CODE RATHER THAN CHANGING THE ANIMATION SO WE CAN TUNE AND BALANCE EASILY
	// right now its just used for lunges, but can be used for like attack speed, damage, etc
	// Forward root motion during a swing's windup in directional fights is multiplied by
	// this: [line TR, TL, BL, BR][opener, chained][light, power].
	static constexpr float LungeMult[4][2][2] = {
		{ { 1.0f, 1.4f }, { 1.0f, 1.4f } },  // TR
		{ { 0.85f, 1.4f * 0.85f }, { 0.85f, 1.4f * 0.85f } },  // TL: a bit shorter, as in life
		{ { 1.0f, 1.4f }, { 1.0f, 1.4f } },  // BL
		{ { 1.0f, 1.4f }, { 1.0f, 1.4f } },  // BR
	};
	// A swing's entry: its line, whether its combo run has a landed hit, and power.
	static float LungeMultFor(Directions line, bool chained, bool power)
	{
		const int Line = static_cast<int>(line);
		return Line >= 0 && Line < 4 ? LungeMult[Line][chained][power] : 1.f;
	}
	// the game assumes that there is a direction that is optimal for poking (which is true if you dont edit the animations)
	static Directions PokeLine(RE::Actor* actor);
	// GetReach, longer on the thrust line for a light: a power there is a cut, not a poke.
	// The per-line reach model in full.
	static float LineReach(RE::Actor* actor, Directions line, bool power);
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
	// The stamina part of CanInitiateAttack: less than the next light swing costs.
	bool LacksAttackStamina(RE::Actor* actor);
	// What preHitFrame charges for a paid swing at the actor's current repeat step.
	float SwingStaminaCost(RE::Actor* actor, bool Power);
	bool InFeintWindow(RE::Actor* actor);
	float SecondsSinceFeint(RE::Actor* actor);

	void AddChamberWindow(RE::Actor* actor);
	void AddFeintWindow(RE::Actor* actor);
	void RemoveFeintWindow(RE::Actor* actor);

	// Old feint:
	// void AddFeintQueue(RE::Actor* actor);
	// bool InFeintQueue(RE::Actor* actor)
	// {
	// 	std::shared_lock lock(FeintQueueMtx);
	// 	return FeintQueue.contains(actor->GetHandle());
	// }

	void GiveAttackSpeedBuff(RE::Actor* actor);
	void GiveSmallAttackSpeedBuff(RE::Actor* actor);
	void RemoveSmallAttackSpeedBuff(RE::Actor* actor);
	// Guard-charge attack speed: Ratio is 0-1 from DirectionHandler's guard
	// charge, cashed in at swing start.
	void GiveChargeSpeedBuff(RE::Actor* actor, float Ratio);
	// Attack-speed penalty for the weak continuation of a combo (same side on
	// four lines, a light poke in any mode). Its own slot rather than netted into
	// the charge, so it keeps applying when guard charge is off. 0 clears it.
	// Routes through ApplySpeedNet; never touches WeaponSpeedMult directly.
	void SetChainLinePenalty(RE::Actor* actor, float Penalty);
	// Net attack-speed delta currently applied to the actor: charge, chain, same-side.
	float GetAttackSpeedDelta(RE::Actor* actor) const;
	// Movement slow on an actor caught in an opponent's combo: stacks per step
	// and lasts the combo window. Cleared when the actor lands a step of its own.
	void AddComboSlow(RE::Actor* actor);
	void ClearComboSlow(RE::Actor* actor);

	void AddLockout(RE::Actor* actor);
	void ResetAttackState(RE::Actor* actor);
	// Resets a melee attack state that has had nothing attacking in the graph for over a second.
	bool ClearStuckAttack(RE::Actor* actor, float& idleSeconds, float delta);
	bool HandleFeint(RE::Actor* actor);
	void HandleFeintChangeDirection(RE::Actor* actor);

	void DoAttack(RE::Actor* actor);
	void DoPowerAttack(RE::Actor* actor);
	// False when the engine refused the action.
	bool DoBash(RE::Actor* actor);

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
	// Also guarded by FeintWindowMtx.
	phmap::flat_hash_map<RE::ActorHandle, std::chrono::steady_clock::time_point> LastFeintAt;
	mutable std::shared_mutex FeintWindowMtx;

	// Old feint:
	// phmap::flat_hash_map<RE::ActorHandle, float> FeintQueue;
	// mutable std::shared_mutex FeintQueueMtx;

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
		SpeedMod ChainLine;   // negative: combo continued on a weak line
		SpeedMod ComboSlow;   // negative SpeedMult: caught in an opponent's combo
		// The one value ever written to WeaponSpeedMult for this actor. Removals
		// use THIS, never a recomputed figure, so the slots above can change
		// underneath without the two drifting apart.
		float appliedDelta = 0.f;
		// Same, for SpeedMult.
		float appliedMoveDelta = 0.f;
	};
	phmap::flat_hash_map<RE::ActorHandle, SpeedEntry> SpeedMods;
	mutable std::shared_mutex SpeedModsMtx;
	// Recomputes the net from live slots and moves the actor value to it.
	// Caller holds SpeedModsMtx.
	void ApplySpeedNet(RE::Actor* actor, SpeedEntry& Entry);
	// The SpeedMult counterpart. Caller holds SpeedModsMtx.
	void ApplyMoveNet(RE::Actor* actor, SpeedEntry& Entry);

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