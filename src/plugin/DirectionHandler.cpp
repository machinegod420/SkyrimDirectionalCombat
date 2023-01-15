#include "DirectionHandler.h"
#include "AIHandler.h"
#include "SettingsLoader.h"
#include "AttackHandler.h"


// in seconds
// slow time should be a multiple as it affects animation events and we don't want to get stuck in the wrong idle
constexpr float TimeBetweenChanges = 0.12f;
constexpr float SlowTimeBetweenChanges = TimeBetweenChanges * 2.f;

void DirectionHandler::Initialize()
{
	RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
	TR = DataHandler->LookupForm<RE::SpellItem>(0x5370, PluginName);
	TL = DataHandler->LookupForm<RE::SpellItem>(0x5371, PluginName);
	BL = DataHandler->LookupForm<RE::SpellItem>(0x5372, PluginName);
	BR = DataHandler->LookupForm<RE::SpellItem>(0x5373, PluginName);
	Debuff = DataHandler->LookupForm<RE::BGSPerk>(0x810, PluginName);
	Unblockable = DataHandler->LookupForm<RE::SpellItem>(0x5374, PluginName);
	NPCKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm");
	BattleaxeKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x6D932, "Skyrim.esm");
	PikeKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x0E457E, "NewArmoury.esp");
	logger::info("DirectionHandler Initialized");
 }

bool DirectionHandler::HasDirectionalPerks(RE::Actor* actor) const
{
	return actor->HasSpell(TR) 
		|| actor->HasSpell(TL)
		|| actor->HasSpell(BL)
		|| actor->HasSpell(BR);
}

bool DirectionHandler::HasBlockAngle(RE::Actor* attacker, RE::Actor* target)
{
	// never can block this
	if (attacker->HasSpell(Unblockable))
	{
		return false;
	}
	// opposite side angle
	if (attacker->HasSpell(TR))
	{
		return target->HasSpell(TL);
	}
	if (attacker->HasSpell(TL))
	{
		return target->HasSpell(TR);
	}
	if (attacker->HasSpell(BL))
	{
		return target->HasSpell(BR);
	}
	if (attacker->HasSpell(BR))
	{
		return target->HasSpell(BL);
	}

	// if no Spell then any angle blocks
	return true;
}

void DirectionHandler::UIDrawAngles(RE::Actor* actor)
{
	if (!UISettings::ShowUI)
	{
		return;
	}
	RE::SpellItem* Perk = GetDirectionalPerk(actor);
	if (Perk != nullptr)
	{
		if (!actor->IsPlayerRef())
		{
			// ignore ui past setting distance
			float TargetDist = RE::PlayerCamera::GetSingleton()->pos.GetSquaredDistance(actor->GetPosition());
			if (TargetDist > (UISettings::DisplayDistance * UISettings::DisplayDistance))
			{
				return;
			}
		}


		UIDirectionState state = UIDirectionState::Default;
		UIHostileState hostileState = UIHostileState::Neutral;
		if (actor->IsBlocking())
		{
			state = UIDirectionState::Blocking;
		}
		if (actor->IsAttacking())
		{
			state = UIDirectionState::Attacking;
		}
		if (actor->HasSpell(Unblockable))
		{
			state = UIDirectionState::Unblockable;
		}

		if (actor->IsPlayerRef())
		{
			hostileState = UIHostileState::Player;
		}
		else if (actor->IsPlayerTeammate())
		{
			hostileState = UIHostileState::Friendly;
		}
		else if (actor->IsHostileToActor(RE::PlayerCharacter::GetSingleton()))
		{
			hostileState = UIHostileState::Hostile;
		}
		bool FirstPerson = (actor->IsPlayerRef() && RE::PlayerCamera::GetSingleton()->IsInFirstPerson());
		RE::NiPoint3 Position = actor->GetPosition() + RE::NiPoint3(0, 0, 90);
		
		if (!FirstPerson)
		{
			// this is probably more expensive than needed
			RE::BGSBodyPart* bodyPart = actor->GetRace()->bodyPartData->parts[RE::BGSBodyPartDefs::LIMB_ENUM::kTorso];
			if (bodyPart)
			{
				Position = actor->GetNodeByName(bodyPart->targetName)->world.translate;
			}
		}
		// only mirror if character is facing the camera
		bool Mirror = DetermineMirrored(actor);

		// handles player as well
		bool Lockout = !AttackHandler::GetSingleton()->CanAttack(actor);

		// since this waits for a mutex we add a task graph command for it
		SKSE::GetTaskInterface()->AddTask([=] {
			UIMenu::AddDrawCommand(Position, PerkToDirection(Perk), Mirror, state, hostileState, FirstPerson, Lockout);
		});
		
	}

}

