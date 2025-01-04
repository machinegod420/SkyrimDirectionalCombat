#include "AIHandler.h"
#include "DirectionHandler.h"
#include "SettingsLoader.h"
#include "AttackHandler.h"
#include "BlockHandler.h"

#include <random>

static std::mt19937 mt_rand(0);
static std::shared_mutex mt_randMtx;

Directions AIAttackCombo[3][3] = { {Directions::TR, Directions::TL, Directions::BL},
								{Directions::BR, Directions::TR, Directions::BR},
								{Directions::BL, Directions::BR, Directions::TR}, };

int GetRand()
{
	std::unique_lock lock(mt_randMtx);
	return mt_rand();
}

constexpr int MaxDirs = 5;

// MUST be higher than the direction switch time otherwise it will try to switch directions too quickly and freeze
constexpr float LowestTime = 0.143f;
// have to be very careful with this number
constexpr float AIJitterRange = 0.04f;

constexpr float MaxMistakeRange = 0.05f;

constexpr int MaxDirectionTracked = 5;

// todo replace with reading game setting
constexpr int BashDistance = 80;
constexpr int BashDistanceSq = BashDistance * BashDistance;
constexpr int JudgeDistance = 69000;

void AIHandler::InitializeValues(PRECISION_API::IVPrecision3* precision)
{
	Precision = precision;
	// time in seconds between each update
	//DifficultyUpdateTimer[Difficulty::Uninitialized] = 0;
	DifficultyUpdateTimer[Difficulty::VeryEasy] = AISettings::VeryEasyUpdateTimer;
	DifficultyUpdateTimer[Difficulty::Easy] = AISettings::EasyUpdateTimer;
	DifficultyUpdateTimer[Difficulty::Normal] = AISettings::NormalUpdateTimer;
	DifficultyUpdateTimer[Difficulty::Hard] = AISettings::HardUpdateTimer;
	DifficultyUpdateTimer[Difficulty::VeryHard] = AISettings::VeryHardUpdateTimer;
	DifficultyUpdateTimer[Difficulty::Legendary] = AISettings::LegendaryUpdateTimer;

	// time between each action
	//DifficultyActionTimer[Difficulty::Uninitialized] = 0;
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
		// 70ms should be the absolute limit of a persons physical reaction time
		iter.second = std::max(iter.second, 0.07f);
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
	if (!RightPowerAttackAction)
	{
		RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
		RightPowerAttackAction = DataHandler->LookupForm<RE::BGSAction>(0x13383, "Skyrim.esm");
		if (RightPowerAttackAction)
		{
			logger::info("Got power attack action");
		}
	}
}

void AIHandler::AddAction(RE::Actor* actor, Actions toDo, Directions attackedDir, bool force)
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
		action.targetDir = attackedDir;
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

