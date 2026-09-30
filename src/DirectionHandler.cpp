#include "DirectionHandler.h"
#include "AIHandler.h"
#include "InputHandler.h"
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

// Guard charge: seconds of holding one guard line to reach a full charge, and
// the WeaponSpeedMult bonus granted at full
static float ChargeRatioFromSeconds(float seconds)
{
	// guard against a misconfigured INI where Full <= Startup (would divide by
	// zero or invert the ramp): treat the ramp as instant past startup
	const float Span = DifficultySettings::GuardChargeFull - DifficultySettings::GuardChargeStartup;
	if (Span <= 0.f)
	{
		return seconds >= DifficultySettings::GuardChargeStartup ? 1.f : 0.f;
	}
	return std::clamp((seconds - DifficultySettings::GuardChargeStartup) / Span, 0.f, 1.f);
}


Directions DirectionHandler::FoldDirection(Directions dir)
{
	if (Settings::IsForHonor() && dir == Directions::TL)
	{
		return Directions::TR;
	}
	if (Settings::IsKCD() && dir == Directions::BR)
	{
		return Directions::BL;
	}
	return dir;
}

uint8_t DirectionHandler::EnabledDirections()
{
	uint8_t Mask = 0;
	for (Directions Dir : { Directions::TR, Directions::TL, Directions::BL, Directions::BR })
	{
		if (DirectionEnabled(Dir))
		{
			Mask |= static_cast<uint8_t>(1u << static_cast<int>(Dir));
		}
	}
	return Mask;
}

// The line that answers an attack: its mirror, folded into the mode. In a mode
// where the mirror has been folded away the attack blocks on its own line, which
// is what the top guard does in For Honor and the low guard does in KCD.
static Directions MirrorDirection(Directions dir)
{
	switch (dir)
	{
	case Directions::TR: return Directions::TL;
	case Directions::TL: return Directions::TR;
	case Directions::BL: return Directions::BR;
	case Directions::BR: return Directions::BL;
	}
	return dir;
}

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
	ComboChain1 = DataHandler->LookupForm<RE::SpellItem>(0x8953, PluginName);
	ComboChain2 = DataHandler->LookupForm<RE::SpellItem>(0x8954, PluginName);
	if (!ComboChain1 || !ComboChain2)
	{
		logger::error("DirectionHandler: combo chain spells 0x8953/0x8954 not found in {}", PluginName);
	}
	ForHonorMarker = DataHandler->LookupForm<RE::SpellItem>(0x8EBA, PluginName);
	KCDMarker = DataHandler->LookupForm<RE::SpellItem>(0x8EBB, PluginName);
	if (!ForHonorMarker || !KCDMarker)
	{
		logger::error("DirectionHandler: mode marker spells 0x8EBA/0x8EBB not found in {}", PluginName);
	}
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

bool DirectionHandler::IsDirectionalOpponent(RE::Actor* actor) const
{
	return HasDirectionalPerks(actor) || CreatureHandler::GetSingleton()->IsCreatureOpponent(actor);
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
	// Creature lookup before the lock below: it takes its own and must not nest.
	Directions creatureDir = Directions::TR;
	const bool creature = CreatureHandler::GetSingleton()->GetAttackLine(attacker, creatureDir);
	std::shared_lock lock(ActiveDirectionsMtx);
	// find() gives existence and the iterator in one lookup; each side is
	// dereferenced at most once rather than re-queried per direction branch.
	auto attackerIt = ActiveDirections.find(attacker->GetHandle());
	auto targetIt = ActiveDirections.find(target->GetHandle());
	if (targetIt == ActiveDirections.end() || (attackerIt == ActiveDirections.end() && !creature))
	{
		return false;
	}
	const Directions attackDir = attackerIt != ActiveDirections.end() ? attackerIt->second : creatureDir;
	const Directions targetDir = targetIt->second;
	// The mirrored line, folded into the mode.
	return targetDir == FoldDirection(MirrorDirection(attackDir));
}

