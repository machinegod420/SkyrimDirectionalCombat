#include "AttackHandler.h"
#include "SettingsLoader.h"
#include "DirectionHandler.h"

constexpr float NPCLockoutTime = 0.15f;
constexpr float AttackSpeedMult = 0.25f;
constexpr float SmallAttackSpeedMult = 0.12f;
constexpr float FeintQueueTime = 0.2f; // carefully tailored magic number
// Leak-guard only. 
constexpr float ChargeBuffDuration = 3.0f;

void AttackHandler::Initialize()
{
	RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
	FeintFX = DataHandler->LookupForm<RE::SpellItem>(0x6E60, "DirectionMod.esp");
	WeaponSpeedBuff = DataHandler->LookupForm<RE::SpellItem>(0x7928, "DirectionMod.esp");

	ActionAttack = (RE::BGSAction*)RE::TESForm::LookupByID(0x13005);
	ActionPowerAttack = (RE::BGSAction*)RE::TESForm::LookupByID(0x13383);

	for (auto& iter : RE::PlayerCharacter::GetSingleton()->GetRace()->attackDataMap->attackDataMap)
	{
		if (iter.first == "attackStart")
		{
			PlayerAttackData = iter.second;
		}
	}
}


bool AttackHandler::InChamberWindow(RE::Actor* actor)
{
	bool ret = false;
	ChamberWindowMtx.lock_shared();
	ret = ChamberWindow.contains(actor->GetHandle());
	ChamberWindowMtx.unlock_shared();
	return ret;
}

float AttackHandler::GetChamberWindowTime(RE::Actor* actor)
{
	float ret = 0.f;
	ChamberWindowMtx.lock_shared();
	if (ChamberWindow.contains(actor->GetHandle()))
	{
		ret = ChamberWindow.at(actor->GetHandle());
	}
	ChamberWindowMtx.unlock_shared();
	return ret;
}

bool AttackHandler::InFeintWindow(RE::Actor* actor)
{
	bool ret = false;
	FeintWindowMtx.lock_shared();
	ret = FeintWindow.contains(actor->GetHandle());
	FeintWindowMtx.unlock_shared();
	return ret;
}

void AttackHandler::RemoveFeintWindow(RE::Actor* actor)
{
	// Exclusive lock — erase is a write, and mutating under a shared lock is UB.
	std::unique_lock lock(FeintWindowMtx);
	FeintWindow.erase(actor->GetHandle());
}

bool AttackHandler::CanAttack(RE::Actor* actor)
{
	bool ret = false;

	AttackLockoutMtx.lock_shared();
	ret = !AttackLockout.contains(actor->GetHandle());
	AttackLockoutMtx.unlock_shared();

	return ret;
}

bool AttackHandler::CanInitiateAttack(RE::Actor* actor)
{
	if (!CanAttack(actor))
	{
		return false;
	}
	if (InFeintWindow(actor) || InFeintQueue(actor))
	{
		return false;
	}
	if (DifficultySettings::AttacksCostStamina)
	{
		auto* Values = actor->AsActorValueOwner();
		if (Values->GetActorValue(RE::ActorValue::kStamina) <
			Values->GetPermanentActorValue(RE::ActorValue::kStamina) * DifficultySettings::StaminaCost)
		{
			return false;
		}
	}
	return true;
}

void AttackHandler::HandleFeintChangeDirection(RE::Actor* actor)
{
	Directions dir = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
	if (dir == Directions::TR || dir == Directions::BR)
	{
		if (Settings::ForHonorMode)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(actor, Directions::BL, true, true, true);
		}
		else
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(actor, Directions::TL, true, true, true);

		}

	}
	else
	{
		DirectionHandler::GetSingleton()->WantToSwitchTo(actor, Directions::TR, true, true, true);
	}

}

void AttackHandler::HandleFeint(RE::Actor* actor)
{
	FeintWindowMtx.lock();
	if (FeintWindow.contains(actor->GetHandle()))
	{
		actor->NotifyAnimationGraph("ForceAttackStop");

		//actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
		actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)->CastSpellImmediate(FeintFX, false, actor, 0.f, false, 0.f, nullptr);
		actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina,15);
		HandleFeintChangeDirection(actor);
		GiveAttackSpeedBuff(actor);
		FeintWindow.erase(actor->GetHandle());
		AddFeintQueue(actor);
	}
	FeintWindowMtx.unlock();
}