void AIHandler::DidAttackExternalCalled(RE::Actor* actor)
{
	DifficultyMapMtx.lock();
	CalcAndInsertDifficulty(actor);
	if (DifficultyMap.contains(actor->GetHandle()))
	{
		unsigned idx = DifficultyMap.at(actor->GetHandle()).currentAttackIdx;
		idx++;

		if (idx > DifficultyMap.at(actor->GetHandle()).attackPattern.size())
		{
			idx = 0;
		}
		//logger::info("{} next attack is {} idx {}", actor->GetName(), (int)DifficultyMap[actor->GetHandle()].attackPattern[idx], idx);
		DifficultyMap[actor->GetHandle()].currentAttackIdx = idx;
	}
	else
	{
		logger::error("couldn't find in map!");
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
				if (!DifficultyMap.contains(actor->GetHandle()))
				{
					CalcAndInsertDifficulty(actor);
				}

				// slower update tick to make AIs reasonable to fight
				if (CanAct(actor))
				{
					
					if (TargetDist < JudgeDistance)
					{
						float CurrentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
						float MaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
						float CurrentStaminaRatio = CurrentStamina / MaxStamina;

						float EnemyCurrentStamina = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
						float EnemyMaxStamina = target->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
						float EnemyStaminaRatio = EnemyCurrentStamina / EnemyMaxStamina;
						// always attack if have perk
						if (DirHandler->IsUnblockable(actor) && TargetDist < DifficultyMap[actor->GetHandle()].CurrentWeaponLengthSQ)
						{
							AddAction(actor, AIHandler::Actions::Riposte);
						}

						if (actor->IsAttacking())
						{
							//SwitchToNewDirection(actor, actor);
							SwitchToNextAttack(actor, true);
							if (IsPowerAttacking(actor))
							{
								AddAction(actor, AIHandler::Actions::Followup, Directions::TR, true);
							}
						}
						else
						{
							bool ShouldDirectionMatch = false;
							bool DontChangeDirection = false;
							bool targetStaggering = false;
							target->GetGraphVariableBool("IsStaggering", targetStaggering);
							// Most important case, attempt to defend
							if (target->IsAttacking() && !DirectionHandler::GetSingleton()->IsUnblockable(target) && !IsBashing(target))
							{
								Actions action = GetQueuedAction(actor);
								// try to block or masterstrike
								if (action != Actions::Riposte && action != Actions::Block)
								{
									
									if (mt_rand() % 10 < 1 && DirHandler->HasBlockAngle(actor, target))
									{
										// masterstrike
										AddAction(actor, AIHandler::Actions::PowerAttack, Directions::TR, true);
									}
									else
									{
										AddAction(actor, Actions::Block, Directions::TR, true);
									}
								}
								if (DirHandler->HasBlockAngle(actor, target))
								{
									DontChangeDirection = true;
								}
								ShouldDirectionMatch = true;

								DifficultyMap[actor->GetHandle()].defending = true;
								DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched = 0;
								DifficultyMap[actor->GetHandle()].numTimesDirectionSame = 0;
							}
							else if (!AttackHandler::GetSingleton()->CanAttack(target) || targetStaggering)
							{
								// target cant attack so start attacking
								DifficultyMap[actor->GetHandle()].defending = false;
								DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched = MaxDirectionTracked + 1;
								DifficultyMap[actor->GetHandle()].numTimesDirectionSame = 0;
								if (actor->IsBlocking())
								{
									AddAction(actor, Actions::EndBlock);
								}
								ShouldDirectionMatch = false;
								DontChangeDirection = false;
							}
							// If they are too close, try bashing
							else if (TargetDist < (BashDistanceSq + 1000) && mt_rand() % 5 < 4
								&& AttackHandler::GetSingleton()->CanAttack(actor) && CurrentStaminaRatio > 0.1)
							{
								AddAction(actor, Actions::Bash);
								ShouldDirectionMatch = true;
								DontChangeDirection = true;
							}
							// uh oh, they might bash us!
							else if (TargetDist < (BashDistanceSq + 2000) && mt_rand() % 5 < 4)
							{
								if (Settings::DMCOSupport)
								{
									// add another check here because the enemy might be unable to bash
									bool TargetStaggering = false;
									target->GetGraphVariableBool("IsStaggering", TargetStaggering);
									bool ShouldDodge = !TargetStaggering;
									if (ShouldDodge && mt_rand() % 5 < 2)
									{
										ShouldDodge = false;
									}
									if (ShouldDodge)
									{
										AddAction(actor, Actions::Dodge);
									}
									else
									{
										if (AttackHandler::GetSingleton()->CanAttack(actor) && !actor->IsBlocking() && CurrentStaminaRatio > 0.1)
										{
											AddAction(actor, Actions::Riposte);
											DifficultyMap[actor->GetHandle()].defending = false;
										}
									}

								}
								else if(AttackHandler::GetSingleton()->CanAttack(actor) && !actor->IsBlocking() && CurrentStaminaRatio > 0.1)
								{
									AddAction(actor, Actions::Riposte);
									DifficultyMap[actor->GetHandle()].defending = false;
								}
							}
							// They guarding the same position a lot, maybe they are attacking?
							else if (!actor->IsBlocking() && DifficultyMap[actor->GetHandle()].defending 
								&& DifficultyMap.at(actor->GetHandle()).numTimesDirectionSame > 3)
							{
								AddAction(actor, Actions::Block);
								ShouldDirectionMatch = true;
							}
							// Stop blocking to avoid burning stamina
							else if (actor->IsBlocking() && DifficultyMap.at(actor->GetHandle()).numTimesDirectionSame < 1 && 
								(mt_rand() % 5 < 2 || CurrentStaminaRatio < 0.4))
							{
								AddAction(actor, Actions::EndBlock);
								DontChangeDirection = true;
								ShouldDirectionMatch = true;
							}
							// always hard defend if cant attack (attack was parried)
							if (!AttackHandler::GetSingleton()->CanAttack(actor))
							{
								DifficultyMap.at(actor->GetHandle()).defending = true;

								ShouldDirectionMatch = true;
							}
							if (DifficultyMap.at(actor->GetHandle()).defending)
							{
								ShouldDirectionMatch = true;
								if (DifficultyMap.at(actor->GetHandle()).numTimesDirectionSame > MaxDirectionTracked ||
									DifficultyMap.at(actor->GetHandle()).numTimesDirectionsSwitched > MaxDirectionTracked)
								{
									DifficultyMap[actor->GetHandle()].defending = false;
									DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched = MaxDirectionTracked + 1;
									DifficultyMap[actor->GetHandle()].numTimesDirectionSame = 0;
								}
							}
							else
							{
								// only restart after blockign is ended
								if (actor->IsBlocking())
								{
									if (DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched < 1)
									{
										DifficultyMap[actor->GetHandle()].defending = true;
									}
									DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched--;
									DontChangeDirection = false;
									ShouldDirectionMatch = false;
								}

							}


							if (!DontChangeDirection)
							{
								if (ShouldDirectionMatch)
								{
									DirectionMatchTarget(actor, target, target->IsAttacking());
								}
								else
								{
									SwitchToNewDirection(actor, target, TargetDist);
									//SwitchToNextAttack(actor);
								}
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
							DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched = 0;
							DifficultyMap[actor->GetHandle()].numTimesDirectionSame = 0;
							DifficultyMap[actor->GetHandle()].defending = false;
						}

						SwitchToNextAttack(actor, false);
					}

					DidAct(actor);
				}

				DifficultyMapMtx.unlock();
			}
			else
			{
				// if enemy has no directions
				if (CanAct(actor))
				{
					SwitchToNextAttack(actor, false);
					DidAct(actor);
				}
			}
		}
	}
}

