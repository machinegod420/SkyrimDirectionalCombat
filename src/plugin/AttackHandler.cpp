#include "AttackHandler.h"
#include "SettingsLoader.h"
#include "DirectionHandler.h"

constexpr float NPCLockoutTime = 0.15f;

bool AttackHandler::InChamberWindow(RE::Actor* actor)
{
	return ChamberWindow.contains(actor->GetHandle());
}

bool AttackHandler::CanAttack(RE::Actor* actor)
{
	if (actor->IsPlayerRef())
	{
		return !PlayerLockout;
	}
	else
	{
		return !AttackLockout.contains(actor->GetHandle());
	}
	
}

void AttackHandler::HandleFeint(RE::Actor* actor)
{
	if (ChamberWindow.contains(actor->GetHandle()))
	{
		actor->NotifyAnimationGraph("attackStop");
		ChamberWindow.erase(actor->GetHandle());
		Directions dir = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
		if (dir == Directions::TR || dir == Directions::BR)
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(actor, Directions::TL, true);
		}
		else
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(actor, Directions::TR, true);
		}
		
	}
}

void AttackHandler::AddChamberWindow(RE::Actor* actor)
{
	// masterstrike window must be a fixed time, otherwise slower attack speeds actually make it easier to masterstrike
	ChamberWindow[actor->GetHandle()] = DifficultySettings::ChamberWindowTime;
}

void AttackHandler::AddNPCSmallLockout(RE::Actor* actor)
{
	if (AttackLockout.contains(actor->GetHandle()))
	{
		AttackLockout[actor->GetHandle()] = NPCLockoutTime;
	}
	
}

void AttackHandler::AddLockout(RE::Actor* actor)
{
	AttackLockout[actor->GetHandle()] = DifficultySettings::AttackTimeoutTime;
}

void AttackHandler::LockoutPlayer()
{
	PlayerLockout = true;
	PlayerLockoutTimer = DifficultySettings::AttackTimeoutTime;
}

void AttackHandler::Update(float delta)
{
	if (PlayerLockout)
	{
		PlayerLockoutTimer -= delta;
		if (PlayerLockoutTimer < 0)
		{
			PlayerLockout = false;
		}
	}
	auto ChamberIter = ChamberWindow.begin();
	while (ChamberIter != ChamberWindow.end())
	{
		if (!ChamberIter->first)
		{
			ChamberIter = ChamberWindow.erase(ChamberIter);
			continue;
		}
		RE::Actor* actor = ChamberIter->first.get().get();
		if (!actor)
		{
			ChamberIter = ChamberWindow.erase(ChamberIter);
			continue;
		}
		ChamberIter->second -= delta;
		if (ChamberIter->second <= 0)
		{
			ChamberIter = ChamberWindow.erase(ChamberIter);
			continue;
		}
		ChamberIter++;
	}

	auto AttackIter = AttackLockout.begin();
	while (AttackIter != AttackLockout.end())
	{
		if (!AttackIter->first)
		{
			AttackIter = AttackLockout.erase(AttackIter);
			continue;
		}
		RE::Actor* actor = AttackIter->first.get().get();
		if (!actor)
		{
			AttackIter = AttackLockout.erase(AttackIter);
			continue;
		}
		AttackIter->second -= delta;
		if (AttackIter->second <= 0)
		{
			AttackIter = AttackLockout.erase(AttackIter);
			continue;
		}
		AttackIter++;
	}
}