void AttackHandler::AddChamberWindow(RE::Actor* actor)
{
	// masterstrike window must be a fixed time, otherwise slower attack speeds actually make it easier to masterstrike
	ChamberWindowMtx.lock();
	ChamberWindow[actor->GetHandle()] = DifficultySettings::ChamberWindowTime;
	ChamberWindowMtx.unlock();
}

void AttackHandler::AddFeintWindow(RE::Actor* actor)
{
	FeintWindowMtx.lock();
	FeintWindow[actor->GetHandle()] = DifficultySettings::FeintWindowTime;
	FeintWindowMtx.unlock();
}

void AttackHandler::RemoveLockout(RE::Actor* actor)
{

	AttackLockoutMtx.lock();
	if (AttackLockout.contains(actor->GetHandle()))
	{
		AttackLockout.erase(actor->GetHandle());
	}
	AttackLockoutMtx.unlock();

}

void AttackHandler::AddLockout(RE::Actor* actor)
{
	// this hack is necessary because of the way MCO works. it will queue an attack on the animation graph once the player clicks after the
	// nextattack window is passed. during this time, the player can still get hit and commit a lockout. but, the queued animation will still play
	// because it is already being parsed by the animation graph, and the plugin cannot intercept it. so we have to force all attacks to end. 
	if (actor->IsAttacking())
	{
		actor->NotifyAnimationGraph("attackStop");
		// sometimes this gets busted so we might have to force reset the state
		// there's actually big problems with setting state like this as skyrim has certain expectations on what state is filled in the actor as well
		actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
	}

	{
		AttackLockoutMtx.lock();
		AttackLockout[actor->GetHandle()] = DifficultySettings::AttackTimeoutTime;
		AttackLockoutMtx.unlock();
	}

}

void AttackHandler::AddFeintQueue(RE::Actor* actor)
{
	FeintQueueMtx.lock();
	if (!FeintQueue.contains(actor->GetHandle()))
	{
		FeintQueue[actor->GetHandle()] = FeintQueueTime;
	}
	FeintQueueMtx.unlock();
}

void DoAction(RE::Actor* actor, RE::BGSAction* action)
{
	std::unique_ptr<RE::TESActionData> data(RE::TESActionData::Create());
	data->source = RE::NiPointer<RE::TESObjectREFR>(actor);
	data->action = action;
	typedef bool func_t(RE::TESActionData*);
	REL::Relocation<func_t> func{ RELOCATION_ID(40551, 41557) };
	bool succ = func(data.get());
	if (!succ)
	{
		if (Settings::VerboseLogging) logger::info("[attack] failed attack action! {}", actor->GetName());
	}
}

void AttackHandler::DoAttack(RE::Actor* actor)
{
	DoAction(actor, ActionAttack);
}

void AttackHandler::DoPowerAttack(RE::Actor* actor)
{
	DoAction(actor, ActionPowerAttack);
}

void AttackHandler::Cleanup()
{

	ChamberWindowMtx.lock();
	ChamberWindow.clear();
	ChamberWindowMtx.unlock();

	AttackLockoutMtx.lock();
	AttackLockout.clear();
	AttackLockoutMtx.unlock();

	FeintWindowMtx.lock();
	FeintWindow.clear();
	FeintWindowMtx.unlock();

	{
		// One pass: back out each actor's applied net, then drop everything.
		std::unique_lock lock(SpeedModsMtx);
		for (auto& Entry : SpeedMods)
		{
			if (!Entry.first)
			{
				continue;
			}
			if (RE::Actor* actor = Entry.first.get().get())
			{
				if (Entry.second.appliedDelta != 0.f)
				{
					actor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kWeaponSpeedMult, -Entry.second.appliedDelta);
				}
			}
		}
		SpeedMods.clear();
	}
}

void AttackHandler::RemoveActor(RE::ActorHandle actor)
{

	ChamberWindowMtx.lock();
	ChamberWindow.erase(actor);
	ChamberWindowMtx.unlock();
	AttackLockoutMtx.lock();
	AttackLockout.erase(actor);
	AttackLockoutMtx.unlock();
	FeintWindowMtx.lock();
	FeintWindow.erase(actor);
	FeintWindowMtx.unlock();

	RE::Actor* a = actor.get().get();

	{
		std::unique_lock lock(SpeedModsMtx);
		auto Iter = SpeedMods.find(actor);
		if (Iter != SpeedMods.end())
		{
			if (a && Iter->second.appliedDelta != 0.f)
			{
				a->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kWeaponSpeedMult, -Iter->second.appliedDelta);
			}
			SpeedMods.erase(Iter);
		}
	}
}