void AIHandler::SwitchTargetExternalCalled(RE::Actor* actor, RE::Actor* newTarget)
{
	if (!actor->IsHostileToActor(newTarget))
	{
		return;
	}

	RE::Actor* currentTarget = actor->GetActorRuntimeData().currentCombatTarget.get().get();
	if (newTarget->IsPlayerRef())
	{
		actor->GetActorRuntimeData().currentCombatTarget = newTarget->GetHandle();
		return;
	}

	if (currentTarget)
	{
		float TargetDist = actor->GetPosition().GetSquaredDistance(currentTarget->GetPosition());
		if (TargetDist > JudgeDistance)
		{
			actor->GetActorRuntimeData().currentCombatTarget = newTarget->GetHandle();
		}
	}
}

// TODO : always return false so we get total control of when the NPC attacks
bool AIHandler::ShouldAttackExternalCalled(RE::Actor* actor, RE::Actor* target)
{
	std::unique_lock lock(DifficultyMapMtx);

	if (!DirectionHandler::GetSingleton()->HasDirectionalPerks(target))
	{
		return true;
	}
	int mod = (int)CalcAndInsertDifficulty(actor);
	bool HasBlockAngle = DirectionHandler::GetSingleton()->HasBlockAngle(actor, target);

	float CurrentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	float MaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
	float CurrentStaminaRatio = CurrentStamina / MaxStamina;
	// stamina filter
	if (CurrentStaminaRatio < 0.2)
	{
		return false;
	}
	if (CurrentStaminaRatio < 0.33)
	{
		if (mt_rand() % 2 < 1)
		{
			return false;
		}
	}

	if (CurrentStaminaRatio < 0.4)
	{
		if (mt_rand() % 4 < 1)
		{
			return false;
		}
	}

	// if target is attacking another angle don't attack wildly ie not masterstriking
	if (target->IsAttacking() && !HasBlockAngle)
	{
		return false;
	}
	if (DirectionHandler::GetSingleton()->GetCurrentDirection(actor) ==
		DifficultyMap[actor->GetHandle()].attackPattern[DifficultyMap[actor->GetHandle()].currentAttackIdx])
	{
		if (HasBlockAngle)
		{
			// jitter this based on difficulty of target
			// and influence based on if the target is blocking or not
			if (target->IsBlocking())
			{
				mod += 2;
			}

			int val = mt_rand() % mod;

			if (val < 2)
			{
				return true;
			}
			else
			{
				return false;
			}
		}
		else
		{
			return true;
		}
	}
	else
	{
		// some RNG to attack anyways
		if (mt_rand() % 10 < 1 && !HasBlockAngle)
		{
			return true;
		}
	}
	return false;
}

