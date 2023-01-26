#include "AIHandler.h"
#include "DirectionHandler.h"
#include "SettingsLoader.h"
#include "AttackHandler.h"

#include <random>

static std::mt19937 mt_rand(0);
static std::shared_mutex mt_randMtx;

int GetRand()
{
	std::unique_lock lock(mt_randMtx);
	return mt_rand();
}

constexpr int MaxDirs = 5;

// MUST be higher than the direction switch time otherwise it will try to switch directions too quickly and freeze
constexpr float LowestTime = 0.13f;
// have to be very careful with this number
constexpr float AIJitterRange = 0.04f;

void AIHandler::InitializeValues()
{
	// time in seconds between each update
	DifficultyUpdateTimer[Difficulty::VeryEasy] = AISettings::VeryEasyUpdateTimer;
	DifficultyUpdateTimer[Difficulty::Easy] = AISettings::EasyUpdateTimer;
	DifficultyUpdateTimer[Difficulty::Normal] = AISettings::NormalUpdateTimer;
	DifficultyUpdateTimer[Difficulty::Hard] = AISettings::HardUpdateTimer;
	DifficultyUpdateTimer[Difficulty::VeryHard] = AISettings::VeryHardUpdateTimer;
	DifficultyUpdateTimer[Difficulty::Legendary] = AISettings::LegendaryUpdateTimer;

	// time between each action
	DifficultyActionTimer[Difficulty::VeryEasy] = AISettings::VeryEasyActionTimer;
	DifficultyActionTimer[Difficulty::Easy] = AISettings::EasyActionTimer;
	DifficultyActionTimer[Difficulty::Normal] = AISettings::NormalActionTimer;
	DifficultyActionTimer[Difficulty::Hard] = AISettings::HardActionTimer;
	DifficultyActionTimer[Difficulty::VeryHard] = AISettings::VeryHardActionTimer;
	// peak human reaction time
	DifficultyActionTimer[Difficulty::Legendary] = AISettings::LegendaryActionTimer;


	logger::info("Difficulty Mult is {}", AISettings::AIDifficultyMult);
	 
	for (auto& iter : DifficultyUpdateTimer)
	{
		iter.second *= AISettings::AIDifficultyMult;
		iter.second = std::max(iter.second, LowestTime);
	}

	for (auto& iter : DifficultyActionTimer)
	{
		iter.second *= AISettings::AIDifficultyMult;
		// actions can be below the lowest time cause it doesnt cause weird direction change issues
		iter.second = std::max(iter.second, LowestTime * 0.5f);
	}
	logger::info("Finished reinitializing difficulty");

	if (!EnableRaceKeyword)
	{
		RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
		EnableRaceKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x800, "DirectionModRaces.esp");
		if (EnableRaceKeyword)
		{
			logger::info("Got race keyword");
		}

	}
}

void AIHandler::AddAction(RE::Actor* actor, Actions toDo, bool force)
{
	std::unique_lock lock(ActionQueueMtx);
	auto Iter = ActionQueue.find(actor->GetHandle());
	// don't do anything if we already have the same action queued
	if (Iter != ActionQueue.end() && Iter->second.toDo == toDo)
	{
		return;
	}
	// if no action or time has expired
	if (Iter == ActionQueue.end() || force || Iter->second.timeLeft <= 0.f || Iter->second.toDo == Actions::None)
	{
		Action action;
		action.timeLeft = CalcActionTimer(actor);
		action.toDo = toDo;

		ActionQueue[actor->GetHandle()] = action;
	}

}

bool AIHandler::CanAct(RE::Actor* actor) const
{
	std::shared_lock lock(UpdateTimerMtx);
	auto Iter = UpdateTimer.find(actor->GetHandle());
	if (Iter != UpdateTimer.end())
	{
		return Iter->second <= 0.f;
	}
	// if not exist, they can act
	return true;
}

void AIHandler::DidAttack(RE::Actor* actor)
{
	DifficultyMapMtx.lock();
	if (DifficultyMap.contains(actor->GetHandle()))
	{
		int idx = DifficultyMap.at(actor->GetHandle()).currentAttackIdx;
		idx++;

		if (idx > DifficultyMap.at(actor->GetHandle()).attackPattern.size())
		{
			idx = 0;
		}
		//logger::info("{} next attack is {} idx {}", actor->GetName(), (int)DifficultyMap[actor->GetHandle()].attackPattern[idx], idx);
		DifficultyMap[actor->GetHandle()].currentAttackIdx = idx;
	}
	DifficultyMapMtx.unlock();
}

