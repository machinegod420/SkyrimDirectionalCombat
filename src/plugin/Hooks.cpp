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
			BlockHandler::GetSingleton()->AddNewAttacker(target, attacker);
			if (BlockHandler::GetSingleton()->HasHyperarmor(target))
			{
				BlockHandler::GetSingleton()->CauseStagger(attacker, target, 2.f);
				ret.bIgnoreHit = true;
				return ret;
			}
			if (target->IsBlocking())
			{
				BlockHandler::GetSingleton()->HandleBlock(attacker, target);
			}
			// make attacks 'safe', a chamber/masterstroke mechanic
			// you are safe during attack windup
			else if (AttackHandler::GetSingleton()->InChamberWindow(target) && target->IsAttacking())
			{
				if (BlockHandler::GetSingleton()->HandleMasterstrike(attacker, target))
				{
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
						AIHandler::GetSingleton()->SignalBadThing(target, dir);
						AIHandler::GetSingleton()->SwitchTarget(target, attacker);
						AIHandler::GetSingleton()->TryBlock(target, attacker);
					}

				}
			}
		}
		
		return ret; 
	}
	void HookOnMeleeHit::OnMeleeHit(RE::Actor* target, RE::HitData& hitData)
	{
		
		RE::Actor* attacker = hitData.aggressor.get().get();
		if (attacker && target)
		{
			// ignore hit if was bash attack against attacking character
			// bash can be used to open up enemies but will fail if you bash after an attack started
			if (hitData.flags.any(RE::HitData::Flag::kBash))
			{
				if (target->IsAttacking())
				{
					BlockHandler::GetSingleton()->CauseStagger(attacker, target, 1.f);

				}
				else
				{
					//bash does stamina damage
					float Damage = target->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina);
					Damage *= 0.5f;
					target->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -Damage);
					// staggers as well
					BlockHandler::GetSingleton()->CauseStagger(target, attacker, 0.5f, true);
					_OnMeleeHit(target, hitData);
				}
				
				return;
			}
			DirectionHandler::GetSingleton()->DebuffActor(attacker);
			bool AttackerUnblockable = DirectionHandler::GetSingleton()->IsUnblockable(attacker);
			bool TargetUnblockable = DirectionHandler::GetSingleton()->IsUnblockable(target);
			hitData.totalDamage *= DifficultySettings::MeleeDamageMult;

			if (AttackerUnblockable)
			{
				hitData.totalDamage *= DifficultySettings::UnblockableDamageMult;
				// restore stamina as well
				attacker->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina,
					attacker->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina) * 0.12f);

			}
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
			
			if (TargetUnblockable)
			{
				hitData.totalDamage *= 0.3f;
				hitData.stagger = 0;
			}


			// only do extra stuff if in melee with directional attacker
			if (DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker))
			{
				RE::NiPoint3 dir = attacker->GetPosition() - target->GetPosition();
				if (hitData.weapon && hitData.weapon->IsMelee())
				{
					hitData.reflectedDamage = std::max(DifficultySettings::KnockbackMult * dir.Length(), hitData.reflectedDamage);
				}

				//hitData.reflectedDamage = 0.f; 
				//dir -= target->GetPosition();

				// do no health damage if hit was blocked
				// for some reason this flag is the only flag that gets set
				//hitData.stagger = 0;
				if (hitData.flags.any(RE::HitData::Flag::kBlocked))
				{
					// manual pushback

					/*
					
							RE::hkVector4 t = RE::hkVector4(-dir.x, -dir.y, -dir.z, 0.f);
					typedef void (*tfoo)(RE::bhkCharacterController* controller, RE::hkVector4& force, float time);
					
					static REL::Relocation<tfoo> foo{ RELOCATION_ID(76442, 0) };
					foo(target->GetCharController(), t, .5f);			
					*/
					if (Settings::ExperimentalMode)
					{
						RE::hkVector4 t = RE::hkVector4(-dir.x, -dir.y, -dir.z, 0.f);
						typedef void (*tfoo)(RE::bhkCharacterController* controller, RE::hkVector4& force, float time);

						static REL::Relocation<tfoo> foo{ RELOCATION_ID(76442, 0) };
						foo(target->GetCharController(), t, .5f);
					}

					BlockHandler::GetSingleton()->ApplyBlockDamage(target, attacker, hitData);
					// apply an attack lockout to the attacker so that the victim is guaranteed to have a window to
					// riposte instead of being gambled

					if (!hitData.flags.any(RE::HitData::Flag::kPowerAttack))
					{
						if (attacker->IsPlayerRef())
						{
							AttackHandler::GetSingleton()->LockoutPlayer();
						}
						else
						{
							AttackHandler::GetSingleton()->AddLockout(attacker);
						}
					}

				}
				else
				{

					//apply lockout to the defender to prevent doubles
					if (target->IsPlayerRef())
					{
						AttackHandler::GetSingleton()->LockoutPlayer();
					}
					else
					{
						AttackHandler::GetSingleton()->AddLockout(target);
					}
					// prccess hit first in case there are bonuses for attacking staggered characters
					// and we want to process the hit before we remove the unblockable bonuses
					_OnMeleeHit(target, hitData);
					if (!TargetUnblockable)
					{
						float magnitude = 0.f;
						if (AttackerUnblockable)
						{
							magnitude = 0.5f;
						}
						BlockHandler::GetSingleton()->CauseStagger(target, attacker, magnitude, AttackerUnblockable);
					}

					// only for NPCs with directional attacks. do not need enemy to have directions
					if (DirectionHandler::GetSingleton()->HasDirectionalPerks(attacker))
					{
						// attack successfully landed, so the attacker gets to add to their combo
						DirectionHandler::GetSingleton()->AddCombo(attacker);
					}
					return;
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
			BlockHandler::GetSingleton()->AddNewAttacker(target, attacker);
			if (BlockHandler::GetSingleton()->HasHyperarmor(target))
			{
				BlockHandler::GetSingleton()->CauseStagger(attacker, target, 2.f);
				return;
			}
			if (target->IsBlocking())
			{
				BlockHandler::GetSingleton()->HandleBlock(attacker, target);
			}
			// make attacks 'safe', a chamber/masterstroke mechanic
			else if (AttackHandler::GetSingleton()->InChamberWindow(target) && target->IsAttacking())
			{
				if (BlockHandler::GetSingleton()->HandleMasterstrike(attacker, target))
				{
					return;
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
						AIHandler::GetSingleton()->SignalBadThing(target, dir);
						AIHandler::GetSingleton()->SwitchTarget(target, attacker);
						AIHandler::GetSingleton()->TryBlock(target, attacker);
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
		_Update();
	}

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

		DirectionHandler::GetSingleton()->UpdateCharacter(a_this, a_delta);
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
		int32_t diff = InputSettings::MouseSens;
		int32_t diff2 = InputSettings::MouseSens * 2;
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
			if (contains && WantDirection == Directions::TL && CurrentDirection == Directions::BL)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TR, true, false);
			}
			else if (contains && WantDirection == Directions::BL && CurrentDirection == Directions::TL)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BR, true, false);
			}
			else
			{
				DirectionHandler::GetSingleton()->SwitchDirectionRight(Player);
			}
			
		}
		if (x < -diff)
		{ 
			if (contains && WantDirection == Directions::TR && CurrentDirection == Directions::BR)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TL, true, false);
			}
			else if (contains && WantDirection == Directions::BR && CurrentDirection == Directions::TR)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BL, true, false);
			}
			else
			{
				DirectionHandler::GetSingleton()->SwitchDirectionLeft(Player);
			}
			

		}
		if (y < -diff)
		{
			if (contains && WantDirection == Directions::BR && CurrentDirection == Directions::BL)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TR, true, false);
			}
			else if (contains && WantDirection == Directions::BL && CurrentDirection == Directions::BR)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TL, true, false);
			}
			else
			{
				DirectionHandler::GetSingleton()->SwitchDirectionUp(Player);
			}
			
		}
		if (y > diff)
		{
			if (contains && WantDirection == Directions::TR && CurrentDirection == Directions::TL)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BR, true, false);
			}
			else if (contains && WantDirection == Directions::TL && CurrentDirection == Directions::TR)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BL, true, false);
			}
			else
			{
				DirectionHandler::GetSingleton()->SwitchDirectionDown(Player);
			}
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
		if (actor->GetActorRuntimeData().currentProcess->high->attackData && Settings::RemovePowerAttacks)
		{
			if (actor->GetActorRuntimeData().currentProcess->high->attackData.get()->data.flags.any(
				RE::AttackData::AttackFlag::kPowerAttack))
			{
				// return false;
			}
		}



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
			if (a_actionData->action->formEditorID.contains("Power"))
			{
				//return false;
			}
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
				if (AIHandler::GetSingleton()->ShouldAttack(actor, target))
				{
					//logger::info("do attack");

					bool ret = _PerformAttackAction(a_actionData);
					return ret;
				}
				else
				{
					//logger::info("do not attack");
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

		// This does not guarantee that the attack actually happens.
		// So it's possible that the AI switches directions and just switches back.
		if (ret && a_attacker && a_target)
		{
			// ai tends to not attack if its blocking
			if (!a_attacker->IsBlocking() && DirectionHandler::GetSingleton()->HasDirectionalPerks(a_attacker))
			{
				if (DirectionHandler::GetSingleton()->HasBlockAngle(a_attacker, a_target))
				{
					// Because of the above issue, we're going to RNG this for now
					int val = std::rand() % 3;
					if (val == 0)
					{
						//AIHandler::GetSingleton()->SwitchToNewDirection(a_attacker, a_target);
					}
					
				}
			}

		}

		return ret;
	}

	bool HookAttackHandler::ProcessAttackHook(RE::AttackBlockHandler* handler, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data)
	{
		auto eventName = a_event->QUserEvent();
		// logger::info("event{}", a_event->GetIDCode());
		// this may cause right hand block events to fail
		// double check here if there are issues
		if (eventName == RE::UserEvents::GetSingleton()->attackStart ||
			eventName == RE::UserEvents::GetSingleton()->attackPowerStart ||
			eventName == RE::UserEvents::GetSingleton()->rightAttack)
		{
			// if player is prevented from attacking by recoil
			if (AttackHandler::GetSingleton()->IsPlayerLocked())
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
	bool isPowerAttacking(RE::Actor* a_actor)
	{
		auto currentProcess = a_actor->GetActorRuntimeData().currentProcess;
		if (currentProcess) {
			auto highProcess = currentProcess->high;
			if (highProcess) {
				auto attackData = highProcess->attackData;
				if (attackData) {
					auto flags = attackData->data.flags;
					return flags.any(RE::AttackData::AttackFlag::kPowerAttack) && !flags.any(RE::AttackData::AttackFlag::kBashAttack);
				}
			}
		}
		return false;
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

			DirectionHandler::GetSingleton()->EndedAttackWindow(actor);
			if (DifficultySettings::AttacksCostStamina)
			{
				unsigned repeatCombos = DirectionHandler::GetSingleton()->GetRepeatCount(actor);
				repeatCombos = std::min(3u, repeatCombos);

				float staminaCost = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina) * StaminaPowerTable[repeatCombos];
				auto Equipped = actor->GetEquippedObject(false);
				if (Equipped)
				{
					staminaCost += (Equipped->GetWeight() * DifficultySettings::WeaponWeightStaminaMult);
				}
				if (isPowerAttacking(actor))
				{
					//staminaCost *= 2.0f;
				}
				actor->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -staminaCost);

			}

		}

		// hijack this as started attack as well
		else if (str == "TryChamberBegin"_h)
		{
			AttackHandler::GetSingleton()->AddChamberWindow(actor);
			AttackHandler::GetSingleton()->AddFeintWindow(actor);
			DirectionHandler::GetSingleton()->EndedAttackWindow(actor);
			//logger::info("trychamberbegin {}", actor->GetName());
			// use this to signal that an attack did happen with the AI
			if (!actor->IsPlayerRef())
			{
				AIHandler::GetSingleton()->DidAttack(actor);
			}
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
			DirectionHandler::GetSingleton()->QueueAnimationEvent(actor);
		}
		// backup to ensure that we exit the attack window
		else if (str == "attackStop"_h)
		{
			DirectionHandler::GetSingleton()->EndedAttackWindow(actor);
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

	bool HookNotifyAnimationGraph::NotifyAnimationGraph_PC(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName)
	{

		return _NotifyAnimationGraph_PC(a_graphHolder, eventName);
	}
	bool HookNotifyAnimationGraph::NotifyAnimationGraph_NPC(RE::IAnimationGraphManagerHolder* a_graphHolder, const RE::BSFixedString& eventName)
	{

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
		logger::info("All hooks installed");

		StaminaPowerTable[0] = DifficultySettings::StaminaCost;
		for (int i = 1; i < 4; i++)
		{
			StaminaPowerTable[i] = StaminaPowerTable[i - 1] * 2.f;
		}
	}
}


