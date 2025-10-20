#include "DirectionHandler.h"
#include "AIHandler.h"
#include "SettingsLoader.h"
#include "AttackHandler.h"
#include "BlockHandler.h"

// in seconds
// slow time should be a multiple as it affects animation events and we don't want to get stuck in the wrong idle
// behavior hardcodes transition time as .22 so has to be later than that
constexpr float TimeBetweenChanges = 0.133f;
constexpr float BehaviorDefinedTime = 0.333f;
constexpr float BehaviorDefinedTimeSlow = 0.5f;
constexpr float BufferTime = 0.02f;
// We need to be careful about sending this event at the exact same time a transition has finished.
// This will cause flickering in the animation
constexpr float SlowTimeBetweenChanges = BehaviorDefinedTime + BufferTime;
constexpr float SlowTimeBetweenChanges2 = BehaviorDefinedTimeSlow + BufferTime;


void DirectionHandler::Initialize(TDM_API::IVTDM2* tdm)
{
	TDM = tdm;
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
	PikeKeyword2 = DataHandler->LookupForm<RE::BGSKeyword>(0x00080D, "MordhauWeaponsReplacer.esp");
	logger::info("DirectionHandler Initialized");
 }

bool DirectionHandler::HasDirectionalPerks(RE::Actor* actor) const
{
	bool ret = false;
	ActiveDirectionsMtx.lock_shared();
	ret = (ActiveDirections.contains(actor->GetHandle()));
	ActiveDirectionsMtx.unlock_shared();
	return ret;
}

bool DirectionHandler::HasBlockAngle(RE::Actor* attacker, RE::Actor* target) const
{
	// never can block this
	if (IsUnblockable(attacker))
	{
		return false;
	}

	if (HasFullShieldBlock(target))
	{
		return true;
	}
	std::shared_lock lock(ActiveDirectionsMtx);
	if (!ActiveDirections.contains(target->GetHandle()) || !ActiveDirections.contains(attacker->GetHandle()))
	{
		return false;
	}
	// opposite side angle

	if (Settings::ForHonorMode)
	{
		if (ActiveDirections.at(attacker->GetHandle()) == Directions::TR)
		{
			return ActiveDirections.at(target->GetHandle()) == Directions::TR;
		}
	}

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

		if(Settings::HasTDM && TDM)
		{
			if (UISettings::OnlyShowTargetted)
			{
				if (actor->GetHandle() != TDM->GetCurrentTarget())
				{
					return;
				}

			}
		}
		else if (UISettings::OnlyShowTargetted)
		{
			auto player = RE::PlayerCharacter::GetSingleton();
			if (!actor->IsHostileToActor(player))
			{
				return;

			}
			if (actor->GetActorRuntimeData().currentCombatTarget != player->GetHandle())
			{
				return;
			}
		}
	}

	ActiveDirectionsMtx.lock_shared();
	if(ActiveDirections.contains(actor->GetHandle()))
	{
		UIDirectionState state = UIDirectionState::Default;
		UIHostileState hostileState = UIHostileState::Neutral;
		bool bHasTimedParry = HasTimedParry(actor);
		bool bHasFullShieldBlock = HasFullShieldBlock(actor);
		if (actor->IsBlocking())
		{
			state = UIDirectionState::Blocking;
		}
		if (ImperfectParry.contains(actor->GetHandle()))
		{
			state = UIDirectionState::ImperfectBlock;
		}
		if (bHasTimedParry)
		{
			state = UIDirectionState::TimedBlock;
		}
		if (bHasFullShieldBlock)
		{
			state = UIDirectionState::FullBlock;
		}
		if (bHasFullShieldBlock && bHasTimedParry)
		{
			state = UIDirectionState::FullBlockAndTimedBlock;
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
		FirstPerson |= (actor->IsPlayerRef() && UISettings::Force1PHud);
		RE::NiPoint3 Position = actor->GetPosition() + RE::NiPoint3(0, 0, 90);
		
		if (!FirstPerson)
		{
			// this is probably more expensive than needed
			RE::BGSBodyPart* bodyPart = actor->GetRace()->bodyPartData->parts[RE::BGSBodyPartDefs::LIMB_ENUM::kTorso];
			if (bodyPart)
			{
				auto Node = actor->GetNodeByName(bodyPart->targetName);
				if (Node)
				{
					Position = Node->world.translate;
				}
				else
				{
					Position = actor->GetLookingAtLocation();
				}
			}
			actor->SetGraphVariableInt("DirModBodyBlendSelect", 0);
		}
		else
		{
			actor->SetGraphVariableInt("DirModBodyBlendSelect", 1);
		}
		// only mirror if character is facing the camera
		bool Mirror = DetermineMirrored(actor);

		// handles player as well
		bool Lockout = !AttackHandler::GetSingleton()->CanAttack(actor);

		// since this waits for a mutex we add a task graph command for it
		/*
		* //this was too slow for imgui swapchain
			SKSE::GetTaskInterface()->AddTask([=] {
			UIMenu::AddDrawCommand(Position, ActiveDirections.at(actor->GetHandle()), Mirror, state, hostileState, FirstPerson, Lockout);
		});	
		*/

		UI::AddDrawCommand(Position, ActiveDirections.at(actor->GetHandle()), Mirror, state, hostileState, FirstPerson, Lockout, actor->IsPlayerRef());
	}
	ActiveDirectionsMtx.unlock_shared();
}

