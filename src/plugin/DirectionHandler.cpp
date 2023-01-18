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
	return (ActiveDirections.contains(actor->GetHandle()));
}

bool DirectionHandler::HasBlockAngle(RE::Actor* attacker, RE::Actor* target) const
{
	// never can block this
	if (IsUnblockable(attacker))
	{
		return false;
	}
	if (!ActiveDirections.contains(target->GetHandle()) || !ActiveDirections.contains(attacker->GetHandle()))
	{
		return false;
	}
	// opposite side angle
	if(ActiveDirections.at(attacker->GetHandle()) == Directions::TR)
	{
		return ActiveDirections.at(target->GetHandle()) == Directions::TL;
	}
	if (ActiveDirections.at(attacker->GetHandle()) == Directions::TL)
	{
		return ActiveDirections.at(target->GetHandle()) == Directions::TR;
	}
	if (ActiveDirections.at(attacker->GetHandle()) == Directions::BL)
	{
		return ActiveDirections.at(target->GetHandle()) == Directions::BR;
	}
	if (ActiveDirections.at(attacker->GetHandle()) == Directions::BR)
	{
		return ActiveDirections.at(target->GetHandle()) == Directions::BL;
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
	if (!actor->IsPlayerRef())
	{
		// ignore ui past setting distance
		float TargetDist = RE::PlayerCamera::GetSingleton()->pos.GetSquaredDistance(actor->GetPosition());
		if (TargetDist > (UISettings::DisplayDistance * UISettings::DisplayDistance))
		{
			return;
		}
	}
	if(ActiveDirections.contains(actor->GetHandle()))
	{
		UIDirectionState state = UIDirectionState::Default;
		UIHostileState hostileState = UIHostileState::Neutral;
		if (actor->IsBlocking())
		{
			state = UIDirectionState::Blocking;
		}
		if (ImperfectParry.contains(actor->GetHandle()))
		{
			state = UIDirectionState::ImperfectBlock;
		}
		if (actor->IsAttacking())
		{
			state = UIDirectionState::Attacking;
		}
		if (IsUnblockable(actor))
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
			UIMenu::AddDrawCommand(Position, ActiveDirections[actor->GetHandle()], Mirror, state, hostileState, FirstPerson, Lockout);
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
	else if (perk == Unblockable)
	{
		return Directions::Unblockable;
	}
	// wrong but graceful out
	return Directions::TR;
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
	//RE::SpellItem* DirectionSpell = DirectionToPerk(dir);
	// This is a totally synchronous function, but will break since asynchronous spell adds will cause segfaults/race conditions
	//actor->GetActorRuntimeData().addedSpells.push_back(DirectionSpell);

	// This is why everything got switched to a map that this plugin maintains. Skyrim behavior does not support or is too unpredictable
	ActiveDirections[actor->GetHandle()] = dir;

	RE::SpellItem* DirectionSpell = GetDirectionalPerk(actor);
	RE::SpellItem* SpellToAdd = DirectionToPerk(dir);
	if (!DirectionSpell)
	{
		actor->AddSpell(SpellToAdd);
	}

	if (PerkToDirection(DirectionSpell) != ActiveDirections[actor->GetHandle()])
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
		actor->AddSpell(SpellToAdd);
	}

	// if blocking they have an imperfect parry
	if (actor->IsBlocking())
	{
		ImperfectParry.insert(actor->GetHandle());
	}
	

}

void DirectionHandler::SwitchDirectionLeft(RE::Actor* actor)
{
	if (ActiveDirections.at(actor->GetHandle()) == Directions::TR)
	{
		WantToSwitchTo(actor, Directions::TL);
	}
	else if (ActiveDirections.at(actor->GetHandle()) == Directions::BR)
	{
		WantToSwitchTo(actor, Directions::BL);
	}
}
void DirectionHandler::SwitchDirectionRight(RE::Actor* actor)
{
	if (ActiveDirections.at(actor->GetHandle()) == Directions::TL)
	{
		WantToSwitchTo(actor, Directions::TR);
	}
	else if (ActiveDirections.at(actor->GetHandle()) == Directions::BL)
	{
		WantToSwitchTo(actor, Directions::BR);
	}
}
void DirectionHandler::SwitchDirectionUp(RE::Actor* actor)
{
	if (ActiveDirections.at(actor->GetHandle()) == Directions::BR)
	{
		WantToSwitchTo(actor, Directions::TR);
	}
	else if (ActiveDirections.at(actor->GetHandle()) == Directions::BL)
	{
		WantToSwitchTo(actor, Directions::TL);
	}
}
void DirectionHandler::SwitchDirectionDown(RE::Actor* actor)
{
	if (ActiveDirections.at(actor->GetHandle()) == Directions::TR)
	{
		WantToSwitchTo(actor, Directions::BR);
	}
	else if (ActiveDirections.at(actor->GetHandle()) == Directions::TL)
	{
		WantToSwitchTo(actor, Directions::BL);
	}
}


void DirectionHandler::WantToSwitchTo(RE::Actor* actor, Directions dir, bool force)
{
	if ((int)dir >= 4)
	{
		logger::info("{} had error switching {}", actor->GetName(), (int)dir);
	}
	// skip if we try to switch to the same dir
	if (ActiveDirections.contains(actor->GetHandle()) && ActiveDirections.at(actor->GetHandle()) == dir)
	{
		return;
	}
	auto Iter = DirectionTimers.find(actor->GetHandle());
	// skip if we already have a direction to switch to that is the same
	if (Iter != DirectionTimers.end() && Iter->second.dir == dir)
	{
		return;
	}
	if (force || Iter == DirectionTimers.end())
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
		ActiveDirections[actor->GetHandle()] = Directions::TR;
		actor->AddSpell(TR);
	}
	else if (weapon->HasKeyword(BattleaxeKeyword))
	{
		ActiveDirections[actor->GetHandle()] = Directions::BR;
		actor->AddSpell(BR);
	}
	else if (PikeKeyword && weapon->HasKeyword(PikeKeyword))
	{
		ActiveDirections[actor->GetHandle()] = Directions::BR;
		actor->AddSpell(BR);
	}
	else
	{
		ActiveDirections[actor->GetHandle()] = Directions::TR;
		actor->AddSpell(TR);
	}

}

