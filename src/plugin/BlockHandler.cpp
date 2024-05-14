#include "BlockHandler.h"
#include "DirectionHandler.h"
#include "AIHandler.h"
#include "SettingsLoader.h"
#include "FXHandler.h"

constexpr float MultiattackTimer = 2.5f;

BlockHandler::BlockHandler()
{
	NPCKeyword = nullptr;
	MultiAttackerFX = nullptr;
}

void BlockHandler::Initialize()
{

	RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
	if (!NPCKeyword)
	{
		NPCKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm");
	}
	if (!MultiAttackerFX)
	{
		MultiAttackerFX = DataHandler->LookupForm<RE::SpellItem>(0x6E60, "DirectionMod.esp");
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

	float AdditionalStamDamage = 0;
	if (!hasShield && AttackerWeaponWeight > DefenderWeaponWeight)
	{
		AdditionalStamDamage = 0.75f * (AttackerWeaponWeight - DefenderWeaponWeight);
		AdditionalStamDamage = std::min(AdditionalStamDamage, 10.f);
	}
	float a = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kBlock);
	a = std::min(a, 99.f);
	float skillMod = 0.8f + (0.2f) * ((100.f - a) / 100.f);
	bool Imperfect = DirectionHandler::GetSingleton()->HasImperfectParry(target);
	
	Damage *= skillMod;
	float ActorMaxStamina = target->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina);
	if (hitData.stagger)
	{
		hitData.stagger = 0;
	}
	if (!Imperfect)
	{
		Damage += AdditionalStamDamage;
	}
	else
	{
		//take damage if it was imperfect as well as increased stamina damage
		FinalDamage = hitData.totalDamage;
		Damage += AdditionalStamDamage;
		Damage *= 1.5f;

		// Always do at least 15% of target stamina if they have imperfect block
		
		Damage = std::max(Damage, ActorMaxStamina * .15f);
	}

	// limit the amount of max stamina damage one can take so people stop getting one shot
	Damage = std::min(Damage, ActorMaxStamina * .5f);

	// breaks stamina
	if (Damage > ActorStamina)
	{
		FinalDamage = Damage - ActorStamina;
		if (target->IsBlocking())
		{
			CauseStagger(target, hitData.aggressor.get().get(), 1.f, true);
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
	if (!actor->IsAttacking())
	{
		ShouldStagger = false;
	}
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
		//actor->NotifyAnimationGraph("attackStop");
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
			AIHandler::GetSingleton()->TryRiposte(target, attacker);
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

	AttackersMap[actor->GetHandle()].attackers.insert(attacker->GetHandle());
	AttackersMap[actor->GetHandle()].timeLeft = MultiattackTimer;


	if (AttackersMap[actor->GetHandle()].attackers.size() > 2)
	{
		//logger::info("has multiple attackers");
		CauseStagger(attacker, actor, 0.5f);
		actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)->CastSpellImmediate(MultiAttackerFX, false, actor, 0.f, false, 0.f, nullptr);
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

void BlockHandler::ParriedAttacker(RE::Actor* actor, RE::Actor* attacker)
{
	LastParriedMapMtx.lock();

	LastParriedMap[actor->GetHandle()].lastParried = attacker->GetHandle();
	LastParriedMap[actor->GetHandle()].timeLeft = MultiattackTimer;
	LastParriedMapMtx.unlock();
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
			CauseStagger(attacker, target, 0.5f);
			// masterstriker gets invulnerability during MS
			GiveHyperarmor(target);
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

	LastParriedMapMtx.lock();
	auto LastParriedMapIter = LastParriedMap.begin();
	while (LastParriedMapIter != LastParriedMap.end())
	{
		if (!LastParriedMapIter->first)
		{
			LastParriedMapIter = LastParriedMap.erase(LastParriedMapIter);
			continue;
		}
		RE::Actor* actor = LastParriedMapIter->first.get().get();
		if (!actor)
		{
			LastParriedMapIter = LastParriedMap.erase(LastParriedMapIter);
			continue;
		}
		LastParriedMapIter++;
	}
	LastParriedMapMtx.unlock();
}
