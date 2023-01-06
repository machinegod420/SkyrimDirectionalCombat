#include "AIHandler.h"
#include "DirectionHandler.h"
#include "SettingsLoader.h"
#include "AttackHandler.h"

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
}

void AIHandler::AddAction(RE::Actor* actor, Actions toDo, bool force)
{
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
	auto Iter = UpdateTimer.find(actor->GetHandle());
	if (Iter != UpdateTimer.end())
	{
		return Iter->second <= 0.f;
	}
	// if not exist, they can act
	return true;
}

void AIHandler::DidAct(RE::Actor* actor)
{
	// increment timer by difficulty
	UpdateTimer[actor->GetHandle()] = CalcUpdateTimer(actor);
}

void AIHandler::RunActor(RE::Actor* actor, float delta)
{

	if (actor->GetActorRuntimeData().currentCombatTarget)
	{
		DirectionHandler* DirHandler = DirectionHandler::GetSingleton();
		RE::Actor* target = actor->GetActorRuntimeData().currentCombatTarget.get().get();
		if (target)
		{

			RE::BGSPerk* perk = DirHandler->GetDirectionalPerk(target);
			if (perk)
			{

				// Actions that occur outside of the normal tick (such as reactions) happen here
				float TargetDist = target->GetPosition().GetSquaredDistance(actor->GetPosition());

				if (TargetDist < 60000)
				{
					// always attack if have perk
					if (DirHandler->IsUnblockable(actor))
					{
						TryAttack(actor);
					}

					if (target->IsAttacking())
					{
						//logger::info("NPC in range");
						AIHandler::GetSingleton()->AddAction(actor, AIHandler::Actions::Block);
					}

					// keep track of target direction switches so we can try to snipe their opposing directions
					// thsi sits outside the usual action cycle
					if (DifficultyMap.contains(actor->GetHandle()))
					{
						if (!DifficultyMap[actor->GetHandle()].currentTarget)
						{
							DifficultyMap[actor->GetHandle()].currentTarget = target->GetHandle();
							DifficultyMap[actor->GetHandle()].targetLastDir = DirHandler->PerkToDirection(perk);
							DifficultyMap[actor->GetHandle()].targetSwitchTimer = 0.f;

							logger::info("adding new targets");
						}
						else
						{
							if (DifficultyMap[actor->GetHandle()].targetLastDir == DirHandler->PerkToDirection(perk))
							{
								DifficultyMap[actor->GetHandle()].targetSwitchTimer += delta;
								//logger::info("old angle from target{}", (int)DirHandler->PerkToDirection(perk));
							}
							else
							{
								DifficultyMap[actor->GetHandle()].targetSwitchTimer = 0.f;
								DifficultyMap[actor->GetHandle()].targetLastDir = DirHandler->PerkToDirection(perk);
								//logger::info("new angle from target{}", (int)DirHandler->PerkToDirection(perk));
							}
						}
					}
				}

				// slower update tick to make AIs reasonable to fight
				if (CanAct(actor))
				{

					if (TargetDist < 60000)
					{

						if (actor->IsAttacking())
						{
							DirHandler->SwitchToNewDirection(actor, actor);
						}
						else
						{
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
								DirHandler->SwitchToNewDirection(actor, target);
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


						int rand = std::rand() % 10;
						if (rand == 1)
						{
							DirHandler->WantToSwitchTo(actor, Directions::TR);
						}
						else if (rand == 2)
						{
							DirHandler->WantToSwitchTo(actor, Directions::BR);
						}
						else if (rand == 3)
						{
							DirHandler->WantToSwitchTo(actor, Directions::BL);
						}

					}

					DidAct(actor);
				}

			}
		}
	}
}

bool AIHandler::ShouldAttack(RE::Actor* actor, RE::Actor* target)
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

		int val = std::rand() % mod;

		if (val < 1)
		{
			return true;
		}
		return false;
	}

	return true;
}

void AIHandler::TryRiposte(RE::Actor* actor)
{
	int mod = (int)CalcAndInsertDifficulty(actor);
	// 12 - 27 range
	// 6 - 13
	// .83 - .97
	mod += 3;
	mod *= 3;
	mod = (int)(mod * 0.5);
	int val = std::rand() % mod;
	if (val > 0)
	{
		AddAction(actor, AIHandler::Actions::Riposte, true);
	}
}