bool DirectionHandler::DetermineMirrored(RE::Actor* actor)
{
	// don't do anything funky in first person
	if (actor->IsPlayerRef())
	{
		return false;
	}
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

	if (actor->AsActorState()->IsSprinting())
	{
		return false;
	}
	std::shared_lock lock(InAttackWinMtx);
	return !(actor->IsAttacking() && !InAttackWin.contains(actor->GetHandle()));
}

bool DirectionHandler::HasTimedParry(RE::Actor* actor) const
{
	std::shared_lock lock(TimedParryMtx);
	if (TimedParry.contains(actor->GetHandle()))
	{
		float TimeRemaining = TimedParry.at(actor->GetHandle());
		if (TimeRemaining > 0 && TimeRemaining < DifficultySettings::TimedBlockActiveTime)
		{
			return true;
		}
	}
	return false;
}

void DirectionHandler::AddTimedParry(RE::Actor* actor)
{
	std::unique_lock lock(TimedParryMtx);
	if (!TimedParry.contains(actor->GetHandle()))
	{
		TimedParry[actor->GetHandle()] = DifficultySettings::TimedBlockActiveTime + DifficultySettings::TimedBlockStartup;
	}
}

void DirectionHandler::SwitchDirectionSynchronous(RE::Actor* actor, Directions dir, bool wasBlocking)
{
	// ever since the perks were switched to spells, there has been some async problems where there can be a 
	// race condition that when removing all spells, the character update gets called and adds new spells before
	// the replacement can be added
	//RE::SpellItem* DirectionSpell = DirectionToPerk(dir);
	// This is a totally synchronous function, but will break since asynchronous spell adds will cause segfaults/race conditions
	//actor->GetActorRuntimeData().addedSpells.push_back(DirectionSpell);

	// This is why everything got switched to a map that this plugin maintains. Skyrim behavior does not support or is too unpredictable
	ActiveDirectionsMtx.lock();
	ActiveDirections[actor->GetHandle()] = dir;
	ActiveDirectionsMtx.unlock();

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
	if (actor->IsBlocking() && wasBlocking)
	{
		if (!HasFullShieldBlock(actor))
		{
			// set timed parry to cooldown
			TimedParryMtx.lock();
			if (TimedParry.contains(actor->GetHandle()))
			{
				TimedParry[actor->GetHandle()] = -0.001;
			}
			TimedParryMtx.unlock();
		}

		// Use imperfect parry only if we dont want switching guards to cost stamina
		if (Settings::SwitchingCostsStamina)
		{
			float ActorMaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
			float staminaCost = ActorMaxStamina * 0.1f;
			// shield switches direction for free past 80% stamina
			if (!HasFullShieldBlock(actor))
			{

				actor->AsActorValueOwner()->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kStamina, -staminaCost);
			}
			
		}
		else
		{
			ImperfectParryMtx.lock();
			ImperfectParry.insert(actor->GetHandle());
			ImperfectParryMtx.unlock();
		}

	}
	

}

