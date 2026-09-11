#include "Hooks.h"
#include "SettingsLoader.h"

#include <MinHook.h>

// this file is quickly becoming hard to parse
// 

// Attack stamina cost by consecutive same-direction count. Built in
// Hooks::Install from StaminaCost * RepeatCostMult.
static float StaminaPowerTable[4] = {};
constexpr float RepeatCostMult[4] = { 1.f, 1.4f, 2.f, 2.7f };

constexpr float ComboStaggerMagnitude = 0.5f;

// Stamina regen is driven from here rather than left to the engine, and is
// zeroed outright while committed to an action. Because we write the base rate
// every frame, the engine's StaminaRateMult multiplies OUR number — stacked
// Fortify Stamina Regeneration would scale it directly, so divide the
// multiplier back out and re-add a saturating share of it. The floor keeps the
// divisor at 100 for debuffs (cold, disease), which stay at full strength.
static void ApplyStaminaRegen(RE::Actor* actor)
{
	auto* Values = actor->AsActorValueOwner();
	if (actor->IsBlocking() || actor->IsAttacking() || DodgeHandler::GetSingleton()->IsDodging(actor))
	{
		Values->SetActorValue(RE::ActorValue::kStaminaRate, 0.f);
		return;
	}
	const float Mult = std::max(100.f, Values->GetActorValue(RE::ActorValue::kStaminaRateMult));
	const float Raw = (Mult / 100.f) - 1.f;
	const float Bonus = DifficultySettings::MaxRegenBonus * Raw / (Raw + 1.f);
	Values->SetActorValue(RE::ActorValue::kStaminaRate,
		DifficultySettings::StaminaRegenMult * (1.f + Bonus) * 100.f / Mult);
}

