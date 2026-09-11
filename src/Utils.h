#pragma once

#include "Direction.h"
#include <vector>

// hash functions
// https://github.com/D7ry/valhallaCombat/blob/Master/src/bin/events/animEventHandler.cpp#L7
constexpr uint32_t hash(const char* data, size_t const size) noexcept
{
	uint32_t hash = 5381;

	for (const char* c = data; c < data + size; ++c) {
		hash = ((hash << 5) + hash) + (unsigned char)*c;
	}

	return hash;
}

constexpr uint32_t operator"" _h(const char* str, size_t size) noexcept
{
	return hash(str, size);
}

// stolen because need to hash actorhandle
namespace std
{
	template <>
	struct hash<RE::ActorHandle>
	{
		uint32_t operator()(const RE::ActorHandle& a_handle) const
		{
			uint32_t nativeHandle = const_cast<RE::ActorHandle*>(&a_handle)->native_handle();  // ugh
			return nativeHandle;
		}
	};
}

// Debug snapshot of an actor's engine-side state, for diffing what an event
// leaves behind on them. Declared here but defined in Utils.cpp — AttackHandler.h
// includes this header, so it can't pull in the handlers the body needs.
void DumpActorState(RE::Actor* a_actor, const char* a_tag);

inline bool IsPowerAttacking(RE::Actor* a_actor)
{
	auto currentProcess = a_actor->GetActorRuntimeData().currentProcess;
	if (currentProcess) {
		auto highProcess = currentProcess->high;
		if (highProcess) {
			auto attackData = highProcess->attackData;
			if (attackData) {
				auto flags = attackData->data.flags;
				return flags.any(RE::AttackData::AttackFlag::kPowerAttack) && !flags.any(RE::AttackData::AttackFlag::kBashAttack);
			}
		}
	}
	return false;
}

inline bool IsBashing(RE::Actor* a_actor)
{
	return a_actor->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kBash;
}

inline bool IsLockedInAnimation(RE::Actor* a_actor)
{
	if (!a_actor) return false;

	if (a_actor->IsInKillMove()) return true;
	if (a_actor->GetOccupiedFurniture()) return true;
	if (a_actor->IsInRagdollState()) return true;
	if (a_actor->IsInBleedout()) return true;
	if (a_actor->GetActorRuntimeData().boolBits.all(RE::Actor::BOOL_BITS::kParalyzed)) return true;
	if (a_actor->IsAnimationDriven()) return true;

	bool isSynced = false;
	if (a_actor->GetGraphVariableBool("bIsSynced", isSynced) && isSynced) return true;

	return false;
}

// Queue a disarm on the target via the engine's task queue (same path as
// the Disarm shout). The target's equipped weapon drops to the world,
// becomes lootable, and the engine handles all the inventory / extra-data
// bookkeeping internally. Queued, so it's safe to call from inside a hit
// handler — no re-entrancy. Returns true if a disarm was queued.
inline bool QueueDisarm(RE::Actor* a_target, RE::Actor* a_attacker)
{
	if (!a_target || !a_attacker)
	{
		return false;
	}
	auto* queue = RE::TaskQueueInterface::GetSingleton();
	if (!queue)
	{
		return false;
	}
	auto targetHandle = a_target->GetHandle();
	auto attackerHandle = a_attacker->GetHandle();
	queue->QueueActorDisarm(targetHandle, attackerHandle);
	return true;
}

inline bool UnequipRangedWeapon(RE::Actor* a_actor)
{
	if (!a_actor)
	{
		return false;
	}
	auto* equipped = a_actor->GetEquippedObject(false);
	if (!equipped)
	{
		return false;
	}
	auto* weapon = equipped->As<RE::TESObjectWEAP>();
	if (!weapon)
	{
		return false;
	}
	if (!weapon->IsBow() && !weapon->IsCrossbow() && !weapon->IsStaff())
	{
		return false;
	}
	auto* equipManager = RE::ActorEquipManager::GetSingleton();
	if (!equipManager)
	{
		return false;
	}
	return equipManager->UnequipObject(a_actor, weapon);
}

// Queue a disarm only if the target's right-hand weapon is a bow, crossbow,
// or staff. Right hand only because vanilla ranged weapons are all two-handed
// and offhand staves aren't a thing. Returns true if a disarm was queued.
inline bool DisarmRangedWeapon(RE::Actor* a_target, RE::Actor* a_attacker)
{
	if (!a_target)
	{
		return false;
	}
	auto* equipped = a_target->GetEquippedObject(false);
	if (!equipped)
	{
		return false;
	}
	auto* weapon = equipped->As<RE::TESObjectWEAP>();
	if (!weapon)
	{
		return false;
	}
	if (!weapon->IsBow() && !weapon->IsCrossbow() && !weapon->IsStaff())
	{
		return false;
	}
	if (!QueueDisarm(a_target, a_attacker))
	{
		return false;
	}
	auto targetHandle = a_target->GetHandle();
	SKSE::GetTaskInterface()->AddTask([targetHandle]()
	{
		auto actor = targetHandle.get();
		if (!actor)
		{
			return;
		}
		actor->AsActorState()->actorState2.weaponState = RE::WEAPON_STATE::kDrawn;
	});
	return true;
}

inline bool HasShield(RE::Actor* a_actor)
{
	auto LeftHand = a_actor->GetEquippedObject(true);
	return LeftHand ? LeftHand->IsArmor() : false;
}

inline bool HasFullShieldBlock(RE::Actor* a_actor)
{
	if (!a_actor->IsBlocking())
	{
		return false;
	}
	bool bHasShield = HasShield(a_actor);
	if (!bHasShield)
	{
		return false;
	}
	float ActorMaxStamina = a_actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
	float ActorCurrentStamina = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	float Ratio = ActorCurrentStamina / ActorMaxStamina;
	return Ratio > 0.8f;
}

class CircularArray
{
public:
	CircularArray(unsigned size) : currentIdx(0), size(0)
	{
		array.reserve(size);
	}
	void add(Directions dir)
	{
		array[currentIdx] = dir;
		Increment();
	}
private:
	void Increment()
	{
		currentIdx++;
		if (currentIdx >= size)
		{
			currentIdx = 0;
		}
	}
	void Decrement()
	{
		currentIdx--;
		if (currentIdx < 0)
		{
			currentIdx = size - 1;
		}
	}
	std::vector<Directions> array;
	int size;
	int currentIdx;
};