void DirectionHandler::SwitchDirectionLeft(RE::Actor* actor, bool ChangeQueued)
{
	

	if (Settings::ForHonorMode)
	{
		WantToSwitchTo(actor, Directions::BL);
	}
	else
	{ 
		Directions WantDirection;
		bool IsQueued = HasQueuedDirection(actor, WantDirection);
		if (ChangeQueued && IsQueued)
		{
			if (WantDirection == Directions::TR)
			{
				WantToSwitchTo(actor, Directions::TL, true, false);
			}
			else if (WantDirection == Directions::BR)
			{
				WantToSwitchTo(actor, Directions::BL, true, false);
			}
		}
		else
		{
			Directions dir = GetCurrentDirection(actor);
			if (dir == Directions::TR)
			{
				WantToSwitchTo(actor, Directions::TL);
			}
			else if (dir == Directions::BR)
			{
				WantToSwitchTo(actor, Directions::BL);
			}
		}

		
	}

}
void DirectionHandler::SwitchDirectionRight(RE::Actor* actor, bool ChangeQueued)
{
	

	if (Settings::ForHonorMode)
	{
		WantToSwitchTo(actor, Directions::BR);
	}
	else
	{
		Directions WantDirection;
		bool IsQueued = HasQueuedDirection(actor, WantDirection);
		if (ChangeQueued && IsQueued)
		{

			if (WantDirection == Directions::TL)
			{
				WantToSwitchTo(actor, Directions::TR, true, false);
			}
			else if (WantDirection == Directions::BL)
			{
				WantToSwitchTo(actor, Directions::BR, true, false);
			}


		}
		else
		{
			Directions dir = GetCurrentDirection(actor);
			if (dir == Directions::TL)
			{
				WantToSwitchTo(actor, Directions::TR);
			}
			else if (dir == Directions::BL)
			{
				WantToSwitchTo(actor, Directions::BR);
			}
		}

	}
}
void DirectionHandler::SwitchDirectionUp(RE::Actor* actor, bool ChangeQueued)
{
	if (Settings::ForHonorMode)
	{
		WantToSwitchTo(actor, Directions::TR);
	}
	else
	{
		Directions WantDirection;
		bool IsQueued = HasQueuedDirection(actor, WantDirection);
		if (ChangeQueued && IsQueued)
		{
			if (WantDirection == Directions::BR)
			{
				WantToSwitchTo(actor, Directions::TR, true, false);
			}
			else if (WantDirection == Directions::BL)
			{
				WantToSwitchTo(actor, Directions::TL, true, false);
			}
		}
		else
		{
			Directions dir = GetCurrentDirection(actor);

			if (dir == Directions::BR)
			{
				WantToSwitchTo(actor, Directions::TR);
			}
			else if (dir == Directions::BL)
			{
				WantToSwitchTo(actor, Directions::TL);
			}
		}

	}

}
void DirectionHandler::SwitchDirectionDown(RE::Actor* actor, bool ChangeQueued)
{
	Directions WantDirection;
	bool IsQueued = HasQueuedDirection(actor, WantDirection);
	if (ChangeQueued && IsQueued)
	{
		if (WantDirection == Directions::TR)
		{
			WantToSwitchTo(actor, Directions::BR, true, false);
		}
		else if (WantDirection == Directions::TL)
		{
			WantToSwitchTo(actor, Directions::BL, true, false);
		}
		
	}
	else
	{
		Directions dir = GetCurrentDirection(actor);
		if (dir == Directions::TR)
		{
			WantToSwitchTo(actor, Directions::BR);
		}
		else if (dir == Directions::TL)
		{
			WantToSwitchTo(actor, Directions::BL);
		}
	}

}


void DirectionHandler::WantToSwitchTo(RE::Actor* actor, Directions dir, bool force, bool overwrite, bool lock)
{
	if (Settings::ForHonorMode)
	{
		if (dir == Directions::TL)
		{
			dir = Directions::TR;
		}
	}
	// skip if we try to switch to the same dir
	std::shared_lock lock1(ActiveDirectionsMtx);
	if (ActiveDirections.contains(actor->GetHandle()) && ActiveDirections.at(actor->GetHandle()) == dir)
	{
		return;
	}

	std::unique_lock lock2(DirectionTimersMtx);
	auto Iter = DirectionTimers.find(actor->GetHandle());
	// skip if we already have a direction to switch to that is the same
	if (Iter != DirectionTimers.end() && Iter->second.dir == dir)
	{
		Iter->second.locked = lock;
		return;
	}
	// skip if locked - very specific case
	if (Iter != DirectionTimers.end() && Iter->second.locked)
	{
		return;
	}
	float timeleft = TimeBetweenChanges;
	if (force && !overwrite && Iter != DirectionTimers.end())
	{
		// oddly specific change
		timeleft = Iter->second.timeLeft;
	}
	if (force || Iter == DirectionTimers.end())
	{
		DirectionSwitch ToDir;
		ToDir.dir = dir;
		ToDir.timeLeft = timeleft;
		ToDir.locked = lock;
		ToDir.wasBlocking = actor->IsBlocking();
		
		DirectionTimers[actor->GetHandle()] = ToDir;
	}
}

