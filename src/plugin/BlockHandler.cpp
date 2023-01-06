#include "BlockHandler.h"
#include "DirectionHandler.h"
#include "AIHandler.h"
#include "SettingsLoader.h"
#include "FXHandler.h"

void BlockHandler::ApplyBlockDamage(RE::Actor* actor, RE::HitData& hitData)
{
	float Damage = hitData.totalDamage;
	float ActorStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	float FinalDamage = 0.f;
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
	actor->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -Damage);
	hitData.totalDamage = FinalDamage;

}

void BlockHandler::CauseStagger(RE::Actor* actor, RE::Actor* heading, float magnitude)
{
	if (StaggerTimer.find(actor->GetHandle()) == StaggerTimer.end() && !DirectionHandler::GetSingleton()->IsUnblockable(actor))
	{
		float headingAngle = actor->GetHeadingAngle(heading->GetPosition(), false);
		float direction = (headingAngle >= 0.0f) ? headingAngle / 360.0f : (360.0f + headingAngle) / 360.0f;
		actor->SetGraphVariableFloat("staggerDirection", direction);
		actor->SetGraphVariableFloat("StaggerMagnitude", magnitude);
		actor->NotifyAnimationGraph("staggerStart");

		StaggerTimer[actor->GetHandle()] = DifficultySettings::StaggerResetTimer;
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
				Directions dir = DirectionHandler::GetSingleton()->PerkToDirection(DirectionHandler::GetSingleton()->GetDirectionalPerk(attacker));
				if (DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
				{
					AIHandler::GetSingleton()->SignalBadThing(target, dir);
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
			DirectionHandler::GetSingleton()->SwitchToNewDirection(target, attacker);
			AIHandler::GetSingleton()->SignalGoodThing(target, 
				DirectionHandler::GetSingleton()->PerkToDirection(DirectionHandler::GetSingleton()->GetDirectionalPerk(attacker)));
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