// The single point that writes WeaponSpeedMult. Backs out the previously
// applied value and applies the new net — never a recomputed removal, so the
// two can't drift even if a slot's inputs changed underneath.

void AttackHandler::ApplySpeedNet(RE::Actor* actor, SpeedEntry& Entry)
{
	const float Target = Entry.Chain.Live() + Entry.SmallChain.Live() +
		Entry.Charge.Live() + Entry.SameSide.Live();
	if (Target == Entry.appliedDelta)
	{
		return;
	}
	if (Entry.appliedDelta != 0.f)
	{
		actor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kWeaponSpeedMult, -Entry.appliedDelta);
	}
	if (Target != 0.f)
	{
		actor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kWeaponSpeedMult, Target);
	}
	Entry.appliedDelta = Target;
}

void AttackHandler::GiveAttackSpeedBuff(RE::Actor* actor)
{
	std::unique_lock lock(SpeedModsMtx);
	auto& Entry = SpeedMods[actor->GetHandle()];
	Entry.Chain.Set(AttackSpeedMult, 1.f);
	ApplySpeedNet(actor, Entry);
}

void AttackHandler::GiveSmallAttackSpeedBuff(RE::Actor* actor)
{
	std::unique_lock lock(SpeedModsMtx);
	auto& Entry = SpeedMods[actor->GetHandle()];
	Entry.SmallChain.Set(SmallAttackSpeedMult, 2.f);
	ApplySpeedNet(actor, Entry);
}

// Signed: this slot carries the NET attack-speed delta at swing start, not just
// the charge bonus. The same-side penalty rides here rather than becoming a
// fourth WeaponSpeedMult consumer — three already write to that actor value
// (AttackSpeedMult, SmallAttackSpeedMult, and this), each with its own removal
// sites, and a missed removal is permanent speed drift on that actor. Netting
// into an existing slot adds no bookkeeping and no new cleanup paths.
void AttackHandler::GiveChargeSpeedBuff(RE::Actor* actor, float Ratio)
{
	std::unique_lock lock(SpeedModsMtx);
	auto& Entry = SpeedMods[actor->GetHandle()];
	Entry.Charge.Set(std::clamp(Ratio, 0.f, 1.f) * DifficultySettings::GuardChargeMaxSpeedBonus,
		ChargeBuffDuration);
	ApplySpeedNet(actor, Entry);
}

void AttackHandler::SetSameSideSpeedPenalty(RE::Actor* actor, bool Penalise)
{
	std::unique_lock lock(SpeedModsMtx);
	auto& Entry = SpeedMods[actor->GetHandle()];
	Entry.SameSide.Set(-DifficultySettings::SameSideSpeedPenalty,
		Penalise ? ChargeBuffDuration : 0.f);
	ApplySpeedNet(actor, Entry);
}

void AttackHandler::RemoveSmallAttackSpeedBuff(RE::Actor* actor)
{
	std::unique_lock lock(SpeedModsMtx);
	auto Iter = SpeedMods.find(actor->GetHandle());
	if (Iter == SpeedMods.end())
	{
		return;
	}
	Iter->second.SmallChain.Set(0.f, 0.f);
	ApplySpeedNet(actor, Iter->second);
}

