#include "AttackHandler.h"
#include "SettingsLoader.h"
#include "DirectionHandler.h"

constexpr float NPCLockoutTime = 0.15f;
constexpr float AttackSpeedMult = 0.25f;
constexpr float SmallAttackSpeedMult = 0.12f;
constexpr float FeintQueueTime = 0.18f; // carefully tailored magic number

void AttackHandler::Initialize()
{
	RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
	FeintFX = DataHandler->LookupForm<RE::SpellItem>(0x6E60, "DirectionMod.esp");
	WeaponSpeedBuff = DataHandler->LookupForm<RE::SpellItem>(0x7928, "DirectionMod.esp");

	ActionAttack = (RE::BGSAction*)RE::TESForm::LookupByID(0x13005);
	ActionPowerAttack = (RE::BGSAction*)RE::TESForm::LookupByID(0x13383);
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
	FeintWindowMtx.lock_shared();
	FeintWindow.erase(actor->GetHandle());
	FeintWindowMtx.unlock_shared();
}

bool AttackHandler::CanAttack(RE::Actor* actor)
{
	bool ret = false;

	AttackLockoutMtx.lock_shared();
	ret = !AttackLockout.contains(actor->GetHandle());
	AttackLockoutMtx.unlock_shared();

	return ret;
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
		actor->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -15);
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
		logger::info("failed attack action! {}", actor->GetName());
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
		// make sure we reset values!
		SpeedBuffMtx.lock();
		auto SpeedIter = SpeedBuff.begin();
		while (SpeedIter != SpeedBuff.end())
		{
			if (!SpeedIter->first)
			{
				SpeedIter = SpeedBuff.erase(SpeedIter);
				continue;
			}
			RE::Actor* actor = SpeedIter->first.get().get();
			if (!actor)
			{
				SpeedIter = SpeedBuff.erase(SpeedIter);
				continue;
			}

			SpeedIter = SpeedBuff.erase(SpeedIter);
			actor->AsActorValueOwner()->ModActorValue(RE::ActorValue::kWeaponSpeedMult, -AttackSpeedMult);
			continue;
		}
		SpeedBuff.clear();
		SpeedBuffMtx.unlock();
	}

	{
		// make sure we reset values!
		SmallSpeedBuffMtx.lock();
		auto SpeedIter = SmallSpeedBuff.begin();
		while (SpeedIter != SmallSpeedBuff.end())
		{
			if (!SpeedIter->first)
			{
				SpeedIter = SmallSpeedBuff.erase(SpeedIter);
				continue;
			}
			RE::Actor* actor = SpeedIter->first.get().get();
			if (!actor)
			{
				SpeedIter = SmallSpeedBuff.erase(SpeedIter);
				continue;
			}

			SpeedIter = SmallSpeedBuff.erase(SpeedIter);
			actor->AsActorValueOwner()->ModActorValue(RE::ActorValue::kWeaponSpeedMult, -SmallAttackSpeedMult);
			continue;
		}
		SmallSpeedBuff.clear();
		SmallSpeedBuffMtx.unlock();
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

	{
		// make sure we reset values!
		SpeedBuffMtx.lock();
		if (SpeedBuff.contains(actor))
		{
			SpeedBuff.erase(actor);
			
			actor.get().get()->AsActorValueOwner()->ModActorValue(RE::ActorValue::kWeaponSpeedMult, -AttackSpeedMult);
		}
		SpeedBuffMtx.unlock();
	}
}

void AttackHandler::GiveAttackSpeedBuff(RE::Actor* actor)
{
	SpeedBuffMtx.lock();
	if (!SpeedBuff.contains(actor->GetHandle()))
	{
		SpeedBuff[actor->GetHandle()] = 2.f; 
		actor->AsActorValueOwner()->ModActorValue(RE::ActorValue::kWeaponSpeedMult, AttackSpeedMult);
	}
	SpeedBuffMtx.unlock();
}

void AttackHandler::GiveSmallAttackSpeedBuff(RE::Actor* actor)
{
	SmallSpeedBuffMtx.lock();
	if (!SmallSpeedBuff.contains(actor->GetHandle()))
	{
		SmallSpeedBuff[actor->GetHandle()] = 2.f;
		actor->AsActorValueOwner()->ModActorValue(RE::ActorValue::kWeaponSpeedMult, SmallAttackSpeedMult);
	}
	SmallSpeedBuffMtx.unlock();
}

void AttackHandler::RemoveSmallAttackSpeedBuff(RE::Actor* actor)
{
	SmallSpeedBuffMtx.lock();
	if (SmallSpeedBuff.contains(actor->GetHandle()))
	{
		actor->AsActorValueOwner()->ModActorValue(RE::ActorValue::kWeaponSpeedMult, -SmallAttackSpeedMult);
		SmallSpeedBuff.erase(actor->GetHandle());
	}
	SmallSpeedBuffMtx.unlock();
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

		SpeedBuffMtx.lock();
		auto SpeedIter = SpeedBuff.begin();
		while (SpeedIter != SpeedBuff.end())
		{
			if (!SpeedIter->first)
			{
				SpeedIter = SpeedBuff.erase(SpeedIter);
				continue;
			}
			RE::Actor* actor = SpeedIter->first.get().get();
			if (!actor)
			{
				SpeedIter = SpeedBuff.erase(SpeedIter);
				continue;
			}
			SpeedIter->second -= delta;
			if (SpeedIter->second <= 0)
			{
				SpeedIter = SpeedBuff.erase(SpeedIter);
				actor->AsActorValueOwner()->ModActorValue(RE::ActorValue::kWeaponSpeedMult, -AttackSpeedMult);
				continue;
			}
			SpeedIter++;
		}
		SpeedBuffMtx.unlock();
	}

	{

		SmallSpeedBuffMtx.lock();
		auto SpeedIter = SmallSpeedBuff.begin();
		while (SpeedIter != SmallSpeedBuff.end())
		{
			if (!SpeedIter->first)
			{
				SpeedIter = SmallSpeedBuff.erase(SpeedIter);
				continue;
			}
			RE::Actor* actor = SpeedIter->first.get().get();
			if (!actor)
			{
				SpeedIter = SmallSpeedBuff.erase(SpeedIter);
				continue;
			}
			SpeedIter->second -= delta;
			if (SpeedIter->second <= 0)
			{
				SpeedIter = SmallSpeedBuff.erase(SpeedIter);
				actor->AsActorValueOwner()->ModActorValue(RE::ActorValue::kWeaponSpeedMult, -SmallAttackSpeedMult);
				continue;
			}
			SpeedIter++;
		}
		SmallSpeedBuffMtx.unlock();
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