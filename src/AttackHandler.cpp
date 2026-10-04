#include "AttackHandler.h"
#include "SettingsLoader.h"
#include "DirectionHandler.h"

constexpr float NPCLockoutTime = 0.15f;
constexpr float AttackSpeedMult = 0.1f;
constexpr float SmallAttackSpeedMult = 0.04f;
// SpeedMult points each combo step takes off the actor it lands on, and the
// floor they stack to. Lasts the combo window so it covers the finisher.
constexpr float ComboSlowPerStep = -15.f;
constexpr float ComboSlowMax = -30.f;
// Leak-guard only.
constexpr float ChargeBuffDuration = 3.0f;
// timer to figure out if someone is stuck in an attack state but not attacking so we can unfuck them
constexpr float StuckAttackGraceSeconds = 1.f;
// Old auto-mixup feint, off while the hard feint is tried; uncomment every "old feint" block to restore.
// constexpr float FeintQueueTime = 0.2f; // carefully tailored magic number

// this is a horrendous hack to figure out which directions have the longest range poke attack
// doing this analyitically is actually super hard
// if you have custom animations uhhhhh
constexpr Directions PokeLineTable[3][5] = {
	/* Normal   */ { Directions::BL, Directions::BR, Directions::BL, Directions::BR, Directions::BL },
	/* ForHonor */ { Directions::BL, Directions::BR, Directions::BL, Directions::BR, Directions::BL },
	/* KCD      */ { Directions::BL, Directions::BL, Directions::BL, Directions::BL, Directions::BL },
};
// guestimate to figure out how much more reach the poke has
constexpr float PokeReachMult = 1.1f;

Directions AttackHandler::PokeLine(RE::Actor* actor)
{
	const auto Set = DirectionHandler::GetSingleton()->AnimationSet(actor);
	return PokeLineTable[static_cast<int>(Settings::ActiveDirectionMode)][static_cast<int>(Set)];
}

float AttackHandler::LineReach(RE::Actor* actor, Directions line, bool power)
{
	return actor->GetReach() * (!power && line == PokeLine(actor) ? PokeReachMult : 1.f);
}

void AttackHandler::Initialize()
{
	RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
	FeintFX = DataHandler->LookupForm<RE::SpellItem>(0x6E60, "DirectionMod.esp");
	WeaponSpeedBuff = DataHandler->LookupForm<RE::SpellItem>(0x7928, "DirectionMod.esp");

	ActionAttack = (RE::BGSAction*)RE::TESForm::LookupByID(0x13005);
	ActionPowerAttack = (RE::BGSAction*)RE::TESForm::LookupByID(0x13383);

	for (auto& iter : RE::PlayerCharacter::GetSingleton()->GetRace()->attackDataMap->attackDataMap)
	{
		if (iter.first == "attackStart")
		{
			PlayerAttackData = iter.second;
		}
	}
}


bool AttackHandler::InChamberWindow(RE::Actor* actor)
{
	bool ret = false;
	ChamberWindowMtx.lock_shared();
	ret = ChamberWindow.contains(actor->GetHandle());
	ChamberWindowMtx.unlock_shared();
	return ret;
}

float AttackHandler::GetChamberWindowTime(RE::Actor* actor)
{
	float ret = 0.f;
	ChamberWindowMtx.lock_shared();
	if (ChamberWindow.contains(actor->GetHandle()))
	{
		ret = ChamberWindow.at(actor->GetHandle());
	}
	ChamberWindowMtx.unlock_shared();
	return ret;
}

bool AttackHandler::InFeintWindow(RE::Actor* actor)
{
	bool ret = false;
	FeintWindowMtx.lock_shared();
	ret = FeintWindow.contains(actor->GetHandle());
	FeintWindowMtx.unlock_shared();
	return ret;
}

float AttackHandler::SecondsSinceFeint(RE::Actor* actor)
{
	std::shared_lock lock(FeintWindowMtx);
	auto It = LastFeintAt.find(actor->GetHandle());
	if (It == LastFeintAt.end())
	{
		return FLT_MAX;
	}
	return std::chrono::duration<float>(std::chrono::steady_clock::now() - It->second).count();
}