void AIHandler::TryRiposteExternalCalled(RE::Actor* actor, RE::Actor* attacker)
{
	if (!AttackHandler::GetSingleton()->CanAttack(actor))
	{
		return;
	}
	std::unique_lock lock(DifficultyMapMtx);
	int mod = (int)CalcAndInsertDifficulty(actor);
	// 8 - 13 range
	// 4 - 7
	// .75 - .86
	mod += 7;
	mod = (int)(mod * 0.5);
	int val = mt_rand() % mod;
	// force block stop to avoid weird stamina issues


	if (val > 0)
	{

		SwitchToNextAttack(actor, true);
		bool ShouldFeint = DirectionHandler::GetSingleton()->HasBlockAngle(actor, attacker);
		float TotalStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		float CurrentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		if (CurrentStamina > TotalStamina * 0.15f)
		{
			DifficultyMap[actor->GetHandle()].defending = false;
			if (CurrentStamina < TotalStamina * 0.5f)
			{
				ShouldFeint = false;
			}
			if (val < 2)
			{
				ShouldFeint = false;
			}
			if (ShouldFeint)
			{
				if (actor->IsBlocking())
				{
					actor->NotifyAnimationGraph("blockStop");
				}
				AddAction(actor, AIHandler::Actions::StartFeint, Directions::TR, true);
			}
			else
			{
				if (actor->IsBlocking())
				{
					actor->NotifyAnimationGraph("blockStop");
				}
				AddAction(actor, AIHandler::Actions::Riposte, Directions::TR, true);
			}
		}
		else
		{
			AIHandler::GetSingleton()->AddAction(actor, AIHandler::Actions::EndBlock, Directions::TR);
		}

	}
	else
	{
		AIHandler::GetSingleton()->AddAction(actor, AIHandler::Actions::EndBlock, Directions::TR);
	}
}

void AIHandler::TryBlockExternalCalled(RE::Actor* actor, RE::Actor* attacker)
{
	std::unique_lock lock(DifficultyMapMtx);
	int mod = (int)CalcAndInsertDifficulty(actor);
	mod += 3;
	mod *= 4;
	int val = mt_rand() % mod;
	DifficultyMap[actor->GetHandle()].defending = true;
	if (val > 0)
	{
		Directions CurrentTargetDir = DirectionHandler::GetSingleton()->GetCurrentDirection(attacker);
		AddAction(actor, AIHandler::Actions::Block, CurrentTargetDir);
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
	//Directions CurrentDir = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
	// if we have to switch directions, then add a chance to make a mistkae
	// simulates conditioning the AI to block in certain directions
	int TR = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR];
	int TL = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL];
	int BL = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL];
	int BR = DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR];
	TR = std::min(TR, 25);
	TL = std::min(TL, 25);
	BR = std::min(BR, 25);
	BL = std::min(BL, 25);
	if (Settings::ForHonorMode)
	{
		TL = 0;
	}
	if (ToCounter != CurrentTargetDir)
	{
		int DifficultyMod = 8 * AISettings::AIMistakeRatio;
		DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched++;
		DifficultyMap[actor->GetHandle()].numTimesDirectionSame = 0;
		if (DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched < MaxDirectionTracked)
		{
			DifficultyMap[actor->GetHandle()].directionChangeChance[ToCounter] +=
				(DifficultyMod / mod) * DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched;

		}


		int random = mt_rand() % 100;
		// add some RNG to mix it up
		// have a chance to switch to a direction they anticipate instead
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
			//logger::info("{} failed direction {} {} {} {}", actor->GetName(), TR, TL, BL, BR);

			int DifficultyMod = 5;
			TR -= DifficultyMod;
			TR = std::max(0, TR);
			TL -= DifficultyMod;
			TL = std::max(0, TL);
			BL -= DifficultyMod;
			BL = std::max(0, BL);
			BR -= DifficultyMod;
			BR = std::max(0, BR);
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR] = TR;
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL] = TL;
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL] = BL;
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR] = BR;
		}
		else
		{
			// don't make them track that easily so comment out for now
			// this will cause the ai to lag for 2+ cycles
			//ToCounter = CurrentTargetDir;
			random = mt_rand() % 50;
			if (random < mod)
			{
				ToCounter = CurrentTargetDir;
			}
			TR -= 1;
			TR = std::max(0, TR);
			TL -= 1;
			TL = std::max(0, TL);
			BL -= 1;
			BL = std::max(0, BL);
			BR -= 1;
			BR = std::max(0, BR);
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR] = TR;
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL] = TL;
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL] = BL;
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR] = BR;
		}
	}
	else
	{
		// tracking
		DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched =
			std::max(DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched - 1, 0);
		DifficultyMap[actor->GetHandle()].numTimesDirectionSame++;
		if (DifficultyMap[actor->GetHandle()].numTimesDirectionSame < 5)
		{
			int DifficultyMod = 8 * AISettings::AIMistakeRatio;
			DifficultyMap[actor->GetHandle()].directionChangeChance[ToCounter] += (DifficultyMod / mod);
			TR -= mod / 2;
			TR = std::max(0, TR);
			TL -= mod / 2;
			TL = std::max(0, TL);
			BL -= mod / 2;
			BL = std::max(0, BL);
			BR -= mod / 2;
			BR = std::max(0, BR);
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR] = TR;
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL] = TL;
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL] = BL;
			DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR] = BR;
		}

	}
	// if currently attacking toward this spot, slowly increase percentage
	if (force)
	{
		int DifficultyMod = 8 * AISettings::AIMistakeRatio;
		DifficultyMap[actor->GetHandle()].directionChangeChance[CurrentTargetDir]
			+= (DifficultyMod / mod);
	}


	//ToCounter = DirectionHandler::GetSingleton()->GetCurrentDirection(target);
	Directions ToSwitch = DirectionHandler::GetCounterDirection(ToCounter);
	DirectionHandler::GetSingleton()->WantToSwitchTo(actor, ToSwitch, force);

	DifficultyMap[actor->GetHandle()].lastDirectionTracked = CurrentTargetDir;

}