bool DirectionHandler::DetermineMirrored(RE::Actor* actor)
{
	// don't do anything funky in first person
	bool FirstPerson = (actor->IsPlayerRef() && RE::PlayerCamera::GetSingleton()->IsInFirstPerson());
	if (!FirstPerson)
	{
		// god dam this is broken?
		float headingAngle = actor->GetHeadingAngle(RE::PlayerCamera::GetSingleton()->pos, true);
		// actor has to turn less than 90 degrees in 1 direction so they are facing the camera

		// The output of this function is apparently sometimes wrong.	
		if (headingAngle < 110)
		{
			//return true;
		}

		// For now, we just show it as always mirrored if its not the player
		return !actor->IsPlayerRef();
	}
	return false;
}


RE::SpellItem* DirectionHandler::DirectionToPerk(Directions dir) const
{
	switch (dir)
	{
	case Directions::TR:
		return TR;
	case Directions::TL:
		return TL;
	case Directions::BL:
		return BL;
	case Directions::BR:
		return BR;
	}

	return nullptr;
}

RE::SpellItem* DirectionHandler::GetDirectionalPerk(RE::Actor* actor) const
{
	
	if (actor->HasSpell(Unblockable))
	{
		return Unblockable;
	}
	if (actor->HasSpell(TR))
	{
		return TR;
	}
	if (actor->HasSpell(TL))
	{
		return TL;
	}
	if (actor->HasSpell(BL))
	{
		return BL;
	}
	if (actor->HasSpell(BR))
	{
		return BR;
	}
	return nullptr;
}

Directions DirectionHandler::PerkToDirection(RE::SpellItem* perk) const
{
	if (perk == TR)
	{
		return Directions::TR;
	}
	else if (perk == TL)
	{
		return Directions::TL;
	}
	else if (perk == BL) 
	{
		return Directions::BL;
	}
	else if (perk == BR)
	{
		return Directions::BR;
	}
	else
	{
		return Directions::Unblockable;
	}

}

bool DirectionHandler::CanSwitch(RE::Actor* actor)
{
	return !(actor->IsAttacking() && !InAttackWin.contains(actor->GetHandle()));
}

void DirectionHandler::SwitchDirectionSynchronous(RE::Actor* actor, Directions dir)
{
	// ever since the perks were switched to spells, there has been some async problems where there can be a 
	// race condition that when removing all spells, the character update gets called and adds new spells before
	// the replacement can be added
	// it's likely that the removespell function is asynchronous, so we should make sure that there shouldn't be a point
	// where a spell does not exist on the actor. fortunately, the addspell()  function does return on success
	//RemoveDirectionalPerks(actor);
	RE::SpellItem* DirectionSpell = DirectionToPerk(dir);
	if (!actor->HasSpell(DirectionSpell))
	{
		if (actor->AddSpell(DirectionSpell))
		{
			if (dir != Directions::TR)
			{
				actor->RemoveSpell(TR);
			}
			if (dir != Directions::TL)
			{
				actor->RemoveSpell(TL);
			}
			if (dir != Directions::BL)
			{
				actor->RemoveSpell(BL);
			}
			if (dir != Directions::BR)
			{
				actor->RemoveSpell(BR);
			}
		}
		else 
		{
			logger::info("error: {} could not add spell", actor->GetName());
		}
	}
}

