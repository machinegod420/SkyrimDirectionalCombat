#include "Hooks.h"
#include "SettingsLoader.h"

// this file is quickly becoming hard to parse
// 

// precalc powers
// lets be honest here, was this really necessary?
static float StaminaPowerTable[4] = { 0.1f, 0.2f, 0.4f, 0.8f };

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
			if (target->IsBlocking())
			{
				BlockHandler::GetSingleton()->HandleBlock(attacker, target);
			}
			// make attacks 'safe', a chamber/masterstroke mechanic
			// you are safe during attack windup
			else if (AttackHandler::GetSingleton()->InChamberWindow(target) && target->IsAttacking())
			{
				if (DirectionHandler::GetSingleton()->HasBlockAngle(attacker, target))
				{
					// potential collision issue here where these two events may happen close to the same time
					// so we check if a character is staggered first to determine if the masterstrike already happened
					bool targetStaggering = false;
					bool attackerStaggering = false;
					target->GetGraphVariableBool("IsStaggering", targetStaggering);
					attacker->GetGraphVariableBool("IsStaggering", attackerStaggering);
					if (!targetStaggering && !attackerStaggering)
					{
						FXHandler::GetSingleton()->PlayMasterstrike(target);
						ret.bIgnoreHit = true;
						BlockHandler::GetSingleton()->CauseStagger(attacker, target, 1.f);
					}

				}
			}
		}

		return ret; 
	}

	void HookOnMeleeHit::OnMeleeHit(RE::Actor* victim, RE::HitData& hitData)
	{
		RE::Actor* actor = hitData.aggressor.get().get();

		if (actor && victim)
		{
			// ignore hit if was bash attack against attacking character
			// bash can be used to open up enemies but will fail if you bash after an attack started
			if (victim->IsAttacking() && hitData.flags.any(RE::HitData::Flag::kBash))
			{
				BlockHandler::GetSingleton()->CauseStagger(actor, victim, 1.f);
				//return;
			}
			DirectionHandler::GetSingleton()->DebuffActor(actor);
			// do no health damage if hit was blocked
			// for some reason this flag is the only flag that gets set
			if (hitData.flags.any(RE::HitData::Flag::kBlocked))
			{
				hitData.stagger = 0;
				BlockHandler::GetSingleton()->ApplyBlockDamage(victim, hitData);
				// apply an attack lockout to the attacker so that the victim is guaranteed to have a window to
				// riposte instead of being gambled

				if (actor->IsPlayerRef())
				{
					AttackHandler::GetSingleton()->LockoutPlayer();
				}
				else 
				{
					AttackHandler::GetSingleton()->AddLockout(actor);
				}
			}
			else
			{
				hitData.totalDamage *= DifficultySettings::MeleeDamageMult;
				if (DirectionHandler::GetSingleton()->IsUnblockable(actor))
				{
					hitData.totalDamage *= DifficultySettings::UnblockableDamageMult;
				}
				// prccess hit first in case there are bonuses for attacking staggered characters
				// and we want to process the hit before we remove the unblockable bonuses
				_OnMeleeHit(victim, hitData);

				// only for NPCs with directional attacks. do not need enemy to have directions
				if (DirectionHandler::GetSingleton()->HasDirectionalPerks(actor))
				{
					// attack successfully landed, so the attacker gets to add to their combo
					DirectionHandler::GetSingleton()->AddCombo(actor);

					// lockout for NPC AI to not overcommit
					if (!actor->IsPlayerRef())
					{
						AttackHandler::GetSingleton()->AddNPCSmallLockout(actor);
					}
				}

				BlockHandler::GetSingleton()->CauseStagger(victim, actor, 0.f);
				if (!victim->IsPlayerRef())
				{
					// mostly prevents follow ups
					AIHandler::GetSingleton()->TryBlock(victim, actor);
				}
				return;
			}
		}

		_OnMeleeHit(victim, hitData);
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
						return;
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
		if ((!currentProcess || !currentProcess->InHighProcess()) && !a_this->IsPlayerRef()) 
		{
			return;
		}
		if (DifficultySettings::AttacksCostStamina)
		{
			if (a_this->IsBlocking() || a_this->IsAttacking())
			{

				// sometimes AI too dumb so they have faster regen rate

				a_this->AsActorValueOwner()->SetActorValue(RE::ActorValue::kStaminaRate, 2.f);
			}
			else
			{
				a_this->AsActorValueOwner()->SetActorValue(RE::ActorValue::kStaminaRate,
					a_this->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina) * DifficultySettings::StaminaRegenMult);
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
				a_this->AsActorValueOwner()->SetActorValue(RE::ActorValue::kStaminaRate, 1.f);
			}
			else
			{
				a_this->AsActorValueOwner()->SetActorValue(RE::ActorValue::kStaminaRate,
					a_this->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina) * DifficultySettings::StaminaRegenMult);
			}
		}
		
		DirectionHandler::GetSingleton()->UpdateCharacter(a_this, a_delta);
	}

	void HookMouseMovement::SharedInput(int x, int y)
	{
		if (Settings::InvertY)
		{
			y = -y;
		}
		auto Player = RE::PlayerCharacter::GetSingleton();
		bool Mirrored = DirectionHandler::GetSingleton()->DetermineMirrored(Player);
		int32_t diff = InputSettings::MouseSens;
		int32_t diff2 = InputSettings::MouseSens;
		if (x > diff2 && y < -diff2)
		{
			if (!Mirrored)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TR);
			}
			else
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TL);
			}

		}
		else if (x > diff2 && y > diff2)
		{
			if (!Mirrored)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BR);
			}
			else
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BL);
			}
		}
		else if (x < -diff2 && y > diff2)
		{
			if (!Mirrored)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BL);
			}
			else
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BR);
			}
		}
		else if (x < -diff2 && y < -diff2)
		{
			if (!Mirrored)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TL);
			}
			else
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TR);
			}
		}
		else if (x > diff)
		{
			if (!Mirrored)
			{
				DirectionHandler::GetSingleton()->SwitchDirectionRight(Player);
			}
			else
			{
				DirectionHandler::GetSingleton()->SwitchDirectionLeft(Player);
			}

		}
		else if (x < -diff)
		{
			if (!Mirrored)
			{
				DirectionHandler::GetSingleton()->SwitchDirectionLeft(Player);
			}
			else
			{
				DirectionHandler::GetSingleton()->SwitchDirectionRight(Player);
			}

		}
		else if (y < -diff)
		{
			DirectionHandler::GetSingleton()->SwitchDirectionUp(Player);
		}
		else if (y > diff)
		{
			DirectionHandler::GetSingleton()->SwitchDirectionDown(Player);
		}
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

		// try to prevent power attacks, however this is not guaranteed to be populated
		if (actor->GetActorRuntimeData().currentProcess->high->attackData)
		{
			if (actor->GetActorRuntimeData().currentProcess->high->attackData.get()->data.flags.any(
				RE::AttackData::AttackFlag::kPowerAttack))
			{
				return false;
			}
		}

		if (!actor)
		{
			return _PerformAttackAction(a_actionData);
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
				return false;
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
			if (!a_attacker->IsBlocking())
			{
				if (DirectionHandler::GetSingleton()->HasBlockAngle(a_attacker, a_target))
				{
					// Because of the above issue, we're going to RNG this for now
					int val = std::rand() % 3;
					if (val == 0)
					{
						DirectionHandler::GetSingleton()->SwitchToNewDirection(a_attacker, a_target);
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
			
		}
		return _ProcessAttackHook(handler, a_event, a_data);
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
				actor->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, actor->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kStamina) * -StaminaPowerTable[repeatCombos]);

			}

		}

		// hijack this as started attack as well
		else if (str == "TryChamberBegin"_h)
		{
			AttackHandler::GetSingleton()->AddChamberWindow(actor);
			DirectionHandler::GetSingleton()->EndedAttackWindow(actor);

		}
		else if (str == "MCO_WinOpen"_h)
		{
			// make sure we remove
			// this is the only place in the code that both adds and removes to this set
			// so we need to make sure it doesn't leak (unlikely as there would be a cell transition between an attack, but definitely possible)
			DirectionHandler::GetSingleton()->StartedAttackWindow(actor);
		}
		else if (str == "MCO_WinClose"_h)
		{
			DirectionHandler::GetSingleton()->EndedAttackWindow(actor);
		}
		// backup to ensure that we exit the attack window
		else if (str == "attackStop"_h)
		{
			DirectionHandler::GetSingleton()->EndedAttackWindow(actor);
		}

		// feint annotations
		else if (str == "FeintToTR"_h)
		{ 
			//DirectionHandler::GetSingleton()->SwitchDirectionSynchronous(actor, Directions::TR);
		}
		else if (str == "FeintToTL"_h)
		{
			//DirectionHandler::GetSingleton()->SwitchDirectionSynchronous(actor, Directions::TL);
		}
		else if (str == "FeintToBL"_h)
		{
			//DirectionHandler::GetSingleton()->SwitchDirectionSynchronous(actor, Directions::BL);
		}
		else if (str == "FeintToBR"_h)
		{
			//DirectionHandler::GetSingleton()->SwitchDirectionSynchronous(actor, Directions::BR);
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
}