void AIHandler::DidAct(RE::Actor* actor)
{
	// increment timer by difficulty
	UpdateTimerMtx.lock();
	UpdateTimer[actor->GetHandle()] = CalcUpdateTimer(actor);
	UpdateTimerMtx.unlock();
}

void AIHandler::RunActor(RE::Actor* actor, float delta)
{

	if (actor->GetActorRuntimeData().currentCombatTarget)
	{
		DirectionHandler* DirHandler = DirectionHandler::GetSingleton();
		RE::Actor* target = actor->GetActorRuntimeData().currentCombatTarget.get().get();
		if (target)
		{

			if (DirHandler->HasDirectionalPerks(target))
			{

				// Actions that occur outside of the normal tick (such as reactions) happen here
				float TargetDist = target->GetPosition().GetSquaredDistance(actor->GetPosition());
				DifficultyMapMtx.lock();
				if (TargetDist < 70000)
				{
					// keep track of target direction switches so we can try to snipe their opposing directions
					// thsi sits outside the usual action cycle
					if (DifficultyMap.contains(actor->GetHandle()))
					{
						if (!DifficultyMap[actor->GetHandle()].currentTarget)
						{
							DifficultyMap[actor->GetHandle()].currentTarget = target->GetHandle();
							DifficultyMap[actor->GetHandle()].targetLastDir = DirHandler->GetCurrentDirection(target);
							DifficultyMap[actor->GetHandle()].targetSwitchTimer = 0.f;

							logger::info("adding new targets");
						}
						else
						{
							if (DifficultyMap[actor->GetHandle()].targetLastDir == DirHandler->GetCurrentDirection(target))
							{
								DifficultyMap[actor->GetHandle()].targetSwitchTimer += delta;
								//logger::info("old angle from target{}", (int)DirHandler->PerkToDirection(perk));
							}
							else
							{
								DifficultyMap[actor->GetHandle()].targetSwitchTimer = 0.f;
								DifficultyMap[actor->GetHandle()].targetLastDir = DirHandler->GetCurrentDirection(target);
								//logger::info("new angle from target{}", (int)DirHandler->PerkToDirection(perk));
							}
						}
					}
				}

				// slower update tick to make AIs reasonable to fight
				if (CanAct(actor))
				{

					if (TargetDist < 70000)
					{
						// always attack if have perk
						if (DirHandler->IsUnblockable(actor))
						{
							TryAttack(actor);
						}

						if (actor->IsAttacking())
						{
							//SwitchToNewDirection(actor, actor);
							SwitchToNextAttack(actor);
						}
						else
						{

							if (target->IsAttacking())
							{
								//logger::info("NPC in range");
								AIHandler::GetSingleton()->AddAction(actor, AIHandler::Actions::Block);
							}

							// if they are blocking theyre probably not attacking
							bool ShouldDirectionMatch = !target->IsBlocking();
							// if the target takes too long in one guard, try switching
							if (DifficultyMap.contains(actor->GetHandle()) && DifficultyMap[actor->GetHandle()].targetSwitchTimer > AISettings::AIWaitTimer)
							{
								ShouldDirectionMatch = false;
								//logger::info("try to move to a new angle");
								// unless they are attacking and havent moved

							}
							if (target->IsAttacking())
							{
								//definitely direction match
								//AI shouldn't be too good at this though
								ShouldDirectionMatch = true;
							}
							// hard defensive if cant attack
							if (!AttackHandler::GetSingleton()->CanAttack(actor))
							{
								ShouldDirectionMatch = true;
							}
							if (!ShouldDirectionMatch)
							{
								SwitchToNewDirection(actor, target);
							}
							else
							{
								// direction match
								DirectionMatchTarget(actor, target, target->IsAttacking());
							}

						}

						
					}
					else 
					{
						//logger::info("NPC out of range");
						if (actor->IsBlocking())
						{
							actor->NotifyAnimationGraph("blockStop");
						}
						if (DifficultyMap.contains(actor->GetHandle()))
						{
							ReduceDifficulty(actor);
						}

						SwitchToNewDirection(actor, target);
					}

					DidAct(actor);
				}

				DifficultyMapMtx.unlock();
			}
		}
	}
}