void AttackHandler::Update(float delta)
{
	{
		ChamberWindowMtx.lock();
		auto ChamberIter = ChamberWindow.begin();
		while (ChamberIter != ChamberWindow.end())
		{
			if (!ChamberIter->first)
			{
				ChamberIter = ChamberWindow.erase(ChamberIter);
				continue;
			}
			RE::Actor* actor = ChamberIter->first.get().get();
			if (!actor)
			{
				ChamberIter = ChamberWindow.erase(ChamberIter);
				continue;
			}
			ChamberIter->second -= delta;
			if (ChamberIter->second <= 0)
			{
				ChamberIter = ChamberWindow.erase(ChamberIter);
				continue;
			}
			ChamberIter++;
		}
		ChamberWindowMtx.unlock();

	}

	{

		AttackLockoutMtx.lock();
		auto AttackIter = AttackLockout.begin();
		while (AttackIter != AttackLockout.end())
		{
			if (!AttackIter->first)
			{
				AttackIter = AttackLockout.erase(AttackIter);
				continue;
			}
			RE::Actor* actor = AttackIter->first.get().get();
			if (!actor)
			{
				AttackIter = AttackLockout.erase(AttackIter);
				continue;
			}
			AttackIter->second -= delta;
			if (AttackIter->second <= 0)
			{
				// Same as the stagger case: a blocked attack recoils out to a
				// default idle instead of the directional one. Skipped
				// mid-attack so a ForceIdle can't cancel a fresh swing.
				if (!actor->IsAttacking())
				{
					DirectionHandler::GetSingleton()->QueueAnimationEvent(actor);
				}
				AttackIter = AttackLockout.erase(AttackIter);
				continue;
			}
			AttackIter++;
		}
		AttackLockoutMtx.unlock();
	}


	{
		FeintWindowMtx.lock();
		auto FeintIter = FeintWindow.begin();
		while (FeintIter != FeintWindow.end())
		{
			if (!FeintIter->first)
			{
				FeintIter = FeintWindow.erase(FeintIter);
				continue;
			}
			RE::Actor* actor = FeintIter->first.get().get();
			if (!actor)
			{
				FeintIter = FeintWindow.erase(FeintIter);
				continue;
			}
			FeintIter->second -= delta;
			if (FeintIter->second <= 0)
			{
				FeintIter = FeintWindow.erase(FeintIter);
				continue;
			}
			FeintIter++;
		}
		FeintWindowMtx.unlock();

	}

	{
		FeintQueueMtx.lock();

		auto Iter = FeintQueue.begin();
		while (Iter != FeintQueue.end())
		{
			if (!Iter->first)
			{
				Iter = FeintQueue.erase(Iter);
				continue;
			}
			RE::Actor* actor = Iter->first.get().get();
			if (!actor)
			{
				Iter = FeintQueue.erase(Iter);
				continue;
			}
			Iter->second -= delta;
			if (Iter->second <= 0)
			{
				//actor->NotifyAnimationGraph("attackStart");

				DoAttack(actor);

				Iter = FeintQueue.erase(Iter);
				continue;
			}
			Iter++;
		}
		FeintQueueMtx.unlock();
	}

	{
		// One expiry pass for every speed contribution. Each slot runs its own
		// clock; when one lapses the net is recomputed and the actor value moved
		// to it, so a lapsing slot can never remove more (or less) than it added.
		std::unique_lock lock(SpeedModsMtx);
		auto Iter = SpeedMods.begin();
		while (Iter != SpeedMods.end())
		{
			if (!Iter->first)
			{
				Iter = SpeedMods.erase(Iter);
				continue;
			}
			RE::Actor* actor = Iter->first.get().get();
			if (!actor)
			{
				Iter = SpeedMods.erase(Iter);
				continue;
			}
			SpeedEntry& Entry = Iter->second;
			Entry.Chain.Tick(delta);
			Entry.SmallChain.Tick(delta);
			Entry.Charge.Tick(delta);
			Entry.SameSide.Tick(delta);
			ApplySpeedNet(actor, Entry);
			// Nothing live and nothing applied: drop the entry entirely.
			if (!Entry.Chain.Active() && !Entry.SmallChain.Active() &&
				!Entry.Charge.Active() && !Entry.SameSide.Active() &&
				Entry.appliedDelta == 0.f)
			{
				Iter = SpeedMods.erase(Iter);
				continue;
			}
			++Iter;
		}
	}

	{
		AttackChainMtx.lock();
		auto Iter = AttackChains.begin();
		while (Iter != AttackChains.end())
		{
			if (!Iter->first)
			{
				Iter = AttackChains.erase(Iter);
				continue;
			}
			RE::Actor* actor = Iter->first.get().get();
			if (!actor)
			{
				Iter = AttackChains.erase(Iter);
				continue;
			}
			Iter->second.timeLeft -= delta;
			if (Iter->second.timeLeft <= 0)
			{
				Iter = AttackChains.erase(Iter);
				continue;
			}
			Iter++;
		}
		AttackChainMtx.unlock();
	}

}
