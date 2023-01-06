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
	TR = DataHandler->LookupForm<RE::BGSPerk>(0x800, PluginName);
	TL = DataHandler->LookupForm<RE::BGSPerk>(0x801, PluginName);
	BL = DataHandler->LookupForm<RE::BGSPerk>(0x802, PluginName);
	BR = DataHandler->LookupForm<RE::BGSPerk>(0x803, PluginName);
	Debuff = DataHandler->LookupForm<RE::BGSPerk>(0x810, PluginName);
	Unblockable = DataHandler->LookupForm<RE::BGSPerk>(0x80A, PluginName);
	NPCKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x13794, "Skyrim.esm");
	BattleaxeKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x6D932, "Skyrim.esm");
	PikeKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x0E457E, "NewArmoury.esp");
	logger::info("DirectionHandler Initialized");
 }

bool DirectionHandler::HasDirectionalPerks(RE::Actor* actor) const
{
	return actor->HasPerk(TR) 
		|| actor->HasPerk(TL) 
		|| actor->HasPerk(BL) 
		|| actor->HasPerk(BR);
}

bool DirectionHandler::HasBlockAngle(RE::Actor* attacker, RE::Actor* target)
{
	// never can block this
	if (attacker->HasPerk(Unblockable))
	{
		return false;
	}
	// opposite side angle
	if (attacker->HasPerk(TR)) 
	{
		return target->HasPerk(TL);
	}
	if (attacker->HasPerk(TL))
	{
		return target->HasPerk(TR);
	}
	if (attacker->HasPerk(BL))
	{
		return target->HasPerk(BR);
	}
	if (attacker->HasPerk(BR))
	{
		return target->HasPerk(BL);
	}

	// if no perk then any angle blocks
	return true;
}

void DirectionHandler::UIDrawAngles(RE::Actor* actor)
{
	RE::BGSPerk* Perk = GetDirectionalPerk(actor);
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
		if (actor->HasPerk(Unblockable))
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


RE::BGSPerk* DirectionHandler::DirectionToPerk(Directions dir) const
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

RE::BGSPerk* DirectionHandler::GetDirectionalPerk(RE::Actor* actor) const
{
	
	if (actor->HasPerk(Unblockable))
	{
		return Unblockable;
	}
	if (actor->HasPerk(TR))
	{
		return TR;
	}
	if (actor->HasPerk(TL))
	{
		return TL;
	}
	if (actor->HasPerk(BL))
	{
		return BL;
	}
	if (actor->HasPerk(BR))
	{
		return BR;
	}
	return nullptr;
}

Directions DirectionHandler::PerkToDirection(RE::BGSPerk* perk) const
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
	RemoveDirectionalPerks(actor);
	actor->GetActorBase()->AddPerk(DirectionToPerk(dir), 1);
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

void DirectionHandler::SwitchToNewDirection(RE::Actor* attacker, RE::Actor* target)
{
	// This will queue up this event if you cant switch instead

	Directions ToAvoid = PerkToDirection(GetDirectionalPerk(target));
	Directions ToSwitch = ToAvoid;
	int rand = std::rand() % 2;
	switch (ToAvoid)
	{
	case Directions::TR:
		if (rand == 0)
		{
			ToSwitch = Directions::TR;
		}
		else {
			ToSwitch = Directions::BL;
		}
		
		break;
	case Directions::TL:
		if (rand == 0)
		{
			ToSwitch = Directions::TL;
		}
		else 
		{
			ToSwitch = Directions::BR;
		}
		break;
	case Directions::BL:
		if (rand == 0)
		{
			ToSwitch = Directions::TR;
		}
		else
		{
			ToSwitch = Directions::BL;
		}
		break;
	case Directions::BR:
		if (rand == 0)
		{
			ToSwitch = Directions::TL;
		}
		else
		{
			ToSwitch = Directions::BR;
		}
		
		break;
	}
	WantToSwitchTo(attacker, ToSwitch, true);

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
		actor->GetActorBase()->AddPerk(TR, 1);
	}
	else if (weapon->HasKeyword(BattleaxeKeyword))
	{
		actor->GetActorBase()->AddPerk(BR, 1);
	}
	else if (PikeKeyword && weapon->HasKeyword(PikeKeyword))
	{
		actor->GetActorBase()->AddPerk(BR, 1);
	}
	else
	{
		actor->GetActorBase()->AddPerk(TR, 1);
	}
	

}

void DirectionHandler::RemoveDirectionalPerks(RE::Actor* actor)
{
	if (actor->HasPerk(TR))
	{
		actor->GetActorBase()->RemovePerk(TR);
	}
	if (actor->HasPerk(TL))
	{
		actor->GetActorBase()->RemovePerk(TL);
	}
	if (actor->HasPerk(BL))
	{
		actor->GetActorBase()->RemovePerk(BL);
	}
	if (actor->HasPerk(BR))
	{
		actor->GetActorBase()->RemovePerk(BR);
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
	UNUSED(delta);
	RE::WEAPON_STATE WeaponState = actor->AsActorState()->GetWeaponState();
	RE::ATTACK_STATE_ENUM AttackState = actor->AsActorState()->GetAttackState();
	auto Equipped = actor->GetEquippedObject(false);
	float SQDist = RE::PlayerCharacter::GetSingleton()->GetPosition().GetSquaredDistance(actor->GetPosition());
	// too far causes problems
	// square dist
	if (SQDist > Settings::ActiveDistance * Settings::ActiveDistance)
	{
		CleanupActor(actor);
		return;
	}
	// Only weapons (no H2H)
	if (!Equipped || (Equipped && !Equipped->IsWeapon()))
	{
		CleanupActor(actor);
		return;
	}
	// Non melee weapon
	RE::TESObjectWEAP* Weapon = nullptr;
	Weapon = Equipped->As<RE::TESObjectWEAP>();
	if (Weapon && !Weapon->IsMelee())
	{
		CleanupActor(actor);
		return;
	}

	if (WeaponState == RE::WEAPON_STATE::kDrawn && AttackState != RE::ATTACK_STATE_ENUM::kBowAttached)
	{
		if (!HasDirectionalPerks(actor))
		{
			AddDirectional(actor, Weapon);
			// temporary
			//actor->AsActorValueOwner()->SetBaseActorValue(RE::ActorValue::kWeaponSpeedMult, 0.1f);
			logger::info("gave {} perks", actor->GetName());
		}
		else 
		{
			UIDrawAngles(actor);
		}

	}
	else if (WeaponState == RE::WEAPON_STATE::kSheathed || AttackState == RE::ATTACK_STATE_ENUM::kBowAttached)
	{

		if (HasDirectionalPerks(actor))
		{
			RemoveDirectionalPerks(actor);
			// temporary
			//actor->AsActorValueOwner()->SetBaseActorValue(RE::ActorValue::kWeaponSpeedMult, 1.f);
			logger::info("remove {} perks", actor->GetName());
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
	if (actor->HasPerk(Unblockable))
	{
		actor->GetActorBase()->RemovePerk(Unblockable);
	}
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
			if (actor->HasPerk(Unblockable))
			{
				actor->GetActorBase()->RemovePerk(Unblockable);
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
	if (actor->HasPerk(Unblockable))
	{
		actor->GetActorBase()->RemovePerk(Unblockable);
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
	if (Iter != ComboDatas.end() && !actor->HasPerk(Unblockable))
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
			actor->GetActorBase()->AddPerk(Unblockable, 1);
			data.currentIdx = 0;
			data.size = 0;
			data.repeatCount = 0;
		}
		data.timeLeft = DifficultySettings::ComboResetTimer;
	}
}