void AIHandler::SwitchTarget(RE::Actor * actor, RE::Actor * newTarget)
{
	RE::Actor * currentTarget = actor->GetActorRuntimeData().currentCombatTarget.get().get();
	if (currentTarget)
	{
		if (newTarget->GetActorRuntimeData().currentCombatTarget == actor->GetHandle())
		{
			actor->GetActorRuntimeData().currentCombatTarget = newTarget->GetHandle();
		}
		else
		{
			float TargetDist = actor->GetPosition().GetSquaredDistance(currentTarget->GetPosition());
			if (TargetDist > 70000)
			{
				actor->GetActorRuntimeData().currentCombatTarget = newTarget->GetHandle();
			}
		}

	}
}


bool AIHandler::ShouldAttack(RE::Actor* actor, RE::Actor* target)
{
	std::unique_lock lock(DifficultyMapMtx);

	CalcAndInsertDifficulty(actor);


	if (DirectionHandler::GetSingleton()->GetCurrentDirection(actor) == 
		DifficultyMap[actor->GetHandle()].attackPattern[DifficultyMap[actor->GetHandle()].currentAttackIdx])
	{
		if (DirectionHandler::GetSingleton()->HasBlockAngle(actor, target))
		{
			// jitter this based on difficulty of target
			// and influence based on if the target is blocking or not

			int mod = (int)CalcAndInsertDifficulty(actor);
			if (target->IsBlocking())
			{
				mod += 2;
			}

			int val = mt_rand() % mod;

			if (val < 1)
			{
				return true;
			}
			return false;
		}
		return true;
	}
	// some RNG to attack anyways
	if (mt_rand() % 3 < 1 && !DirectionHandler::GetSingleton()->HasBlockAngle(actor, target))
	{
		return true;
	}
	SwitchToNextAttack(actor);

	return false;
}

void AIHandler::TryRiposte(RE::Actor* actor)
{
	std::unique_lock lock(DifficultyMapMtx);
	int mod = (int)CalcAndInsertDifficulty(actor);
	// 12 - 27 range
	// 6 - 13
	// .83 - .97
	mod += 3;
	mod *= 3;
	mod = (int)(mod * 0.5);
	int val = mt_rand() % mod;
	if (val > 0)
	{
		SwitchToNextAttack(actor);
		AddAction(actor, AIHandler::Actions::Riposte, true);
	}
}

void AIHandler::TryBlock(RE::Actor* actor, RE::Actor* attacker)
{
	std::unique_lock lock(DifficultyMapMtx);
	int mod = (int)CalcAndInsertDifficulty(actor);
	mod += 3;
	mod *= 4; 
	int val = mt_rand() % mod;
	if (val > 0)
	{
		// try direction match
		DirectionMatchTarget(actor, attacker, false);
		AddAction(actor, AIHandler::Actions::Block, true);
	}
}