void DirectionHandler::SwitchDirectionLeft(RE::Actor* actor)
{

	if (GetDirectionalPerk(actor) == TR)
	{
		WantToSwitchTo(actor, Directions::TL);
	}
	else if (GetDirectionalPerk(actor) == BR)
	{
		WantToSwitchTo(actor, Directions::BL);
	}
}
void DirectionHandler::SwitchDirectionRight(RE::Actor* actor)
{

	if (GetDirectionalPerk(actor) == TL)
	{
		WantToSwitchTo(actor, Directions::TR);
	}
	else if (GetDirectionalPerk(actor) == BL)
	{
		WantToSwitchTo(actor, Directions::BR);
	}
}
void DirectionHandler::SwitchDirectionUp(RE::Actor* actor)
{

	if (GetDirectionalPerk(actor) == BR)
	{
		WantToSwitchTo(actor, Directions::TR);
	}
	else if (GetDirectionalPerk(actor) == BL)
	{
		WantToSwitchTo(actor, Directions::TL);
	}
}
void DirectionHandler::SwitchDirectionDown(RE::Actor* actor)
{

	if (GetDirectionalPerk(actor) == TR)
	{
		WantToSwitchTo(actor, Directions::BR);
	}
	else if (GetDirectionalPerk(actor) == TL)
	{
		WantToSwitchTo(actor, Directions::BL);
	}
}


void DirectionHandler::WantToSwitchTo(RE::Actor* actor, Directions dir, bool force)
{
	auto Iter = DirectionTimers.find(actor->GetHandle());
	// skip if we already have a direction to switch to that is the same
	if (Iter != DirectionTimers.end() && Iter->second.dir == dir)
	{
		return;
	}
	if (force || DirectionTimers.find(actor->GetHandle()) == DirectionTimers.end())
	{
		DirectionSwitch ToDir;
		ToDir.dir = dir;
		ToDir.timeLeft = TimeBetweenChanges;
		DirectionTimers[actor->GetHandle()] = ToDir;
	}

}

void DirectionHandler::AddDirectional(RE::Actor* actor, RE::TESObjectWEAP* weapon)
{
	// top right by default
	// battleaxes are thrusting polearms so they get BR
	if (!weapon)
	{
		actor->AddSpell(TR);
	}
	else if (weapon->HasKeyword(BattleaxeKeyword))
	{
		actor->AddSpell(BR);
	}
	else if (PikeKeyword && weapon->HasKeyword(PikeKeyword))
	{
		actor->AddSpell(BR);
	}
	else
	{
		actor->AddSpell(TR);
	}
	

}

void DirectionHandler::RemoveDirectionalPerks(RE::Actor* actor)
{
	if (actor->HasSpell(TR))
	{
		actor->RemoveSpell(TR);
	}
	if (actor->HasSpell(TL))
	{
		actor->RemoveSpell(TL);
	}
	if (actor->HasSpell(BL))
	{
		actor->RemoveSpell(BL);
	}
	if (actor->HasSpell(BR))
	{
		actor->RemoveSpell(BR);
	}
	if (actor->HasPerk(Debuff))
	{
		actor->RemovePerk(Debuff);
	}
	// do some cleanup here
	//AIHandler::GetSingleton()->RemoveActor(actor);
}