void AttackHandler::RemoveFeintWindow(RE::Actor* actor)
{
	// Exclusive lock — erase is a write, and mutating under a shared lock is UB.
	std::unique_lock lock(FeintWindowMtx);
	FeintWindow.erase(actor->GetHandle());
}

bool AttackHandler::CanAttack(RE::Actor* actor)
{
	// We drive attacks ourselves instead of through the combat AI that already
	// honors this flag, so we check it here.
	if (IsAttackingDisabled(actor))
	{
		return false;
	}
	bool ret = false;

	AttackLockoutMtx.lock_shared();
	ret = !AttackLockout.contains(actor->GetHandle());
	AttackLockoutMtx.unlock_shared();

	return ret;
}

bool AttackHandler::CanInitiateAttack(RE::Actor* actor)
{
	if (!CanAttack(actor))
	{
		return false;
	}
	if (InFeintWindow(actor) /* old feint: || InFeintQueue(actor) */)
	{
		return false;
	}
	return !LacksAttackStamina(actor);
}

bool AttackHandler::LacksAttackStamina(RE::Actor* actor)
{
	if (!DifficultySettings::AttacksCostStamina)
	{
		return false;
	}
	auto* Values = actor->AsActorValueOwner();
	// An unblockable swing is free, so only the flat floor applies to it.
	const float Needed = DirectionHandler::GetSingleton()->IsUnblockable(actor) ?
		Values->GetPermanentActorValue(RE::ActorValue::kStamina) * DifficultySettings::StaminaCost :
		SwingStaminaCost(actor, false);
	return Values->GetActorValue(RE::ActorValue::kStamina) < Needed;
}

float AttackHandler::SwingStaminaCost(RE::Actor* actor, bool Power)
{
	const unsigned Repeat = std::min(3u, DirectionHandler::GetSingleton()->GetRepeatCount(actor));
	float Cost = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina) *
		DifficultySettings::StaminaCost * RepeatCostMult[Repeat];
	if (auto* Equipped = actor->GetEquippedObject(false))
	{
		Cost += Equipped->GetWeight() * DifficultySettings::WeaponWeightStaminaMult;
	}
	if (Power)
	{
		Cost *= DifficultySettings::PowerAttackStaminaMult;
	}
	return Cost;
}

void AttackHandler::HandleFeintChangeDirection(RE::Actor* actor)
{
	Directions dir = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
	if (dir == Directions::TR || dir == Directions::BR)
	{
		if (Settings::IsForHonor())
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(actor, Directions::BL, true, true, true);
		}
		else
		{
			DirectionHandler::GetSingleton()->WantToSwitchTo(actor, Directions::TL, true, true, true);

		}

	}
	else
	{
		DirectionHandler::GetSingleton()->WantToSwitchTo(actor, Directions::TR, true, true, true);
	}

}

bool AttackHandler::HandleFeint(RE::Actor* actor)
{
	auto* Values = actor->AsActorValueOwner();
	const float FeintCost = Values->GetPermanentActorValue(RE::ActorValue::kStamina) * DifficultySettings::FeintStaminaCost;
	const float Stamina = Values->GetActorValue(RE::ActorValue::kStamina);
	bool Feinted = false;
	FeintWindowMtx.lock();
	const bool WindowOpen = FeintWindow.contains(actor->GetHandle());
	if (WindowOpen && Stamina >= FeintCost)
	{
		Feinted = true;
		if (Settings::VerboseLogging)
		{
			logger::info("[feint] {} feints {} from line {} (cost {:.0f} of {:.0f})", Who(actor),
				IsPowerAttacking(actor) ? "power" : "light",
				static_cast<int>(DirectionHandler::GetSingleton()->GetCurrentDirection(actor)), FeintCost, Stamina);
		}
		actor->NotifyAnimationGraph("ForceAttackStop");

		//actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
		actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant)->CastSpellImmediate(FeintFX, false, actor, 0.f, false, 0.f, nullptr);
		Values->DamageActorValue(RE::ActorValue::kStamina, FeintCost);
		// Old feint: forced switch to the other side, a sped-up follow-up thrown 0.2 s later.
		// Restoring it means dropping the NPC EndFeint's own switch call again. The buff
		// is off because the feint only supports a direction change; it isn't a mixup itself.
		// HandleFeintChangeDirection(actor);
		// GiveAttackSpeedBuff(actor);
		FeintWindow.erase(actor->GetHandle());
		LastFeintAt[actor->GetHandle()] = std::chrono::steady_clock::now();
		// AddFeintQueue(actor);
	}
	FeintWindowMtx.unlock();
	if (WindowOpen && !Feinted && Settings::VerboseLogging)
	{
		logger::info("[feint] {} can't afford a feint (cost {:.0f} of {:.0f})", Who(actor), FeintCost, Stamina);
	}
	return Feinted;
}