// force should not be abused, as it can cause actions to never be finished because they are constantly be overwritten
void AIHandler::DirectionMatchTarget(RE::Actor* actor, RE::Actor* target, bool force)
{
	// direction match
	// really follow the last direction tracked then update it
	int mod = (int)CalcAndInsertDifficulty(actor);

	// follow last tracked direction isntead of the targets current direction
	// the AI is too easy to confuse with this though, since they can lag behind a bit
	Directions ToCounter = DifficultyMap[actor->GetHandle()].lastDirectionTracked;
	Directions CurrentTargetDir = DirectionHandler::GetSingleton()->GetCurrentDirection(target);

	// if we have to switch directions, then add a chance to make a mistkae
	// simulates conditioning the AI to block in certain directions
	if (ToCounter != CurrentTargetDir)
	{
		int random = mt_rand() % 10;
		if (random > 0)
		{
			random = mt_rand() % 100;
			// add some RNG to mix it up
			// have a chance to switch to a direction they anticipate instead
			
			int TR = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR];
			int TL = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL];
			int BL = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL];
			int BR = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR];
			bool found = false;
			random -= TR;
			if (!found && random <= 0)
			{
				ToCounter = Directions::TR;
				found = true;
			}
			random -= TL;
			if (!found && random <= 0)
			{
				ToCounter = Directions::TL;
				found = true;
			}
			random -= BL;
			if (!found && random <= 0)
			{
				ToCounter = Directions::BL;
				found = true;
			}
			random -= BR;
			if (!found && random <= 0)
			{
				ToCounter = Directions::BR;
				found = true;
			}

			if (found)
			{
				CurrentTargetDir = ToCounter;
				TR -= 5;
				TR = std::max(0, TR);
				TL -= 5;
				TL = std::max(0, TL);
				BL -= 5;
				BL = std::max(0, BL);
				BR -= 5;
				BR = std::max(0, BR);
				DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR] = TR;
				DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL] = TL;
				DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL] = BL;
				DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR] = BR;
			}

		}
	}


	// small chance to correctly choose as well
	int random = mt_rand() % 100;
	if (random < mod)
	{
		ToCounter = DirectionHandler::GetSingleton()->GetCurrentDirection(target);
	}

	Directions ToSwitch = Directions::TR;
	switch (ToCounter)
	{
	case Directions::TR:
		ToSwitch = Directions::TL;
		break;
	case Directions::TL:
		ToSwitch = Directions::TR;
		break;
	case Directions::BR:
		ToSwitch = Directions::BL;
		break;
	case Directions::BL:
		ToSwitch = Directions::BR;
		break;
	}
	DifficultyMap[actor->GetHandle()].lastDirectionTracked = CurrentTargetDir;
	DirectionHandler::GetSingleton()->WantToSwitchTo(actor, ToSwitch, force);
}

void AIHandler::SwitchToNewDirection(RE::Actor* actor, RE::Actor* target)
{
	// This will queue up this event if you cant switch instead

	Directions ToAvoid = DirectionHandler::GetSingleton()->GetCurrentDirection(target);
	Directions ToSwitch = ToAvoid;
	DirectionHandler::GetSingleton()->WantToSwitchTo(actor, ToSwitch, true);
}

void AIHandler::ReduceDifficulty(RE::Actor* actor)
{
	if (DifficultyMap.contains(actor->GetHandle()))
	{
		int TR = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR];
		int TL = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL];
		int BL = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL];
		int BR = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR];
		TR -= 5;
		TR = std::max(0, TR);
		TL -= 5;
		TL = std::max(0, TL);
		BL -= 5;
		BL = std::max(0, BL);
		BR -= 5;
		BR = std::max(0, BR);
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR] = TR;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL] = TL;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL] = BL;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR] = BR;
		if (DifficultyMap[actor->GetHandle()].mistakeRatio <= 0.01f && DifficultyMap[actor->GetHandle()].mistakeRatio >= -0.01f)
		{
			DifficultyMap[actor->GetHandle()].mistakeRatio = 0.f;
		}
		else
		{
			DifficultyMap[actor->GetHandle()].mistakeRatio *= 0.5f;
		}
	}

}

void AIHandler::TryAttack(RE::Actor* actor)
{
	// since this is a forced attack, it happens outside of the normal AI attack loop so we need to add checks here as well
	if (AttackHandler::GetSingleton()->CanAttack(actor))
	{
		// load attack data into actor to ensure attacks register correctly
		LoadCachedAttack(actor);
		// set actor state to show that actor is now attacking
		actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kSwing;
		actor->NotifyAnimationGraph("attackStart");
	}
}

void AIHandler::SwitchToNextAttack(RE::Actor* actor)
{
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		CalcAndInsertDifficulty(actor);
	}
	int idx = DifficultyMap[actor->GetHandle()].currentAttackIdx;
	if (idx < DifficultyMap[actor->GetHandle()].attackPattern.size())
	{
		Directions dir = DifficultyMap[actor->GetHandle()].attackPattern[idx];
		if ((int)dir > 3)
		{
			logger::info("{} had error in attack pattern to {}", actor->GetName(), (int)dir);
		}
		DirectionHandler::GetSingleton()->WantToSwitchTo(actor, dir, true);
	}
	else
	{
		DifficultyMap[actor->GetHandle()].currentAttackIdx = 0;
	}
}