void DirectionHandler::AddDirectional(RE::Actor* actor, RE::TESObjectWEAP* weapon)
{
	// initialize for correctness
	actor->SetGraphVariableInt("DirModBodyBlendSelect", 0);
	// top right by default
	// battleaxes are thrusting polearms so they get BR
	ActiveDirectionsMtx.lock();
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
	else if (PikeKeyword2 && weapon->HasKeyword(PikeKeyword2))
	{
		ActiveDirections[actor->GetHandle()] = Directions::BR;
		actor->AddSpell(BR);
	}
	else
	{
		ActiveDirections[actor->GetHandle()] = Directions::TR;
		actor->AddSpell(TR);
	}
	ActiveDirectionsMtx.unlock();
	// hack to fix unblockable issues
	actor->RemoveSpell(Unblockable);
}

void DirectionHandler::AdjustActorScale(RE::Actor* actor)
{
	UNUSED(actor);

}

void DirectionHandler::RemoveDirectionalPerks(RE::ActorHandle handle)
{
	ActiveDirectionsMtx.lock();
	ActiveDirections.erase(handle);
	ActiveDirectionsMtx.unlock();

	RE::Actor* actor = handle.get().get();
	if (!actor)
	{
		return;
	}
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
	float FarDelta = Settings::ActiveDistance + 400.f;

	if (SQDist > FarDelta * FarDelta)
	{
		if (HasDirectionalPerks(actor))
		{
			logger::info("{} removed cause too far", actor->GetName());
			ToRemoveMtx.lock();
			ToRemove.insert(actor->GetHandle());
			ToRemoveMtx.unlock();
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
			ToRemoveMtx.lock();
			ToRemove.insert(actor->GetHandle());
			ToRemoveMtx.unlock();

		}
		return;
	}
	if (actor->IsDead())
	{
		if (HasDirectionalPerks(actor))
		{
			logger::info("{} removed cause dead", actor->GetName());
			ToRemoveMtx.lock();
			ToRemove.insert(actor->GetHandle());
			ToRemoveMtx.unlock();

		}
		return;
	}
	if (actor->IsMarkedForDeletion())
	{
		if (HasDirectionalPerks(actor))
		{
			logger::info("{} removed cause dead", actor->GetName());
			ToRemoveMtx.lock();
			ToRemove.insert(actor->GetHandle());
			ToRemoveMtx.unlock();

		}
		return;
	}

	if (actor->IsBlocking())
	{

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
					ToRemoveMtx.lock();
					ToRemove.insert(actor->GetHandle());
					ToRemoveMtx.unlock();
				}
				return;
			} 
		}
	}

	// check if non melee weapon
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
	
	if (!HasWeapon)
	{
		if (HasDirectionalPerks(actor))
		{
			logger::info("{} removed cause wepaon is not melee", actor->GetName());
			ToRemoveMtx.lock();
			ToRemove.insert(actor->GetHandle());
			ToRemoveMtx.unlock();
		}

		return;
	}
	//check h2h here
	if (Weapon && Weapon->IsHandToHandMelee())
	{
		if (!Settings::EnableForH2H)
		{
			if (HasDirectionalPerks(actor))
			{
				logger::info("{} removed cause wepaon is h2h", actor->GetName());
				ToRemoveMtx.lock();
				ToRemove.insert(actor->GetHandle());
				ToRemoveMtx.unlock();
			}
			return;
		}
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
			
			
			if (!actor->IsBlocking())
			{
				ImperfectParryMtx.lock();
				if (ImperfectParry.contains(actor->GetHandle()))
				{
					ImperfectParry.erase(actor->GetHandle());
				}
				ImperfectParryMtx.unlock();
			}
			//lock direction if sprinting
			// This should be the only time we use synchronous directoin change function
			// since during sprint cannot change directions
			if (actor->AsActorState()->IsSprinting())
			{
				if (PikeKeyword && Weapon && Weapon->HasKeyword(PikeKeyword))
				{
					if (GetCurrentDirection(actor) != Directions::BR)
					{
						SwitchDirectionSynchronous(actor, Directions::BR, false);
					}
				}
				else if (PikeKeyword2 && Weapon && Weapon->HasKeyword(PikeKeyword2))
				{
					if (GetCurrentDirection(actor) != Directions::BR)
					{
						SwitchDirectionSynchronous(actor, Directions::BR, false);
					}
				}
				else
				{
					if (GetCurrentDirection(actor) != Directions::TR)
					{
						SwitchDirectionSynchronous(actor, Directions::TR, false);
					}
				}


			}
		}
		


	}


	// AI stuff
	if (!actor->IsPlayerRef() && HasDirectionalPerks(actor))
	{
		AIHandler::GetSingleton()->RunActor(actor, delta);
	}

}

