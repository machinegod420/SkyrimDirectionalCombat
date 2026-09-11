#include "BlockHandler.h"
#include "DirectionHandler.h"
#include "AIHandler.h"
#include "SettingsLoader.h"
#include "AttackHandler.h"
#include "FXHandler.h"

constexpr float MultiattackTimer = 2.5f;
// Stamina drain reduction on a blocked hit at peak brace. Deliberately small —
// a quality gradient under the direction game, not a way to out-block a mixup,
// and it must not blunt a power attack's guard-breaking role.
constexpr float BraceMaxReduction = 0.2f;
// Ramp over TimedBlockStartup, hold across the parry window, then nothing.
// The bonus is for committing the guard early enough to be set when the attack
// lands — once the window has passed you weren't, so it ends rather than
// fading. Holding longer is a stale block, not a braced one.
static float BraceRatioFromSeconds(float seconds)
{
	const float RampEnd = DifficultySettings::TimedBlockStartup;
	const float PeakEnd = RampEnd + DifficultySettings::TimedBlockActiveTime;
	if (seconds <= 0.f || RampEnd <= 0.f)
	{
		return 0.f;
	}
	if (seconds < RampEnd)
	{
		return seconds / RampEnd;
	}
	if (seconds < PeakEnd)
	{
		return 1.f;
	}
	return 0.f;
}
constexpr float HyperarmorTimer = 0.1f;

BlockHandler::BlockHandler()
{
	NPCKeyword = nullptr;
	MultiAttackerFX = nullptr;
	DisarmSpell = nullptr;
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
	if (!DisarmSpell)
	{
		DisarmSpell = DataHandler->LookupForm<RE::SpellItem>(0x83EF, "DirectionMod.esp");
	}
}


// The defender's whole scaling on a blocked hit
static float BlockSkillMod(RE::Actor* target)
{
	const float Skill = std::clamp(
		target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kBlock), 0.f, 100.f);
	return 1.f - DifficultySettings::BlockSkillMaxReduction * (Skill * 0.01f);
}

void BlockHandler::ApplyBlockDamage(RE::Actor* target, RE::Actor* attacker, RE::HitData& hitData)
{
	// needs to be similar to attack formula, ie no weapon damage or armor is factored in
	float ActorMaxStamina = target->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
	float Damage = ActorMaxStamina * DifficultySettings::BlockCostRatio * BlockSkillMod(target);
	float ActorStamina = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	
	float FinalDamage = 0.f;

	// weapon stamina modifiers
	auto AttackerWeapon = attacker->GetEquippedObject(false);
	float AttackerWeaponWeight = AttackerWeapon ? AttackerWeapon->GetWeight() : 0.f;
	auto DefenderWeapon = target->GetEquippedObject(false);
	float DefenderWeaponWeight = DefenderWeapon ? DefenderWeapon->GetWeight() : 0.f;
	bool hasShield = HasShield(target);

	float AdditionalStamDamage = 0;
	if (AttackerWeaponWeight > DefenderWeaponWeight)
	{
		AdditionalStamDamage = 0.5f * (AttackerWeaponWeight - DefenderWeaponWeight);
	}

	bool Imperfect = DirectionHandler::GetSingleton()->HasImperfectParry(target);
	Damage += AdditionalStamDamage;
	if (hitData.flags.any(RE::HitData::Flag::kPowerAttack))
	{
		Damage *= DifficultySettings::PowerAttackBlockCostMult;
	}
	// After the weight term, so the discount covers the mass being absorbed too.
	if (hasShield)
	{
		Damage *= 0.8f;
	}

	// Braced block: a set guard absorbs better than one thrown up at the last
	// instant. Peaks across the parry window then decays, so it can't be camped.
	Damage *= 1.f - GetBraceRatio(target) * BraceMaxReduction;

	if(Imperfect)
	{
		//take damage if it was imperfect as well as increased stamina damage
		FinalDamage = hitData.totalDamage;
		Damage *= 1.5f;

		// Always do at least 15% of target stamina if they have imperfect block
		Damage = std::max(Damage, ActorMaxStamina * .15f);
	}

	if (DirectionHandler::GetSingleton()->HasTimedParry(target))
	{
		FXHandler::GetSingleton()->PlayTimedBlock(target);
		Damage *= 0.5f;
		CauseStagger(attacker, target, 0.1f, true);
		// Lights only
		if (!IsPowerAttacking(attacker))
		{
			DirectionHandler::GetSingleton()->AddCombo(target, true);
		}
	}

	// cap stamina damage
	float ActorMaxStaminaDamage = ActorMaxStamina * DifficultySettings::StaminaDamageCap;
	Damage = std::min(Damage, ActorMaxStaminaDamage);

	target->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina,Damage);
	// breaks stamina
	if (Damage > ActorStamina)
	{
		FinalDamage = Damage - ActorStamina;
		if (target->IsBlocking())
		{
			CauseStagger(target, hitData.aggressor.get().get(), 0.5f, true);
		}
		// Block fully drained their stamina — disarm them. Uses the engine's
		// disarm task queue (same path as the Disarm shout) which handles
		// the inventory/extra-data bookkeeping internally.
		const bool fullDisarm = target->IsPlayerRef()
			|| (target->IsHostileToActor(RE::PlayerCharacter::GetSingleton()));
		if (fullDisarm)
		{
			QueueDisarm(target, attacker);
		}
		
		attacker->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kStamina,
			attacker->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina) * 0.15f);
		target->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kStamina,
			target->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina) * 0.15f);
		if (target->IsPlayerRef())
		{

		}
	}
	hitData.totalDamage = FinalDamage;

}