void AIHandler::LoadCachedAttack(RE::Actor* actor)
{
	if (!actor->GetActorRuntimeData().currentProcess->high->attackData)
	{
		// seems strange at this point that difficulty is not already created
		CalcAndInsertDifficulty(actor);
		actor->GetActorRuntimeData().currentProcess->high->attackData =
			DifficultyMap[actor->GetHandle()].cachedBasicAttackData;

	}
}

AIHandler::Difficulty AIHandler::CalcAndInsertDifficulty(RE::Actor* actor)
{
	// Important - do not insert into map until we have initialized difficulty first so we properly cache the result
	auto Iter = DifficultyMap.find(actor->GetHandle());
	
	Difficulty ret = Difficulty::Uninitialized;
	if (Iter != DifficultyMap.end())
	{
		return Iter->second.difficulty;
	}
	else 
	{
		// difficulty is only a factor of player level
		uint16_t MyLvl = actor->GetLevel();
		uint16_t PlayerLvl = RE::PlayerCharacter::GetSingleton()->GetLevel();
		int result = MyLvl - PlayerLvl;
		if (result < AISettings::VeryEasyLvl)
		{
			ret = Difficulty::VeryEasy;
		}
		else if (result < AISettings::EasyLvl) 
		{
			ret = Difficulty::Easy;
		}
		else if (result < AISettings::NormalLvl) 
		{
			ret = Difficulty::Normal;
		} 
		else if (result < AISettings::HardLvl)
		{
			ret = Difficulty::Hard;
		}
		else if (result < AISettings::VeryHardLvl)
		{
			ret = Difficulty::VeryHard;
		}
		else 
		{
			ret = Difficulty::Legendary;
		}
		// if race is forced to have directional combat, then we cap its difficulty
		if (RaceForcedDirectionalCombat(actor))
		{
			if (ret > Difficulty::Normal)
			{
				ret = Difficulty::Normal;
			}
			
		}
		logger::info("{} got difficulty level {}", actor->GetName(), (int)ret);
		AIDifficulty aidiff = { ret, 0.f };
		DifficultyMap[actor->GetHandle()] = aidiff;
		DifficultyMap[actor->GetHandle()].lastDirectionsEncountered.reserve(MaxDirs);

		// iterate until we found the attackstart
		
		auto AttackData = FindActorAttackData(actor);
		//default
		DifficultyMap[actor->GetHandle()].lastDirectionTracked = Directions::TR;
		if (AttackData)
		{
			logger::info("{} has basic attack data", actor->GetName());
			DifficultyMap[actor->GetHandle()].cachedBasicAttackData = AttackData;
		}

		// generate attack patterns ahead of time
		// instead of using rand, use their native handle as that is unique per actor and is the same between saves
		// so you can learn after dying
		uint32_t handle = actor->GetHandle().native_handle();
		unsigned size = handle % 3 + 2;
		for (unsigned i = 0; i < size; ++i)
		{
			//logger::info("{} has attack pattern {}", actor->GetName(), size);
			// gross
			// grab the least significant 2 bits, cast to direction, then shift
			unsigned newDir = (handle & 0x3u);
			handle >>= 2;
			if (newDir >= 4)
			{
				logger::info("{} error bitshifting", actor->GetName());
				newDir = 3;
			}
			DifficultyMap[actor->GetHandle()].attackPattern.push_back((Directions)newDir);
		}
		DifficultyMap[actor->GetHandle()].currentAttackIdx = 0;
	}

	return ret;
}

void AIHandler::SignalBadThing(RE::Actor* actor, Directions attackDir)
{
	DifficultyMapMtx.lock();
	// this will populate the map
	CalcAndInsertDifficulty(actor);

	auto Iter = DifficultyMap.find(actor->GetHandle());
	Iter->second.lastDirectionsEncountered.push_back(attackDir);
	// update what the last direction was cause we just got hit
	Iter->second.lastDirectionTracked = attackDir;
	size_t Num = Iter->second.lastDirectionsEncountered.size();
	if (Num > MaxDirs)
	{
		Iter->second.lastDirectionsEncountered.erase(Iter->second.lastDirectionsEncountered.begin());
	}

	// increase percentage of blocking this location
	IncreaseBlockChance(actor, attackDir, mt_rand() % 30 + 10, 2);

	float NewMistakeRatio = DifficultyMap[actor->GetHandle()].mistakeRatio - (AISettings::AIGrowthFactor);
	NewMistakeRatio = std::min(NewMistakeRatio, 0.16f);
	NewMistakeRatio = std::max(NewMistakeRatio, -0.16f);
	DifficultyMap[actor->GetHandle()].mistakeRatio = NewMistakeRatio;
	DifficultyMapMtx.unlock();
}