void AttackHandler::AddChamberWindow(RE::Actor* actor)
{
	// masterstrike window must be a fixed time, otherwise slower attack speeds actually make it easier to masterstrike
	ChamberWindowMtx.lock();
	ChamberWindow[actor->GetHandle()] = DifficultySettings::ChamberWindowTime;
	ChamberWindowMtx.unlock();
}

void AttackHandler::AddFeintWindow(RE::Actor* actor)
{
	FeintWindowMtx.lock();
	FeintWindow[actor->GetHandle()] = DifficultySettings::FeintWindowTime;
	FeintWindowMtx.unlock();
}

void AttackHandler::RemoveLockout(RE::Actor* actor)
{

	AttackLockoutMtx.lock();
	if (AttackLockout.contains(actor->GetHandle()))
	{
		AttackLockout.erase(actor->GetHandle());
	}
	AttackLockoutMtx.unlock();

}

void AttackHandler::AddLockout(RE::Actor* actor)
{
	// this hack is necessary because of the way MCO works. it will queue an attack on the animation graph once the player clicks after the
	// nextattack window is passed. during this time, the player can still get hit and commit a lockout. but, the queued animation will still play
	// because it is already being parsed by the animation graph, and the plugin cannot intercept it. so we have to force all attacks to end. 
	if (actor->IsAttacking())
	{
		ResetAttackState(actor);
	}

	{
		AttackLockoutMtx.lock();
		AttackLockout[actor->GetHandle()] = DifficultySettings::AttackTimeoutTime;
		AttackLockoutMtx.unlock();
	}
	// Losing the initiative ends the chain: the next swing opens from neutral.
	DirectionHandler::GetSingleton()->ResetCombo(actor);
}

void AttackHandler::ResetAttackState(RE::Actor* actor)
{
	actor->NotifyAnimationGraph("attackStop");
	// What AttackStopHandler::Process writes, for a graph that won't emit attackStop.
	if (auto* Process = actor->GetActorRuntimeData().currentProcess; Process && Process->high)
	{
		Process->high->attackData.reset();
	}
	actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
}

bool AttackHandler::ClearStuckAttack(RE::Actor* actor, float& idleSeconds, float delta)
{
	const auto State = actor->AsActorState()->GetAttackState();
	// Bow states are left alone, and so is a graph without the variables.
	const bool Melee = State >= RE::ATTACK_STATE_ENUM::kDraw && State <= RE::ATTACK_STATE_ENUM::kBash;
	bool GraphAttacking = false;
	bool GraphBashing = false;
	const bool Known = actor->GetGraphVariableBool("IsAttacking", GraphAttacking) &&
		actor->GetGraphVariableBool("IsBashing", GraphBashing);
	if (!Melee || !Known || GraphAttacking || GraphBashing)
	{
		idleSeconds = 0.f;
		return false;
	}
	idleSeconds += delta;
	if (idleSeconds < StuckAttackGraceSeconds)
	{
		return false;
	}
	logger::warn("[attack] {} {:08X} state {} cleared: nothing attacking in the graph for {:.1f}s",
		actor->GetName(), actor->GetFormID(), static_cast<int>(State), idleSeconds);
	ResetAttackState(actor);
	idleSeconds = 0.f;
	return true;
}

// Old feint:
// void AttackHandler::AddFeintQueue(RE::Actor* actor)
// {
// 	FeintQueueMtx.lock();
// 	if (!FeintQueue.contains(actor->GetHandle()))
// 	{
// 		FeintQueue[actor->GetHandle()] = FeintQueueTime;
// 	}
// 	FeintQueueMtx.unlock();
// }

