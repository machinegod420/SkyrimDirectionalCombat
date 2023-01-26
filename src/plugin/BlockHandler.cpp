#include "BlockHandler.h"
#include "DirectionHandler.h"
#include "AIHandler.h"
#include "SettingsLoader.h"
#include "FXHandler.h"

constexpr float MultiattackTimer = 5.f;

BlockHandler::BlockHandler()
{
	NPCKeyword = nullptr;
	MultiAttackerFX = nullptr;
}

void BlockHandler::Initialize()
{
	if (!NPCKeyword)
	{
		RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
		NPCKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm");
	}
	if (!MultiAttackerFX)
	{
	}
}


void BlockHandler::ApplyBlockDamage(RE::Actor* target, RE::Actor* attacker, RE::HitData& hitData)
{
	float Damage = hitData.totalDamage;
	float ActorStamina = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	float FinalDamage = 0.f;

	// weapon stamina modifiers
	auto AttackerWeapon = attacker->GetEquippedObject(false);
	float AttackerWeaponWeight = AttackerWeapon ? AttackerWeapon->GetWeight() : 0.f;
	auto DefenderWeapon = target->GetEquippedObject(false);
	float DefenderWeaponWeight = DefenderWeapon ? DefenderWeapon->GetWeight() : 0.f;
	auto DefenderShield = target->GetEquippedObject(true);
	bool hasShield = DefenderShield ? DefenderShield->IsArmor() : false;
	if (!hasShield && AttackerWeaponWeight > DefenderWeaponWeight)
	{
		Damage += (AttackerWeaponWeight - DefenderWeaponWeight);
	}
	float a = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kBlock);
	a = std::min(a, 99.f);
	float skillMod = 0.6f + (0.2f) * ((100.f - a) / 100.f);

	Damage *= skillMod;

	//take damage if it was imperfect as well as increased stamina damage
	if (DirectionHandler::GetSingleton()->HasImperfectParry(target))
	{
		FinalDamage = Damage * 0.5f;
		Damage *= 1.5f;
	}

	// breaks stamina
	if (Damage > ActorStamina)
	{
		FinalDamage = Damage - ActorStamina;
		if (target->IsBlocking())
		{
			CauseStagger(target, hitData.aggressor.get().get(), 1.f);
		}
		FinalDamage *= DifficultySettings::MeleeDamageMult;
	}
	
	target->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -Damage);
	hitData.totalDamage = FinalDamage;

}

void BlockHandler::CauseStagger(RE::Actor* actor, RE::Actor* heading, float magnitude, bool force)
{
	// make sure we can stagger them
	// todo: make sure we can only stagger NPCs since the staggering of trolls and shit totally breaks the game
	// since they can't block

	StaggerTimerMtx.lock();
	bool ShouldStagger = !StaggerTimer.contains(actor->GetHandle());
	if (force)
	{
		ShouldStagger = true;
	}

	if (ShouldStagger)
	{
		float headingAngle = actor->GetHeadingAngle(heading->GetPosition(), false);
		float direction = (headingAngle >= 0.0f) ? headingAngle / 360.0f : (360.0f + headingAngle) / 360.0f;
		actor->SetGraphVariableFloat("staggerDirection", direction);
		actor->SetGraphVariableFloat("StaggerMagnitude", magnitude);
		actor->NotifyAnimationGraph("staggerStart");

		if (actor->GetRace()->HasKeyword(NPCKeyword))
		{
			StaggerTimer[actor->GetHandle()] = DifficultySettings::StaggerResetTimer;
		}
		else
		{
			StaggerTimer[actor->GetHandle()] = DifficultySettings::StaggerResetTimer * DifficultySettings::NonNPCStaggerMult;
		}
		
	}
	StaggerTimerMtx.unlock();
}

void BlockHandler::CauseRecoil(RE::Actor* actor) const
{
	actor->NotifyAnimationGraph("recoilLargeStart");
}