void AIHandler::SwitchToNewDirection(RE::Actor* actor, RE::Actor* target, float TargetDist)
{
	// This will queue up this event if you cant switch instead
	RE::ActorHandle ActorHandle = actor->GetHandle();
	CalcAndInsertDifficulty(actor);
	Directions CurrentDirection = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
	Directions TargetDirection = DirectionHandler::GetSingleton()->GetCurrentDirection(target);
	// This will queue up this event if you cant switch instead
	Directions CounterDirection = DirectionHandler::GetCounterDirection(TargetDirection);
	unsigned idx = DifficultyMap[actor->GetHandle()].currentAttackIdx;

	if (DifficultyMap[ActorHandle].attackPattern[idx] != CounterDirection)
	{
		SwitchToNextAttack(actor, true);
	}
	else
	{
		// if the attack direction is where my enemy is blocking, try feinting
		bool ShouldFeint = mt_rand() % 3 < 1;
		float MaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		float CurrentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		float CurrentStaminaRatio = CurrentStamina / MaxStamina;
		Actions action = GetQueuedAction(actor);
		if (action != Actions::StartFeint && action != Actions::EndFeint && action != Actions::Riposte)
		{

			if (CurrentStaminaRatio > 0.1)
			{

				if (CurrentStaminaRatio < 0.5)
				{
					ShouldFeint = false;
				}
				if (DifficultyMap[actor->GetHandle()].CurrentWeaponLengthSQ < TargetDist)
				{
					if (ShouldFeint)
					{
						SwitchToNextAttack(actor, false);
						if (mt_rand() % 2 < 1)
						{
							AddAction(actor, AIHandler::Actions::StartFeint, Directions::TR, true);
						}
						else
						{
							AddAction(actor, AIHandler::Actions::PowerAttack, Directions::TR, true);
						}
					}
				}

			}
			if (!ShouldFeint)
			{
				// switch between actively changing directions if our current direction is matching
				if (mt_rand() % 4 < 2)
				{
					Directions ToAvoid = DirectionHandler::GetSingleton()->GetCurrentDirection(target);
					DirectionHandler::GetSingleton()->WantToSwitchTo(actor, ToAvoid);
				}
				else
				{
					int Direction = mt_rand() % 4;
					Directions ToSwitch;
					switch (Direction)
					{
					case 0:
						ToSwitch = Directions::TR;
						break;
					case 1:
						ToSwitch = Directions::TL;
						break;
					case 2:
						ToSwitch = Directions::BL;
						break;
					case 3:
					default:
						ToSwitch = Directions::BR;
						break;
					}
					DirectionHandler::GetSingleton()->WantToSwitchTo(actor, ToSwitch);
				}
			}
		}
	}
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
		DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched =
			std::max(DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched - 1, 0);
	}
	else
	{
		logger::error("couldn't find in map!");
	}
}

