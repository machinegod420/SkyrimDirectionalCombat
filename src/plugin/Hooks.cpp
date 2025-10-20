#include "Hooks.h"
#include "SettingsLoader.h"

// this file is quickly becoming hard to parse
// 

// precalc powers
// lets be honest here, was this really necessary?
static float StaminaPowerTable[4] = { 0.12f, 0.24f, 0.48f, 0.8f };

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
				if (AttackHandler::GetSingleton()->InChamberWindow(attacker))
				{
					float TargetMasterstrikeTime = AttackHandler::GetSingleton()->GetChamberWindowTime(target);
					float AttackerMasterstrikeTime = AttackHandler::GetSingleton()->GetChamberWindowTime(attacker);
					if (TargetMasterstrikeTime > AttackerMasterstrikeTime)
					{
						if (BlockHandler::GetSingleton()->HandleMasterstrike(attacker, target))
						{
							logger::info("handle masterstrike! {}", attacker->GetName());
							//ret.bIgnoreHit = true;
						}
					}
					else
					{
						if (BlockHandler::GetSingleton()->HandleMasterstrike(target, attacker))
						{
							logger::info("handle masterstrike! {}", target->GetName());
							//ret.bIgnoreHit = true;
						}
					}
				}
				else
				{
					if (BlockHandler::GetSingleton()->HandleMasterstrike(attacker, target))
					{
						logger::info("handle masterstrike! {}", attacker->GetName());
						//ret.bIgnoreHit = true;
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
		
		return ret; 
	}
	void HookOnMeleeHit::OnMeleeHit(RE::Actor* target, RE::HitData& hitData)
	{
	
		RE::Actor* attacker = hitData.aggressor.get().get();
		// make sure attacker actually has directions!
		if (attacker && target && DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker))
		{

			DirectionHandler::GetSingleton()->DebuffActor(attacker);
			bool AttackerUnblockable = DirectionHandler::GetSingleton()->IsUnblockable(attacker);
			bool TargetUnblockable = DirectionHandler::GetSingleton()->IsUnblockable(target);
			hitData.totalDamage *= DifficultySettings::MeleeDamageMult;

			if (hitData.weapon)
			{
				if (AttackerUnblockable)
				{
					// hack for true armor and to simulate armor piercing effect
					float damage = hitData.weapon->GetAttackDamage() * DifficultySettings::MeleeDamageMult;
					damage = std::max(damage, hitData.totalDamage) * DifficultySettings::UnblockableDamageMult;

					hitData.totalDamage = damage;
					// restore stamina as well
					attacker->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina,
						attacker->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina) * 0.15f);

				}
				bool isTargetStaggered = target->AsActorState()->actorState2.staggered;

				if (isTargetStaggered)
				{
					float damage = hitData.weapon->GetAttackDamage() * DifficultySettings::MeleeDamageMult;
					damage = std::max(damage, hitData.totalDamage) * DifficultySettings::UnblockableDamageMult;

					hitData.totalDamage = damage;
				}
				if (target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) < hitData.weapon->GetAttackDamage())
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
				hitData.stagger = 0;
			}

			if (hitData.totalDamage < 0.5 && hitData.attackDataSpell 
				&& (hitData.attackDataSpell->GetSpellType() == RE::MagicSystem::SpellType::kEnchantment || hitData.attackDataSpell->GetDelivery() == RE::MagicSystem::Delivery::kTouch))
			{
				
				logger::info("empty hit {}", hitData.attackDataSpell->GetName());
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
						BlockHandler::GetSingleton()->CauseStagger(attacker, target, 0.25f);

					}
					else
					{
						// ignore bashes against targets that cant attack because it breaks the game
						if (AttackHandler::GetSingleton()->CanAttack(target))
						{
							//bash does stamina damage
							float Damage = target->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina);

							if (target->IsBlocking())
							{
								Damage *= 0.2f;
								// staggers as well
								BlockHandler::GetSingleton()->CauseStagger(target, attacker, 0.5f, true);
								target->NotifyAnimationGraph("blockStop");
								hitData.stagger = 100;
							}
							else
							{
								Damage *= 0.05f;
								hitData.stagger = 100;
								BlockHandler::GetSingleton()->CauseStagger(target, attacker, 0.25f, true);
							}
							target->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -Damage);
							_OnMeleeHit(target, hitData);
							
							return;
						}
						//hitData.stagger = 0;
						logger::info("failed bash!");
					}

					return;
				}
			}


			if (hitData.flags.none(RE::HitData::Flag::kPowerAttack))
			{
				hitData.reflectedDamage = 300;
			}

			// do no health damage if hit was blocked
			// for some reason this flag is the only flag that gets set
			if (hitData.flags.any(RE::HitData::Flag::kBlocked))
			{
				// something happened and a hit happened from someone who already got flagged as unable to attack so we ignore it
				if (hitData.totalDamage < 0.5 && !AttackHandler::GetSingleton()->CanAttack(attacker))
				{
					logger::info("empty hit {} should not have been able to attack", attacker->GetName());
					return;
				}

				// manual pushback
				if (Settings::ExperimentalMode)
				{
					//RE::hkVector4 t = RE::hkVector4(-dir.x, -dir.y, -dir.z, 0.f);
					//typedef void (*tfoo)(RE::bhkCharacterController* controller, RE::hkVector4& force, float time);

					//static REL::Relocation<tfoo> foo{ RELOCATION_ID(76442, 0) };
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
				}

				// AI stuff
				if (!target->IsPlayerRef() && DirectionHandler::GetSingleton()->HasDirectionalPerks(target) && hitData.flags.none(RE::HitData::Flag::kPowerAttack))
				{
					//AIHandler::GetSingleton()->AddAction(target, AIHandler::Actions::Riposte, true);
					AIHandler::GetSingleton()->TryRiposteExternalCalled(target, attacker);
					AIHandler::GetSingleton()->SignalGoodThingExternalCalled(target, DirectionHandler::GetSingleton()->GetCurrentDirection(attacker));
				}

				_OnMeleeHit(target, hitData);
				return;
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
					DirectionHandler::GetSingleton()->AddCombo(attacker);
					//apply lockout to the defender to prevent doubles
					AttackHandler::GetSingleton()->AddLockout(target);
				}

				// prccess hit first in case there are bonuses for attacking staggered characters
				// and we want to process the hit before we remove the unblockable bonuses
				_OnMeleeHit(target, hitData);
					
				return;
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
					float TargetMasterstrikeTime = AttackHandler::GetSingleton()->GetChamberWindowTime(target);
					float AttackerMasterstrikeTime = AttackHandler::GetSingleton()->GetChamberWindowTime(attacker);
					if (TargetMasterstrikeTime > AttackerMasterstrikeTime)
					{
						if (BlockHandler::GetSingleton()->HandleMasterstrike(attacker, target))
						{
							logger::info("handle masterstrike! {}", attacker->GetName());
						}
					}
					else
					{
						if (BlockHandler::GetSingleton()->HandleMasterstrike(target, attacker))
						{
							logger::info("handle masterstrike! {}", target->GetName());
						}
					}
				}
				else
				{
					if (BlockHandler::GetSingleton()->HandleMasterstrike(attacker, target))
					{
						logger::info("handle masterstrike! {}", attacker->GetName());
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
			if (a_this->IsBlocking() || a_this->IsAttacking())
			{
				a_this->AsActorValueOwner()->SetActorValue(RE::ActorValue::kStaminaRate, 0.f);
			}
			else
			{
				a_this->AsActorValueOwner()->SetActorValue(RE::ActorValue::kStaminaRate, DifficultySettings::StaminaRegenMult);
			}
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
			if (a_this->IsBlocking() || a_this->IsAttacking())
			{
				a_this->AsActorValueOwner()->SetActorValue(RE::ActorValue::kStaminaRate, 0.f);
			}
			else
			{
				a_this->AsActorValueOwner()->SetActorValue(RE::ActorValue::kStaminaRate, DifficultySettings::StaminaRegenMult);
			}
		}
		a_this->GetActorRuntimeData().currentProcess->currentPackage.data;
		DirectionHandler::GetSingleton()->UpdateCharacter(a_this, a_delta);

		/*
			if (Settings::BufferInput)
		{
			if (InputTimer <= 0.f && BufferedHasInput)
			{
				HookMouseMovement::SharedInputMouse(BufferedInput[0], BufferedInput[1]);
				logger::info(" sent {} and {}", BufferedInput[0], BufferedInput[1]);
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
		if (Player->AsActorState()->GetWeaponState() == RE::WEAPON_STATE::kDrawn &&
			DirectionHandler::GetSingleton()->HasDirectionalPerks(Player) &&
			InputSettings::InputType != InputSettings::InputTypes::Keyboard)
		{
			bool Process = (InputSettings::InputType == InputSettings::InputTypes::MouseOnly);
			if (InputSettings::InputType == InputSettings::InputTypes::MouseKeyModifier)
			{
				Process = InputEventHandler::GetSingleton()->GetKeyModifierDown();
			}
			if (Process)
			{
				SharedInput(a_event->mouseInputX, a_event->mouseInputY);
			}
		}
		_ProcessMouseMove(a_this, a_event, a_data);
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

	bool HookAttackHandler::ProcessAttackHook(RE::AttackBlockHandler* handler, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
	{
		auto Player = RE::PlayerCharacter::GetSingleton();
		auto eventName = a_event->QUserEvent();
		// logger::info("event{}", a_event->GetIDCode());
		// this may cause right hand block events to fail
		// double check here if there are issues
		if (eventName == RE::UserEvents::GetSingleton()->attackStart ||
			eventName == RE::UserEvents::GetSingleton()->attackPowerStart ||
			eventName == RE::UserEvents::GetSingleton()->rightAttack)
		{
			// if player is prevented from attacking by recoil
			if (!AttackHandler::GetSingleton()->CanAttack(Player))
			{
				return false;
			}
			// make it so you can only switch direction after feint window has passed
			if (AttackHandler::GetSingleton()->InFeintWindow(Player))
			{
				return false;
			}
			// if queued for a feint attack do not allow attacks
			if (AttackHandler::GetSingleton()->InFeintQueue(Player))
			{
				return false;
			}
			if (DifficultySettings::AttacksCostStamina)
			{
				if (RE::PlayerCharacter::GetSingleton()->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) < 
					RE::PlayerCharacter::GetSingleton()->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina) * StaminaPowerTable[0])
				{
					return false;
				}
			}
			
		}
		return _ProcessAttackHook(handler, a_event, a_data);
	}

	float HookAIMaxRange::GetMaxRange(RE::Actor* a_actor, RE::TESBoundObject* a_object, int64_t a3)
	{
		float Range = _GetMaxRange(a_actor, a_object, a3);
		return Range;
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
						staminaCost *= 1.75f;
					}
					actor->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -staminaCost);
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
		HookAIMaxRange::Install();
		logger::info("All hooks installed");

		StaminaPowerTable[0] = DifficultySettings::StaminaCost;
		for (int i = 1; i < 4; i++)
		{
			StaminaPowerTable[i] = StaminaPowerTable[i - 1] * 2.f;
		}
	}
}


