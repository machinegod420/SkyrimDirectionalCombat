#include "BlockHandler.h"
#include "DirectionHandler.h"
#include "AIHandler.h"
#include "SettingsLoader.h"
#include "AttackHandler.h"
#include "FXHandler.h"
#include "CreatureHandler.h"

constexpr float MultiattackTimer = 2.5f;
// Stagger on the blocker when a creature's blocked hit isn't parried, at
// player size; scales with size, and anything too small to move a braced
// guard does nothing.
constexpr float CreatureBlockStagger = 0.25f;
constexpr float CreatureBlockStaggerMin = 0.1f;
constexpr float CreatureBlockStaggerMaxSize = 2.f;
// Stamina drain reduction on a blocked hit at peak brace. Deliberately small —
// a quality gradient under the direction game, not a way to out-block a mixup,
// and it must not blunt a power attack's guard-breaking role.
constexpr float BraceMaxReduction = 0.2f;
// Stamina a blocked hit adds per point the attacker's weapon outweighs the
// defender's, and the share of the base cost that surcharge may reach.
constexpr float WeightStamPerUnit = 0.5f;
constexpr float WeightStamMaxRatio = 0.5f;
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
// How long a wrong-line guard stays caught out, covering the rest of the swing.
constexpr float MissedParryTime = 0.25f;

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
	// A creature's size stands in only when it carries no weapon; an armed
	// draugr is costed by its weapon like anyone else.
	if (AttackerWeaponWeight <= 0.f && CreatureHandler::GetSingleton()->IsCreatureGraph(attacker->GetRace()))
	{
		AttackerWeaponWeight = CreatureHandler::SizeWeight(attacker);
	}
	auto DefenderWeapon = target->GetEquippedObject(false);
	float DefenderWeaponWeight = DefenderWeapon ? DefenderWeapon->GetWeight() : 0.f;
	bool hasShield = HasShield(target);

	// Flat, unlike the rest of the cost, so a deep stamina pool absorbs a heavy
	// weapon better — the one place the attribute pays off. Capped against the
	// base so a warhammer can't empty a light fighter's bar in three blocks.
	float AdditionalStamDamage = 0;
	if (AttackerWeaponWeight > DefenderWeaponWeight)
	{
		AdditionalStamDamage = std::min(WeightStamPerUnit * (AttackerWeaponWeight - DefenderWeaponWeight),
			Damage * WeightStamMaxRatio);
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
	}
	// A creature has no guard to trade with, so a plain block still gets moved;
	// only the parry holds. After the break stagger, which takes precedence.
	if (CreatureHandler::GetSingleton()->IsCreatureGraph(attacker->GetRace()) &&
		!DirectionHandler::GetSingleton()->HasTimedParry(target))
	{
		const float Stagger = CreatureBlockStagger *
			std::min(CreatureHandler::SizeRatio(attacker), CreatureBlockStaggerMaxSize);
		// The engine's own stagger, so stagger perks apply to this one; the
		// rule staggers elsewhere must stay unperkable. Setting hitData.stagger
		// instead would do nothing: the engine only reads it when damage landed.
		if (Stagger >= CreatureBlockStaggerMin)
		{
			using EngineStagger_t = void (*)(RE::Actor* a_target, float a_magnitude, RE::Actor* a_aggressor);
			static REL::Relocation<EngineStagger_t> EngineStagger{ RELOCATION_ID(36700, 37710) };
			EngineStagger(target, Stagger, attacker);
		}
	}
	if (Settings::VerboseLogging)
	{
		logger::info("[block] {} absorbed from {}: stamina {:.0f} of {:.0f} left, cost {:.0f}, through {:.0f}{} | health {:.1f}, hitData phys {:.1f} total {:.1f} -> {:.1f}, flags {:#x}",
			target->GetName(), attacker->GetName(), ActorStamina, ActorMaxStamina, Damage, FinalDamage,
			Imperfect ? " (imperfect)" : "",
			target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth),
			hitData.physicalDamage, hitData.totalDamage, FinalDamage,
			static_cast<uint32_t>(hitData.flags.underlying()));
	}
	hitData.totalDamage = FinalDamage;
	// The engine re-derives totalDamage from these after we return; zero them too.
	hitData.physicalDamage = FinalDamage;
	hitData.resistedPhysicalDamage = 0.f;
	hitData.resistedTypedDamage = 0.f;
	hitData.percentBlocked = FinalDamage > 0.f ? 0.f : 1.f;
	if (FinalDamage <= 0.f)
	{
		// A full absorb eats the proc too; a break lets it through.
		hitData.attackDataSpell = nullptr;
	}
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
		// The engine's stagger is zeroed for fencers, so its attack-state cleanup never runs.
		if (actor->IsAttacking())
		{
			AttackHandler::GetSingleton()->ResetAttackState(actor);
		}
		float headingAngle = actor->GetHeadingAngle(heading->GetPosition(), false);
		float direction = (headingAngle >= 0.0f) ? headingAngle / 360.0f : (360.0f + headingAngle) / 360.0f;
		actor->SetGraphVariableFloat("staggerDirection", direction);
		actor->SetGraphVariableFloat("StaggerMagnitude", magnitude);
		actor->NotifyAnimationGraph("staggerStart");
		actor->AsActorState()->actorState2.staggered = true;
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
	const bool Angle = DirectionHandler::GetSingleton()->HasBlockAngle(attacker, target);
	if (Settings::VerboseLogging)
	{
		auto* Dir = DirectionHandler::GetSingleton();
		const float Max = target->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		const float Cur = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		logger::info("[block] {} {:08X} ({}) vs {} {:08X} ({}) angle {} unblockable {} perks a/t {}/{} shield {} full {} stam {:.2f}",
			attacker->GetName(), attacker->GetFormID(), static_cast<int>(Dir->GetCurrentDirection(attacker)),
			target->GetName(), target->GetFormID(), static_cast<int>(Dir->GetCurrentDirection(target)),
			Angle, Dir->IsUnblockable(attacker),
			Dir->HasDirectionalPerks(attacker), Dir->HasDirectionalPerks(target),
			HasShield(target), HasFullShieldBlock(target), Max > 0.f ? Cur / Max : 0.f);
		float TargetAgo = -1.f, AttackerAgo = -1.f;
		const Directions TargetPrev = Dir->GetPreviousDirection(target, TargetAgo);
		const Directions AttackerPrev = Dir->GetPreviousDirection(attacker, AttackerAgo);
		logger::info("[block]   guard up for {:.3f}s, parry {}, locked target {:08X}, target was ({}) {:.3f}s ago, attacker was ({}) {:.3f}s ago",
			GetBraceSeconds(target), Dir->HasTimedParry(target), Dir->GetLockedTargetFormID(),
			static_cast<int>(TargetPrev), TargetAgo,
			static_cast<int>(AttackerPrev), AttackerAgo);
	}
	if (!Angle)
	{
		target->SetGraphVariableBool("IsBlocking", false);
		target->NotifyAnimationGraph("blockStop");
		target->AsActorState()->actorState2.wantBlocking = false;
		// Latch the miss for the rest of the swing: a second hit event from the same
		// swing must not find a guard still standing and be rescued as a block.
		{
			std::unique_lock lock(MissedParryMtx);
			MissedParry[target->GetHandle()] = MissedParryTime;
		}

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
		// A guard switch still blending when the hit lands: drop the slow transitions and snap to the line.
		if (target->IsBlocking() && DirectionHandler::GetSingleton()->ClearAnimationQueue(target))
		{
			target->NotifyAnimationGraph("ForceBlockFast");
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

// A clash never reaches the hit path: both NPC sides hear it as a connection. The counter
// started late, so its side doesn't time the windup.
static void NotifyBladesMet(RE::Actor* attacker, RE::Actor* target)
{
	if (!target->IsPlayerRef())
	{
		AIHandler::GetSingleton()->NotifyPowerAttackHitExternalCalled(target, attacker);
	}
	if (!attacker->IsPlayerRef())
	{
		AIHandler::GetSingleton()->NotifyPowerAttackHitExternalCalled(attacker, target, false);
	}
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
				// A light can't be masterstruck, so it stays safe to open with: it clashes.
				// Nobody is hit and both swings stop. The first swing was already charged at
				// its pre-hit frame; the counter stops before its own charge, which is its win.
				if (!attackerPowerattack)
				{
					// Read-only stats: both swings end here; the target countered.
					if (attacker->IsPlayerRef() || target->IsPlayerRef())
					{
						AIHandler::GetSingleton()->ResolvePlayerSwing(AIHandler::SwingOutcome::Clashed);
					}
					if (target->IsPlayerRef())
					{
						AIHandler::GetSingleton()->RecordPlayerDefense(DirectionHandler::GetSingleton()->AnimationSet(target),
							false, DirectionHandler::GetSingleton()->GetComboStep(attacker) > 0, AIHandler::DefenseOutcome::Countered);
					}
					FXHandler::GetSingleton()->PlayMasterstrike(target);
					// Blades met: a connection for both reach learners.
					NotifyBladesMet(attacker, target);
					for (RE::Actor* Clashed : { attacker, target })
					{
						if (Clashed->IsAttacking())
						{
							Clashed->NotifyAnimationGraph("attackStop");
							Clashed->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
						}
					}
					if (Settings::VerboseLogging)
					{
						logger::info("[hit] clash: {} ran into {}'s counter", attacker->GetName(), target->GetName());
					}
					return true;
				}
				// Only a power masterstrikes a power.
				if (targetPowerattack)
				{
					// Read-only stats. The masterstriker's own power swings on and resolves later.
					if (attacker->IsPlayerRef())
					{
						AIHandler::GetSingleton()->ResolvePlayerSwing(AIHandler::SwingOutcome::Masterstruck);
					}
					else if (target->IsPlayerRef())
					{
						AIHandler::GetSingleton()->RecordPlayerDefense(DirectionHandler::GetSingleton()->AnimationSet(target),
							true, DirectionHandler::GetSingleton()->GetComboStep(attacker) > 0, AIHandler::DefenseOutcome::Countered);
					}
					FXHandler::GetSingleton()->PlayMasterstrike(target);
					NotifyBladesMet(attacker, target);
					CauseStagger(attacker, target, 0.5f);
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

// Seconds the guard has been up, or -1 if it isn't tracked. Diagnostic: a value
// near zero when a block misbehaves means the hit beat the guard, not the logic.
bool BlockHandler::HasMissedParry(RE::Actor* actor) const
{
	std::shared_lock lock(MissedParryMtx);
	auto Iter = MissedParry.find(actor->GetHandle());
	return Iter != MissedParry.end() && Iter->second > 0.f;
}

float BlockHandler::GetBraceSeconds(RE::Actor* actor) const
{
	std::shared_lock lock(BraceTimerMtx);
	auto Iter = BraceTimer.find(actor->GetHandle());
	return Iter == BraceTimer.end() ? -1.f : Iter->second;
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
		std::unique_lock lock(MissedParryMtx);
		auto Iter = MissedParry.begin();
		while (Iter != MissedParry.end())
		{
			Iter->second -= delta;
			Iter = Iter->second <= 0.f ? MissedParry.erase(Iter) : std::next(Iter);
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