void AIHandler::ResetDifficulty(RE::Actor* actor)
{
	if (DifficultyMap.contains(actor->GetHandle()))
	{

		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TR] = 0;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::TL] = 0;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BL] = 0;
		DifficultyMap[actor->GetHandle()].directionChangeChance[Directions::BR] = 0;

		DifficultyMap[actor->GetHandle()].mistakeRatio = 0.f;

	}
	else
	{
		logger::error("couldn't find in map!");
	}
}

bool AIHandler::TryAttack(RE::Actor* actor, bool force)
{
	// since this is a forced attack, it happens outside of the normal AI attack loop so we need to add checks here as well
	if (AttackHandler::GetSingleton()->CanAttack(actor) && actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) > 10)
	{
		// load attack data into actor to ensure attacks register correctly
		if (LoadCachedAttack(actor, force))
		{
			// set actor state to show that actor is now attacking
			actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kSwing;
			actor->NotifyAnimationGraph("attackStart");
			return true;
		}
	}
	return false;
}

bool AIHandler::TryPowerAttack(RE::Actor* actor)
{
	/*
	std::unique_ptr<RE::TESActionData> data(RE::TESActionData::Create());
	data->action = RightPowerAttackAction;
	data->source = RE::NiPointer<RE::TESObjectREFR>(actor);
	typedef bool func_t(RE::TESActionData*);
	REL::Relocation<func_t> func{ RELOCATION_ID(40551, 41557) };
	bool succ = func(data.get());
	return succ;
	*/

	// since this is a forced attack, it happens outside of the normal AI attack loop so we need to add checks here as well
	if (AttackHandler::GetSingleton()->CanAttack(actor) && actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) > 10 && !actor->IsBlocking())
	{
		// load attack data into actor to ensure attacks register correctly
		if (LoadCachedPowerAttack(actor))
		{
			// set actor state to show that actor is now attacking
			actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kSwing;
			actor->NotifyAnimationGraph("attackPowerStartInPlace");
			return true;
		}
	}
	return false;
}

void AIHandler::SwitchToNextAttack(RE::Actor* actor, bool force)
{
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		CalcAndInsertDifficulty(actor);
	}

	DifficultyMap[actor->GetHandle()].numTimesDirectionSame = 0;
	int idx = DifficultyMap[actor->GetHandle()].currentAttackIdx;
	if (idx < DifficultyMap[actor->GetHandle()].attackPattern.size())
	{
		Directions dir = DifficultyMap[actor->GetHandle()].attackPattern[idx];
		if ((int)dir > 3)
		{
			logger::info("{} had error in attack pattern to {}", actor->GetName(), (int)dir);
		}
		DirectionHandler::GetSingleton()->WantToSwitchTo(actor, dir);
	}
	else
	{
		DifficultyMap[actor->GetHandle()].currentAttackIdx = 0;
	}
}

Directions AIHandler::GetNextAttack(RE::Actor* actor)
{
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		CalcAndInsertDifficulty(actor);
	}
	int idx = DifficultyMap[actor->GetHandle()].currentAttackIdx;
	if (idx > DifficultyMap[actor->GetHandle()].attackPattern.size())
	{
		DifficultyMap[actor->GetHandle()].currentAttackIdx = 0;
		idx = 0;
	}
	Directions dir = DifficultyMap[actor->GetHandle()].attackPattern[idx];
	return dir;
}

AIHandler::Actions AIHandler::GetQueuedAction(RE::Actor* actor)
{
	Actions ret = Actions::None;
	ActionQueueMtx.lock();
	if (ActionQueue.contains(actor->GetHandle()))
	{
		ret = ActionQueue.at(actor->GetHandle()).toDo;
	}
	ActionQueueMtx.unlock();
	return ret;
}


bool AIHandler::LoadCachedAttack(RE::Actor* actor, bool force)
{
	// seems strange at this point that difficulty is not already created
	CalcAndInsertDifficulty(actor);
	if (!DifficultyMap[actor->GetHandle()].cachedBasicAttackData)
	{
		return false;
	}
	if (!force)
	{
		if (!actor->GetActorRuntimeData().currentProcess->high->attackData)
		{
			actor->GetActorRuntimeData().currentProcess->high->attackData =
				DifficultyMap[actor->GetHandle()].cachedBasicAttackData;
			return true;
		}
	}
	else
	{
		actor->GetActorRuntimeData().currentProcess->high->attackData =
			DifficultyMap[actor->GetHandle()].cachedBasicAttackData;
		return true;
	}
	return false;
}