void BlockHandler::CauseStagger(RE::Actor* actor, RE::Actor* heading, float magnitude, bool force)
{
	// make sure we can stagger them
	// todo: make sure we can only stagger NPCs since the staggering of trolls and shit totally breaks the game
	// since they can't block
	if (actor->AsActorState()->actorState2.staggered)
	{
		return;
	}

	StaggerTimerMtx.lock();
	bool ShouldStagger = actor->IsAttacking();
	if (force && !StaggerTimer.contains(actor->GetHandle()))
	{
		ShouldStagger = true;
	}


	if (ShouldStagger)
	{
		if (actor->IsBlocking())
		{
			actor->SetGraphVariableBool("IsBlocking", false);
			actor->NotifyAnimationGraph("blockStop");
			actor->AsActorState()->actorState2.wantBlocking = false;
		}
		// reset actor attack state because sometimes it can get screwed up staggering mid bash
		// actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
		float headingAngle = actor->GetHeadingAngle(heading->GetPosition(), false);
		float direction = (headingAngle >= 0.0f) ? headingAngle / 360.0f : (360.0f + headingAngle) / 360.0f;
		actor->SetGraphVariableFloat("staggerDirection", direction);
		actor->SetGraphVariableFloat("StaggerMagnitude", magnitude);
		actor->NotifyAnimationGraph("staggerStart");
		actor->AsActorState()->actorState2.staggered = true;
		//actor->NotifyAnimationGraph("attackStop");
		

		/*
			typedef void (*tfoo)(RE::Actor* a_target, float a_staggerMult, RE::Actor* a_aggressor);
		REL::Relocation<tfoo> func{ REL::RelocationID(36700, 37710) };
		func(actor, magnitude, heading);	
		
		*/

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

void BlockHandler::CauseKnockdown(RE::Actor* target, RE::Actor* attacker, float magnitude)
{
	typedef void(_fastcall* tPushActorAway_sub_14067D4A0)(RE::AIProcess* a_causer, RE::Actor* a_target, RE::NiPoint3& a_origin, float a_magnitude);
	static REL::Relocation<tPushActorAway_sub_14067D4A0> pushActorAway{ RELOCATION_ID(38858, 39895) };
	auto targetPoint = attacker->GetNodeByName(attacker->GetActorRuntimeData().race->bodyPartData->parts[0]->targetName.c_str());
	RE::NiPoint3 vec = targetPoint->world.translate;
	pushActorAway(attacker->GetActorRuntimeData().currentProcess, target, vec, magnitude);
}

void BlockHandler::CauseRecoil(RE::Actor* actor) const
{
	actor->NotifyAnimationGraph("recoilLargeStart");
}

void BlockHandler::HandleBlock(RE::Actor* attacker, RE::Actor* target)
{
	// hyperarmor acts as a fullblock
	if (HasHyperarmor(target))
	{
		return;
	}
	if (!DirectionHandler::GetSingleton()->HasBlockAngle(attacker, target))
	{
		target->SetGraphVariableBool("IsBlocking", false);
		target->NotifyAnimationGraph("blockStop");
		target->AsActorState()->actorState2.wantBlocking = false;
		//logger::info("had wrong block angle");

		// Disconfirmation must fire HERE: this strip clears IsBlocking before
		// the hit event reaches SignalBadThing, so its wasBlocking check can
		// never see the wrong-line case — the AI's failed belief has to drain
		// at the moment the wrong guard is caught out.
		if (!target->IsPlayerRef())
		{
			AIHandler::GetSingleton()->SignalWrongLineBlockExternalCalled(target);
		}
	}
	else
	{
		// succesffully blocked so remove any lockout if they cannot attack
		AttackHandler::GetSingleton()->RemoveLockout(target);
		GiveHyperarmor(target, attacker);
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
	// only the attacker should be allowed to get staggered
	if (DirectionHandler::GetSingleton()->HasBlockAngle(attacker, target))
	{
		bool targetStaggering = target->AsActorState()->actorState2.staggered;
		bool attackerStaggering = attacker->AsActorState()->actorState2.staggered;
		//target->GetGraphVariableBool("IsStaggering", targetStaggering);
		//attacker->GetGraphVariableBool("IsStaggering", attackerStaggering);

		bool targetPowerattack = IsPowerAttacking(target);
		bool attackerPowerattack = IsPowerAttacking(attacker);

		float TargetMasterstrikeTime = AttackHandler::GetSingleton()->GetChamberWindowTime(target);
		float AttackerMasterstrikeTime = AttackHandler::GetSingleton()->GetChamberWindowTime(attacker);

		// we use staggering as a flag that someone has already been in a masterstrike event
		if (!targetStaggering && !attackerStaggering)
		{
			// greater time means they attacked later
			if (TargetMasterstrikeTime >= AttackerMasterstrikeTime)
			{
				// power attack always has priority, you cannot masterstrike a power attack with a regular attack
				if (!attackerPowerattack || (attackerPowerattack && targetPowerattack))
				{
					FXHandler::GetSingleton()->PlayMasterstrike(target);
					CauseStagger(attacker, target, 0.25f);
					return true;
				}
				
			}
		}
	}
	return false;
}

void BlockHandler::GiveHyperarmor(RE::Actor* actor, RE::Actor* attacker)
{
	HyperArmorTimerMtx.lock();
	HyperArmorTimer[actor->GetHandle()] = HyperarmorTimer;
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

void BlockHandler::AddBrace(RE::Actor* actor)
{
	std::unique_lock lock(BraceTimerMtx);
	BraceTimer[actor->GetHandle()] = 0.f;
}

void BlockHandler::ResetBrace(RE::Actor* actor)
{
	std::unique_lock lock(BraceTimerMtx);
	auto Iter = BraceTimer.find(actor->GetHandle());
	if (Iter != BraceTimer.end())
	{
		Iter->second = 0.f;
	}
}

float BlockHandler::GetBraceRatio(RE::Actor* actor) const
{
	std::shared_lock lock(BraceTimerMtx);
	auto Iter = BraceTimer.find(actor->GetHandle());
	return Iter == BraceTimer.end() ? 0.f : BraceRatioFromSeconds(Iter->second);
}

void BlockHandler::Update(float delta)
{
	{
		// Only actors that actually raised a block are in here, so this is
		// cheap; entries drop out as soon as the block does.
		std::unique_lock lock(BraceTimerMtx);
		// Stop counting once the window has closed — everything past it reads
		// as zero anyway, and letting it climb forever would overflow on a
		// long hold.
		const float Cap = DifficultySettings::TimedBlockStartup +
			DifficultySettings::TimedBlockActiveTime;
		auto Iter = BraceTimer.begin();
		while (Iter != BraceTimer.end())
		{
			RE::Actor* actor = Iter->first ? Iter->first.get().get() : nullptr;
			if (!actor || !actor->IsBlocking())
			{
				Iter = BraceTimer.erase(Iter);
				continue;
			}
			Iter->second = std::min(Iter->second + delta, Cap);
			Iter++;
		}
	}

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
				// Recovering from a stagger drops the graph back to a default
				// idle rather than the directional one, so re-assert it.
				// Skipped mid-attack: a ForceIdle there cancels the swing.
				if (!actor->IsAttacking())
				{
					DirectionHandler::GetSingleton()->QueueAnimationEvent(actor);
				}
				Iter = StaggerTimer.erase(Iter);
				continue;

			}
			Iter++;
		}
		StaggerTimerMtx.unlock();

	}

	{

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
	}

	{

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


	{
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

}
