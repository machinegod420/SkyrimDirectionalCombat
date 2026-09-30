#include "Hooks.h"
#include "SettingsLoader.h"
#include "CreatureHandler.h"

#include <MinHook.h>

// this file is quickly becoming hard to parse
// 

// Attack stamina cost by consecutive same-direction count. Built in
// Hooks::Install from StaminaCost * RepeatCostMult.
static float StaminaPowerTable[4] = {};
constexpr float FinisherPushBackMin = 400.f;
constexpr float FinisherPushBackMax = 600.f;
constexpr float FinisherPushBackFullWeight = 25.f;  // a steel warhammer
constexpr float ComboStaggerMagnitude = 0.5f;
// Share of the target's armor a hit on a staggered target ignores.
constexpr float StaggerArmorPierce = 0.5f;
// Mid-chain poke slow per weapon set, scaling ChainPokeSpeedPenalty: the thrust's
// timing comes from the set's animations. 1h, 1h+shield, 2h, battleaxe, warhammer.
constexpr float ChainPokePenaltyBySet[5] = { 1.f, 1.f, 1.f, 1.f, 1.f };

// Stamina regen is driven from here. We write it every frame, and we
// actually nerf the shit out of stamina regen buffs and debuffs because it breaks the combat
// too much. Sorry
static void ApplyStaminaRegen(RE::Actor* actor)
{
	auto* Values = actor->AsActorValueOwner();
	const bool Committed = (actor->IsBlocking() && !IsAttackingDisabled(actor)) ||
		actor->IsAttacking() || DodgeHandler::GetSingleton()->IsDodging(actor);
	if (Committed)
	{
		Values->SetActorValue(RE::ActorValue::kStaminaRate, 0.f);
		return;
	}
	// The engine scales the rate we write by StaminaRateMult, so we divide it
	// back out and apply our own curve. this means that regen buffs are not as strong as you think they are
	const float Mult = std::max(1.f, Values->GetActorValue(RE::ActorValue::kStaminaRateMult));
	const float Ratio = Mult / 100.f;
	float Bonus;
	if (Ratio >= 1.f)
	{
		const float Raw = Ratio - 1.f;
		Bonus = DifficultySettings::MaxRegenBonus * Raw / (Raw + 1.f);
	}
	else
	{
		Bonus = -DifficultySettings::MaxRegenPenalty * (1.f - Ratio);
	}
	const float Rate = CreatureHandler::GetSingleton()->IsCreatureGraph(actor->GetRace()) ?
		DifficultySettings::CreatureStaminaRegenMult : DifficultySettings::StaminaRegenMult;
	Values->SetActorValue(RE::ActorValue::kStaminaRate, Rate * (1.f + Bonus) * 100.f / Mult);
}

constexpr float BlockFacingCone = 70.f;

static bool CountsAsBlock(RE::Actor* attacker, RE::Actor* target, const RE::HitData& hitData,
	bool* outRescued = nullptr)
{
	// Unblockable means it, whatever the engine flagged.
	if (DirectionHandler::GetSingleton()->IsUnblockable(attacker))
	{
		return false;
	}
	if (hitData.flags.any(RE::HitData::Flag::kBlocked))
	{
		return true;
	}
	if (BlockHandler::GetSingleton()->HasMissedParry(target) || !IsGuardUp(target) ||
		!DirectionHandler::GetSingleton()->HasBlockAngle(attacker, target))
	{
		return false;
	}
	const RE::NiPoint3 To = attacker->GetPosition() - target->GetPosition();
	float Bearing = std::atan2(To.x, To.y) - target->GetAngleZ();
	while (Bearing > 3.14159265f) Bearing -= 6.28318531f;
	while (Bearing < -3.14159265f) Bearing += 6.28318531f;
	if (std::fabs(Bearing) * 57.2957795f > BlockFacingCone)
	{
		return false;
	}
	// A block the engine never flagged: none of its feedback will fire.
	if (outRescued)
	{
		*outRescued = true;
	}
	return true;
}