bool AIHandler::LoadCachedPowerAttack(RE::Actor* actor)
{
	if (!actor->GetActorRuntimeData().currentProcess->high->attackData)
	{
		// seems strange at this point that difficulty is not already created
		CalcAndInsertDifficulty(actor);
		if (!DifficultyMap[actor->GetHandle()].cachedPowerAttackData)
		{
			return false;
		}
		actor->GetActorRuntimeData().currentProcess->high->attackData =
			DifficultyMap[actor->GetHandle()].cachedPowerAttackData;
		return true;
	}
	return false;
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

		auto PowerAttackData = FindActorPowerAttackData(actor);
		if (PowerAttackData)
		{
			logger::info("{} has power attack data", actor->GetName());
			DifficultyMap[actor->GetHandle()].cachedPowerAttackData = PowerAttackData;
		}

		// generate attack patterns ahead of time
		// instead of using rand, use their native handle as that is unique per actor and is the same between saves
		// so you can learn after dying
		uint32_t handle = actor->GetHandle().native_handle();
		int Idx = handle % 3;
		for (unsigned i = 0; i < 3; ++i)
		{
			DifficultyMap[actor->GetHandle()].attackPattern.push_back(AIAttackCombo[Idx][i]);
		}


		// create modifier to see how good or bad the AI is at judging distance
		DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched = 0;
		DifficultyMap[actor->GetHandle()].currentAttackIdx = 0;
		DifficultyMap[actor->GetHandle()].defending = false;

		// seed with deterministic input
		DifficultyMap[actor->GetHandle()].npcRand.seed(handle);

		if (Settings::HasPrecision && Precision)
		{
			float CurrentWeaponLength = Precision->GetAttackCollisionCapsuleLength(actor->GetHandle());
			DifficultyMap[actor->GetHandle()].CurrentWeaponLengthSQ = (CurrentWeaponLength * CurrentWeaponLength);
		}
		else
		{
			auto Equipped = actor->GetEquippedObject(false);
			if (Equipped)
			{
				auto Weapon = Equipped->As<RE::TESObjectWEAP>();
				if (Weapon && Weapon->IsMelee())
				{
					float CurrentWeaponLength = Weapon->GetReach();
					DifficultyMap[actor->GetHandle()].CurrentWeaponLengthSQ = (CurrentWeaponLength * CurrentWeaponLength);
				}
			}
		}
	}

	return ret;
}

void AIHandler::SignalBadThingExternalCalled(RE::Actor* actor, Directions attackDir)
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
	IncreaseBlockChance(actor, attackDir, mt_rand() % 10 + 5, 4);

	float NewMistakeRatio = DifficultyMap[actor->GetHandle()].mistakeRatio - (AISettings::AIGrowthFactor * 0.5f);
	NewMistakeRatio = std::min(NewMistakeRatio, MaxMistakeRange);
	NewMistakeRatio = std::max(NewMistakeRatio, -MaxMistakeRange);
	DifficultyMap[actor->GetHandle()].mistakeRatio = NewMistakeRatio;
	DifficultyMapMtx.unlock();
}

void AIHandler::SignalGoodThingExternalCalled(RE::Actor* actor, Directions attackedDir)
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
	IncreaseBlockChance(actor, attackedDir, mt_rand() % 5 + 3, 5);


	unsigned size = std::max(1u, (unsigned)dirs.size());
	float NewMistakeRatio = DifficultyMap[actor->GetHandle()].mistakeRatio + (AISettings::AIGrowthFactor * size);
	NewMistakeRatio = std::min(NewMistakeRatio, MaxMistakeRange);
	NewMistakeRatio = std::max(NewMistakeRatio, -MaxMistakeRange);
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
	if (RaceToNormalAttack.contains(actor->GetRace()))
	{
		return RaceToNormalAttack.at(actor->GetRace());
	}
	for (auto& iter : actor->GetRace()->attackDataMap->attackDataMap)
	{
		if (iter.first == "attackStart")
		{
			RaceToNormalAttack[actor->GetRace()] = iter.second;
			return iter.second;
		}
	}
	return nullptr;
}