void DirectionHandler::UpdateCharacter(RE::Actor* actor, float delta)
{
	// There is an extremely serious bug here. Some, very specific NPCs in skyrim can flicker in between the
	// sheathed and drawn states while having their weapon drawn. There is no obvious reason why this happens.
	// This is almost certainly an issue with skyrim and there is no easy fix for this.

	// One fix could be to add perks on the Drawing and Sheathing states instead of Drawn and Sheathed.
	// This can lead to edge cases where if the Sheathed state doesn't get hit you will have perks or vise versa
	RE::WEAPON_STATE WeaponState = actor->AsActorState()->GetWeaponState();
	float SQDist = RE::PlayerCharacter::GetSingleton()->GetPosition().GetSquaredDistance(actor->GetPosition());
	// too far causes problems
	// square dist
	if (SQDist > Settings::ActiveDistance * Settings::ActiveDistance)
	{
		if (HasDirectionalPerks(actor))
		{
			logger::info("{} removed cause too far", actor->GetName());
			CleanupActor(actor);
		}
		return;
	}
	if (WeaponState != RE::WEAPON_STATE::kDrawn)
	{
		if (HasDirectionalPerks(actor))
		{
			logger::info("{} removed cause weapon is not drawn {}", actor->GetName(), (int)WeaponState);
			CleanupActor(actor);

		}
		return;
	}
	auto Equipped = actor->GetEquippedObject(false);
	auto EquippedLeft = actor->GetEquippedObject(true);
	// Only weapons (no H2H)
	// however, if race can attack then we force it anyway
	bool RaceCanFight = AIHandler::GetSingleton()->RaceForcedDirectionalCombat(actor);
	if (!RaceCanFight)
	{
		if (!Equipped || (Equipped && !Equipped->IsWeapon()))
		{
			if (!EquippedLeft || (EquippedLeft && !EquippedLeft->IsWeapon()))
			{
				if (HasDirectionalPerks(actor))
				{
					logger::info("{} removed cause of lack of weapon", actor->GetName());
					CleanupActor(actor);
				}
				return;
			} 
		}
	}

	// Non melee weapon
	RE::TESObjectWEAP* Weapon = nullptr;
	RE::TESObjectWEAP* WeaponLeft = nullptr;
	bool HasWeapon = RaceCanFight;
	if (Equipped)
	{
		Weapon = Equipped->As<RE::TESObjectWEAP>();
		if (Weapon && Weapon->IsMelee())
		{
			HasWeapon = true;
		}
	}
	if (EquippedLeft)
	{
		WeaponLeft = EquippedLeft->As<RE::TESObjectWEAP>();
		if (WeaponLeft && WeaponLeft->IsMelee())
		{
			HasWeapon = true;
		}
	}
	if (!HasWeapon)
	{
		if (HasDirectionalPerks(actor))
		{
			logger::info("{} removed cause wepaon is not melee", actor->GetName());
			CleanupActor(actor);

		}

		return;
	}
	if (WeaponState == RE::WEAPON_STATE::kDrawn)
	{
		if (!HasDirectionalPerks(actor))
		{
			AddDirectional(actor, Weapon);
			// temporary
			//actor->AsActorValueOwner()->SetBaseActorValue(RE::ActorValue::kWeaponSpeedMult, 0.1f);
			logger::info("gave {} {} perks", actor->GetName(), actor->GetHandle().native_handle());
		}
		else
		{
			UIDrawAngles(actor);
		}

	}


	// AI stuff
	if (!actor->IsPlayerRef() && HasDirectionalPerks(actor))
	{
		AIHandler::GetSingleton()->RunActor(actor, delta);
	}

}

void DirectionHandler::CleanupActor(RE::Actor* actor)
{
	RemoveDirectionalPerks(actor);
	if (actor->HasSpell(Unblockable))
	{
		actor->RemoveSpell(Unblockable);
	}
	DirectionTimers.erase(actor->GetHandle());
	AnimationTimer.erase(actor->GetHandle());
	ComboDatas.erase(actor->GetHandle());
	InAttackWin.erase(actor->GetHandle());
	AIHandler::GetSingleton()->RemoveActor(actor);
}

void DirectionHandler::Cleanup()
{
	DirectionTimers.clear();
	AnimationTimer.clear();
	ComboDatas.clear();
	InAttackWin.clear();
}