void DirectionHandler::CleanupActor(RE::ActorHandle actor)
{
	
	RemoveDirectionalPerks(actor);

	UnblockableActorsMtx.lock();
	UnblockableActors.erase(actor);
	UnblockableActorsMtx.unlock();

	DirectionTimersMtx.lock();
	DirectionTimers.erase(actor);
	DirectionTimersMtx.unlock();

	AnimationTimerMtx.lock();
	AnimationTimer.erase(actor);
	AnimationTimerMtx.unlock();

	ComboDatasMtx.lock();
	ComboDatas.erase(actor);
	ComboDatasMtx.unlock();

	InAttackWinMtx.lock();
	InAttackWin.erase(actor);
	InAttackWinMtx.unlock();

	ActiveDirectionsMtx.lock();
	ActiveDirections.erase(actor);
	ActiveDirectionsMtx.unlock();

	AIHandler::GetSingleton()->RemoveActor(actor);

	ImperfectParryMtx.lock();
	ImperfectParry.erase(actor);
	ImperfectParryMtx.unlock();

	BlockHandler::GetSingleton()->RemoveActor(actor);
	AttackHandler::GetSingleton()->RemoveActor(actor);
}

void DirectionHandler::Cleanup()
{
	DirectionTimersMtx.lock();
	DirectionTimers.clear();
	DirectionTimersMtx.unlock();

	AnimationTimerMtx.lock();
	AnimationTimer.clear();
	AnimationTimerMtx.unlock();

	ComboDatasMtx.lock();
	ComboDatas.clear();
	ComboDatasMtx.unlock();

	InAttackWinMtx.lock();
	InAttackWin.clear();
	InAttackWinMtx.unlock();

	ActiveDirectionsMtx.lock();
	ActiveDirections.clear();
	ActiveDirectionsMtx.unlock();

	UnblockableActorsMtx.lock();
	UnblockableActors.clear();
	UnblockableActorsMtx.unlock();

	ImperfectParryMtx.lock();
	ImperfectParry.clear();
	ImperfectParryMtx.unlock();
}

void DirectionHandler::QueueAnimationEvent(RE::Actor* actor)
{
	AnimationTimerMtx.lock();
	constexpr int MaxSize = 4;
	if (AnimationTimer.contains(actor->GetHandle()))
	{
		int size = AnimationTimer[actor->GetHandle()].size();
		
		if (size < MaxSize)
		{
			// Since the time between changes is greater than the transition time in the behavior, consecutive
			// adds to the queue will cause the delay between the sent event and the transition time to be
			// greater. For example, if there is 3 items in the queue, the second item will be sent at 0.72, while
			// the behavior transition will occur at 0.66, which is a 66ms delay. 
			if (!actor->IsBlocking() && !actor->IsAttacking())
			{
				AnimationTimer[actor->GetHandle()].push_back({ SlowTimeBetweenChanges2, true });
			}
			else
			{
				// blocking actors dont get the slow transition
				AnimationTimer[actor->GetHandle()].push_back({ SlowTimeBetweenChanges, false });
			}
		}
	}
	else
	{
		SendAnimationEvent(actor, false);
		AnimationTimer[actor->GetHandle()].resize(MaxSize);
		AnimationTimer[actor->GetHandle()].push_back({ SlowTimeBetweenChanges, false });
	}
	AnimationTimerMtx.unlock();
}

void DirectionHandler::ClearAnimationQueue(RE::Actor* actor)
{
	AnimationTimerMtx.lock();
	auto Iter = AnimationTimer.find(actor->GetHandle());
	if (Iter != AnimationTimer.end())
	{
		AnimationTimer.erase(Iter);
	}
	AnimationTimerMtx.unlock();
}