RE::NiPointer<RE::BGSAttackData> AIHandler::FindActorPowerAttackData(RE::Actor* actor)
{
	// iterate until we found the attackstart
	if (RaceToPowerAttack.contains(actor->GetRace()))
	{
		return RaceToPowerAttack.at(actor->GetRace());
	}
	for (auto& iter : actor->GetRace()->attackDataMap->attackDataMap)
	{
		if (iter.first == "attackPowerStartInPlace")
		{
			RaceToPowerAttack[actor->GetRace()] = iter.second;
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
				// sit in queue until we can attack again
				if (TryAttack(actor, false))
				{
					ActionQueueIter->second.toDo = Actions::None;
				}
				
			}
			else if (ActionQueueIter->second.toDo == Actions::Followup)
			{
				// this is a special case as it can force an attack to go thru as another attack is in progress
				if (DirectionHandler::GetSingleton()->CanSwitch(actor))
				{
					if (TryAttack(actor, true))
					{
						ActionQueueIter->second.toDo = Actions::None;
					}
				}
				

			}
			else if (ActionQueueIter->second.toDo == Actions::Block)
			{
				actor->NotifyAnimationGraph("blockStart");
				ActionQueueIter->second.toDo = Actions::None;
			}
			else if (ActionQueueIter->second.toDo == Actions::ProBlock)
			{
				actor->NotifyAnimationGraph("blockStart");
				ActionQueueIter->second.toDo = Actions::None;
			}
			else if (ActionQueueIter->second.toDo == Actions::Bash)
			{
				actor->NotifyAnimationGraph("bashStart");
				actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kBash;
				ActionQueueIter->second.toDo = Actions::ReleaseBash;
				ActionQueueIter->second.timeLeft = 0.1f;

				
			}
			else if (ActionQueueIter->second.toDo == Actions::ReleaseBash)
			{
				actor->NotifyAnimationGraph("bashRelease");
				ActionQueueIter->second.toDo = Actions::None;
			}
			else if (ActionQueueIter->second.toDo == Actions::EndBlock)
			{
				actor->NotifyAnimationGraph("blockStop");
				ActionQueueIter->second.toDo = Actions::None;
			}
			else if (ActionQueueIter->second.toDo == Actions::StartFeint)
			{
				if (actor->IsBlocking())
				{
					actor->NotifyAnimationGraph("blockStop");
				}
				if (TryAttack(actor, false))
				{
					// add action will freeze due to mutex lock
					// hack fix later
					ActionQueueIter->second.toDo = Actions::EndFeint;
					ActionQueueIter->second.timeLeft = DifficultySettings::FeintWindowTime - (CalcActionTimer(actor) * .5f);
				}


			}
			else if (ActionQueueIter->second.toDo == Actions::EndFeint)
			{
				if (!actor->IsBlocking())
				{
					AttackHandler::GetSingleton()->HandleFeint(actor);

					// queue another attack
					ActionQueueIter->second.toDo = Actions::Riposte;
					ActionQueueIter->second.timeLeft = 0.15f + (CalcActionTimer(actor) * .5f);
				}
				else
				{
					// we tried but they attacked anyway
					ActionQueueIter->second.toDo = Actions::None;
				}

			}
			else if (ActionQueueIter->second.toDo == Actions::PowerAttack)
			{
				// sit in queue until we can attack again
				if (TryPowerAttack(actor))
				{
					ActionQueueIter->second.toDo = Actions::None;
				}

			}
			else if (ActionQueueIter->second.toDo == Actions::Dodge)
			{
				int rand = mt_rand() % 4;
				if (rand == 3)
				{
					actor->SetGraphVariableInt("Dodge_Direction", 4);
					actor->NotifyAnimationGraph("Dodge_RB");
					actor->NotifyAnimationGraph("Dodge");
				}
				else if (rand == 2)
				{
					actor->SetGraphVariableInt("Dodge_Direction", 6);
					actor->NotifyAnimationGraph("Dodge_LB");
					actor->NotifyAnimationGraph("Dodge");
				}
				else
				{
					actor->SetGraphVariableInt("Dodge_Direction", 5);
					actor->NotifyAnimationGraph("Dodge_B");
					actor->NotifyAnimationGraph("Dodge");
				}

				ActionQueueIter->second.toDo = Actions::None;
			}
			// once we have executed, the timeleft should be negative and this should be no action
			// this is what we use to determine if we are done with actions for this actor

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