void DirectionHandler::Update(float delta)
{

	// prevent instant direction switches
	auto Iter = DirectionTimers.begin();
	while (Iter != DirectionTimers.end())
	{
		if (!Iter->first)
		{
			Iter = DirectionTimers.erase(Iter);
			continue;
		}
		RE::Actor* actor = Iter->first.get().get();
		if (!actor)
		{
			Iter = DirectionTimers.erase(Iter);
			continue;
		}

		Iter->second.timeLeft -= delta;
		if (Iter->second.timeLeft <= 0)
		{
			// make sure actor is not in a state that prevents it from switching directions, first
			// this will sit in queue until this happens
			if (CanSwitch(actor))
			{
				SwitchDirectionSynchronous(actor, Iter->second.dir);

				// We use a timer instead of below code in order to slow down the animation transitions (purely visual)
				// Since we want the animation time to be longer than the actual time to switch directions
				/*
				if (actor->IsBlocking())
				{
					actor->NotifyAnimationGraph("ForceBlockIdle");
				}
				else {
					actor->NotifyAnimationGraph("ForceIdleTest");
				}
				*/

				if (AnimationTimer.contains(Iter->first))
				{
					if (AnimationTimer[Iter->first].size() < 3)
					{
						AnimationTimer[Iter->first].push(SlowTimeBetweenChanges);
					}
				}
				else 
				{
					SendAnimationEvent(actor);
					AnimationTimer[Iter->first].push(SlowTimeBetweenChanges);
				}
				
				Iter = DirectionTimers.erase(Iter);
				continue;
			}

		}
		
		Iter++;
	}

	// slow down animations
	auto AnimIter = AnimationTimer.begin();
	while (AnimIter != AnimationTimer.end())
	{
		if (!AnimIter->first)
		{
			AnimIter = AnimationTimer.erase(AnimIter);
			continue;
		}
		RE::Actor* actor = AnimIter->first.get().get();
		if (!actor)
		{
			AnimIter = AnimationTimer.erase(AnimIter);
			continue;
		}

		AnimIter->second.front() -= delta;
		if (AnimIter->second.front() <= 0)
		{
			AnimIter->second.pop();
			if (AnimIter->second.empty())
			{
				AnimIter = AnimationTimer.erase(AnimIter);
				continue;
			}
			else 
			{
				SendAnimationEvent(actor);
			}
		}
		AnimIter++;
	}

	auto ComboIter = ComboDatas.begin();
	while (ComboIter != ComboDatas.end())
	{
		if (!ComboIter->first)
		{
			ComboIter = ComboDatas.erase(ComboIter);
			continue;
		}
		RE::Actor* actor = ComboIter->first.get().get();
		if (!actor)
		{
			ComboIter = ComboDatas.erase(ComboIter);
			continue;
		}

		ComboIter->second.timeLeft -= delta;
		if (ComboIter->second.timeLeft <= 0)
		{
			ComboData& data = ComboIter->second;
			data.size--;
			data.size = std::max(0, data.size);
			if (actor->HasSpell(Unblockable))
			{
				actor->RemoveSpell(Unblockable);
			}
			if (data.size == 0)
			{
				data.repeatCount = 0;
				data.currentIdx = 0;
			}
			else
			{
				data.repeatCount--;
				data.repeatCount = std::max(0, data.repeatCount);
				data.currentIdx--;
				if (data.currentIdx < 0)
				{
					data.currentIdx = 2;
				}
			}

		}

		ComboIter++;
	}



}

void DirectionHandler::SendAnimationEvent(RE::Actor* actor)
{
	// does this still need to be seperate animation events?
	if (actor->IsBlocking())
	{
		actor->NotifyAnimationGraph("ForceBlockIdle");
	}
	else
	{
		actor->NotifyAnimationGraph("ForceIdleTest");

	}
}

void DirectionHandler::DebuffActor(RE::Actor* actor)
{
	if (!actor->HasPerk(Debuff))
	{
		actor->AddPerk(Debuff);
	}
}

void DirectionHandler::AddCombo(RE::Actor* actor)
{
	if (actor->HasSpell(Unblockable))
	{
		actor->RemoveSpell(Unblockable);
		return;
	}
	// insert first if doesnt exist
	constexpr int comboSize = 2;
	if (!ComboDatas.contains(actor->GetHandle()))
	{
		ComboDatas[actor->GetHandle()].lastAttackDirs.resize(comboSize);
	}
	auto Iter = ComboDatas.find(actor->GetHandle());
	// wait for the unblockable attack to end before starting new combo
	if (Iter != ComboDatas.end() && !actor->HasSpell(Unblockable))
	{
		ComboData& data = Iter->second;
		// assume we have enough space for 3 as it should be prereserved
		data.lastAttackDirs[data.currentIdx] = PerkToDirection(GetDirectionalPerk(actor));
		if (data.size > 0)
		{
			int lastIdx = data.currentIdx - 1;
			if (lastIdx < 0)
			{
				lastIdx = comboSize - 1;
			}
			if (data.lastAttackDirs[data.currentIdx] == data.lastAttackDirs[lastIdx])
			{
				data.repeatCount++;
			}
			else 
			{
				//reset
				data.repeatCount = 0;
			}
		}
		data.currentIdx++;
		data.size++;
		// clamp
		data.size = std::min(data.size, comboSize);
		if (data.currentIdx > comboSize - 1)
		{
			data.currentIdx = 0;
		}

		// apply perk
		// clean up all combo stuff if we can apply it
		if (data.size >= 2 && data.repeatCount == 0)
		{
			actor->AddSpell(Unblockable);
			data.currentIdx = 0;
			data.size = 0;
			data.repeatCount = 0;
		}
		data.timeLeft = DifficultySettings::ComboResetTimer;
	}
}