void BlockHandler::HandleBlock(RE::Actor* attacker, RE::Actor* target)
{
	if (!DirectionHandler::GetSingleton()->HasBlockAngle(attacker, target))
	{
		target->SetGraphVariableBool("IsBlocking", false);
		target->NotifyAnimationGraph("blockStop");
		//logger::info("had wrong block angle");

		if (DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker))
		{
			if (!target->IsPlayerRef())
			{
				// AI stuff here
				Directions dir = DirectionHandler::GetSingleton()->GetCurrentDirection(attacker);
				if (DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
				{
					AIHandler::GetSingleton()->SignalBadThing(target, dir);
					AIHandler::GetSingleton()->SwitchTarget(target, attacker);
					AIHandler::GetSingleton()->TryBlock(target, attacker);
				}
				
			}
		}
	}
	else
	{
		// wnot sure why we have to reimpelment recoil here
		//attacker->NotifyAnimationGraph("recoilStart");
		// succesffully blocked so give hyperarmor
		GiveHyperarmor(target);

		if (!target->IsPlayerRef() && DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
		{
			//AIHandler::GetSingleton()->AddAction(target, AIHandler::Actions::Riposte, true);
			AIHandler::GetSingleton()->TryRiposte(target);
			AIHandler::GetSingleton()->SignalGoodThing(target, 
			DirectionHandler::GetSingleton()->GetCurrentDirection(attacker));
		}
	}
}

void BlockHandler::AddNewAttacker(RE::Actor* actor, RE::Actor* attacker)
{
	if (!actor->IsHostileToActor(attacker))
	{
		return;
	}
	// only directional perks have this
	if (!DirectionHandler::GetSingleton()->HasDirectionalPerks(actor))
	{
		return;
	}
	std::unique_lock lock(AttackersMapMtx);
	if (actor->GetActorRuntimeData().currentCombatTarget)
	{
		if (attacker->GetHandle() != actor->GetActorRuntimeData().currentCombatTarget)
		{
			AttackersMap[actor->GetHandle()].attackers.insert(attacker->GetHandle());
			AttackersMap[actor->GetHandle()].timeLeft = MultiattackTimer;
		}
	}
	else
	{
		AttackersMap[actor->GetHandle()].attackers.insert(attacker->GetHandle());
		AttackersMap[actor->GetHandle()].timeLeft = MultiattackTimer;
	}
}

int BlockHandler::GetNumberAttackers(RE::Actor* actor) const
{
	std::shared_lock lock(AttackersMapMtx);
	if (AttackersMap.contains(actor->GetHandle()))
	{
		return (int)AttackersMap.at(actor->GetHandle()).attackers.size();
	}
	return 0;
}

bool BlockHandler::HandleMasterstrike(RE::Actor* attacker, RE::Actor* target)
{
	if (DirectionHandler::GetSingleton()->HasBlockAngle(attacker, target))
	{
		bool targetStaggering = false;
		bool attackerStaggering = false;
		target->GetGraphVariableBool("IsStaggering", targetStaggering);
		attacker->GetGraphVariableBool("IsStaggering", attackerStaggering);
		if (!targetStaggering && !attackerStaggering)
		{
			FXHandler::GetSingleton()->PlayMasterstrike(target);
			CauseStagger(attacker, target, 1.f);
			return true;
		}
	}
	return false;
}

void BlockHandler::GiveHyperarmor(RE::Actor* actor)
{
	HyperArmorTimerMtx.lock();
	HyperArmorTimer[actor->GetHandle()] = DifficultySettings::HyperarmorTimer;
	HyperArmorTimerMtx.unlock();
}

void BlockHandler::RemoveActor(RE::ActorHandle actor)
{
	HyperArmorTimerMtx.lock();
	HyperArmorTimer.erase(actor);
	HyperArmorTimerMtx.unlock();

	StaggerTimerMtx.lock();
	StaggerTimer.erase(actor);
	StaggerTimerMtx.unlock();

}

void BlockHandler::Update(float delta)
{
	StaggerTimerMtx.lock();
	auto Iter = StaggerTimer.begin();
	while (Iter != StaggerTimer.end())
	{
		if (!Iter->first)
		{
			Iter = StaggerTimer.erase(Iter);
			continue;
		}
		RE::Actor* actor = Iter->first.get().get();
		if (!actor)
		{
			Iter = StaggerTimer.erase(Iter);
			continue;
		}
		Iter->second -= delta;
		if (Iter->second <= 0)
		{
			Iter = StaggerTimer.erase(Iter);
			continue;

		}
		Iter++;
	}
	StaggerTimerMtx.unlock();

	HyperArmorTimerMtx.lock();
	auto HAIter = HyperArmorTimer.begin();
	while (HAIter != HyperArmorTimer.end())
	{
		if (!HAIter->first)
		{
			HAIter = HyperArmorTimer.erase(HAIter);
			continue;
		}
		RE::Actor* actor = HAIter->first.get().get();
		if (!actor)
		{
			HAIter = HyperArmorTimer.erase(HAIter);
			continue;
		}
		HAIter->second -= delta;
		if (HAIter->second <= 0)
		{
			HAIter = HyperArmorTimer.erase(HAIter);
			continue;

		}
		HAIter++;
	}
	HyperArmorTimerMtx.unlock();

	AttackersMapMtx.lock();
	auto AttackersIter = AttackersMap.begin();
	while (AttackersIter != AttackersMap.end())
	{
		if (!AttackersIter->first)
		{
			AttackersIter = AttackersMap.erase(AttackersIter);
			continue;
		}
		RE::Actor* actor = AttackersIter->first.get().get();
		if (!actor)
		{
			AttackersIter = AttackersMap.erase(AttackersIter);
			continue;
		}
		AttackersIter->second.timeLeft -= delta;
		if (AttackersIter->second.timeLeft <= 0)
		{
			if (AttackersIter->second.attackers.size() > 0)
			{
				AttackersIter->second.attackers.erase(AttackersIter->second.attackers.begin());
				AttackersMap[actor->GetHandle()].timeLeft = MultiattackTimer;
			}
			if (AttackersIter->second.attackers.size() == 0)
			{
				AttackersMap.erase(AttackersIter);
			}
		}
		AttackersIter++;
	}
	AttackersMapMtx.unlock();
}