bool DoAction(RE::Actor* actor, RE::BGSAction* action, const char* animEvent,
	bool allowPlayerEvent = false)
{
	// this is the REAL way of getting an npc to do an attack action, modders take notes
	std::unique_ptr<RE::TESActionData> data(RE::TESActionData::Create());
	data->source = RE::NiPointer<RE::TESObjectREFR>(actor);
	data->action = action;
	// Vanilla combat AI (CombatBehaviorContextMelee::StartAttack) fills the event
	// from the attack it chose
	if (animEvent && (allowPlayerEvent || !actor->IsPlayerRef()))
	{
		auto* Race = actor->GetRace();
		const RE::BSFixedString Event(animEvent);
		if (Race && Race->attackDataMap && Race->attackDataMap->attackDataMap.find(Event) != Race->attackDataMap->attackDataMap.end())
		{
			data->animEvent = Event;
		}
	}
	typedef bool func_t(RE::TESActionData*);
	REL::Relocation<func_t> func{ RELOCATION_ID(40551, 41557) };
	bool succ = func(data.get());
	if (!succ)
	{
		if (Settings::VerboseLogging) logger::info("[attack] failed attack action! {}", Who(actor));
	}
	return succ;
}

void AttackHandler::DoAttack(RE::Actor* actor)
{
	DoAction(actor, ActionAttack, "attackStart");
}

void AttackHandler::DoPowerAttack(RE::Actor* actor)
{
	DoAction(actor, ActionPowerAttack, "attackPowerStartInPlace");
}

// skyrim doesnt have attackdata for bashstart, internally they just use the normal attack data
bool AttackHandler::DoBash(RE::Actor* actor)
{
	return DoAction(actor, ActionAttack, "bashStart", true);
}

void AttackHandler::Cleanup()
{

	ChamberWindowMtx.lock();
	ChamberWindow.clear();
	ChamberWindowMtx.unlock();

	AttackLockoutMtx.lock();
	AttackLockout.clear();
	AttackLockoutMtx.unlock();

	FeintWindowMtx.lock();
	FeintWindow.clear();
	LastFeintAt.clear();
	FeintWindowMtx.unlock();

	{
		// One pass: back out each actor's applied net, then drop everything.
		std::unique_lock lock(SpeedModsMtx);
		for (auto& Entry : SpeedMods)
		{
			if (!Entry.first)
			{
				continue;
			}
			if (RE::Actor* actor = Entry.first.get().get())
			{
				if (Entry.second.appliedDelta != 0.f)
				{
					actor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kWeaponSpeedMult, -Entry.second.appliedDelta);
				}
				if (Entry.second.appliedMoveDelta != 0.f)
				{
					actor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, -Entry.second.appliedMoveDelta);
				}
			}
		}
		SpeedMods.clear();
	}
}

void AttackHandler::RemoveActor(RE::ActorHandle actor)
{

	ChamberWindowMtx.lock();
	ChamberWindow.erase(actor);
	ChamberWindowMtx.unlock();
	AttackLockoutMtx.lock();
	AttackLockout.erase(actor);
	AttackLockoutMtx.unlock();
	FeintWindowMtx.lock();
	FeintWindow.erase(actor);
	FeintWindowMtx.unlock();

	RE::Actor* a = actor.get().get();

	{
		std::unique_lock lock(SpeedModsMtx);
		auto Iter = SpeedMods.find(actor);
		if (Iter != SpeedMods.end())
		{
			if (a && Iter->second.appliedDelta != 0.f)
			{
				a->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kWeaponSpeedMult, -Iter->second.appliedDelta);
			}
			if (a && Iter->second.appliedMoveDelta != 0.f)
			{
				a->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, -Iter->second.appliedMoveDelta);
			}
			SpeedMods.erase(Iter);
		}
	}
}

// The single point that writes WeaponSpeedMult. Backs out the previously
// applied value and applies the new net — never a recomputed removal, so the
// two can't drift even if a slot's inputs changed underneath.

void AttackHandler::ApplySpeedNet(RE::Actor* actor, SpeedEntry& Entry)
{
	const float Target = Entry.Chain.Live() + Entry.SmallChain.Live() +
		Entry.Charge.Live() + Entry.ChainLine.Live();
	if (Target == Entry.appliedDelta)
	{
		return;
	}
	if (Entry.appliedDelta != 0.f)
	{
		actor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kWeaponSpeedMult, -Entry.appliedDelta);
	}
	if (Target != 0.f)
	{
		actor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kWeaponSpeedMult, Target);
	}
	Entry.appliedDelta = Target;
}

