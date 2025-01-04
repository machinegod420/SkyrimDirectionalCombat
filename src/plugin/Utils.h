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