static int BufferedInput[2] = {0, 0};
static double InputTimer = 0.f;
static bool BufferedHasInput = false;

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
			DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker) && 
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

			if (target->IsBlocking())
			{
				BlockHandler::GetSingleton()->HandleBlock(attacker, target);
			}
			// make attacks 'safe', a chamber/masterstroke mechanic
			// you are safe during attack windup
			else if (target->IsAttacking() && AttackHandler::GetSingleton()->InChamberWindow(target))
			{
				// This should always be from the attacker side, not the target side, because the first attacker is the hit we want to ignore
				if (BlockHandler::GetSingleton()->HandleMasterstrike(attacker, target))
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
					if (DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
					{
						AIHandler::GetSingleton()->SignalBadThingExternalCalled(target, dir);
						AIHandler::GetSingleton()->SwitchTargetExternalCalled(target, attacker);
						AIHandler::GetSingleton()->TryBlockExternalCalled(target, attacker);
					}

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
			if (!AttackHandler::GetSingleton()->CanAttack(attacker))
			{
				if (Settings::VerboseLogging) logger::info("[hit] empty hit {} should not have been able to attack", attacker->GetName());
				if (attacker->IsAttacking())
				{
					attacker->NotifyAnimationGraph("attackStop");
					attacker->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
				}
				return;
			}
			bool AttackerUnblockable = DirectionHandler::GetSingleton()->IsUnblockable(attacker);
			bool TargetUnblockable = DirectionHandler::GetSingleton()->IsUnblockable(target);
			// Armor pierce. physicalDamage is the pre-mitigation value
			// totalDamage is that same figure after the target's armor. Taking
			// physicalDamage makes an unblockable land the same regardless of
			// what the target is wearing. Must precede the MeleeDamageMult
			// below, which has only ever been applied to totalDamage.
			if (AttackerUnblockable)
			{
				hitData.totalDamage = hitData.physicalDamage;
			}
			hitData.totalDamage *= DifficultySettings::MeleeDamageMult;
			float CurrentTargetStamina = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
			if (hitData.weapon)
			{
				if (AttackerUnblockable)
				{
					hitData.totalDamage *= DifficultySettings::UnblockableDamageMult;
					// restore stamina as well
					attacker->AsActorValueOwner()->RestoreActorValue(RE::ActorValue::kStamina,
						attacker->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina) * 0.15f);

				}
				bool isTargetStaggered = target->AsActorState()->actorState2.staggered;

				if (isTargetStaggered)
				{
					float damage = hitData.weapon->GetAttackDamage() * DifficultySettings::MeleeDamageMult;
					damage = std::max(damage, hitData.totalDamage) * DifficultySettings::UnblockableDamageMult;

					hitData.totalDamage = damage;
				}
				if (CurrentTargetStamina < hitData.weapon->GetAttackDamage())
				{
					float damage = hitData.weapon->GetAttackDamage() * DifficultySettings::MeleeDamageMult;
					damage = std::max(damage, hitData.totalDamage);
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

			if (hitData.totalDamage < 0.5 && hitData.attackDataSpell 
				&& (hitData.attackDataSpell->GetSpellType() == RE::MagicSystem::SpellType::kEnchantment || hitData.attackDataSpell->GetDelivery() == RE::MagicSystem::Delivery::kTouch))
			{
				
				if (Settings::VerboseLogging) logger::info("[hit] empty hit {}", hitData.attackDataSpell->GetName());
				_OnMeleeHit(target, hitData);
				return;
			}

			//logger::info("attack info {} {} {}", hitData.totalDamage, hitData.attackData->data.damageMult, hitData.resistedTypedDamage);
			// only do extra stuff if in melee with directional attacker
			if (DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
			{
				// ignore hit if was bash attack against attacking character
				// bash can be used to open up enemies but will fail if you bash after an attack started
				if (hitData.flags.any(RE::HitData::Flag::kBash))
				{
					if (target->IsAttacking())
					{
						BlockHandler::GetSingleton()->CauseStagger(attacker, target, 0.1f);
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
								BlockHandler::GetSingleton()->CauseStagger(target, attacker, 0.25f, true);
								/*
															// staggers as well
								if (CurrentTargetStamina < StaminaDamage * 0.5)
								{
									BlockHandler::GetSingleton()->CauseKnockdown(target, attacker);
								}
								else
								{
									BlockHandler::GetSingleton()->CauseStagger(target, attacker, 0.25f, true);
								}
								*/
	
							}
							else
							{
								BlockHandler::GetSingleton()->CauseStagger(target, attacker, 0.25f, true);
								/*
								if (CurrentTargetStamina < 10)
								{
									BlockHandler::GetSingleton()->CauseKnockdown(target, attacker);
								}
								else
								{
									BlockHandler::GetSingleton()->CauseStagger(target, attacker, 0.25f, true);
								}
								*/

								StaminaDamage *= 0.05f;
							}
							target->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina,StaminaDamage);
							//_OnMeleeHit(target, hitData);
							
							return;
						}
						//hitData.stagger = 0;
						if (Settings::VerboseLogging) logger::info("[hit] failed bash!");
					}

					return;
				}
			}


			// Stops the AI's power-swing stopwatch. Above the blocked/unblocked
			// split because the measurement is the same either way — it only
			// cares that the swing connected, blocked or not. 
			if (!target->IsPlayerRef())
			{
				AIHandler::GetSingleton()->NotifyPowerAttackHitExternalCalled(target);
			}

			// do no health damage if hit was blocked
			// for some reason this flag is the only flag that gets set
			if (hitData.flags.any(RE::HitData::Flag::kBlocked))
			{

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
				}
				else
				{
					//lockout defender if attacked w/ powerattack
					AttackHandler::GetSingleton()->AddLockout(target);
					if (hitData.flags.none(RE::HitData::Flag::kBash))
					{
						DirectionHandler::GetSingleton()->AddCombo(attacker);
						// a power attack into a block advances the combo, so it
						// can complete one — same stagger as the clean-hit path
						if (DirectionHandler::GetSingleton()->IsUnblockable(attacker))
						{
							BlockHandler::GetSingleton()->CauseStagger(target, attacker,
								ComboStaggerMagnitude, true);
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
					if (!target->IsPlayerRef() && DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
					{
						// AI stuff here
						Directions dir = DirectionHandler::GetSingleton()->GetCurrentDirection(attacker);
						if (DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
						{
							AIHandler::GetSingleton()->SignalBadThingExternalCalled(target, dir);
							AIHandler::GetSingleton()->SwitchTargetExternalCalled(target, attacker);
							AIHandler::GetSingleton()->TryBlockExternalCalled(target, attacker);
						}
					}
					DirectionHandler::GetSingleton()->ResetGuardCharge(target);
					DirectionHandler::GetSingleton()->AddCombo(attacker);
					// The hit that completes a combo staggers: without it the
					// target simply backpedals out of the unblockable it just
					// earned, and the combo work is wasted.
					if (DirectionHandler::GetSingleton()->IsUnblockable(attacker))
					{
						BlockHandler::GetSingleton()->CauseStagger(target, attacker,
							ComboStaggerMagnitude, true);
					}
					//apply lockout to the defender to prevent doubles
					AttackHandler::GetSingleton()->AddLockout(target);
				}
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
			DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker) &&
			DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
		{

			if (target->IsBlocking())
			{
				BlockHandler::GetSingleton()->HandleBlock(attacker, target);
			}
			// make attacks 'safe', a chamber/masterstroke mechanic
			else if (!IsBashing(attacker) && !IsBashing(target) && AttackHandler::GetSingleton()->InChamberWindow(target) && target->IsAttacking())
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
					if (DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
					{
						AIHandler::GetSingleton()->SignalBadThingExternalCalled(target, dir);
						AIHandler::GetSingleton()->SwitchTargetExternalCalled(target, attacker);
						AIHandler::GetSingleton()->TryBlockExternalCalled(target, attacker);
					}

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
		BlockHandler::GetSingleton()->Update(*g_DeltaTime);
		AttackHandler::GetSingleton()->Update(*g_DeltaTime);
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

		
		/*
			if (Settings::BufferInput)
		{
			if (InputTimer <= 0.f && BufferedHasInput)
			{
				HookMouseMovement::SharedInputMouse(BufferedInput[0], BufferedInput[1]);
				if (Settings::VerboseLogging) logger::info("[input] sent {} and {}", BufferedInput[0], BufferedInput[1]);
				BufferedInput[0] = 0;
				BufferedInput[1] = 0;
				BufferedHasInput = false;
			}
			if (InputTimer > 0.f && BufferedHasInput)
			{
				InputTimer -= a_delta;
				//logger::info("test {}", a_delta);
			}
		}	
		*/


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
		if (Settings::ForHonorMode)
		{
			SharedInputForHonor(x, y);
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
			return false;
			//seems slow but also seems to work
			//one way to do this is to slowly build up a hashmap over the lifetime of the game of if this is a power attack
			if (DifficultySettings::AttacksCostStamina)
			{
				if (RE::PlayerCharacter::GetSingleton()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) < 5)
				{
					return false;
				}
			}
			RE::Actor* target = actor->GetActorRuntimeData().currentCombatTarget.get().get();
			if (target)
			{
				if (AIHandler::GetSingleton()->ShouldAttackExternalCalled(actor, target))
				{
					//logger::info("do attack");
					return _PerformAttackAction(a_actionData);
				}
				else
				{
					return false;
				}
			}
		}
		bool ret = _PerformAttackAction(a_actionData);
		return ret;
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

	bool HookAttackHandler::ProcessAttackHook(RE::AttackBlockHandler* handler, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
	{
		
		auto eventName = a_event->QUserEvent();
		// logger::info("event{}", a_event->GetIDCode());
		auto* userEvents = RE::UserEvents::GetSingleton();

		bool IsAttackEvent =
			eventName == userEvents->attackStart ||
			eventName == userEvents->attackPowerStart ||
			eventName == userEvents->rightAttack;
		if (IsAttackEvent && !CanPlayerAttack())
		{
			return false;
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

		const float vanillaInner = *a_outInner;
		const float vanillaOuter = *a_outOuter;
		AIHandler::GetSingleton()->ApplySpacingExternalCalled(a_actor, a_outInner, a_outOuter);

		if (Settings::VerboseLogging)
		{
			logger::info("[spacing] {} -> {} vanilla=[{:.1f},{:.1f}] adjusted=[{:.1f},{:.1f}]",
				a_actor ? a_actor->GetName() : "<null>",
				a_target ? a_target->GetName() : "<null>",
				vanillaInner, vanillaOuter, *a_outInner, *a_outOuter);
		}
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
			if (!actor->IsPlayerRef())
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
					unsigned repeatCombos = DirectionHandler::GetSingleton()->GetRepeatCount(actor);
					repeatCombos = std::min(3u, repeatCombos);

					float staminaCost = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina) * StaminaPowerTable[repeatCombos];
					auto Equipped = actor->GetEquippedObject(false);
					if (Equipped)
					{
						staminaCost += (Equipped->GetWeight() * DifficultySettings::WeaponWeightStaminaMult);
					}
					if (IsPowerAttacking(actor))
					{
						staminaCost *= DifficultySettings::PowerAttackStaminaMult;
					}
					actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina,staminaCost);
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

			//logger::info("trychamberbegin {}", actor->GetName());

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
			//logger::info("Got MCO dodge event");
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
				DirectionHandler::GetSingleton()->AddTimedParry(actor);
				BlockHandler::GetSingleton()->AddBrace(actor);
				// should be called on any event that might interrupt actor changing guards
				DirectionHandler::GetSingleton()->ClearAnimationQueue(actor);
			}

		}
		else if (eventName.contains("attack"))
		{
			RE::Actor* actor = static_cast<RE::Actor*>(a_graphHolder);
			if (eventName.contains("Start"))
			{
				bool DidPowerAttack = false;
				// should be called on any event that might interrupt actor changing guards
				DirectionHandler::GetSingleton()->ClearAnimationQueue(actor);

				// Cash in the guard charge: time spent set on one line becomes
				// attack speed.
				{
					// Called unconditionally: a ratio of 0 clears any buff still
					// running, so each swing gets exactly the speed its own
					// charge bought and a chain follow-up can't ride the first
					// attack's leftover.
					const float ChargeRatio = DirectionHandler::GetSingleton()->ConsumeGuardCharge(actor);
					AttackHandler::GetSingleton()->GiveChargeSpeedBuff(actor, ChargeRatio);

					// Same-side penalty — a soft constraint on COMBOING, not on
					// swinging. Continuing a combo from the side you're already
					// on is the weak continuation, so it comes out slower, and
					// slower means REACTABLE, which is the real consequence.
					//
					// Combo-relative on purpose: GetLastAttackDirection reads
					// the combo ring, which only advances on a landed hit (or a
					// blocked power), so a whiff or a blocked light between two
					// swings doesn't count as the previous cut. That matches
					// repeatCount, which sits in the same ComboData and gates
					// the same way — both penalties describe the combo you're
					// building, not the last motion you made.
					//
					// Hard rules can't do this job: no 3-cycle over four
					// directions can alternate sides, so forbidding same-side
					// would leave every NPC pattern unable to complete a combo.
					// A cost keeps all eight legal and lets the weak option stay
					// live, under-guarded in proportion to being weak.
					//
					// Set every swing so a crossing one clears it, and kept
					// independent of guard charge so it still applies when that
					// mechanic is switched off.
					bool SameSide = false;
					Directions PrevDir;
					if (DirectionHandler::GetSingleton()->GetLastAttackDirection(actor, PrevDir))
					{
						const Directions CurDir =
							DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
						SameSide = DirectionHandler::IsLeftSide(PrevDir) ==
							DirectionHandler::IsLeftSide(CurDir);
					}
					AttackHandler::GetSingleton()->SetSameSideSpeedPenalty(actor, SameSide);
				}

				if (AttackHandler::GetSingleton()->InAttackChain(actor, DidPowerAttack))
				{
					//hack because power attack property flags are not guaranteed to have power in the event name
					if (eventName.contains("Power"))
					{
						if (!DidPowerAttack)
						{
							//logger::info("power attack {}", DidPowerAttack);
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
							//logger::info("normal attack {}", DidPowerAttack);
							AttackHandler::GetSingleton()->GiveSmallAttackSpeedBuff(actor);
						}
						else
						{
							AttackHandler::GetSingleton()->RemoveSmallAttackSpeedBuff(actor);
						}
					}
				}


			}
			else if (eventName.contains("Release"))
			{
				AttackHandler::GetSingleton()->RemoveAttackChain(actor);
			}
		}
	}
	bool HookNotifyAnimationGraph::NotifyAnimationGraph_PC(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName)
	{
		SharedNotifyAnimationGraph(a_graphHolder, eventName);
		return _NotifyAnimationGraph_PC(a_graphHolder, eventName);
	}
	bool HookNotifyAnimationGraph::NotifyAnimationGraph_NPC(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName)
	{
		SharedNotifyAnimationGraph(a_graphHolder, eventName);
		return _NotifyAnimationGraph_NPC(a_graphHolder, eventName);
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
		SKSE::AllocTrampoline(128);
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
		HookCharacterStateOnGround::Install();
		HookCharacterStateInAir::Install();
		logger::info("All hooks installed");

		for (int i = 0; i < 4; i++)
		{
			StaminaPowerTable[i] = DifficultySettings::StaminaCost * RepeatCostMult[i];
		}
	}
}