void AIHandler::SignalGoodThing(RE::Actor* actor, Directions attackedDir)
{
	DifficultyMapMtx.lock();
	// this will populate the map
	CalcAndInsertDifficulty(actor);

	auto Iter = DifficultyMap.find(actor->GetHandle());
	Iter->second.lastDirectionsEncountered.push_back(attackedDir);
	size_t Num = Iter->second.lastDirectionsEncountered.size();
	if (Num > MaxDirs)
	{
		Iter->second.lastDirectionsEncountered.erase(Iter->second.lastDirectionsEncountered.begin());
	}
	// if the player landed complex attack patterns then it gets easier
	phmap::parallel_flat_hash_set<Directions> dirs;
	for (auto dir : Iter->second.lastDirectionsEncountered)
	{
		dirs.insert(dir);
	}
	// increase percentage of blocking this location
	IncreaseBlockChance(actor, attackedDir, mt_rand() % 20 + 10, 3);
	

	unsigned size = std::max(1u, (unsigned)dirs.size());
	float NewMistakeRatio = DifficultyMap[actor->GetHandle()].mistakeRatio + (AISettings::AIGrowthFactor * size);
	NewMistakeRatio = std::min(NewMistakeRatio, 0.16f);
	NewMistakeRatio = std::max(NewMistakeRatio, -0.16f);
	DifficultyMap[actor->GetHandle()].mistakeRatio = NewMistakeRatio;
	DifficultyMapMtx.unlock();
}

void AIHandler::IncreaseBlockChance(RE::Actor* actor, Directions dir, int percent, int modifier)
{
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		// this will populate the map
		CalcAndInsertDifficulty(actor);
	}
	DifficultyMap[actor->GetHandle()].directionChangeChance[dir] += percent;
	DifficultyMap[actor->GetHandle()].directionChangeChance[dir] = std::min(90, DifficultyMap[actor->GetHandle()].directionChangeChance[dir]);
	switch (dir)
	{
	case Directions::TR:
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL] -= percent / modifier;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL] -= percent / modifier;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR] -= percent / modifier;
		break;
	case Directions::TL:
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR] -= percent / modifier;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL] -= percent / modifier;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR] -= percent / modifier;
		break;
	case Directions::BL:
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL] -= percent / modifier;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR] -= percent / modifier;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR] -= percent / modifier;
		break;
	case Directions::BR:
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL] -= percent / modifier;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL] -= percent / modifier;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR] -= percent / modifier;
		break;
	}
	DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR] = std::max(0, DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR]);
	DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL] = std::max(0, DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL]);
	DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL] = std::max(0, DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL]);
	DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR] = std::max(0, DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR]);
}

float AIHandler::CalcUpdateTimer(RE::Actor* actor)
{
	DifficultyUpdateTimerMtx.lock_shared();
	float base = DifficultyUpdateTimer.at(CalcAndInsertDifficulty(actor));
	DifficultyUpdateTimerMtx.unlock_shared();
	float mistakeRatio = DifficultyMap.at(actor->GetHandle()).mistakeRatio;
	base += mistakeRatio;
	// add jitter
	// so ugly
	//float result = (float)(mt_rand()) / ((float)(mt_rand.max() / (AIJitterRange * 2.f)));
	//result -= AIJitterRange;
	// don't break animation direction switching by letting AI flicker changes
	base = std::max(base, LowestTime);
	return base;
}