void DirectionHandler::Update(float delta)
{
	// synchronously remove to avoid any possible race conditions
	ToRemoveMtx.lock();
	for (auto handle : ToRemove)
	{
		CleanupActor(handle);
	}
	ToRemove.clear();
	ToRemoveMtx.unlock();

	{
		// prevent instant direction switches
		DirectionTimersMtx.lock();
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
					SwitchDirectionSynchronous(actor, Iter->second.dir, Iter->second.wasBlocking);

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

					QueueAnimationEvent(actor);

					Iter = DirectionTimers.erase(Iter);
					continue;
				}

			}

			Iter++;
		}
		DirectionTimersMtx.unlock();
	}

	{
		AnimationTimerMtx.lock();
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

			AnimIter->second.front().timeLeft -= delta;
			if (AnimIter->second.front().timeLeft <= 0)
			{
				// if the actor is in a state that prevents it from registering this animation event, queue up the event instead
				// this means no attacking and no sprinting
				if (!actor->IsAttacking() && !actor->AsActorState()->IsSprinting())
				{
					AnimIter->second.erase(AnimIter->second.begin());
					if (AnimIter->second.empty())
					{
						AnimIter = AnimationTimer.erase(AnimIter);
						continue;
					}
					else
					{
						SendAnimationEvent(actor, AnimIter->second.front().slow);
					}
				}

			}
			AnimIter++;
		}
		AnimationTimerMtx.unlock();
	}


	{
		ComboDatasMtx.lock();
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
		ComboDatasMtx.unlock();
	}

	{
		ActiveDirectionsMtx.lock();
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
		ActiveDirectionsMtx.unlock();
	}

	{
		TimedParryMtx.lock();
		auto ParryIter = TimedParry.begin();
		
		while (ParryIter != TimedParry.end())
		{
			if (!ParryIter->first)
			{
				ParryIter = TimedParry.erase(ParryIter);
				continue;
			}
			RE::Actor* actor = ParryIter->first.get().get();
			if (!actor)
			{
				ParryIter = TimedParry.erase(ParryIter);
				continue;
			}
			// only decrement if they are blocking if this has not started yet
			// basically if they get staggered before they can enter block stance
			// this is a very particular case
			// if they already started blocking and ticking and then get staggered, then we still tick
			// basically it is if the blocking input was made but the actor did not start blocking due to an attack or something else that caused them not to actually block 
			// the reason why this is a problem is because we have a cooldown period before you can attempt another timed block
			// the other way to do this is to have another map that just tracks the cooldown but that requires more mutexes etc
			
			if (actor->IsBlocking())
			{
				ParryIter->second -= delta;
			}
			else
			{
				if (ParryIter->second < DifficultySettings::TimedBlockActiveTime + DifficultySettings::TimedBlockStartup - 0.001)
				{
					ParryIter->second -= delta;
				}
			}
			
			if (ParryIter->second <= -DifficultySettings::TimedBlockCooldown)
			{
				ParryIter = TimedParry.erase(ParryIter);
				continue;
			}
			ParryIter++;
		}
		TimedParryMtx.unlock();
	}

}

void DirectionHandler::SendAnimationEvent(RE::Actor* actor, bool slow)
{
	// does this still need to be seperate animation events?
	if (actor->IsBlocking())
	{
		actor->NotifyAnimationGraph("ForceBlockIdle");
	}
	else
	{
		if (slow && !(actor->IsPlayerRef() && RE::PlayerCamera::GetSingleton()->IsInFirstPerson()))
		{
			actor->NotifyAnimationGraph("ForceIdleTestSlow");
		}
		else
		{
			actor->NotifyAnimationGraph("ForceIdleTest");
		}

	}
}

void DirectionHandler::DebuffActor(RE::Actor* actor)
{
	if (!actor->HasPerk(Debuff))
	{
		//actor->AddPerk(Debuff);
	}
}

void DirectionHandler::AddCombo(RE::Actor* actor)
{

	if (UnblockableActors.contains(actor->GetHandle()))
	{
		actor->RemoveSpell(Unblockable);
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
			actor->AddSpell(Unblockable);
			//actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)->CastSpellImmediate(Unblockable, false, actor, 0.f, false, 0.f, nullptr);
			data.currentIdx = 0;
			data.size = 0;
			data.repeatCount = 0;
		}
		data.timeLeft = DifficultySettings::ComboResetTimer;
	}
}