namespace Hooks
{
	PRECISION_API::PreHitCallbackReturn PrecisionCallback::PrecisionPrehit(const PRECISION_API::PrecisionHitData& a_precisionHitData)
	{
		PRECISION_API::PreHitCallbackReturn ret;
		RE::Actor* attacker = a_precisionHitData.attacker;
		if (!a_precisionHitData.target)
		{
			return ret;
		}
		RE::Actor* target = a_precisionHitData.target->As<RE::Actor>();
		if (attacker && target &&
			(DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker) ||
				CreatureHandler::GetSingleton()->IsDirectionalAttacker(attacker)) &&
			DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
		{
			//BlockHandler::GetSingleton()->AddNewAttacker(target, attacker);
			/*
					if (BlockHandler::GetSingleton()->HasHyperarmor(target, attacker))
			{
				BlockHandler::GetSingleton()->CauseStagger(attacker, target, 2.f);
				ret.bIgnoreHit = true;
				return ret;
			}	
			*/

			// A hit OnMeleeHit will discard must not strip the guard on the way past.
			if (DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker) &&
				!AttackHandler::GetSingleton()->CanAttack(attacker))
			{
				if (Settings::VerboseLogging)
				{
					logger::info("[prehit] {} locked out, hit ignored", attacker->GetName());
				}
				ret.bIgnoreHit = true;
				return ret;
			}
			// A hit reaching OnMeleeHit with no prehit above it came from vanilla.
			const bool GuardUp = IsGuardUp(target);
			if (Settings::VerboseLogging)
			{
				logger::info("[prehit] {} -> {} guard {}", attacker->GetName(), target->GetName(), GuardUp);
			}
			if (GuardUp)
			{
				BlockHandler::GetSingleton()->HandleBlock(attacker, target);
			}
			// make attacks 'safe', a chamber/masterstroke mechanic
			// you are safe during attack windup
			else if (DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker) && target->IsAttacking() && AttackHandler::GetSingleton()->InChamberWindow(target))
			{
				// This should always be from the attacker side, not the target side, because the first attacker is the hit we want to ignore
				// A bash into an attack is OnMeleeHit's to fail; a clash here would drop it first.
				if (!IsBashing(attacker) && BlockHandler::GetSingleton()->HandleMasterstrike(attacker, target))
				{
					if (Settings::VerboseLogging) logger::info("[hit] handle masterstrike! {}", attacker->GetName());
					ret.bIgnoreHit = true;
				}
			}
			else
			{
				if (!target->IsPlayerRef())
				{
					// AI stuff here
					Directions dir = DirectionHandler::GetSingleton()->GetCurrentDirection(attacker);
					AIHandler::GetSingleton()->SignalBadThingExternalCalled(target, dir);
					AIHandler::GetSingleton()->TryBlockExternalCalled(target, attacker);

				}
			}
		}
		
		return ret; 
	}
	void HookOnMeleeHit::OnMeleeHit(RE::Actor* target, RE::HitData& hitData)
	{
		RE::Actor* attacker = hitData.aggressor.get().get();
		// Ranged-weapon-caught-in-melee handler. Two behaviors:
		//   - Full disarm (drops weapon to the world, lootable): the player,
		//     or NPCs that are hostile to the player.
		//   - Soft unequip only (weapon stays in inventory): everyone else —
		//     followers, guards, neutral NPCs. They re-equip a melee fallback
		//     but don't permanently lose their gear. Prevents losing weapons
		//     to bad combat AI interactions or accidental hits.
		if (target && attacker)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			const bool fullDisarm = target->IsPlayerRef()
				|| (player && target->IsHostileToActor(player));
			const bool fired = fullDisarm
				? DisarmRangedWeapon(target, attacker)
				: UnequipRangedWeapon(target);
			if (fired)
			{
				BlockHandler::GetSingleton()->CauseStagger(target, attacker, 0.5f, true);
			}
		}

		// make sure attacker actually has directions!
		if (attacker && target && DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker))
		{
			// something happened and a hit happened from someone who already got flagged as unable to attack so we ignore it
			// kNone means the swing already ended: vanilla detection firing after
			// attackStop dropped Precision's collisions.
			const bool Outlived =
				attacker->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone;
			if (!AttackHandler::GetSingleton()->CanAttack(attacker) || Outlived)
			{
				if (attacker->IsAttacking())
				{
					attacker->NotifyAnimationGraph("attackStop");
					attacker->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
				}
				return;
			}
			// Unblockables pay against anything fought on lines, fencer or
			// creature; never against an archer or mage.
			bool AttackerUnblockable = DirectionHandler::GetSingleton()->IsUnblockable(attacker) &&
				DirectionHandler::GetSingleton()->IsDirectionalOpponent(target);
			bool TargetUnblockable = DirectionHandler::GetSingleton()->IsUnblockable(target);
			// Armor pierce. physicalDamage is the pre-mitigation value
			// totalDamage is that same figure after the target's armor. 
			if (AttackerUnblockable)
			{
				hitData.totalDamage = hitData.physicalDamage;
				// Knocks the target out of measure so the exchange resets to neutral
				// instead of the attacker opening a new chain on a locked-out target.
				const auto* FinisherWeapon = attacker->GetEquippedObject(false);
				const float FinisherWeight = FinisherWeapon ? FinisherWeapon->GetWeight() : 0.f;
				hitData.pushBack = FinisherPushBackMin + (FinisherPushBackMax - FinisherPushBackMin) *
					std::clamp(FinisherWeight / FinisherPushBackFullWeight, 0.f, 1.f);
			}
			hitData.totalDamage *= DifficultySettings::MeleeDamageMult;
			float CurrentTargetStamina = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
			if (hitData.weapon)
			{
				if (AttackerUnblockable)
				{
					// No stamina refund: the finisher should end the exchange, not
					// bankroll the next chain.
					hitData.totalDamage *= DifficultySettings::UnblockableDamageMult;
				}
				// A staggered target is an opening: part of its armor doesn't apply. The finisher
				// already pierces all of it.
				if (target->AsActorState()->actorState2.staggered && !AttackerUnblockable)
				{
					const float Pierced = hitData.physicalDamage * DifficultySettings::MeleeDamageMult;
					hitData.totalDamage += std::max(0.f, Pierced - hitData.totalDamage) * StaggerArmorPierce;
				}
			}

			/*
			int numAttackersTarget = (BlockHandler::GetSingleton()->GetNumberAttackers(target));
			if (numAttackersTarget > 1)
			{
				hitData.totalDamage *= 0.75f;
			}
			int numAttackersAttacker = (BlockHandler::GetSingleton()->GetNumberAttackers(attacker));
			if (numAttackersAttacker > 1)
			{
				hitData.totalDamage *= 1.25f;
			}		
			
			*/
			
			if (TargetUnblockable)
			{
				hitData.totalDamage *= 0.5f;
			}
			// The finisher scales with the target, not the weapon: at least a share of its
			// max health, so completing a combo is a real clock against a tanky enemy. Last,
			// so it's a floor under all the math above.
			if (AttackerUnblockable)
			{
				const float Floor = target->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth) *
					(target->IsPlayerRef() ? DifficultySettings::PlayerUnblockableHealthFloor : DifficultySettings::UnblockableHealthFloor);
				if (hitData.totalDamage < Floor)
				{
					hitData.totalDamage = Floor;
					// The engine can re-derive totalDamage from these after we return, block
					// share included, and an unblockable into a raised guard may carry one.
					hitData.physicalDamage = Floor;
					hitData.resistedPhysicalDamage = 0.f;
					hitData.resistedTypedDamage = 0.f;
					hitData.percentBlocked = 0.f;
				}
			}

			if (hitData.totalDamage < 0.5 && hitData.attackDataSpell 
				&& (hitData.attackDataSpell->GetSpellType() == RE::MagicSystem::SpellType::kEnchantment || hitData.attackDataSpell->GetDelivery() == RE::MagicSystem::Delivery::kTouch))
			{
				// This returns before the block branch, so without checking here
				// a proc lands on a guard that answered the line.
				if (CountsAsBlock(attacker, target, hitData))
				{
					if (Settings::VerboseLogging) logger::info("[hit] blocked proc {} from {}",
						hitData.attackDataSpell->GetName(), attacker->GetName());
					return;
				}
				if (Settings::VerboseLogging) logger::info("[hit] empty hit {}", hitData.attackDataSpell->GetName());
				_OnMeleeHit(target, hitData);
				return;
			}

			// only do extra stuff if in melee with directional attacker
			if (DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
			{
				hitData.stagger = 0.f;
				// ignore hit if was bash attack against attacking character
				// bash can be used to open up enemies but will fail if you bash after an attack started
				if (hitData.flags.any(RE::HitData::Flag::kBash))
				{
					if (target->IsAttacking())
					{
						BlockHandler::GetSingleton()->CauseStagger(attacker, target, 0.25f);
						return;
					}
					else
					{
						// ignore bashes against targets that cant attack because it breaks the game
						if (AttackHandler::GetSingleton()->CanAttack(target))
						{
							//bash does stamina damage
							float StaminaDamage = target->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina);
							if (target->IsBlocking())
							{
															// staggers as well
								if (CurrentTargetStamina < StaminaDamage * 0.5)
								{
									BlockHandler::GetSingleton()->CauseKnockdown(target, attacker);
								}
								else
								{
									BlockHandler::GetSingleton()->CauseStagger(target, attacker, 0.5f, true);
								}
								
								StaminaDamage *= 0.5f;
							}
							else
							{
								
								if (CurrentTargetStamina < 10)
								{
									BlockHandler::GetSingleton()->CauseKnockdown(target, attacker);
								}
								else
								{
									BlockHandler::GetSingleton()->CauseStagger(target, attacker, 0.5f, true);
								}
								

								StaminaDamage *= 0.25f;
							}
							target->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina,StaminaDamage);
							_OnMeleeHit(target, hitData);
							
							return;
						}
						//hitData.stagger = 0;
						if (Settings::VerboseLogging) logger::info("[hit] failed bash!");
					}

					return;
				}
			}


			// Stops the AI's power-swing stopwatch.
			if (!target->IsPlayerRef())
			{
				AIHandler::GetSingleton()->NotifyPowerAttackHitExternalCalled(target, attacker);
			}

			// Only between two guards; an unperked blocker is vanilla's business and
			// falls to the else. CountsAsBlock also rescues a line-correct guard the
			// engine never flagged.
			if (Settings::VerboseLogging && IsGuardUp(target) &&
				hitData.flags.none(RE::HitData::Flag::kBlocked))
			{
				// Vanilla only counts a block inside a frontal arc, so print how
				// far off the attacker was: a wide bearing means the engine is
				// right and our line check is what is missing a facing test.
				const RE::NiPoint3 To = attacker->GetPosition() - target->GetPosition();
				float Bearing = std::atan2(To.x, To.y) - target->GetAngleZ();
				while (Bearing > 3.14159265f) Bearing -= 6.28318531f;
				while (Bearing < -3.14159265f) Bearing += 6.28318531f;
				logger::info("[block] {} was blocking but the hit from {} carried no blocked flag (bearing {:.0f} deg)",
					target->GetName(), attacker->GetName(), std::fabs(Bearing) * 57.2957795f);
			}
			bool RescuedBlock = false;
			if (CountsAsBlock(attacker, target, hitData, &RescuedBlock) &&
				DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
			{
				if (RescuedBlock && Settings::VerboseLogging)
				{
					logger::info("[block] rescued: {} blocked {} on line with no engine flag (no recoil or sound fired)",
						target->GetName(), attacker->GetName());
				}
				// Read-only stats, before AddCombo below moves the attacker's combo on.
				if (attacker->IsPlayerRef())
				{
					AIHandler::GetSingleton()->ResolvePlayerSwing(AIHandler::SwingOutcome::Blocked);
				}
				else if (target->IsPlayerRef())
				{
					AIHandler::GetSingleton()->RecordPlayerDefense(DirectionHandler::GetSingleton()->AnimationSet(target),
						hitData.flags.any(RE::HitData::Flag::kPowerAttack), DirectionHandler::GetSingleton()->GetComboStep(attacker) > 0,
						DirectionHandler::GetSingleton()->HasTimedParry(target) ? AIHandler::DefenseOutcome::Parried : AIHandler::DefenseOutcome::Blocked);
				}

				// manual pushback
				if (Settings::ExperimentalMode)
				{
					//RE::hkVector4 t = RE::hkVector4(-dir.x, -dir.y, -dir.z, 0.f);
					//typedef void (*tfoo)(RE::bhkCharacterController* controller, RE::hkVector4& force, float time);

					//static REL::Relocation<tfoo> foo{ RELOCATION_ID(76442, 40740) };
					//foo(target->GetCharController(), t, .5f);

				}


				BlockHandler::GetSingleton()->ApplyBlockDamage(target, attacker, hitData);
				// apply an attack lockout to the attacker so that the victim is guaranteed to have a window to
				// riposte instead of being gambled
					
				if (hitData.flags.none(RE::HitData::Flag::kPowerAttack))
				{
					AttackHandler::GetSingleton()->AddLockout(attacker);
					AttackHandler::GetSingleton()->ClearComboSlow(target);
				}
				else
				{
					//lockout defender if attacked w/ powerattack
					AttackHandler::GetSingleton()->AddLockout(target);
					// A timed parry gives the attacker no combo step.
					if (hitData.flags.none(RE::HitData::Flag::kBash) && !DirectionHandler::GetSingleton()->HasTimedParry(target))
					{
						DirectionHandler::GetSingleton()->AddCombo(attacker);
						if (DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker))
						{
							AttackHandler::GetSingleton()->AddComboSlow(target);
						}
						AttackHandler::GetSingleton()->ClearComboSlow(attacker);
						// Unblockable only now if this step completed the combo: stagger so it can land.
						if (DirectionHandler::GetSingleton()->IsUnblockable(attacker))
						{
							BlockHandler::GetSingleton()->CauseStagger(target, attacker, ComboStaggerMagnitude, true);
						}
					}
				}

				// AI stuff
				if (!target->IsPlayerRef() && DirectionHandler::GetSingleton()->HasDirectionalPerks(target) && hitData.flags.none(RE::HitData::Flag::kPowerAttack))
				{
					//AIHandler::GetSingleton()->AddAction(target, AIHandler::Actions::Riposte, true);
					AIHandler::GetSingleton()->TryRiposteExternalCalled(target, attacker);
					AIHandler::GetSingleton()->SignalGoodThingExternalCalled(target, DirectionHandler::GetSingleton()->GetCurrentDirection(attacker));
				}

			}
			else
			{
				// make sure we don't count bashes as normal attacks
				if (hitData.flags.none(RE::HitData::Flag::kBash))
				{
					// Read-only stats, before AddCombo below moves the attacker's combo on. A
					// wrong-line guard was already stripped at prehit; the missed-parry latch keeps it.
					if (attacker->IsPlayerRef())
					{
						// An unperked blocker is left to vanilla and lands here, flagged.
						AIHandler::GetSingleton()->ResolvePlayerSwing(hitData.flags.any(RE::HitData::Flag::kBlocked) ?
							AIHandler::SwingOutcome::Blocked : AIHandler::SwingOutcome::Landed);
					}
					else if (target->IsPlayerRef() && DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
					{
						const auto Outcome = AttackerUnblockable ? AIHandler::DefenseOutcome::Unblockable :
							(BlockHandler::GetSingleton()->HasMissedParry(target) || IsGuardUp(target)) ? AIHandler::DefenseOutcome::GuardMissed :
							AIHandler::DefenseOutcome::NoGuard;
						AIHandler::GetSingleton()->RecordPlayerDefense(DirectionHandler::GetSingleton()->AnimationSet(target),
							hitData.flags.any(RE::HitData::Flag::kPowerAttack), DirectionHandler::GetSingleton()->GetComboStep(attacker) > 0, Outcome);
					}
					if (!target->IsPlayerRef() && DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
					{
						// AI stuff here
						Directions dir = DirectionHandler::GetSingleton()->GetCurrentDirection(attacker);
						AIHandler::GetSingleton()->SignalBadThingExternalCalled(target, dir);
						AIHandler::GetSingleton()->TryBlockExternalCalled(target, attacker);
					}
					// The combo is line variety and runs against anything fought
					// on lines, creature included. The lockout is initiative and
					// needs a guard to trade with.
					if (DirectionHandler::GetSingleton()->IsDirectionalOpponent(target))
					{
						DirectionHandler::GetSingleton()->ResetGuardCharge(target);
						// a landed power attack counts two steps atm
						const bool Finisher = DirectionHandler::GetSingleton()->IsUnblockable(attacker);
						const bool PowerStep = hitData.flags.any(RE::HitData::Flag::kPowerAttack) &&
							!Settings::IsForHonor() && !Finisher;
						DirectionHandler::GetSingleton()->AddCombo(attacker, false, PowerStep ? 1 : 0);
						if (PowerStep)
						{
							FXHandler::GetSingleton()->PlayComboPowerStep(attacker);
						}
						// The finisher spends the vortex; the exchange resets to measure.
						if (Finisher)
						{
							AttackHandler::GetSingleton()->ClearComboSlow(target);
						}
						else if (DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker))
						{
							AttackHandler::GetSingleton()->AddComboSlow(target);
						}
						AttackHandler::GetSingleton()->ClearComboSlow(attacker);
						// Unblockable only now if this hit completed the combo: stagger so it can land.
						if (DirectionHandler::GetSingleton()->IsUnblockable(attacker))
						{
							BlockHandler::GetSingleton()->CauseStagger(target, attacker, ComboStaggerMagnitude, true);
						}
					}
					//apply lockout to the defender to prevent doubles
					if (DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
					{
						AttackHandler::GetSingleton()->AddLockout(target);
					}
				}
			}
		}
		// Creature attacker: block angle and cost only
		else if (attacker && target &&
			CreatureHandler::GetSingleton()->IsDirectionalAttacker(attacker) &&
			DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
		{
			// Same stopwatch stop as the perked path, so an NPC learns a
			// creature's power windup too.
			if (!target->IsPlayerRef())
			{
				AIHandler::GetSingleton()->NotifyPowerAttackHitExternalCalled(target, attacker);
			}
			bool RescuedBlock = false;
			if (CountsAsBlock(attacker, target, hitData, &RescuedBlock))
			{
				if (RescuedBlock && Settings::VerboseLogging)
				{
					logger::info("[block] rescued (creature): {} blocked {} on line with no engine flag",
						target->GetName(), attacker->GetName());
				}
				BlockHandler::GetSingleton()->ApplyBlockDamage(target, attacker, hitData);
			}
			else
			{
				DirectionHandler::GetSingleton()->ResetGuardCharge(target);
			}
		}
		_OnMeleeHit(target, hitData);
	}

	void HookProjectileHit::OnArrowHit(RE::Projectile* a_this, RE::hkpAllCdPointCollector* a_AllCdPointCollector)
	{
		if (a_this)
		{
			a_this->GetProjectileRuntimeData().weaponDamage *= DifficultySettings::ProjectileDamageMult;
		}
		_OnArrowHit(a_this, a_AllCdPointCollector);
	}

	void HookProjectileHit::OnMissileHit(RE::Projectile* a_this, RE::hkpAllCdPointCollector* a_AllCdPointCollector)
	{
		if (a_this)
		{
			a_this->GetProjectileRuntimeData().weaponDamage *= DifficultySettings::ProjectileDamageMult;
			if (a_this->GetProjectileRuntimeData().explosion)
			{

			}
		}
		_OnMissileHit(a_this, a_AllCdPointCollector);
	}


	void HookBeginMeleeHit::OnBeginMeleeHit(RE::Actor* attacker, RE::Actor* target, std::int64_t a_int1, bool a_bool, void* a_unkptr)
	{
		// if the target was hit and is blocking, check if the block has the correct angles first
		if (attacker && target &&
			(DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker) ||
				CreatureHandler::GetSingleton()->IsDirectionalAttacker(attacker)) &&
			DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
		{
			// A hit we discard must not strip the defender's guard on the way past.
			const bool Outlived =
				attacker->AsActorState()->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone;
			if (DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker) &&
				(!AttackHandler::GetSingleton()->CanAttack(attacker) || Outlived))
			{
				if (Settings::VerboseLogging)
				{
					logger::info("[hit] {} vanilla-path hit ignored{}", attacker->GetName(),
						Outlived ? " (attack already ended)" : " (locked out)");
				}
				_OnBeginMeleeHit(attacker, target, a_int1, a_bool, a_unkptr);
				return;
			}
			// Precision owns melee hits, so a live swing reaching vanilla detection is
			// suspect: the ghost hits. Logged whatever the verbosity setting.
			if (Settings::HasPrecision && DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker))
			{
				auto* Process = attacker->GetActorRuntimeData().currentProcess;
				const auto* AttackData = Process && Process->high ? Process->high->attackData.get() : nullptr;
				logger::warn("[hit] vanilla-path hit {} -> {}: state {}, attack data {}, bashing {}, guard {}",
					attacker->GetName(), target->GetName(), static_cast<int>(attacker->AsActorState()->GetAttackState()),
					AttackData ? AttackData->event.c_str() : "null", IsBashing(attacker), IsGuardUp(target));
			}

			if (IsGuardUp(target))
			{
				BlockHandler::GetSingleton()->HandleBlock(attacker, target);
			}
			// make attacks 'safe', a chamber/masterstroke mechanic
			else if (DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker) && !IsBashing(attacker) && !IsBashing(target) && AttackHandler::GetSingleton()->InChamberWindow(target) && target->IsAttacking())
			{
				if (AttackHandler::GetSingleton()->InChamberWindow(attacker))
				{
					// This should always be from the attacker side, not the target side, because the first attacker is the hit we want to ignore
					if (BlockHandler::GetSingleton()->HandleMasterstrike(attacker, target))
					{
						if (Settings::VerboseLogging) logger::info("[hit] handle masterstrike! {}", attacker->GetName());
					}
				}
			}
			else
			{
				if (!target->IsPlayerRef())
				{
					// AI stuff here
					Directions dir = DirectionHandler::GetSingleton()->GetCurrentDirection(attacker);
					AIHandler::GetSingleton()->SignalBadThingExternalCalled(target, dir);
					AIHandler::GetSingleton()->TryBlockExternalCalled(target, attacker);

				}
			}
		}

		_OnBeginMeleeHit(attacker, target, a_int1, a_bool, a_unkptr);
	}

	void HookUpdate::Update()
	{
		//https://github.com/ersh1/Precision/blob/main/src/Offsets.h#L5
		static float* g_DeltaTime = (float*)RELOCATION_ID(523661, 410200).address();                 // 2F6B94C, 30064CC
		DirectionHandler::GetSingleton()->Update(*g_DeltaTime);
		AIHandler::GetSingleton()->Update(*g_DeltaTime);
		CreatureHandler::GetSingleton()->Update(*g_DeltaTime);
		BlockHandler::GetSingleton()->Update(*g_DeltaTime);
		AttackHandler::GetSingleton()->Update(*g_DeltaTime);
		InputEventHandler::GetSingleton()->Update(*g_DeltaTime);
		// dodge advances inside HookCharacterStateOnGround per physics tick — not here
		//TextAlertHandler::GetSingleton()->Update(*g_DeltaTime);
		_Update();
	}

	// a_delta is a lie
	void HookCharacter::Update(RE::Actor* a_this, float a_delta)
	{
		_Update(a_this, a_delta);
		if (!a_this) 
		{
			return;
		}
		auto currentProcess = a_this->GetActorRuntimeData().currentProcess;
		if ((!currentProcess || !currentProcess->InHighProcess())) 
		{
			return;
		}

		if (DifficultySettings::AttacksCostStamina)
		{
			ApplyStaminaRegen(a_this);
		}

		static float* g_DeltaTime = (float*)RELOCATION_ID(523661, 410200).address();                 // 2F6B94C, 30064CC
		DirectionHandler::GetSingleton()->UpdateCharacter(a_this, *g_DeltaTime);
	}

	void HookPlayerCharacter::Update(RE::Actor* a_this, float a_delta)
	{
		_Update(a_this, a_delta);
		if (!a_this)
		{
			return;
		}

		if (DifficultySettings::AttacksCostStamina)
		{
			ApplyStaminaRegen(a_this);
		}
		DirectionHandler::GetSingleton()->UpdateCharacter(a_this, a_delta);

		// Swing stats summary at the end of each fight.
		static bool WasInCombat = false;
		const bool InCombat = a_this->IsInCombat();
		if (WasInCombat && !InCombat)
		{
			AIHandler::GetSingleton()->LogPlayerStats("combat over");
		}
		WasInCombat = InCombat;
	}


	void HookMouseMovement::SharedInputMNB(int x, int y)
	{
		auto Player = RE::PlayerCharacter::GetSingleton();
		int32_t diff = InputSettings::MouseSens;
	
		if (x > diff)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BR);
		}
		else if (x < -diff)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TL);
		}
		else if (y < -diff)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TR);
		}
		else if (y > diff)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BL);
		}
	}

	void HookMouseMovement::SharedInputForHonor(int x, int y)
	{
		auto Player = RE::PlayerCharacter::GetSingleton();
		int32_t diff = InputSettings::MouseSens;

		if (y < -diff)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TR);
		}
		else if (x > diff)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BR);
		}
		else if (x < -diff)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BL);
		}


	}

	void HookMouseMovement::SharedInputKCD(int x, int y)
	{
		auto Player = RE::PlayerCharacter::GetSingleton();
		int32_t diff = InputSettings::MouseSens;

		if (y > diff)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BL);
		}
		else if (x > diff)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TR);
		}
		else if (x < -diff)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TL);
		}
	}

	void HookMouseMovement::SharedInputMouse(int x, int y)
	{

		auto Player = RE::PlayerCharacter::GetSingleton();
		int32_t diff = InputSettings::MouseSens * 2;
		int32_t diff2 = InputSettings::MouseSens;
		Directions CurrentDirection = DirectionHandler::GetSingleton()->GetCurrentDirection(Player);
		Directions WantDirection;
		bool contains = DirectionHandler::GetSingleton()->HasQueuedDirection(Player, WantDirection);
		if (x > diff2 && y < -diff2)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TR);
			return;
		}
		if (x > diff2 && y > diff2)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BR);
			return;
		}
		if (x < -diff2 && y > diff2)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BL);
			return;
		}
		if (x < -diff2 && y < -diff2)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TL);
			return;
		}
		if (x > diff)
		{ 
			DirectionHandler::GetSingleton()->SwitchDirectionRight(Player, true);
			return;
		}
		if (x < -diff)
		{ 
			DirectionHandler::GetSingleton()->SwitchDirectionLeft(Player, true);

			return;

		}
		if (y < -diff)
		{
			DirectionHandler::GetSingleton()->SwitchDirectionUp(Player, true);

			return;
		}
		if (y > diff)
		{

			DirectionHandler::GetSingleton()->SwitchDirectionDown(Player, true);
			//AttackHandler::GetSingleton()->LockoutPlayer(Player);
			return;
		}
	}

	void HookMouseMovement::SharedInput(int x, int y)
	{
		if (InputSettings::InvertY)
		{
			y = -y;
		}
		if (Settings::IsForHonor())
		{
			SharedInputForHonor(x, y);
			return;
		}

		if (Settings::IsKCD())
		{
			SharedInputKCD(x, y);
			return;
		}

		if (Settings::MNBMode)
		{
			SharedInputMNB(x, y);
			return;
		}

		SharedInputMouse(x, y);
		return;


	}

	void HookMouseMovement::ProcessMouseMove(RE::LookHandler* a_this, RE::MouseMoveEvent* a_event, RE::PlayerControlsData* a_data)
	{
		auto Player = RE::PlayerCharacter::GetSingleton();
		bool LockCamera = false;
		if (Player->AsActorState()->GetWeaponState() == RE::WEAPON_STATE::kDrawn &&
			DirectionHandler::GetSingleton()->HasDirectionalPerks(Player) &&
			InputSettings::InputType != InputSettings::InputTypes::Keyboard)
		{
			const bool ModifierDown = InputEventHandler::GetSingleton()->GetKeyModifierDown();
			bool Process = (InputSettings::InputType == InputSettings::InputTypes::MouseOnly);
			if (InputSettings::InputType == InputSettings::InputTypes::MouseKeyModifier)
			{
				Process = ModifierDown;
			}
			if (Process)
			{
				SharedInput(a_event->mouseInputX, a_event->mouseInputY);
			}
			LockCamera = InputSettings::KeyModifierLocksCamera && ModifierDown;
		}
		if (!LockCamera)
		{
			_ProcessMouseMove(a_this, a_event, a_data);
		}
	}

	void HookMouseMovement::ProcessThumbstick(RE::LookHandler* a_this, RE::ThumbstickEvent* a_event, RE::PlayerControlsData* a_data)
	{
		auto Player = RE::PlayerCharacter::GetSingleton();
		if (Player->AsActorState()->GetWeaponState() == RE::WEAPON_STATE::kDrawn &&
			DirectionHandler::GetSingleton()->HasDirectionalPerks(Player) &&
			InputSettings::InputType != InputSettings::InputTypes::Keyboard)
		{
			// this is wrong as these floats are almost certainly normalized values
			SharedInput((int)(a_event->xValue * 10.f), (int)(a_event->yValue * 10.f));
		}
		_ProcessThumbstick(a_this, a_event, a_data);
	}

	bool HookOnAttackAction::PerformAttackAction(RE::TESActionData* a_actionData)
	{
		RE::Actor* actor = a_actionData->source.get()->As<RE::Actor>();
		// doing a bad thing here
		if (!AttackHandler::GetSingleton()->CanAttack(actor))
		{
			return false;
		}
		if (!actor)
		{
			return _PerformAttackAction(a_actionData);
		}

		// try to prevent power attacks, however this is not guaranteed to be populated
		/*
			if (actor->GetActorRuntimeData().currentProcess->high->attackData && Settings::RemovePowerAttacks)
		{
			if (actor->GetActorRuntimeData().currentProcess->high->attackData.get()->data.flags.any(
				RE::AttackData::AttackFlag::kPowerAttack))
			{
				// return false;
			}
		}	
		*/

		if (DifficultySettings::AttacksCostStamina)
		{
			if (actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) < actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina) * StaminaPowerTable[0])
			{
				return false;
			}
		}

		if (DirectionHandler::GetSingleton()->IsUnblockable(actor))
		{
			_PerformAttackAction(a_actionData);
			return true;
		}

		if (DirectionHandler::GetSingleton()->HasDirectionalPerks(actor))
		{
			RE::Actor* target = GetCombatTarget(actor);
			if (target && !AIHandler::GetSingleton()->ShouldAttackExternalCalled(actor, target))
			{
				return false;
			}
		}
		return _PerformAttackAction(a_actionData);
	}


	bool HookHasAttackAngle::GetAttackAngle(RE::Actor* a_attacker, RE::Actor* a_target, const RE::NiPoint3& a3, const RE::NiPoint3& a4, RE::BGSAttackData* a_attackData, float a6, void* a7, bool a8)
	{
		bool ret = _GetAttackAngle(a_attacker, a_target, a3, a4, a_attackData, a6, a7, a8);

		// parse in range events here
		if (ret && a_attacker && a_target)
		{


		}

		return ret;
	}

	bool HookAttackHandler::CanPlayerAttack()
	{
		return AttackHandler::GetSingleton()->CanInitiateAttack(RE::PlayerCharacter::GetSingleton());
	}

	void HookAttackHandler::OnPlayerAttackRefused()
	{
		FXHandler::GetSingleton()->PlayAttackRefused();
		if (Settings::VerboseLogging)
		{
			auto* Player = RE::PlayerCharacter::GetSingleton();
			auto* Attacks = AttackHandler::GetSingleton();
			// CanInitiateAttack's checks, in its order.
			const char* Reason = !Attacks->CanAttack(Player) ? "lockout" :
				Attacks->InFeintWindow(Player) ? "feint window" : "stamina";
			logger::info("[press] refused ({}): stamina {:.0f}, a light swing needs {:.0f}", Reason,
				Player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina),
				Attacks->SwingStaminaCost(Player, false));
		}
	}

	bool HookAttackHandler::ProcessAttackHook(RE::AttackBlockHandler* handler, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
	{
		
		auto eventName = a_event->QUserEvent();
		auto* userEvents = RE::UserEvents::GetSingleton();

		// The block button: pressing it cancels a buffered attack, since defending wins.
		if (eventName == userEvents->leftAttack)
		{
			if (a_event->IsDown())
			{
				InputEventHandler::GetSingleton()->OnBlockPressed();
			}
			return _ProcessAttackHook(handler, a_event, a_data);
		}

		bool IsAttackEvent =
			eventName == userEvents->attackStart ||
			eventName == userEvents->attackPowerStart ||
			eventName == userEvents->rightAttack;
		// Only the press decides anything; holds and releases always reach vanilla.
		if (IsAttackEvent && a_event->IsDown())
		{
			// One line per click: which attack state a press lands in, and which gate it meets.
			if (Settings::VerboseLogging)
			{
				auto* Player = RE::PlayerCharacter::GetSingleton();
				bool GraphAttacking = false;
				bool GraphBashing = false;
				const bool HasAttacking = Player->GetGraphVariableBool("IsAttacking", GraphAttacking);
				const bool HasBashing = Player->GetGraphVariableBool("IsBashing", GraphBashing);
				Directions Pending;
				// The gate's three parts, so a refusal says which one refused.
				auto* Attacks = AttackHandler::GetSingleton();
				logger::info("[press] attack: state={} graph IsAttacking={} IsBashing={} queued={} buffered={} canAttack={} (lockedOut={} feintWindow={} stamina {:.0f}, light swing {:.0f})",
					(int)Player->AsActorState()->GetAttackState(),
					HasAttacking ? (GraphAttacking ? "true" : "false") : "n/a",
					HasBashing ? (GraphBashing ? "true" : "false") : "n/a",
					DirectionHandler::GetSingleton()->HasQueuedDirection(Player, Pending),
					InputEventHandler::GetSingleton()->HasBufferedAttack(),
					CanPlayerAttack(), !Attacks->CanAttack(Player), Attacks->InFeintWindow(Player),
					Player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina), Attacks->SwingStaminaCost(Player, false));
			}
			auto* Input = InputEventHandler::GetSingleton();
			if (Input->ShouldHoldPress())
			{
				Input->BufferAttack(false);
				return false;
			}
			if (!CanPlayerAttack())
			{
				OnPlayerAttackRefused();
				return false;
			}
		}
		return _ProcessAttackHook(handler, a_event, a_data);
	}


	void HookCombatAdvanceRadius::Install()
	{
		const MH_STATUS init = MH_Initialize();
		if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
		{
			logger::error("[spacing] MH_Initialize failed: {}", static_cast<int>(init));
			return;
		}

		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(49716, 46133) };
		void* addr = reinterpret_cast<void*>(target.address());

		if (MH_CreateHook(addr, reinterpret_cast<void*>(&ComputeAdvanceRadii),
				reinterpret_cast<void**>(&_ComputeAdvanceRadii)) != MH_OK ||
			MH_EnableHook(addr) != MH_OK)
		{
			logger::error("[spacing] failed to hook ComputeAdvanceRadii at {:#x}", target.address());
			return;
		}
		logger::info("[spacing] hooked ComputeAdvanceRadii at {:#x}", target.address());
	}

	void HookCombatAdvanceRadius::ComputeAdvanceRadii(RE::Actor* a_actor, RE::Actor* a_target, float* a_outInner, float* a_outOuter)
	{
		_ComputeAdvanceRadii(a_actor, a_target, a_outInner, a_outOuter);

		if (!a_outInner || !a_outOuter)
		{
			return;
		}

		AIHandler::GetSingleton()->ApplySpacingExternalCalled(a_actor, a_outInner, a_outOuter);
	}

	void HookProcessMotionData::Install()
	{
		const MH_STATUS init = MH_Initialize();
		if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
		{
			logger::error("[motion] MH_Initialize failed: {}", static_cast<int>(init));
			return;
		}

		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(31949, 32703) };
		void* addr = reinterpret_cast<void*>(target.address());

		if (MH_CreateHook(addr, reinterpret_cast<void*>(&ProcessMotionData),
				reinterpret_cast<void**>(&_ProcessMotionData)) != MH_OK ||
			MH_EnableHook(addr) != MH_OK)
		{
			logger::error("[motion] failed to hook ProcessMotionData at {:#x}", target.address());
			return;
		}
		logger::info("[motion] hooked ProcessMotionData at {:#x}", target.address());
	}

	bool HookProcessMotionData::ProcessMotionData(RE::Character* a_this, float a_dt, RE::NiPoint3* a_translation, RE::NiPoint3* a_rotation, bool* a_flag)
	{
		const bool Moved = _ProcessMotionData(a_this, a_dt, a_translation, a_rotation, a_flag);
		// Lunges in directional fights: the windup's forward motion, by the swing's table entry.
		if (Moved && a_translation && IsInWindup(a_this) &&
			DirectionHandler::GetSingleton()->HasDirectionalPerks(a_this))
		{
			auto* Dir = DirectionHandler::GetSingleton();
			a_translation->y *= AttackHandler::LungeMultFor(Dir->GetCurrentDirection(a_this),
				Dir->GetComboStep(a_this) > 0, IsPowerAttacking(a_this));
		}
		return Moved;
	}

	void HookAnimEvent::ProcessCharacterEvent(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_sink, RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource)
	{
		UNUSED(a_sink);
		UNUSED(a_eventSource);
		if (!a_event->holder)
		{
			return;
		}

		std::string_view eventTag = a_event->tag.data();
		uint32_t str = hash(eventTag.data(), eventTag.size());
		// oh god
		RE::Actor* actor = const_cast<RE::Actor*>(a_event->holder->As<RE::Actor>());
		if (!actor)
		{
			return;
		}
		if (str == "preHitFrame"_h)
		{
			// use this to signal that an attack did happen with the AI
			if (!actor->IsPlayerRef() && DirectionHandler::GetSingleton()->HasDirectionalPerks(actor))
			{
				AIHandler::GetSingleton()->DidAttackExternalCalled(actor);
			}

			// make sure we cant feint past this window
			AttackHandler::GetSingleton()->RemoveFeintWindow(actor);
			DirectionHandler::GetSingleton()->EndedAttackWindow(actor);
			if (DifficultySettings::AttacksCostStamina)
			{
				// unblockable attacks are free
				if (!DirectionHandler::GetSingleton()->IsUnblockable(actor))
				{
					actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina,
						AttackHandler::GetSingleton()->SwingStaminaCost(actor, IsPowerAttacking(actor)));
				}
				AttackHandler::GetSingleton()->AddAttackChain(actor, IsPowerAttacking(actor));
			}
		}
		else if (str == "HitFrame"_h)
		{

		}
		// hijack this as started attack as well 
		// this only triggers on the first attack, not chain attacks!
		else if (str == "TryChamberBegin"_h)
		{
			AttackHandler::GetSingleton()->AddChamberWindow(actor);
			AttackHandler::GetSingleton()->AddFeintWindow(actor);
			DirectionHandler::GetSingleton()->EndedAttackWindow(actor);

		}
		else if (str == "MCO_WinOpen"_h)
		{
			// make sure we remove
			// this is the only place in the code that both adds and removes to this set
			// so we need to make sure it doesn't leak (unlikely as there would be a cell transition between an attack, but definitely possible)
			DirectionHandler::GetSingleton()->StartedAttackWindow(actor);
			// prevent feints from happening past this window
			AttackHandler::GetSingleton()->RemoveFeintWindow(actor);

		}
		else if (str == "MCO_WinClose"_h)
		{
			DirectionHandler::GetSingleton()->EndedAttackWindow(actor);
		}
		// backup to ensure that we exit the attack window
		else if (str == "attackStop"_h)
		{
			DirectionHandler::GetSingleton()->EndedAttackWindow(actor);
			DirectionHandler::GetSingleton()->ClearAnimationQueue(actor);
			// needs to be queued as an attack might end up in a different guard
			DirectionHandler::GetSingleton()->QueueAnimationEvent(actor);
		}
		// feint events
		else if (str == "FeintToTR"_h)
		{ 
			DirectionHandler::GetSingleton()->SwitchDirectionSynchronous(actor, Directions::TR, false);
		}
		else if (str == "FeintToTL"_h)
		{
			DirectionHandler::GetSingleton()->SwitchDirectionSynchronous(actor, Directions::TL, false);
		}
		else if (str == "FeintToBL"_h)
		{
			DirectionHandler::GetSingleton()->SwitchDirectionSynchronous(actor, Directions::BL, false);
		}
		else if (str == "FeintToBR"_h)
		{
			DirectionHandler::GetSingleton()->SwitchDirectionSynchronous(actor, Directions::BR, false);
		}
		else if (str == "MCO_DodgeClose"_h)
		{
		}
	}
	RE::BSEventNotifyControl HookAnimEvent::ProcessEvent_NPC(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_sink, RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource)
	{
		ProcessCharacterEvent(a_sink, a_event, a_eventSource);
		return _ProcessEvent_NPC(a_sink, a_event, a_eventSource);
	}
	RE::BSEventNotifyControl HookAnimEvent::ProcessEvent_PC(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_sink, RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource)
	{
		ProcessCharacterEvent(a_sink, a_event, a_eventSource);
		return _ProcessEvent_PC(a_sink, a_event, a_eventSource);
	}

	void SharedNotifyAnimationGraph(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName)
	{
		if (eventName.contains("blockStart"))
		{
		
			RE::Actor* actor = static_cast<RE::Actor*>(a_graphHolder);
			if (!actor->IsBlocking())
			{
				// A held-off actor's guard isn't its own decision, so it earns no
				// parry window and burns no cooldown. The brace still counts: the
				// guard really is set, whoever set it.
				if (!IsAttackingDisabled(actor))
				{
					DirectionHandler::GetSingleton()->AddTimedParry(actor);
				}
				BlockHandler::GetSingleton()->AddBrace(actor);
				// should be called on any event that might interrupt actor changing guards
				DirectionHandler::GetSingleton()->ClearAnimationQueue(actor);
			}

		}
		// Attack starts are handled after the send, by AfterNotifyAnimationGraph.
		else if (eventName.contains("attack") && !eventName.contains("Start") && eventName.contains("Release"))
		{
			AttackHandler::GetSingleton()->RemoveAttackChain(static_cast<RE::Actor*>(a_graphHolder));
		}
	}

	// A swing start the graph accepted.
	static void OnAttackStarted(RE::Actor* actor, const RE::BSFixedString& eventName)
	{
		bool DidPowerAttack = false;
		// should be called on any event that might interrupt actor changing guards
		DirectionHandler::GetSingleton()->ClearAnimationQueue(actor);
		// A chained swing has no TryChamberBegin to close the window it started in.
		DirectionHandler::GetSingleton()->EndedAttackWindow(actor);

		// Cash in the guard charge: time spent set on one line becomes
		// attack speed.
		{
			// Called unconditionally: a ratio of 0 clears any buff still
			// running, so each swing gets exactly the speed its own
			// charge bought and a chain follow-up can't ride the first
			// attack's leftover.
			const float ChargeRatio = DirectionHandler::GetSingleton()->ConsumeGuardCharge(actor);
			AttackHandler::GetSingleton()->GiveChargeSpeedBuff(actor, ChargeRatio);

			// Chain speed penalties, set every swing so a crossing one clears them. Relative to the
			// combo, not the last motion: the previous line only advances on a landed hit.
			// Same side is four lines only; the poke penalty is lights only.
			float ChainPenalty = 0.f;
			Directions PrevDir;
			const WeaponSet Set = DirectionHandler::GetSingleton()->AnimationSet(actor);
			const bool Chained = DirectionHandler::GetSingleton()->GetLastAttackDirection(actor, PrevDir);
			// The event name only: attackData still holds the previous swing's here.
			const bool PowerSwing = eventName.contains("Power");
			if (Chained)
			{
				// 4 direction mode needed some extra stuff to make it more readable
				// so attacks comboing on the same side are slower
				// not documenting this obviously because who plays 4 direction mod
				const Directions CurDir =
					DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
				if (DirectionHandler::EnabledDirections() == 0xF &&
					DirectionHandler::IsLeftSide(PrevDir) == DirectionHandler::IsLeftSide(CurDir))
				{
					ChainPenalty = DifficultySettings::SameSideSpeedPenalty;
				}
				if (!PowerSwing && CurDir == AttackHandler::PokeLine(actor))
				{
					ChainPenalty = std::max(ChainPenalty,
						DifficultySettings::ChainPokeSpeedPenalty * ChainPokePenaltyBySet[static_cast<int>(Set)]);
				}
				// Once per player swing, unlike the NPCs watching it. Row 4 is the
				// opener, so an Unblockable last line mustn't reach it.
				if (actor->IsPlayerRef() && static_cast<int>(PrevDir) < 4)
				{
					AIHandler::GetSingleton()->RecordPlayerChainTransition(Set, static_cast<int>(PrevDir), CurDir);
				}
			}
			else if (actor->IsPlayerRef())
			{
				AIHandler::GetSingleton()->RecordPlayerChainTransition(Set, AIHandler::OpenerRow,
					DirectionHandler::GetSingleton()->GetCurrentDirection(actor));
			}
			RE::Actor* SwingTarget = DirectionHandler::GetSingleton()->GetSwingTarget(actor);
			const int ComboStep = Chained ? DirectionHandler::GetSingleton()->GetComboStep(actor) : 0;
			// -1 with no target.
			const float StartDistance = SwingTarget ? std::sqrt(TorsoDistanceSq(actor, SwingTarget)) : -1.f;
			// Before this swing's own cost, which preHitFrame charges.
			const float StartStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
			const float MaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
			const float StaminaRatio = MaxStamina > 0.f ? StartStamina / MaxStamina : 0.f;
			const bool PowerAffordable = StartStamina >= AttackHandler::GetSingleton()->SwingStaminaCost(actor, true);
			// Read-only stats; swings with no one to swing at don't count.
			if (actor->IsPlayerRef() && SwingTarget)
			{
				AIHandler::GetSingleton()->RecordPlayerSwing(Set, ComboStep, PowerSwing, StartDistance, StaminaRatio, PowerAffordable);
				AIHandler::GetSingleton()->BeginPlayerSwing(Set, ComboStep, DirectionHandler::GetSingleton()->GetCurrentDirection(actor));
			}
			AttackHandler::GetSingleton()->SetChainLinePenalty(actor, ChainPenalty);
			if (Settings::VerboseLogging)
			{
				const auto* Equipped = actor->GetEquippedObject(false);
				const auto* Weapon = Equipped ? Equipped->As<RE::TESObjectWEAP>() : nullptr;
				logger::info("[swing] {} {:08X} ({} {}) {} {}, chain penalty {:.2f}, speed delta {:.2f}, weapon speed {:.2f}, step {}, power {}, start distance {:.0f}, stamina {:.2f}, power affordable {}",
					actor->GetName(), actor->GetFormID(), WeaponSetName(Set),
					static_cast<int>(DirectionHandler::GetSingleton()->GetCurrentDirection(actor)),
					eventName.c_str(), Chained ? "in chain" : "opener", ChainPenalty,
					AttackHandler::GetSingleton()->GetAttackSpeedDelta(actor), Weapon ? Weapon->weaponData.speed : 0.f,
					ComboStep, PowerSwing, StartDistance, StaminaRatio, PowerAffordable);
			}
		}

		if (AttackHandler::GetSingleton()->InAttackChain(actor, DidPowerAttack))
		{
			//hack because power attack property flags are not guaranteed to have power in the event name
			if (eventName.contains("Power"))
			{
				if (!DidPowerAttack)
				{
					AttackHandler::GetSingleton()->GiveSmallAttackSpeedBuff(actor);
				}
				else
				{
					AttackHandler::GetSingleton()->RemoveSmallAttackSpeedBuff(actor);
				}
			}
			else
			{
				if (DidPowerAttack)
				{
					AttackHandler::GetSingleton()->GiveSmallAttackSpeedBuff(actor);
				}
				else
				{
					AttackHandler::GetSingleton()->RemoveSmallAttackSpeedBuff(actor);
				}
			}
		}
	}

	// The notify's return is whether the graph took the event. A refused attack start (a
	// press during the windup) must not re-run the swing setup against the swing still playing.
	static void AfterNotifyAnimationGraph(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName, bool a_accepted)
	{
		if (a_accepted && eventName.contains("attack") && eventName.contains("Start"))
		{
			OnAttackStarted(static_cast<RE::Actor*>(a_graphHolder), eventName);
		}
	}

	bool HookNotifyAnimationGraph::NotifyAnimationGraph_PC(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName)
	{
		SharedNotifyAnimationGraph(a_graphHolder, eventName);
		const bool Accepted = _NotifyAnimationGraph_PC(a_graphHolder, eventName);
		AfterNotifyAnimationGraph(a_graphHolder, eventName, Accepted);
		return Accepted;
	}
	bool HookNotifyAnimationGraph::NotifyAnimationGraph_NPC(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName)
	{
		SharedNotifyAnimationGraph(a_graphHolder, eventName);
		const bool Accepted = _NotifyAnimationGraph_NPC(a_graphHolder, eventName);
		AfterNotifyAnimationGraph(a_graphHolder, eventName, Accepted);
		return Accepted;
	}
	void HookCharacterStateOnGround::SimulateStatePhysics(RE::bhkCharacterStateOnGround* a_this, RE::bhkCharacterController* a_controller)
	{
		if (a_controller)
		{
			DodgeHandler::GetSingleton()->ApplyOnGround(a_controller);
		}
		_SimulateStatePhysics(a_this, a_controller);
	}

	void HookCharacterStateInAir::SimulateStatePhysics(RE::bhkCharacterStateInAir* a_this, RE::bhkCharacterController* a_controller)
	{
		if (a_controller)
		{
			DodgeHandler::GetSingleton()->CancelDodgeOnAir(a_controller);
		}
		_SimulateStatePhysics(a_this, a_controller);
	}

	void Hooks::Install()
	{
		logger::info("Installing hooks...");
		HookOnMeleeHit::Install();
		HookProjectileHit::Install();
		HookBeginMeleeHit::Install();
		HookUpdate::Install();
		HookCharacter::Install();
		HookPlayerCharacter::Install();
		HookMouseMovement::Install();
		HookOnAttackAction::Install();
		HookHasAttackAngle::Install();
		HookAttackHandler::Install();
		HookAnimEvent::Install();
		HookNotifyAnimationGraph::Install();
		HookCombatAdvanceRadius::Install();
		HookProcessMotionData::Install();
		HookCharacterStateOnGround::Install();
		HookCharacterStateInAir::Install();
		logger::info("All hooks installed");

		for (int i = 0; i < 4; i++)
		{
			StaminaPowerTable[i] = DifficultySettings::StaminaCost * RepeatCostMult[i];
		}
	}
}


