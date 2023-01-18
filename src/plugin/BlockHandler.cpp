#include "BlockHandler.h"
#include "DirectionHandler.h"
#include "AIHandler.h"
#include "SettingsLoader.h"
#include "FXHandler.h"

BlockHandler::BlockHandler()
{
	NPCKeyword = nullptr;
}

void BlockHandler::Initialize()
{
	if (!NPCKeyword)
	{
		RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
		NPCKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm");
	}
}


void BlockHandler::ApplyBlockDamage(RE::Actor* actor, RE::HitData& hitData)
{
	float Damage = hitData.totalDamage;
	float ActorStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	float FinalDamage = 0.f;

	//take damage if it was imperfect as well as increased stamina damage
	if (DirectionHandler::GetSingleton()->HasImperfectParry(actor))
	{
		FinalDamage = Damage * 0.5f;
		Damage *= 1.5f;
	}

	// breaks stamina
	if (Damage > ActorStamina)
	{
		FinalDamage = Damage - ActorStamina;
		if (actor->IsBlocking())
		{
			CauseStagger(actor, hitData.aggressor.get().get(), 1.f);
		}
		FinalDamage *= DifficultySettings::MeleeDamageMult;
	}
	hitData.attackDataSpell = nullptr;
	hitData.criticalEffect = nullptr;
	hitData.attackData->data.attackSpell = nullptr;
	
	actor->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -Damage);
	hitData.totalDamage = FinalDamage;

}

void BlockHandler::CauseStagger(RE::Actor* actor, RE::Actor* heading, float magnitude, bool force)
{
	// make sure we can stagger them
	// todo: make sure we can only stagger NPCs since the staggering of trolls and shit totally breaks the game
	// since they can't block
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
}

void BlockHandler::CauseRecoil(RE::Actor* actor)
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
				Directions dir = DirectionHandler::GetSingleton()->GetCurrentDirection(attacker);
				if (DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
				{
					AIHandler::GetSingleton()->SignalBadThing(target, dir);
					AIHandler::GetSingleton()->SwitchTarget(target, attacker);
				}
				
			}
		}
	}
	else
	{
		// wnot sure why we have to reimpelment recoil here
		attacker->NotifyAnimationGraph("recoilStart");
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
			BlockHandler::GetSingleton()->CauseStagger(attacker, target, 1.f);
			return true;
		}
	}
	return false;
}

void BlockHandler::GiveHyperarmor(RE::Actor* actor)
{
	HyperArmorTimer[actor->GetHandle()] = DifficultySettings::HyperarmorTimer;
}

void BlockHandler::Update(float delta)
{
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
}