bool DirectionHandler::ShouldShowMarker(RE::Actor* actor) const
{
	// ignore ui past setting distance
	float TargetDist = RE::PlayerCamera::GetSingleton()->GetRuntimeData2().pos.GetSquaredDistance(actor->GetPosition());
	if (TargetDist > (UISettings::DisplayDistance * UISettings::DisplayDistance))
	{
		return false;
	}

	if (Settings::HasTDM && TDM)
	{
		if (UISettings::OnlyShowTargetted)
		{
			if (actor->GetHandle() != TDM->GetCurrentTarget())
			{
				return false;
			}
		}
	}
	else if (UISettings::OnlyShowTargetted)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!actor->IsHostileToActor(player))
		{
			return false;
		}
		if (actor->GetActorRuntimeData().currentCombatTarget != player->GetHandle())
		{
			return false;
		}
	}
	return true;
}

void DirectionHandler::UIDrawAngles(RE::Actor* actor)
{

	if (!UISettings::ShowUI)
	{
		return;
	}
	if (!actor->IsPlayerRef() && !ShouldShowMarker(actor))
	{
		return;
	}

	// Query before taking ActiveDirectionsMtx: the AI handler takes its own
	// lock and is itself a caller of DirectionHandler while holding it, so
	// nesting the two here would invert that order
	std::array<int, 4> Conditioning{};
	float Confidence = 0.f;
	const float ComboFlash = GetComboFlash(actor);
	const float ComboProgress = GetComboProgress(actor);
	if (UISettings::ShowConditioningArcs)
	{
		AIHandler::GetSingleton()->GetGuardConditioningExternalCalled(actor, Conditioning, Confidence);
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

		const Directions Drawn = ActiveDirections.at(actor->GetHandle());
		UI::AddDrawCommand(Position, Drawn, Mirror, state, ComboFlash, ComboProgress, hostileState, FirstPerson, Lockout, actor->IsPlayerRef(), Conditioning, actor->GetHandle().native_handle(), Confidence, EnabledDirections());
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
		float headingAngle = actor->GetHeadingAngle(RE::PlayerCamera::GetSingleton()->GetRuntimeData2().pos, true);
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

bool DirectionHandler::CanSwitch(RE::Actor* actor) const
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

float DirectionHandler::GetGuardChargeRatio(RE::Actor* actor) const
{
	std::shared_lock lock(GuardChargeMtx);
	auto Iter = GuardCharge.find(actor->GetHandle());
	if (Iter == GuardCharge.end())
	{
		return 0.f;
	}
	return ChargeRatioFromSeconds(Iter->second);
}

float DirectionHandler::ConsumeGuardCharge(RE::Actor* actor)
{
	std::unique_lock lock(GuardChargeMtx);
	auto Iter = GuardCharge.find(actor->GetHandle());
	if (Iter == GuardCharge.end())
	{
		return 0.f;
	}
	const float ratio = ChargeRatioFromSeconds(Iter->second);
	Iter->second = 0.f;
	return ratio;
}

void DirectionHandler::ResetGuardCharge(RE::Actor* actor)
{
	std::unique_lock lock(GuardChargeMtx);
	auto Iter = GuardCharge.find(actor->GetHandle());
	if (Iter != GuardCharge.end())
	{
		Iter->second = 0.f;
	}
}

void DirectionHandler::ApplyDirectionSpells(RE::Actor* actor, Directions dir)
{
	RE::SpellItem* DirectionSpell = GetDirectionalPerk(actor);
	RE::SpellItem* SpellToAdd = DirectionToPerk(dir);
	if (!DirectionSpell)
	{
		actor->AddSpell(SpellToAdd);
	}

	if (PerkToDirection(DirectionSpell) != dir)
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
}

uint32_t DirectionHandler::GetLockedTargetFormID() const
{
	RE::Actor* Locked = GetLockedTarget();
	return Locked ? Locked->GetFormID() : 0;
}

RE::Actor* DirectionHandler::GetLockedTarget() const
{
	if (!Settings::HasTDM || !TDM)
	{
		return nullptr;
	}
	auto Handle = TDM->GetCurrentTarget();
	return Handle ? Handle.get().get() : nullptr;
}

RE::Actor* DirectionHandler::GetSwingTarget(RE::Actor* actor) const
{
	RE::Actor* Target = actor->IsPlayerRef() ? GetLockedTarget() : nullptr;
	return Target ? Target : GetCombatTarget(actor);
}

Directions DirectionHandler::GetPreviousDirection(RE::Actor* actor, float& outAgo) const
{
	std::shared_lock lock(SwitchHistoryMtx);
	auto Iter = SwitchHistory.find(actor->GetHandle());
	if (Iter == SwitchHistory.end())
	{
		outAgo = -1.f;
		return GetCurrentDirection(actor);
	}
	outAgo = std::chrono::duration<float>(std::chrono::steady_clock::now() - Iter->second.at).count();
	return Iter->second.previous;
}

void DirectionHandler::SwitchDirectionSynchronous(RE::Actor* actor, Directions dir, bool wasBlocking)
{
	// The map only ever holds a live line. Feint events arrive unfolded.
	dir = FoldDirection(dir);
	// there is some funkiness with spells not applying at the same time as directions is being read so hide them under the same mutex
	ActiveDirectionsMtx.lock();
	// Diagnostic history: what the line was before this switch, and when.
	{
		auto Prev = ActiveDirections.find(actor->GetHandle());
		std::unique_lock HistLock(SwitchHistoryMtx);
		auto& Hist = SwitchHistory[actor->GetHandle()];
		Hist.previous = Prev != ActiveDirections.end() ? Prev->second : dir;
		Hist.at = std::chrono::steady_clock::now();
	}
	ApplyDirectionSpells(actor, dir);
	ActiveDirections[actor->GetHandle()] = dir;
	ActiveDirectionsMtx.unlock();

	// A completed switch spends the guard charge
	{
		std::unique_lock ChargeLock(GuardChargeMtx);
		GuardCharge[actor->GetHandle()] = 0.f;
	}

	// A switch un-sets the guard on both clocks.
	BlockHandler::GetSingleton()->ResetBrace(actor);


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

				actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina,staminaCost);
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
	

	// A folded mode has one line on the far side, so the side inputs address it
	// directly instead of flipping — otherwise the merged line is a dead end.
	if (Settings::IsForHonor())
	{
		WantToSwitchTo(actor, Directions::BL);
	}
	else if (Settings::IsKCD())
	{
		WantToSwitchTo(actor, Directions::TL);
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
	if (Settings::IsForHonor())
	{
		WantToSwitchTo(actor, Directions::BR);
	}
	else if (Settings::IsKCD())
	{
		WantToSwitchTo(actor, Directions::TR);
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
	if (Settings::IsForHonor())
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
	dir = FoldDirection(dir);
	// Timer lock first. The update loop commits under it and takes
	// ActiveDirectionsMtx inside, so taking these in the other order here is
	// the one cycle between the two.
	std::unique_lock lock2(DirectionTimersMtx);
	// skip if we try to switch to the same dir
	{
		std::shared_lock lock1(ActiveDirectionsMtx);
		auto CurIter = ActiveDirections.find(actor->GetHandle());
		if (CurIter != ActiveDirections.end() && CurIter->second == dir)
		{
			return;
		}
	}

	auto Iter = DirectionTimers.find(actor->GetHandle());
	// skip if we already have a direction to switch to that is the same
	if (Iter != DirectionTimers.end() && Iter->second.dir == dir)
	{
		return;
	}
	// mostly for feints
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
		ToDir.wasBlocking = actor->IsBlocking();
		
		DirectionTimers[actor->GetHandle()] = ToDir;
	}
}

void DirectionHandler::AddDirectional(RE::Actor* actor, RE::TESObjectWEAP* weapon)
{
	// initialize for correctness
	actor->SetGraphVariableInt("DirModBodyBlendSelect", 0);
	// top right by default; the polearm set is thrusting so it starts low
	Directions Initial = AnimationSet(actor) == WeaponSet::Battleaxe ? Directions::BR : Directions::TR;
	// Direct map write, so it folds here like every other one.
	Initial = FoldDirection(Initial);
	ActiveDirectionsMtx.lock();
	ActiveDirections[actor->GetHandle()] = Initial;
	actor->AddSpell(DirectionToPerk(Initial));
	ActiveDirectionsMtx.unlock();
	ApplyModeMarker(actor);
	// hack to fix unblockable issues
	actor->RemoveSpell(Unblockable);
}

WeaponSet DirectionHandler::AnimationSet(RE::Actor* actor) const
{
	auto* Equipped = actor->GetEquippedObject(false);
	auto* Weapon = Equipped ? Equipped->As<RE::TESObjectWEAP>() : nullptr;
	if (!Weapon)
	{
		return WeaponSet::OneHand;
	}
	if (Weapon->IsTwoHandedSword())
	{
		return WeaponSet::TwoHandSword;
	}
	if (Weapon->HasKeyword(BattleaxeKeyword) ||
		(PikeKeyword && Weapon->HasKeyword(PikeKeyword)) ||
		(PikeKeyword2 && Weapon->HasKeyword(PikeKeyword2)))
	{
		return WeaponSet::Battleaxe;
	}
	if (Weapon->IsTwoHandedAxe())
	{
		// The battleaxe is the odd one out; every other two-handed axe is a hammer.
		return WeaponSet::Warhammer;
	}
	return HasShield(actor) ? WeaponSet::OneHandShield : WeaponSet::OneHand;
}

// One marker at a time, matching the active mode. Normal mode carries neither.
void DirectionHandler::ApplyModeMarker(RE::Actor* actor)
{
	RE::SpellItem* Wanted = Settings::IsForHonor() ? ForHonorMarker :
		Settings::IsKCD() ? KCDMarker : nullptr;
	for (RE::SpellItem* Marker : { ForHonorMarker, KCDMarker })
	{
		if (Marker && Marker != Wanted && actor->HasSpell(Marker))
		{
			actor->RemoveSpell(Marker);
		}
	}
	if (Wanted && !actor->HasSpell(Wanted))
	{
		actor->AddSpell(Wanted);
	}
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

	GuardChargeMtx.lock();
	GuardCharge.erase(handle);
	GuardChargeMtx.unlock();

	RE::Actor* actor = handle.get().get();
	if (!actor)
	{
		return;
	}
	actor->NotifyAnimationGraph("DirmodStop");
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
	
	for (RE::SpellItem* Marker : { ForHonorMarker, KCDMarker })
	{
		if (Marker && actor->HasSpell(Marker))
		{
			actor->RemoveSpell(Marker);
		}
	}
	if (actor->HasPerk(Debuff))
	{
		actor->RemovePerk(Debuff);
	}
	ApplyComboChain(actor, 0);
	// do some cleanup here
	//AIHandler::GetSingleton()->RemoveActor(actor);
}

bool DirectionHandler::CanHaveDirectionalPerks(RE::Actor* actor, RE::TESObjectWEAP*& outWeapon) const
{
	outWeapon = nullptr;
	if (!actor) return false;
	if (actor->IsDead() || actor->IsMarkedForDeletion()) return false;
	// The guard animations only exist on the humanoid graph. Anything else is
	// a creature, weapon or not — a giant's club must not make it a fencer.
	if (CreatureHandler::GetSingleton()->IsCreatureGraph(actor->GetRace())) return false;

	auto Equipped = actor->GetEquippedObject(false);
	auto EquippedLeft = actor->GetEquippedObject(true);
	// Only weapons (no H2H)
	if ((!Equipped || !Equipped->IsWeapon()) &&
		(!EquippedLeft || !EquippedLeft->IsWeapon()))
	{
		return false;
	}

	// check if non melee weapon
	RE::TESObjectWEAP* Weapon = nullptr;
	bool HasWeapon = false;
	if (Equipped)
	{
		Weapon = Equipped->As<RE::TESObjectWEAP>();
		if (Weapon && Weapon->IsMelee())
		{
			HasWeapon = true;
		}
	}

	if (!HasWeapon) return false;
	// check h2h here
	if (Weapon && Weapon->IsHandToHandMelee() && !Settings::EnableForH2H) return false;

	outWeapon = Weapon;
	return true;
}

bool DirectionHandler::ShouldHaveDirectionalPerks(RE::Actor* actor, RE::TESObjectWEAP*& outWeapon)
{
	outWeapon = nullptr;

	// TDMOnlyLockedHumanoids: when enabled, the player only receives directional
	// perks while target-locked (via TDM) on a perked actor. 
	if (Settings::TDMOnlyLockedHumanoids && actor->IsPlayerRef() && Settings::HasTDM && TDM)
	{
		bool hasLockedCapableTarget = false;
		auto targetHandle = TDM->GetCurrentTarget();
		if (auto targetPtr = targetHandle.get())
		{
			RE::Actor* targetActor = targetPtr.get();
			if (targetActor)
			{
				RE::TESObjectWEAP* unused = nullptr;
				if (CanHaveDirectionalPerks(targetActor, unused))
				{
					hasLockedCapableTarget = true;
				}
			}
		}

		if (!hasLockedCapableTarget)
		{
			if (HasDirectionalPerks(actor))
			{
				if (Settings::VerboseLogging) logger::info("[perk] {} removed: TDMOnlyLockedHumanoids gate (no capable target locked)", actor->GetName());
				ToRemoveMtx.lock();
				ToRemove.insert(actor->GetHandle());
				ToRemoveMtx.unlock();
			}
			return false;
		}
	}

	const RE::WEAPON_STATE WeaponState = actor->AsActorState()->GetWeaponState();
	// The far cutoff is handled by UpdateCharacter, the only caller.
	const float SQDist = RE::PlayerCharacter::GetSingleton()->GetPosition().GetSquaredDistance(actor->GetPosition());

	if (SQDist > Settings::ActiveDistance * Settings::ActiveDistance)
	{
		if (!HasDirectionalPerks(actor))
		{
			// hysteresis zone — don't add yet, but also don't remove. Skip update.
			return false;
		}
	}
	// Accept kWantToDraw and kDrawing as well as kDrawn
	if (WeaponState != RE::WEAPON_STATE::kDrawn
		&& WeaponState != RE::WEAPON_STATE::kDrawing
		&& WeaponState != RE::WEAPON_STATE::kWantToDraw)
	{
		if (HasDirectionalPerks(actor))
		{
			if (Settings::VerboseLogging) logger::info("[perk] {} removed cause weapon is not drawn", actor->GetName());
			ToRemoveMtx.lock();
			ToRemove.insert(actor->GetHandle());
			ToRemoveMtx.unlock();
		}
		return false;
	}

	// Self-side capability — race/equipment/alive. Removal queueing stays here;
	// CanHaveDirectionalPerks is side-effect free.
	if (!CanHaveDirectionalPerks(actor, outWeapon))
	{
		if (HasDirectionalPerks(actor))
		{
			if (Settings::VerboseLogging) logger::info("[perk] {} removed: not capable (dead/no melee weapon/h2h disabled)", actor->GetName());
			ToRemoveMtx.lock();
			ToRemove.insert(actor->GetHandle());
			ToRemoveMtx.unlock();
		}
		return false;
	}

	// Engagement signal is per-actor-type. NEVER mix:
	//   - Player → TDM lock target. currentCombatTarget is unreliable for the
	//     player (lags TDM, can be null while locked, can be wrong actor).
	//   - NPC → currentCombatTarget. NPCs don't have a TDM equivalent.
	if (Settings::TDMOnlyLockedHumanoids)
	{
		RE::Actor* target = nullptr;

		if (actor->IsPlayerRef())
		{
			if (Settings::HasTDM && TDM)
			{
				if (auto lockedHandle = TDM->GetCurrentTarget())
				{
					if (auto lockedPtr = lockedHandle.get())
					{
						target = lockedPtr.get();
					}
				}
			}
		}
		else
		{
			target = actor->GetActorRuntimeData().currentCombatTarget.get().get();
		}

		bool targetEligible = false;
		if (target)
		{
			RE::TESObjectWEAP* unused = nullptr;
			targetEligible = CanHaveDirectionalPerks(target, unused);
		}

		if (!targetEligible)
		{
			if (HasDirectionalPerks(actor))
			{
				if (Settings::VerboseLogging)
				{
					logger::info("{} removed: {}", actor->GetName(),
						target ? "target not capable of directional combat" : "no engagement target");
				}
				ToRemoveMtx.lock();
				ToRemove.insert(actor->GetHandle());
				ToRemoveMtx.unlock();
			}
			outWeapon = nullptr;
			return false;
		}
	}

	return true;
}

void DirectionHandler::UpdateCharacter(RE::Actor* actor, float delta)
{
	// Cheapest gate, so it runs first. A distant actor cannot affect anything
	// until it returns, and the reconcile below catches a spell desync then.
	const float SQDist = RE::PlayerCharacter::GetSingleton()->GetPosition().GetSquaredDistance(actor->GetPosition());
	const float FarDelta = Settings::ActiveDistance + 400.f;
	if (SQDist > FarDelta * FarDelta)
	{
		if (HasDirectionalPerks(actor))
		{
			if (Settings::VerboseLogging) logger::info("[perk] {} removed cause too far", actor->GetName());
			ToRemoveMtx.lock();
			ToRemove.insert(actor->GetHandle());
			ToRemoveMtx.unlock();
		}
		return;
	}

	SettingsLoader::GetSingleton()->RetargetStaticBones(actor);

	// Accrue guard charge: holding a line while neither swinging nor blocking
	// faster next attack. 
	if (DifficultySettings::EnableGuardCharge)
	{
		if (HasDirectionalPerks(actor))
		{
			std::unique_lock ChargeLock(GuardChargeMtx);
			float& Charge = GuardCharge[actor->GetHandle()];
			if (actor->IsAttacking() || actor->IsBlocking())
			{
				Charge = 0.f;
			}
			else
			{
				Charge = std::min(Charge + delta, DifficultySettings::GuardChargeFull);
			}
		}
		else
		{
			// Backstop for an actor that lost perks without going through
			// RemoveDirectionalPerks. 
			bool HasEntry = false;
			{
				std::shared_lock ChargeReadLock(GuardChargeMtx);
				HasEntry = GuardCharge.contains(actor->GetHandle());
			}
			if (HasEntry)
			{
				std::unique_lock ChargeLock(GuardChargeMtx);
				GuardCharge.erase(actor->GetHandle());
			}
		}
	}


	// Keep the graph's dirmod state in step with whether this actor currently
	// has perks. Only TDMOnlyLockedHumanoids needs the desync correction — that
	// mode churns add/remove on every lock/unlock, so the graph falls out of
	// step often enough to be worth catching.
	if (Settings::TDMOnlyLockedHumanoids && Settings::HasTDM && TDM)
	{
		const std::int32_t shouldhaveperks = HasDirectionalPerks(actor) ? 1 : 0;
		std::int32_t actual = 0;
		actor->SetGraphVariableInt("iEnableDirmod", shouldhaveperks);
		if (actor->GetGraphVariableInt("iSyncDirmod", actual) && actual != shouldhaveperks)
		{
			// If we're going to fire AddLockout (attackStop) anyway, skip the
			// DirmodStart/Stop event — the attackStop forces a graph transition
			// and the graph picks up the new dirmod state from iEnableDirmod
			// when it re-evaluates. Firing both events back-to-back causes ice
			// skating as the attack animation cancels mid-swing while the
			// dirmod transition is also in flight.
			bool fireLockout = false;
			if (actor->IsPlayerRef() && actor->IsAttacking())
			{
				bool isFlicker = false;
				if (shouldhaveperks == 0)
				{
					if (auto lockedHandle = TDM->GetCurrentTarget())
					{
						if (auto lockedPtr = lockedHandle.get())
						{
							if (auto* lockedActor = lockedPtr.get())
							{
								if (HasDirectionalPerks(lockedActor))
								{
									isFlicker = true;
								}
							}
						}
					}
				}
				fireLockout = !isFlicker;
			}

			if (fireLockout)
			{
				AttackHandler::GetSingleton()->AddLockout(actor);
			}
			else
			{
				actor->NotifyAnimationGraph(shouldhaveperks ? "DirmodStart" : "DirmodStop");
			}
		}
	}
	else
	{
		// Directional mode isn't gated on a lock, so the graph stays enabled
		// rather than tracking perk add/remove.
		actor->SetGraphVariableInt("iEnableDirmod", 1);
		std::int32_t actual = 0;
		if (actor->GetGraphVariableInt("iSyncDirmod", actual) && actual != 1)
		{
			actor->NotifyAnimationGraph("DirmodStart");
		}
	}

	// Reconcile the direction spell against ActiveDirections. Nothing to do
	// with the TDM gate above — AddSpell/RemoveSpell can silently fail in any
	// mode, that one just surfaces it more often.
	ActiveDirectionsMtx.lock_shared();
	bool hasEntry = ActiveDirections.contains(actor->GetHandle());
	Directions desired = hasEntry ? ActiveDirections.at(actor->GetHandle()) : Directions::TR;
	ActiveDirectionsMtx.unlock_shared();

	RE::SpellItem* currentSpell = GetDirectionalPerk(actor);

	if (hasEntry && !currentSpell)
	{
		RE::SpellItem* desiredSpell = DirectionToPerk(desired);
		if (desiredSpell)
		{
			if (Settings::VerboseLogging)
			{
				logger::info("{} reconciling missing direction spell ({})", actor->GetName(), (int)desired);
			}
			actor->AddSpell(desiredSpell);
		}
	}
	// Spell present but on the wrong line. Only reconcile when nothing is
	// pending — a queued switch is allowed to be mid-flight.
	else if (hasEntry && PerkToDirection(currentSpell) != desired)
	{
		Directions Queued;
		if (!HasQueuedDirection(actor, Queued))
		{
			if (Settings::VerboseLogging)
			{
				logger::info("{} reconciling stale direction spell ({} -> {})",
					actor->GetName(), (int)PerkToDirection(currentSpell), (int)desired);
			}
			ApplyDirectionSpells(actor, desired);
		}
	}
	else if (!hasEntry && (currentSpell || actor->HasSpell(Unblockable)))
	{
		if (Settings::VerboseLogging)
		{
			logger::info("{} stripping stale direction spells", actor->GetName());
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
		if (actor->HasSpell(Unblockable))
		{
			actor->RemoveSpell(Unblockable);
		}
		ApplyComboChain(actor, 0);
	}

	RE::TESObjectWEAP* Weapon = nullptr;
	if (!ShouldHaveDirectionalPerks(actor, Weapon))
	{
		CreatureHandler::GetSingleton()->UpdateActor(actor);
		return;
	}

	if (!HasDirectionalPerks(actor))
	{
		actor->NotifyAnimationGraph("DirmodStart");
		AddDirectional(actor, Weapon);
		// temporary
		//actor->AsActorValueOwner()->SetBaseActorValue(RE::ActorValue::kWeaponSpeedMult, 0.1f);
		if (Settings::VerboseLogging) logger::info("[perk] gave {} {} perks", actor->GetName(), actor->GetHandle().native_handle());
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
		// lock direction if sprinting. This is the only place we use the
		// synchronous direction change function since during sprint we
		// cannot change directions.
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

	// AI stuff. A held-off actor decides nothing: switching and dodging drain the
	// bar it needs when it rotates back in.
	if (!actor->IsPlayerRef() && HasDirectionalPerks(actor))
	{
		if (IsAttackingDisabled(actor))
		{
			AIHandler::GetSingleton()->NotifyHeldOff(actor);
		}
		else
		{
			AIHandler::GetSingleton()->RunActor(actor, delta);
		}
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

	GuardChargeMtx.lock();
	GuardCharge.clear();
	GuardChargeMtx.unlock();
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
		// A forced idle under a pending swing orphans it (accepted as kDraw, never played): defer.
		const bool SwingPending = actor->IsAttacking() ||
			(actor->IsPlayerRef() ? InputEventHandler::GetSingleton()->HasBufferedAttack() :
				AIHandler::GetSingleton()->HasSwingQueued(actor));
		if (SwingPending)
		{
			AnimationTimer[actor->GetHandle()].push_back({ SlowTimeBetweenChanges, false });
		}
		else
		{
			SendAnimationEvent(actor, false);
			// reserve, not resize: resize made four zero-timer entries, each a ForceIdle on the next frame.
			AnimationTimer[actor->GetHandle()].reserve(MaxSize);
			AnimationTimer[actor->GetHandle()].push_back({ SlowTimeBetweenChanges, false });
		}
	}
	AnimationTimerMtx.unlock();
}

bool DirectionHandler::ClearAnimationQueue(RE::Actor* actor)
{
	AnimationTimerMtx.lock();
	auto Iter = AnimationTimer.find(actor->GetHandle());
	const bool Cleared = Iter != AnimationTimer.end();
	if (Cleared)
	{
		AnimationTimer.erase(Iter);
	}
	AnimationTimerMtx.unlock();
	return Cleared;
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

			// make sure actor is not in a state that prevents it from switching directions, first
			// this will sit in queue until this happens
			if (Iter->second.timeLeft <= 0.f && CanSwitch(actor))
			{
				SwitchDirectionSynchronous(actor, Iter->second.dir, Iter->second.wasBlocking);
				// A timer rather than a direct ForceIdle notify, so the
				// animation transition can be slower than the logical switch.
				QueueAnimationEvent(actor);
				Iter = DirectionTimers.erase(Iter);
				continue;
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
		std::unique_lock ComboLock(ComboDatasMtx);
		auto ComboIter = ComboDatas.begin();
		while (ComboIter != ComboDatas.end())
		{
			RE::Actor* actor = ComboIter->first ? ComboIter->first.get().get() : nullptr;
			if (!actor)
			{
				ComboIter = ComboDatas.erase(ComboIter);
				continue;
			}

			ComboIter->second.flashLeft = std::max(0.f, ComboIter->second.flashLeft - delta);
			ComboIter->second.timeLeft -= delta;
			if (ComboIter->second.timeLeft <= 0)
			{
				std::unique_lock UnblockableLock(UnblockableActorsMtx);
				if (UnblockableActors.erase(ComboIter->first) > 0)
				{
					actor->RemoveSpell(Unblockable);
				}
				ApplyComboChain(actor, 0);
				ComboIter = ComboDatas.erase(ComboIter);
				continue;
			}

			ComboIter++;
		}
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
		std::unique_lock ChargeLock(GuardChargeMtx);
		auto ChargeIter = GuardCharge.begin();
		while (ChargeIter != GuardCharge.end())
		{
			if (!ChargeIter->first || !ChargeIter->first.get().get())
			{
				ChargeIter = GuardCharge.erase(ChargeIter);
				continue;
			}
			ChargeIter++;
		}
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

void DirectionHandler::AddCombo(RE::Actor* actor, bool ForceComplete, int ExtraSteps)
{
	if (!ForceComplete)
	{
		std::unique_lock UnblockableLock(UnblockableActorsMtx);
		if (UnblockableActors.erase(actor->GetHandle()) > 0)
		{
			actor->RemoveSpell(Unblockable);
			return;
		}
	}

	// An actor with no direction entry has nothing to record.
	Directions CurrentDir = Directions::TR;
	if (!ForceComplete)
	{
		std::shared_lock DirLock(ActiveDirectionsMtx);
		auto DirIter = ActiveDirections.find(actor->GetHandle());
		if (DirIter == ActiveDirections.end())
		{
			return;
		}
		CurrentDir = DirIter->second;
	}

	// Three in every mode; RunAdvances decides what counts.
	constexpr int comboSize = 3;
	{
		std::unique_lock ComboLock(ComboDatasMtx);
		auto Result = ComboDatas.try_emplace(actor->GetHandle());
		if (Result.second)
		{
			Result.first->second.lastAttackDirs.resize(comboSize);
		}
		ComboData& data = Result.first->second;
		if (!ForceComplete)
		{
			const int RunBefore = ComboRun(data);
			const int StepsBefore = RunBefore + data.bonus;
			data.lastAttackDirs[data.currentIdx] = CurrentDir;
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
			// Credited steps only survive while the run keeps growing.
			if (ComboRun(data) <= RunBefore)
			{
				data.bonus = 0;
			}
			data.bonus = std::min(data.bonus + ExtraSteps, comboSize);
			if (ComboRun(data) + data.bonus > StepsBefore)
			{
				data.flashLeft = ComboFlashTime;
			}
		}

		// apply perk
		// clean up all combo stuff if we can apply it
		if (ForceComplete || ComboRun(data) + data.bonus >= comboSize)
		{
			std::unique_lock UnblockableLock(UnblockableActorsMtx);
			if (UnblockableActors.insert(actor->GetHandle()).second)
			{
				actor->AddSpell(Unblockable);
			}
			data.flashLeft = ComboFlashTime;
			data.currentIdx = 0;
			data.size = 0;
			data.repeatCount = 0;
			data.bonus = 0;
		}
		data.timeLeft = DifficultySettings::ComboResetTimer;
		ApplyComboChain(actor, ComboRun(data) + data.bonus);
	}
}

void DirectionHandler::ResetCombo(RE::Actor* actor)
{
	std::unique_lock ComboLock(ComboDatasMtx);
	// make sure we remove unblockable spell
	{
		std::unique_lock UnblockableLock(UnblockableActorsMtx);
		if (UnblockableActors.erase(actor->GetHandle()) > 0)
		{
			actor->RemoveSpell(Unblockable);
		}
	}
	auto Iter = ComboDatas.find(actor->GetHandle());
	if (Iter != ComboDatas.end())
	{
		ApplyComboChain(actor, 0);
		ComboDatas.erase(Iter);
	}
}

void DirectionHandler::ApplyComboChain(RE::Actor* actor, int run)
{
	if (!ComboChain1 || !ComboChain2)
	{
		return;
	}
	RE::SpellItem* Stages[2] = { ComboChain1, ComboChain2 };
	for (int i = 0; i < 2; ++i)
	{
		const bool Want = run == i + 1;
		if (Want && !actor->HasSpell(Stages[i]))
		{
			actor->AddSpell(Stages[i]);
		}
		else if (!Want && actor->HasSpell(Stages[i]))
		{
			actor->RemoveSpell(Stages[i]);
		}
	}
}