void DirectionHandler::RemoveDirectionalPerks(RE::Actor* actor)
{
	ActiveDirections.erase(actor->GetHandle());
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
	RE::WEAPON_STATE WeaponState = actor->AsActorState()->GetWeaponState();
	float SQDist = RE::PlayerCharacter::GetSingleton()->GetPosition().GetSquaredDistance(actor->GetPosition());
	// too far causes problems
	// square dist
	float FarDelta = Settings::ActiveDistance + 200.f;

	if (SQDist > FarDelta * FarDelta)
	{
		if (HasDirectionalPerks(actor))
		{
			logger::info("{} removed cause too far", actor->GetName());
			CleanupActor(actor);
		}
		return;
	}
	else if (SQDist > Settings::ActiveDistance * Settings::ActiveDistance)
	{
		if (!HasDirectionalPerks(actor))
		{
			// don't do anything if we're not ready to be added yet
			return;
		}
	}
	if (WeaponState != RE::WEAPON_STATE::kDrawn)
	{
		if (HasDirectionalPerks(actor))
		{
			logger::info("{} removed cause weapon is not drawn", actor->GetName());
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
			if (!actor->IsBlocking() && ImperfectParry.contains(actor->GetHandle()))
			{
				ImperfectParry.erase(actor->GetHandle());
			}
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
	UnblockableActors.erase(actor->GetHandle());
	DirectionTimers.erase(actor->GetHandle());
	AnimationTimer.erase(actor->GetHandle());
	ComboDatas.erase(actor->GetHandle());
	InAttackWin.erase(actor->GetHandle());
	ActiveDirections.erase(actor->GetHandle());
	AIHandler::GetSingleton()->RemoveActor(actor);
	ImperfectParry.erase(actor->GetHandle());
}

void DirectionHandler::Cleanup()
{
	DirectionTimers.clear();
	AnimationTimer.clear();
	ComboDatas.clear();
	InAttackWin.clear();
	ActiveDirections.clear();
	UnblockableActors.clear();
	ImperfectParry.clear();
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
			if (UnblockableActors.contains(actor->GetHandle()))
			{
				UnblockableActors.erase(actor->GetHandle());
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

	auto DirIter = ActiveDirections.begin();
	while (DirIter != ActiveDirections.end())
	{
		if (!DirIter->first)
		{
			DirIter = ActiveDirections.erase(DirIter);
			continue;
		}
		RE::Actor* actor = DirIter->first.get().get();
		if (!actor)
		{
			DirIter = ActiveDirections.erase(DirIter);
			continue;
		}

		DirIter++;
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

	if (UnblockableActors.contains(actor->GetHandle()))
	{
		UnblockableActors.erase(actor->GetHandle());
		return;
	}
	// insert first if doesnt exist
	constexpr int comboSize = 2;
	if (!ComboDatas.contains(actor->GetHandle()))
	{
		ComboDatas[actor->GetHandle()].currentIdx = 0;
		ComboDatas[actor->GetHandle()].repeatCount = 0;
		ComboDatas[actor->GetHandle()].timeLeft = 0.f;
		ComboDatas[actor->GetHandle()].lastAttackDirs.resize(comboSize);
	}
	auto Iter = ComboDatas.find(actor->GetHandle());
	if (Iter != ComboDatas.end())
	{
		ComboData& data = Iter->second;
		// assume we have enough space for 3 as it should be prereserved
		data.lastAttackDirs[data.currentIdx] = ActiveDirections[actor->GetHandle()];
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
			UnblockableActors.insert(actor->GetHandle());
			data.currentIdx = 0;
			data.size = 0;
			data.repeatCount = 0;
		}
		data.timeLeft = DifficultySettings::ComboResetTimer;
	}
}