void AIHandler::TryBlock(RE::Actor* actor, RE::Actor* attacker)
{
	int mod = (int)CalcAndInsertDifficulty(actor);
	mod += 3;
	mod *= 4; 
	int val = std::rand() % mod;
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
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		CalcAndInsertDifficulty(actor);
	}
	//DifficultyMap[actor->GetHandle()].lastDirectionTracked;
	// follow last tracked direction isntead of the targets current direction
	// the AI is too easy to confuse with this though, since they can lag behind a bit
	Directions ToCounter = DifficultyMap[actor->GetHandle()].lastDirectionTracked;
	
	// add some RNG to mix it up
	int mod = (int)CalcAndInsertDifficulty(actor);
	// 33% chance of legendary AI switching immediately
	mod = (int)(mod * 0.5);
	int val = std::rand() % mod;
	if (val > 0)
	{
		ToCounter = DirectionHandler::GetSingleton()->PerkToDirection(DirectionHandler::GetSingleton()->GetDirectionalPerk(target));
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
	DifficultyMap[actor->GetHandle()].lastDirectionTracked = 
		DirectionHandler::GetSingleton()->PerkToDirection(DirectionHandler::GetSingleton()->GetDirectionalPerk(target));
	DirectionHandler::GetSingleton()->WantToSwitchTo(actor, ToSwitch, force);
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


void AIHandler::LoadCachedAttack(RE::Actor* actor)
{
	if (!actor->GetActorRuntimeData().currentProcess->high->attackData)
	{
		if (DifficultyMap.contains(actor->GetHandle()))
		{
			actor->GetActorRuntimeData().currentProcess->high->attackData =
				DifficultyMap[actor->GetHandle()].cachedBasicAttackData;
		}
		else 
		{
			// seems strange at this point that difficulty is not already created
			CalcAndInsertDifficulty(actor);
			actor->GetActorRuntimeData().currentProcess->high->attackData =
				DifficultyMap[actor->GetHandle()].cachedBasicAttackData;
		}
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
		// doesn't seem to do anything
		actor->GetActorBase()->combatStyle->meleeData.powerAttackBlockingMult = 0.f;
		actor->GetActorBase()->combatStyle->meleeData.specialAttackMult = 0.f;
		actor->GetActorBase()->SetCombatStyle(actor->GetActorBase()->combatStyle);
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
	}

	return ret;
}

void AIHandler::SignalBadThing(RE::Actor* actor, Directions attackDir)
{
	
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		// this will populate the map
		CalcAndInsertDifficulty(actor);
	}
	auto Iter = DifficultyMap.find(actor->GetHandle());
	Iter->second.lastDirectionsEncountered.push_back(attackDir);
	// update what the last direction was cause we just got hit
	Iter->second.lastDirectionTracked = attackDir;
	size_t Num = Iter->second.lastDirectionsEncountered.size();
	if (Num > MaxDirs)
	{
		Iter->second.lastDirectionsEncountered.erase(Iter->second.lastDirectionsEncountered.begin());
	}

	std::unordered_set<Directions> dirs;
	for (auto dir : Iter->second.lastDirectionsEncountered)
	{
		dirs.insert(dir);
	}
	int dupes = (int)(Num - dirs.size());
	dupes = std::max(1, dupes);
	float NewMistakeRatio = DifficultyMap[actor->GetHandle()].mistakeRatio - (AISettings::AIGrowthFactor * dupes);
	NewMistakeRatio = std::min(NewMistakeRatio, 0.1f);
	NewMistakeRatio = std::max(NewMistakeRatio, -0.1f);
	DifficultyMap[actor->GetHandle()].mistakeRatio += NewMistakeRatio;
}

void AIHandler::SignalGoodThing(RE::Actor* actor, Directions attackedDir)
{
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		// this will populate the map
		CalcAndInsertDifficulty(actor);
	}
	auto Iter = DifficultyMap.find(actor->GetHandle());
	Iter->second.lastDirectionsEncountered.push_back(attackedDir);
	size_t Num = Iter->second.lastDirectionsEncountered.size();
	if (Num > MaxDirs)
	{
		Iter->second.lastDirectionsEncountered.erase(Iter->second.lastDirectionsEncountered.begin());
	}
	// if the player landed complex attack patterns then it gets easier
	std::unordered_set<Directions> dirs;
	for (auto dir : Iter->second.lastDirectionsEncountered)
	{
		dirs.insert(dir);
	}
	unsigned size = std::max(1u, (unsigned)dirs.size());
	float NewMistakeRatio = DifficultyMap[actor->GetHandle()].mistakeRatio + (AISettings::AIGrowthFactor * size);
	NewMistakeRatio = std::min(NewMistakeRatio, 0.1f);
	NewMistakeRatio = std::max(NewMistakeRatio, -0.1f);
	DifficultyMap[actor->GetHandle()].mistakeRatio += NewMistakeRatio;;
}

float AIHandler::CalcUpdateTimer(RE::Actor* actor)
{
	float base = DifficultyUpdateTimer[CalcAndInsertDifficulty(actor)];
	float mistakeRatio = DifficultyMap[actor->GetHandle()].mistakeRatio;
	base += mistakeRatio;
	// add jitter
	// so ugly
	float result = (float)(std::rand()) / ((float)(RAND_MAX / (AIJitterRange * 2.f)));
	result -= AIJitterRange;
	base += result;
	// don't break animation direction switching by letting AI flicker changes
	base = std::max(base, LowestTime);
	return base;
}

float AIHandler::CalcActionTimer(RE::Actor* actor)
{
	float base = DifficultyActionTimer[CalcAndInsertDifficulty(actor)];
	float mistakeRatio = DifficultyMap[actor->GetHandle()].mistakeRatio;
	mistakeRatio *= 0.5;
	base += mistakeRatio;
	float result = (float)(std::rand()) / ((float)(RAND_MAX / (AIJitterRange * 2.f)));
	result -= AIJitterRange;
	base += result;
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
	ActionQueue.clear();
	UpdateTimer.clear();
	DifficultyMap.clear();
}


void AIHandler::Update(float delta)
{
	// two seperate actions to handle
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
			// once we have executed, the timeleft should be negative and this should be no action
			// this is what we use to determine if we are done with actions for this actor
			ActionQueueIter->second.toDo = Actions::None;
			// do not erase every time for perf reasons
			//ActionQueueIter = ActionQueue.erase(ActionQueueIter);
			//continue;

		}
		ActionQueueIter++;
	}

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

	// This is mostly cleanup since actors die/get unloaded
	// Theorectically this may memory leak
	auto DifficultyMapIter = DifficultyMap.begin();
	while (DifficultyMapIter != DifficultyMap.end())
	{
		if (!DifficultyMapIter->first)
		{
			DifficultyMapIter = DifficultyMap.erase(DifficultyMapIter);
			continue;
		}
		RE::Actor* actor = DifficultyMapIter->first.get().get();
		if (!actor)
		{
			DifficultyMapIter = DifficultyMap.erase(DifficultyMapIter);
			continue;
		}
		DifficultyMapIter++;
	}
}