void AttackHandler::ApplyMoveNet(RE::Actor* actor, SpeedEntry& Entry)
{
	const float Target = Entry.ComboSlow.Live();
	if (Target == Entry.appliedMoveDelta)
	{
		return;
	}
	if (Entry.appliedMoveDelta != 0.f)
	{
		actor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, -Entry.appliedMoveDelta);
	}
	if (Target != 0.f)
	{
		actor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, Target);
	}
	Entry.appliedMoveDelta = Target;
}

void AttackHandler::AddComboSlow(RE::Actor* actor)
{
	std::unique_lock lock(SpeedModsMtx);
	auto& Entry = SpeedMods[actor->GetHandle()];
	Entry.ComboSlow.Set(std::max(Entry.ComboSlow.Live() + ComboSlowPerStep, ComboSlowMax),
		DifficultySettings::ComboResetTimer);
	ApplyMoveNet(actor, Entry);
}

void AttackHandler::ClearComboSlow(RE::Actor* actor)
{
	std::unique_lock lock(SpeedModsMtx);
	auto Iter = SpeedMods.find(actor->GetHandle());
	if (Iter == SpeedMods.end())
	{
		return;
	}
	Iter->second.ComboSlow.Set(0.f, 0.f);
	ApplyMoveNet(actor, Iter->second);
}

void AttackHandler::GiveAttackSpeedBuff(RE::Actor* actor)
{
	std::unique_lock lock(SpeedModsMtx);
	auto& Entry = SpeedMods[actor->GetHandle()];
	Entry.Chain.Set(AttackSpeedMult, 1.f);
	ApplySpeedNet(actor, Entry);
}

void AttackHandler::GiveSmallAttackSpeedBuff(RE::Actor* actor)
{
	std::unique_lock lock(SpeedModsMtx);
	auto& Entry = SpeedMods[actor->GetHandle()];
	Entry.SmallChain.Set(SmallAttackSpeedMult, 2.f);
	ApplySpeedNet(actor, Entry);
}

// Signed: this slot carries the NET attack-speed delta at swing start, not just
// the charge bonus. The same-side penalty rides here rather than becoming a
// fourth WeaponSpeedMult consumer — three already write to that actor value
// (AttackSpeedMult, SmallAttackSpeedMult, and this), each with its own removal
// sites, and a missed removal is permanent speed drift on that actor
void AttackHandler::GiveChargeSpeedBuff(RE::Actor* actor, float Ratio)
{
	std::unique_lock lock(SpeedModsMtx);
	auto& Entry = SpeedMods[actor->GetHandle()];
	Entry.Charge.Set(std::clamp(Ratio, 0.f, 1.f) * DifficultySettings::GuardChargeMaxSpeedBonus,
		ChargeBuffDuration);
	ApplySpeedNet(actor, Entry);
}

float AttackHandler::GetAttackSpeedDelta(RE::Actor* actor) const
{
	std::shared_lock lock(SpeedModsMtx);
	auto Iter = SpeedMods.find(actor->GetHandle());
	return Iter != SpeedMods.end() ? Iter->second.appliedDelta : 0.f;
}

void AttackHandler::SetChainLinePenalty(RE::Actor* actor, float Penalty)
{
	std::unique_lock lock(SpeedModsMtx);
	auto& Entry = SpeedMods[actor->GetHandle()];
	Entry.ChainLine.Set(-Penalty, Penalty > 0.f ? ChargeBuffDuration : 0.f);
	ApplySpeedNet(actor, Entry);
}

void AttackHandler::RemoveSmallAttackSpeedBuff(RE::Actor* actor)
{
	std::unique_lock lock(SpeedModsMtx);
	auto Iter = SpeedMods.find(actor->GetHandle());
	if (Iter == SpeedMods.end())
	{
		return;
	}
	Iter->second.SmallChain.Set(0.f, 0.f);
	ApplySpeedNet(actor, Iter->second);
}