float AIHandler::CalcActionTimer(RE::Actor* actor)
{
	DifficultyActionTimerMtx.lock_shared();
	float base = DifficultyActionTimer.at(CalcAndInsertDifficulty(actor));
	DifficultyActionTimerMtx.unlock_shared();
	float mistakeRatio = DifficultyMap.at(actor->GetHandle()).mistakeRatio;
	//mistakeRatio *= 0.5;
	base += mistakeRatio;
	//float result = (float)(mt_rand()) / ((float)(mt_rand.max() / (AIJitterRange * 2.f)));
	//result -= AIJitterRange;
	
	base = std::max(base, LowestTime);

	return base;
}

RE::NiPointer<RE::BGSAttackData> AIHandler::FindActorAttackData(RE::Actor* actor)
{
	// iterate until we found the attackstart
	for (auto& iter : actor->GetRace()->attackDataMap->attackDataMap)
	{
		if (iter.first == "attackStart")
		{
			return iter.second;
		}
	}
	return nullptr;
}

void AIHandler::Cleanup()
{
	ActionQueueMtx.lock();
	ActionQueue.clear();
	ActionQueueMtx.unlock();

	UpdateTimerMtx.lock();
	UpdateTimer.clear();
	UpdateTimerMtx.unlock();

	DifficultyMapMtx.lock();
	DifficultyMap.clear();
	DifficultyMapMtx.unlock();
}


void AIHandler::Update(float delta)
{
	// two seperate actions to handle
	ActionQueueMtx.lock();
	auto ActionQueueIter = ActionQueue.begin();
	while (ActionQueueIter != ActionQueue.end())
	{
		if (!ActionQueueIter->first)
		{
			ActionQueueIter = ActionQueue.erase(ActionQueueIter);
			continue;
		}
		RE::Actor* actor = ActionQueueIter->first.get().get();
		if (!actor)
		{
			ActionQueueIter = ActionQueue.erase(ActionQueueIter);
			continue;
		}
		if (ActionQueueIter->second.timeLeft >= 0)
		{
			ActionQueueIter->second.timeLeft -= delta;
		}
		else
		{
			if (ActionQueueIter->second.toDo == Actions::Riposte)
			{ 
				TryAttack(actor);
			}
			else if (ActionQueueIter->second.toDo == Actions::Block)
			{ 
				actor->NotifyAnimationGraph("blockStart");
			}
			else if (ActionQueueIter->second.toDo == Actions::ProBlock)
			{
				actor->NotifyAnimationGraph("blockStart");
			}
			else if (ActionQueueIter->second.toDo == Actions::Bash)
			{
				actor->NotifyAnimationGraph("bashStart");
			}
			else if (ActionQueueIter->second.toDo == Actions::StartFeint)
			{
				TryAttack(actor);
				
			}
			else if (ActionQueueIter->second.toDo == Actions::EndFeint)
			{
				AttackHandler::GetSingleton()->HandleFeint(actor);
				// queue another attack
			}
			// once we have executed, the timeleft should be negative and this should be no action
			// this is what we use to determine if we are done with actions for this actor
			ActionQueueIter->second.toDo = Actions::None;
			// do not erase every time for perf reasons
			//ActionQueueIter = ActionQueue.erase(ActionQueueIter);
			//continue;

		}
		ActionQueueIter++;
	}
	ActionQueueMtx.unlock();

	UpdateTimerMtx.lock();
	// spread out AI actions to control difficulty
	auto UpdateTimerIter = UpdateTimer.begin();
	while (UpdateTimerIter != UpdateTimer.end())
	{
		if (!UpdateTimerIter->first)
		{
			UpdateTimerIter = UpdateTimer.erase(UpdateTimerIter);
			continue;
		}
		RE::Actor* actor = UpdateTimerIter->first.get().get();
		if (!actor)
		{
			UpdateTimerIter = UpdateTimer.erase(UpdateTimerIter);
			continue;
		}
		if (UpdateTimerIter->second >= 0)
		{
			// don't erase actually, since these AI can be acting a lot this will cause a lot of memory allocations
			//UpdateTimerIter = UpdateTimer.erase(UpdateTimerIter);
			//continue;

			UpdateTimerIter->second -= delta;
		}
		else
		{
			//UpdateTimerIter = UpdateTimer.erase(UpdateTimerIter);
			//continue;
		}
		UpdateTimerIter++;
	}
	UpdateTimerMtx.unlock();
}