void AttackHandler::Update(float delta)
{
	{
		ChamberWindowMtx.lock();
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
		ChamberWindowMtx.unlock();

	}

	{

		AttackLockoutMtx.lock();
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
				// Same as the stagger case: a blocked attack recoils out to a
				// default idle instead of the directional one. Skipped
				// mid-attack so a ForceIdle can't cancel a fresh swing.
				if (!actor->IsAttacking())
				{
					DirectionHandler::GetSingleton()->QueueAnimationEvent(actor);
				}
				AttackIter = AttackLockout.erase(AttackIter);
				continue;
			}
			AttackIter++;
		}
		AttackLockoutMtx.unlock();
	}


	{
		FeintWindowMtx.lock();
		auto FeintIter = FeintWindow.begin();
		while (FeintIter != FeintWindow.end())
		{
			if (!FeintIter->first)
			{
				FeintIter = FeintWindow.erase(FeintIter);
				continue;
			}
			RE::Actor* actor = FeintIter->first.get().get();
			if (!actor)
			{
				FeintIter = FeintWindow.erase(FeintIter);
				continue;
			}
			FeintIter->second -= delta;
			if (FeintIter->second <= 0)
			{
				FeintIter = FeintWindow.erase(FeintIter);
				continue;
			}
			FeintIter++;
		}
		FeintWindowMtx.unlock();

	}

	// Old feint: throws the follow-up when the queue expires.
	// {
	// 	FeintQueueMtx.lock();
	// 	auto Iter = FeintQueue.begin();
	// 	while (Iter != FeintQueue.end())
	// 	{
	// 		if (!Iter->first)
	// 		{
	// 			Iter = FeintQueue.erase(Iter);
	// 			continue;
	// 		}
	// 		RE::Actor* actor = Iter->first.get().get();
	// 		if (!actor)
	// 		{
	// 			Iter = FeintQueue.erase(Iter);
	// 			continue;
	// 		}
	// 		Iter->second -= delta;
	// 		if (Iter->second <= 0)
	// 		{
	// 			DoAttack(actor);
	// 			Iter = FeintQueue.erase(Iter);
	// 			continue;
	// 		}
	// 		Iter++;
	// 	}
	// 	FeintQueueMtx.unlock();
	// }

	{
		// One expiry pass for every speed contribution. Each slot runs its own
		// clock; when one lapses the net is recomputed and the actor value moved
		// to it, so a lapsing slot can never remove more (or less) than it added.
		std::unique_lock lock(SpeedModsMtx);
		auto Iter = SpeedMods.begin();
		while (Iter != SpeedMods.end())
		{
			if (!Iter->first)
			{
				Iter = SpeedMods.erase(Iter);
				continue;
			}
			RE::Actor* actor = Iter->first.get().get();
			if (!actor)
			{
				Iter = SpeedMods.erase(Iter);
				continue;
			}
			SpeedEntry& Entry = Iter->second;
			Entry.Chain.Tick(delta);
			Entry.SmallChain.Tick(delta);
			Entry.Charge.Tick(delta);
			Entry.ChainLine.Tick(delta);
			Entry.ComboSlow.Tick(delta);
			ApplySpeedNet(actor, Entry);
			ApplyMoveNet(actor, Entry);
			// Nothing live and nothing applied: drop the entry entirely.
			if (!Entry.Chain.Active() && !Entry.SmallChain.Active() &&
				!Entry.Charge.Active() && !Entry.ChainLine.Active() &&
				!Entry.ComboSlow.Active() &&
				Entry.appliedDelta == 0.f && Entry.appliedMoveDelta == 0.f)
			{
				Iter = SpeedMods.erase(Iter);
				continue;
			}
			++Iter;
		}
	}

	{
		AttackChainMtx.lock();
		auto Iter = AttackChains.begin();
		while (Iter != AttackChains.end())
		{
			if (!Iter->first)
			{
				Iter = AttackChains.erase(Iter);
				continue;
			}
			RE::Actor* actor = Iter->first.get().get();
			if (!actor)
			{
				Iter = AttackChains.erase(Iter);
				continue;
			}
			Iter->second.timeLeft -= delta;
			if (Iter->second.timeLeft <= 0)
			{
				Iter = AttackChains.erase(Iter);
				continue;
			}
			Iter++;
		}
		AttackChainMtx.unlock();
	}

}
