#include "AIHandler.h"
#include <cassert>
#include "DirectionHandler.h"
#include "SettingsLoader.h"
#include "AttackHandler.h"
#include "BlockHandler.h"
#include "DodgeHandler.h"

#include <random>

static std::mt19937 mt_rand(0);
static std::shared_mutex mt_randMtx;

// One pattern per direction subset, so an NPC is known by the guard it never
// attacks into. Adding the four ordering variants would add rows, not character:
// every length-3 pattern has the same shape.
constexpr int TotalAttackCombos = 4;
constexpr int AttackComboLength = 3;
Directions AIAttackCombo[TotalAttackCombos][AttackComboLength] = {
								{Directions::TR, Directions::TL, Directions::BL},  // never BR
								{Directions::TR, Directions::BL, Directions::BR},  // never TL
								{Directions::TL, Directions::BR, Directions::BL},  // never TR
								{Directions::BR, Directions::TL, Directions::TR}, };// never BL

int GetRand()
{
	std::unique_lock lock(mt_randMtx);
	return mt_rand();
}

constexpr int MaxDirs = 5;

// MUST be higher than the direction switch time otherwise it will try to switch directions too quickly and freeze
constexpr float LowestTime = 0.143f;
// Stamina headroom before the AI spends its combo finisher as a power attack.
constexpr float ComboFinisherStaminaRatio = 0.65f;
// simulates human mouse or contorller input to change guard
constexpr float GuardInputSeconds = 0.15f;
// Mirrors DirectionHandler's TimeBetweenChanges todo keep it in a central constexpr file to avoid this horrible hack
constexpr float GuardTransitionSeconds = 0.133f;
// Decision to actually standing on the new line. Nothing shorter than this is
// worth chasing — the answer arrives after the target has already moved on.
constexpr float GuardCommitLatency = GuardInputSeconds + GuardTransitionSeconds;
constexpr float MaxAttackDirWait = GuardCommitLatency + 0.02f;

// Forward travel an attack animation carries the attacker through, rough estimate
constexpr float AttackLungeUnits = 100.f;
// How far past its estimate of the target's reach an actor treats a swing as
// worth defending, as a fraction. The lunge already covers the physical
// closing, so this is the psychological bias — never below 1, because guessing
// "in range" wrongly wastes a block while guessing "out" wrongly takes a hit.
// Caution decides how much more slack, not whether there is any.
constexpr float DefendReachBase = 1.0f;
constexpr float DefendReachCautionScale = 0.2f;

// Extra stamina a cautious actor keeps in hand before attacking, added to every
// attack threshold so the whole ladder shifts together. 
static float StaminaReserve(float CautionMod)
{
	return std::clamp(CautionMod * 0.25f, 0.f, 0.25f);
}

// Odds an actor reads a power attack out of the animation in time to aim its
// parry at the window. Recognising one is trained, not learned mid-fight, so it
// is gated by tier alone
constexpr int ParryReadChanceMin = 50;  // VeryEasy
constexpr int ParryReadChanceMax = 90;  // Legendary
static bool RollPowerParryRead(AIHandler::Difficulty difficulty)
{
	const int Tier = std::clamp(static_cast<int>(difficulty),
		static_cast<int>(AIHandler::Difficulty::VeryEasy),
		static_cast<int>(AIHandler::Difficulty::Legendary));
	const int Span = static_cast<int>(AIHandler::Difficulty::Legendary) -
		static_cast<int>(AIHandler::Difficulty::VeryEasy);
	const int Chance = ParryReadChanceMin +
		(Tier - static_cast<int>(AIHandler::Difficulty::VeryEasy)) *
		(ParryReadChanceMax - ParryReadChanceMin) / Span;
	return static_cast<int>(mt_rand() % 100) < Chance;
}

// Every roll in this file is evaluated once per decision tick, so a bare
// probability is really a rate that scales with the tier's update timer —
// Legendary ticks twice as often as VeryEasy and so did everything else,
// invisibly. Express the intent as events per second and convert here instead.
// The tick cancels out, so ini timer changes no longer move any of these rates.
static bool RollPerSecond(float RatePerSecond, float TickLength)
{
	const float P = std::clamp(RatePerSecond * TickLength, 0.f, 1.f);
	return (mt_rand() % 10000u) < static_cast<unsigned>(P * 10000.f);
}

// A unit, not a knob: the tick length the original per-tick odds were written
// against, which is what turns "3 in 8 per tick" into 2.5/sec. 0.15 is the
// source's LegendaryUpdateTimer, chosen so the conversion was a no-op there and
// only corrected the slower tiers.
constexpr float ReferenceTick = 0.15f;
static bool RollTickScaled(float Numerator, float Denominator, float TickLength)
{
	return RollPerSecond((Numerator / Denominator) / ReferenceTick, TickLength);
}

// Belief drains have the same problem as the rolls: accumulation is paced by
// what the player does, but forgetting was a fixed amount per decision tick, so
// a faster tier forgot faster. Legendary bled twice as fast as VeryEasy — the
// sharpest AI had the shortest memory, and dropping its tick to 0.15 sped that
// up again without anyone re-tuning the drain.
//
// Carries the fraction rather than rounding it. Rounding per tick quantized the
// achievable rates to multiples of 1/tick, which at 0.15s meant the only
// choices were 6.7, 13.3 and 20/s — no way to tune between "as now" and "twice
// the memory", and a residual spread across tiers from the rounding itself.
static int DrainPerTick(float PerSecond, float TickLength, float& Remainder)
{
	Remainder += PerSecond * TickLength;
	const int Whole = static_cast<int>(Remainder);
	Remainder -= static_cast<float>(Whole);
	return Whole;
}
// Points per second bled from EVERY line, so the memory horizon reads directly:
// a belief worth N points survives N/rate seconds, and the cascade fires it N%
// of the time. At 8 a 40-point read lasts 5s — about a circling phase — where
// 13.3 gave it 3s. Raising these makes the AI live more in the moment; too far
// down and beliefs sit high and stale, and fixation pins it on old reads.
constexpr float HoldDrainPerSecond = 8.f;
constexpr float ForgetDrainPerSecond = 4.f;

// Stamina bands, and the attack rate permitted in each. 
constexpr float AttackStaminaFloor = 0.20f;  // never attacks below this
constexpr float AttackStaminaLow = 0.33f;
constexpr float AttackStaminaMid = 0.45f;
constexpr float AttackStaminaHigh = 0.60f;

constexpr int AttackChanceLow = 10;   // percent allowed in Floor..Low
constexpr int AttackChanceMid = 25;   // Low..Mid
constexpr int AttackChanceHigh = 50;  // Mid..High

// rate scaled attack
static float StaminaAttackScale(float Ratio, float FullRatio, float Reserve, float Commitment)
{
	const float Floor = AttackStaminaFloor + Reserve;
	// Commitment drags the full-rate point down, so an actor mid-combo keeps
	// swinging at stamina where it would otherwise be easing off
	const float Full = std::max(FullRatio + Reserve - Commitment, Floor + 0.01f);
	return std::clamp((Ratio - Floor) / (Full - Floor), 0.f, 1.f);
}
// Attacks/sec per point of (difficulty + aggression). At 0.57 a neutral
// Legendary sits near its previous 3.4/sec and Normal near 1.7 — raise it if
// low tiers still read as passive.
constexpr float AttackRatePerMod = 0.57f;
// Subtracted from the full-rate point in proportion to combo progress. Sized
// against the taper's width (AttackStaminaOffLine - AttackStaminaFloor = 0.30),
// which it compresses: at 0.15 a finished combo roughly doubles the taper's
// steepness, reaching full rate at 35% instead of 50%. Much past that and the
// ramp collapses and stamina stops mattering once a combo is underway.
constexpr float ComboCommitmentDiscount = 0.15f;
// The AI's own attack decision in SwitchToNewDirection, separate from the
// engine-driven ladder above.
constexpr float AttackStaminaOffLine = 0.5f;   // attacking off the guarded line
constexpr float AttackStaminaFeint = 0.4f;     // the feint branch
// have to be very careful with this number
constexpr float AIJitterRange = 0.04f;

constexpr float MaxMistakeRange = 0.05f;

// Dynamic spacing. Skyrim's combat AI picks a stand-position inside an
// inner/outer engagement band
constexpr float SpacingMultMin = 0.7f;
constexpr float SpacingMultMax = 1.8f;
constexpr float SpacingStaminaWeight = 0.5f;     // fully winded -> give ground
constexpr float SpacingLockoutWeight = 0.35f;    // can't attack -> proximity is pure risk
constexpr float SpacingPressWeight = 0.25f;      // per discrete press opportunity
constexpr float SpacingAdvantageWeight = 0.45f;  // full stamina edge -> close hard
// Offense closes flat out, regardless of personality or weapon. Anything at or
// below 0.5 bottoms out against SpacingMinBandWidth, so that floor — not this
// value — is what actually sets the closing distance.
constexpr float SpacingOffenseMult = 0.3f;
// How far a full stamina deficit holds an actor OUT of measure when it wants to
// attack
constexpr float SpacingEntryDiscipline = 1.5f;

// Seconds of commit window added per recent guard switch by the target, and the
// cap on how many count. At 0.03 and 5 a full churn roughly doubles a Legendary's
// window — earned by playing irregularly rather than granted as a constant.
constexpr float DisengageStaminaFloor = 0.5f;
// Caution moves that floor, so willingness to give ground is a personality tell
// rather than a universal reflex: an Evasive spends stamina to keep its measure,
// an Aggressor needs a near-full bar before it will consider backing off at all.
// Clamped so the floor stays above what a dodge actually costs and below always.
constexpr float DisengageCautionScale = 0.4f;
constexpr float DisengageFloorMin = 0.25f;
constexpr float DisengageFloorMax = 0.85f;

// Learned power-attack windup. New samples fold in at this weight, so a weapon
// swap converges in a few swings without one odd reading throwing it.
constexpr float PowerWindupSmoothing = 0.35f;
// A swing that hasn't connected in this long was whiffed, cancelled, or blocked
// by terrain — drop it rather than record an inflated windup. Overestimating is
// the one direction that hurts, since it presses the guard late.
constexpr float PowerWindupTimeout = 2.0f;

// Step-in counter, gated on bait tendency. 0.6 admits Counter (1.0) and
// Trickster (0.7) and nobody else — Turtle sits at 0.5, and the aggressive
// archetypes are negative. The rate then scales by the same trait, so a Counter
// takes the opening more readily than a Trickster does.
constexpr float CounterBaitThreshold = 0.6f;
constexpr float CounterRatePerSecond = 5.f;


constexpr float ConfusionPerSwitch = 0.03f;
constexpr int ConfusionMaxSwitches = 5;
// How much patience damps it. Turtle (+1.0) takes half the churn penalty,
// Aggressor (-0.8) takes 1.4x.
constexpr float ConfusionPatienceScale = 0.5f;
constexpr float SpacingAdvantageAggression = 0.8f;  // aggression scales the press appetite
constexpr float SpacingAdvantageCaution = 0.6f;     // caution damps it toward zero
constexpr float SpacingPersonalityScale = 0.3f;  // caution pushes out, aggression pulls in
// Game units of standoff per 1.0 of multiplier above neutral. This sets the
// fallback DESTINATION: the engine retreats to bodyRadius + inner, so at the
// 1.8 clamp this is the distance a fully-committed disengage reaches. 
constexpr float SpacingStandoffScale = 450.f;
constexpr float SpacingMinBandWidth = 64.f;      // keep outer meaningfully beyond inner
constexpr float SpacingEaseRate = 4.f;           // exponential ease, ~0.25s to 63%

// How often the perception layer samples the target.
constexpr float PerceptionInterval = 0.02f;

// todo replace with reading game setting
float BashDistance = 110;
float BashDistanceSq = BashDistance * BashDistance;
int JudgeDistance = 70000;

// AI combat personality archetypes. Each NPC instance gets assigned one of
// these based on a hash of their native handle (see CalcAndInsertDifficulty),
// with small (±0.1) per-NPC jitter so NPCs sharing an archetype aren't
// identical.=
struct AIPersonalityArchetype
{
	const char* name;
	float aggression;    // attack frequency, retreat reluctance, counter-attack readiness
	float patience;      // willingness to hold block vs. release
	float bait;          // holds same direction longer to bait commits (unused for now)
	float caution;       // dodge/block frequency, anticipation threshold
	float powerAttack;   // prefers power attacks over light
	float feint;         // probability of feinting on an attack
};
static constexpr AIPersonalityArchetype kPersonalityArchetypes[] = {
	// name           agg    pat    bait   caut   pow    feint   reads as
	{ "Aggressor",   +1.0f, -0.8f, -0.5f, -0.7f, +0.6f,  0.0f }, // charges in, swings constantly, rarely dodges
	{ "Turtle",      -0.8f, +1.0f, +0.5f, +0.8f, -0.4f, -0.5f }, // holds block, waits, hard to break
	{ "Trickster",   +0.3f, +0.3f, +0.7f, +0.2f, -0.3f, +1.0f }, // feints constantly, mind games heavy
	{ "Counter",     -0.3f, +0.8f, +1.0f, +0.5f, +0.3f, +0.4f }, // patient, waits for masterstrike opportunity
	{ "Brute",       +0.6f, -0.3f, -0.4f, -0.5f, +1.0f, -0.7f }, // power-attack-heavy heavy hitter
	{ "Evasive",     -0.2f,  0.0f, -0.2f, +1.0f, -0.5f, +0.3f }, // dodges everything, mobile fighter
};
constexpr std::size_t kNumPersonalityArchetypes = std::size(kPersonalityArchetypes);

void AIHandler::InitializeValues(PRECISION_API::IVPrecision3* precision)
{
	RE::Setting *CombatBashSetting = RE::GameSettingCollection::GetSingleton()->GetSetting("fCombatBashReach");
	//BashDistance = CombatBashSetting->GetFloat();
	logger::info("Read fCombatBashReach {}", BashDistance);
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

void AIHandler::AddAction(RE::Actor* actor, Actions toDo, Directions attackedDir, bool force, int priority, DodgeDirection dodgeDir, float TargetDelay)
{
	std::unique_lock lock(ActionQueueMtx);
	auto Iter = ActionQueue.find(actor->GetHandle());
	// don't do anything if we already have the same action queued
	if (actor->IsBlocking() && toDo == Actions::Block)
	{
		return;
	}
	if (!actor->IsBlocking() && toDo == Actions::EndBlock)
	{
		return;
	}
	if (Iter != ActionQueue.end() && Iter->second.toDo == toDo)
	{
		return;
	}
	// prevent infinite queuing
	if (Iter != ActionQueue.end() && Iter->second.wasForced && force)
	{
		if (Settings::VerboseLogging) logger::info("[action] duplicate action! {} tried to {} but already was trying to {}", actor->GetName(), (int)toDo, (int)Iter->second.toDo);
		return;
	}
	// hack to prevent anything from getting in the way of resetting state
	if (Iter != ActionQueue.end() && (Iter->second.toDo == Actions::ResetState ||
		Iter->second.toDo == Actions::ReleaseBash || Iter->second.toDo == Actions::EndFeint))
	{
		return;
	}
	// short circuit here because this causes issues where the actor will block forever
	if (Iter != ActionQueue.end() && actor->IsBlocking() && Iter->second.toDo == Actions::EndBlock)
	{
		return;
	}


	// if no action or time has expired
	if (Iter == ActionQueue.end() || force || Iter->second.timeLeft <= 0.f || Iter->second.toDo == Actions::None)
	{
		Action action;
		// Floored by the action timer: a deadline can hold the hand back, never
		// move it faster than the actor can act. When the deadline is already
		// past, this collapses to normal behaviour — which is the difficulty
		// gate for timed blocking, since a tier whose action timer exceeds the
		// delay simply presses immediately.
		const float actionTime = std::max(CalcActionTimer(actor), TargetDelay);
		action.timeLeft = actionTime;
		action.baseTimer = actionTime;
		action.toDo = toDo;
		action.targetDir = attackedDir;
		action.wasForced = force;
		action.priority = priority;
		action.dodgeDir = dodgeDir;
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
		auto& diff = DifficultyMap[actor->GetHandle()];
		unsigned idx = diff.currentAttackIdx;
		idx++;

		if (idx >= diff.attackPattern.size())
		{
			idx = 0;
		}
		diff.currentAttackIdx = idx;
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
	float NewTimer = CalcUpdateTimer(actor);
	// NOTE: DifficultyMap accessed under the caller's lock — every RunActor
	// path now takes DifficultyMapMtx before calling in.
	auto DiffIter = DifficultyMap.find(actor->GetHandle());
	if (DiffIter != DifficultyMap.end())
	{
		// flush the perception ring at the end of EVERY decision tick — the
		// ring means "switches since the last decision". 
		DiffIter->second.observedSwitchCount = 0;
		// The decision has fired, so a new stimulus may preempt again.
		DiffIter->second.preemptSpent = false;
	}
	UpdateTimerMtx.lock();
	UpdateTimer[actor->GetHandle()] = NewTimer;
	UpdateTimerMtx.unlock();
}


bool AIHandler::IsBlockableSwing(RE::Actor* target)
{
	return !IsBashing(target) && !DirectionHandler::GetSingleton()->IsUnblockable(target);
}

// "Prepared" in the reflex-block sense: in defensive stance, hands free, and
// enough wind that the block won't immediately break (0.2 floor). 
bool AIHandler::IsPreparedToBlock(RE::Actor* actor, const AIDifficulty& diff) const
{
	if (!diff.defending || actor->IsBlocking() || actor->IsAttacking())
	{
		return false;
	}
	const float Stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	const float MaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
	return MaxStamina > 0.f && (Stamina / MaxStamina) >= 0.2f;
}

void AIHandler::NotifyPowerAttackHitExternalCalled(RE::Actor* actor)
{
	std::unique_lock lock(DifficultyMapMtx);
	auto Iter = DifficultyMap.find(actor->GetHandle());
	if (Iter == DifficultyMap.end())
	{
		return;
	}
	auto& diff = Iter->second;
	// swingWasPower was read from IsPowerAttacking at the start edge, which
	// already excludes bashes — so the caller needs no flag checks of its own.
	if (diff.swingWasPower)
	{
		RecordPowerWindupSample(diff);
	}
}

void AIHandler::RecordPowerWindupSample(AIDifficulty& diff) const
{
	if (diff.swingElapsed < 0.f)
	{
		return;
	}
	const float sample = diff.swingElapsed;
	if (sample <= 0.f || sample > PowerWindupTimeout)
	{
		return;
	}
	diff.powerWindupEstimate = (diff.powerWindupEstimate <= 0.f) ?
		sample :
		diff.powerWindupEstimate + (sample - diff.powerWindupEstimate) * PowerWindupSmoothing;
}

float AIHandler::CommitWindow(const AIDifficulty& diff) const
{
	std::shared_lock lock(DifficultyUpdateTimerMtx);
	auto Iter = DifficultyUpdateTimer.find(diff.difficulty);
	const float base = (Iter != DifficultyUpdateTimer.end()) ? Iter->second : ReferenceTick;

	// Churn. Watching a guard line is not what costs a person tracking — the
	// line is visible the whole time. What costs them is irregular timing,
	// which defeats anticipation and pins them at choice-reaction speed. The
	// AI has no equivalent: its tick is the same length whether the target is
	// metronomic or chaotic. This is the stand-in for that, using the switch
	// streak as a proxy for irregularity — it climbs while the target keeps
	// changing and bleeds off the moment they settle, so a predictable target
	// buys no window at all.
	const int churn = std::clamp(diff.numTimesDirectionsSwitched, 0, ConfusionMaxSwitches);
	// Composure is patience: a Turtle is already just watching and holding, so
	// churn barely moves it; an Aggressor is hunting an opening and is exactly
	// who gets drawn out of position by movement.
	const float composure = std::clamp(1.f - diff.patienceMod * ConfusionPatienceScale, 0.f, 2.f);
	return base * AISettings::CommitWindowTicks +
		static_cast<float>(churn) * ConfusionPerSwitch * composure;
}

// The wrong-line rule: only raise a block when the guard already covers the
// attack line, or the commit window has passed so the correcting guard
// switch is allowed to arrive.
bool AIHandler::CanAnswerLine(RE::Actor* actor, RE::Actor* target, const AIDifficulty& diff) const
{
	return DirectionHandler::GetSingleton()->HasBlockAngle(actor, target) ||
		diff.timeSinceLineChange >= CommitWindow(diff);
}

float AIHandler::CalcSpacingTarget(RE::Actor* actor, RE::Actor* target, const AIDifficulty& diff,
	float ownStaminaRatio, float enemyStaminaRatio) const
{
	// Personality decides how OFTEN an actor takes its turn (the defend-exit
	// roll), never how close it stands once it has. Everything below shapes
	// the DEFENDING band only.
	if (!diff.defending)
	{
		// Entry is the decision the stamina read should gate, not just how far
		// out to stand while defending. Deciding to attack used to close flat
		// out regardless of who was winded, so an actor entered measure — where
		// the line guess is unreactable — with no read behind it.
		//
		// Scaled by difficulty, which is the one place a mod term belongs in
		// spacing: knowing when NOT to enter is what separates a fighter from
		// something that walks in whenever a timer expires. VeryEasy still
		// charges; Legendary waits for the edge.
		const float deficit = std::clamp(enemyStaminaRatio - ownStaminaRatio, 0.f, 1.f);
		const float discipline = std::clamp(
			static_cast<float>(static_cast<int>(diff.difficulty)) /
				static_cast<float>(static_cast<int>(Difficulty::Legendary)), 0.f, 1.f);
		return SpacingOffenseMult + deficit * SpacingEntryDiscipline * discipline;
	}

	float spacing = 1.f;

	// Winded — give ground to recover. Continuous ramp from half stamina down,
	// so there is no threshold for this to oscillate across.
	spacing += std::clamp((0.5f - ownStaminaRatio) / 0.5f, 0.f, 1.f) * SpacingStaminaWeight;

	// Attack lockout: cannot attack at all, so standing in range is pure
	// downside
	if (!AttackHandler::GetSingleton()->CanAttack(actor))
	{
		spacing += SpacingLockoutWeight;
	}

	// Press a target that cannot punish us.
	if (target->AsActorState()->actorState2.staggered)
	{
		spacing -= SpacingPressWeight;
	}
	if (!AttackHandler::GetSingleton()->CanAttack(target))
	{
		spacing -= SpacingPressWeight;
	}

	// Stamina ADVANTAGE, not the opponent's stamina in isolation. Being the
	// fresher fighter is what makes closing correct: if both are exhausted
	// neither can capitalise, and the own-stamina term above should win and
	// pull us out instead. Clamped at zero so this can only ever press —
	// the disadvantage case is already owned by that term, and letting this
	// go negative would double-count it.
	const float staminaEdge = std::clamp(ownStaminaRatio - enemyStaminaRatio, 0.f, 1.f);

	// Personality decides how hard the advantage gets pressed, not just where
	// the band sits. An Aggressor smells blood and closes; a Turtle holding the
	// same read simply keeps its ground and lets the opponent come back to it.
	const float pressAppetite = std::clamp(
		1.f + diff.aggressionMod * SpacingAdvantageAggression
			- diff.cautionMod * SpacingAdvantageCaution,
		0.f, 2.f);
	spacing -= staminaEdge * SpacingAdvantageWeight * pressAppetite;

	// Same wide personality spread used everywhere else: a Turtle genuinely
	// fights at range, an Aggressor crowds.
	spacing += diff.cautionMod * SpacingPersonalityScale;
	spacing -= diff.aggressionMod * SpacingPersonalityScale;

	return std::clamp(spacing, SpacingMultMin, SpacingMultMax);
}

void AIHandler::ApplySpacingExternalCalled(RE::Actor* actor, float* a_inOutInner, float* a_inOutOuter)
{
	if (!actor || !a_inOutInner || !a_inOutOuter)
	{
		return;
	}

	float mult = 1.f;
	{
		// find(), never operator[] — this runs on the combat-AI thread and an
		// insert here could rehash the map under the properly-locked readers.
		std::shared_lock lock(DifficultyMapMtx);
		auto Iter = DifficultyMap.find(actor->GetHandle());
		if (Iter == DifficultyMap.end())
		{
			return;
		}
		mult = Iter->second.spacingMult;
	}

	// Additive on inner because vanilla's inner is 0 and a multiplier can never
	// lift it off zero; multiplicative on outer because it has a real baseline.
	const float inner = std::max(0.f, (mult - 1.f) * SpacingStandoffScale);
	const float outer = std::max(*a_inOutOuter * mult, inner + SpacingMinBandWidth);
	*a_inOutInner = inner;
	*a_inOutOuter = outer;
}

// i abandoned good coding conventions a long time ago
void AIHandler::RunActor(RE::Actor* actor, float delta)
{
	if (!actor->GetActorRuntimeData().currentCombatTarget)
	{
		return;
	}

	DirectionHandler* DirHandler = DirectionHandler::GetSingleton();
	RE::Actor* target = actor->GetActorRuntimeData().currentCombatTarget.get().get();
	if (!target)
	{
		return;
	}

	if (!DirHandler->HasDirectionalPerks(target))
	{
		// if enemy has no directions
		if (CanAct(actor))
		{
			// Same contract as the directional path below: SwitchToNextAttack
			// inserts into DifficultyMap and DidAct reads it, both expecting the
			// caller to hold this. Taken here rather than inside them so the
			// order stays DifficultyMapMtx -> AIHandlerDataMtx/ActionQueueMtx.
			std::unique_lock DiffLock(DifficultyMapMtx);
			SwitchToNextAttack(actor, false);
			DidAct(actor);
		}
		return;
	}

	// Actions that occur outside of the normal tick (such as reactions) happen here
	float TargetDistSQ = target->GetPosition().GetSquaredDistance(actor->GetPosition());
	std::unique_lock DiffLock(DifficultyMapMtx);
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		CalcAndInsertDifficulty(actor);
	}
	// Cache once — RunActor accesses this entry ~37 times. Safe to hold
	// a reference: the contains check above guarantees the key exists,
	// and the only writes within this scope target the same key (no
	// new keys get inserted, so no rehashing).
	auto& diff = DifficultyMap[actor->GetHandle()];
	// tick cooldown
	if (diff.DodgeCooldown >= 0)
	{
		diff.DodgeCooldown -= delta;
	}
	if (diff.BashCooldown >= 0)
	{
		diff.BashCooldown -= delta;
	}

	// Perception layer: observe the target's guard EVERY frame
	{
		bool salientEvent = false;
		// perception is per-target: reset everything on retarget,
		// otherwise the first frame against the new target
		// fabricates a switch from the OLD target's remembered
		// guard, injects it as belief, and re-arms the
		// acquisition gate against a target who never moved
		const uint32_t targetId = target->GetHandle().native_handle();
		if (diff.observedTargetId != targetId)
		{
			diff.observedTargetId = targetId;
			diff.hasObservation = false;
			diff.observedSwitchCount = 0;
			diff.lastObservedAttackState = 0;
			diff.timeSinceLineChange = 0.f;
			// Windup is a property of the opponent's weapon and animations, so
			// it means nothing against a different one.
			diff.powerWindupEstimate = 0.f;
			diff.swingElapsed = -1.f;
		}
		diff.timeSinceLineChange += delta;
		diff.defendTime = diff.defending ? diff.defendTime + delta : 0.f;
		// Runs per frame, not per perception tick — this is the measurement the
		// timed block is built on, so its resolution shouldn't be 20ms.
		if (diff.swingElapsed >= 0.f)
		{
			diff.swingElapsed += delta;
			// Backstop only. The swing's end edge is what normally closes this;
			// the timeout just catches a swing whose end we never observed.
			if (diff.swingElapsed > PowerWindupTimeout)
			{
				diff.swingElapsed = -1.f;
			}
		}
		// Framerate-independent ease toward the tick-computed spacing
		// intent. The band is recomputed at ~3.5Hz (measured)
		diff.spacingMult += (diff.spacingTarget - diff.spacingMult) *
			(1.f - std::exp(-delta * SpacingEaseRate));
		// Everything above integrates real time and stays per-frame. Everything
		// below is perception
		diff.perceptionAccum += delta;
		if (diff.perceptionAccum >= PerceptionInterval)
		{
		diff.perceptionAccum = 0.f;

		const Directions seen = DirectionHandler::GetSingleton()->GetCurrentDirection(target);
		if (!diff.hasObservation)
		{
			diff.hasObservation = true;
			diff.lastObservedDirection = seen;
		}
		else if (seen != diff.lastObservedDirection)
		{
			if (diff.observedSwitchCount < static_cast<int>(diff.observedSwitchesFrom.size()))
			{
				diff.observedSwitchesFrom[diff.observedSwitchCount] = diff.lastObservedDirection;
				diff.observedSwitchCount++;
			}
			diff.lastObservedDirection = seen;
			diff.timeSinceLineChange = 0.f;
			// Deliberately NOT a preempt. A switch is information, not a
			// threat, and it is far more frequent than an attack — spending
			// the reaction budget on it means the swing that follows can't
			// claim one.
		}
		// a swing starting is the other stimulus worth waking for.
		const auto targetAttackState = target->AsActorState()->actorState1.meleeAttackState;
		const bool inSwingPhase =
			targetAttackState == RE::ATTACK_STATE_ENUM::kDraw ||
			targetAttackState == RE::ATTACK_STATE_ENUM::kSwing ||
			targetAttackState == RE::ATTACK_STATE_ENUM::kBash;
		const bool wasSwingPhase =
			diff.lastObservedAttackState == static_cast<int>(RE::ATTACK_STATE_ENUM::kDraw) ||
			diff.lastObservedAttackState == static_cast<int>(RE::ATTACK_STATE_ENUM::kSwing) ||
			diff.lastObservedAttackState == static_cast<int>(RE::ATTACK_STATE_ENUM::kBash);
		if (inSwingPhase && !wasSwingPhase)
		{
			// A bash is deliberately unreactable — the counter is spacing, not
			// reflexes — so it updates perception but never spends a preempt.
			if (targetAttackState != RE::ATTACK_STATE_ENUM::kBash)
			{
				salientEvent = true;
			}
			// re-arm the once-per-swing belief spike for THIS
			// swing — chained swings each teach once, instead of
			// the whole chain teaching once
			diff.lastCallForced = false;

			// Arm the swing lifecycle. Stops when the hit arrives, blocked or
			// not, so the windup is learned from exactly the attacks the AI is
			// currently failing to answer — and so a swing that ends without
			// connecting can be recognised as a whiff.
			diff.swingElapsed = 0.f;
			diff.swingWasPower = IsPowerAttacking(target);

			// Reflex block: for an AI already in defensive stance,
			// guard-up on an incoming swing is a pre-loaded motor
			// program (~150ms in humans), not a choice reaction —
			// so it queues at the stimulus and pays only the
			// action timer, instead of capture latency + action
			// timer serially (~250-290ms). An AI caught out of
			// stance still pays the full choice-reaction path via
			// the decision tick.
			if (IsPreparedToBlock(actor, diff) && IsBlockableSwing(target) &&
				CanAnswerLine(actor, target, diff))
			{
				// Full action timer, not a discounted one. A prepared
				// flinch skips the DECISION stage, and queuing here
				// rather than on the decision tick is what grants that
				// — the motor stage still costs what it costs. The two
				// block paths then differ by exactly one update timer,
				// which is what "prepared" should be worth.
				//
				// Against a power attack whose windup it has learned, it
				// holds the press so the parry window lands on the hit
				// instead of opening and closing before it. This is the
				// only counter to power spam that exists: a blocked power
				// locks the defender out (no masterstrike) and advances
				// the attacker's combo regardless, so blocking early just
				// feeds it. Aiming at the window's middle, and the press
				// is still far earlier than the last moment a block would
				// land — so a wrong estimate degrades to a normal block
				// rather than to standing there unguarded.
				// Reading it is a roll, not a flag check — a power attack is not
				// labelled, you recognise the animation, and not everyone does in
				// time. Rolled here at the start edge and nowhere else: this block
				// runs once per swing, and a per-tick roll across a 700ms windup
				// would converge on certainty.
				float TargetDelay = -1.f;
				if (diff.swingWasPower && diff.powerWindupEstimate > 0.f &&
					RollPowerParryRead(diff.difficulty))
				{
					TargetDelay = diff.powerWindupEstimate -
						DifficultySettings::TimedBlockStartup -
						DifficultySettings::TimedBlockActiveTime * 0.5f;
				}
				AddAction(actor, Actions::Block, Directions::TR, true, 0,
					DodgeDirection::Backward, TargetDelay);
			}
		}
		// Swing ended. If nothing connected during it, the target committed to
		// an attack and missed — the moment entering measure was a mistake, and
		// the only thing that makes entering a decision rather than a formality.
		// Swing ended. Closes the measurement at the edge rather than waiting
		// for the timeout, so a sample is available immediately and a slow
		// attack is never mistaken for one that never landed.
		else if (!inSwingPhase && wasSwingPhase)
		{
			diff.swingElapsed = -1.f;
		}
		diff.lastObservedAttackState = static_cast<int>(targetAttackState);

		// Attention capture: restart the decision clock from the stimulus, so a
		// swing is answered exactly one update timer later wherever it landed
		// in the tick grid. Set rather than clamped — clamping leaves the
		// answer on the grid, which is what capture exists to remove, and needs
		// an arbitrary floor to stop a stimulus arriving just before a tick
		// being answered in a few milliseconds. Setting needs no such number.
		if (salientEvent && !diff.preemptSpent)
		{
			const float reaction = CalcUpdateTimer(actor);
			UpdateTimerMtx.lock();
			auto timerIter = UpdateTimer.find(actor->GetHandle());
			if (timerIter != UpdateTimer.end())
			{
				timerIter->second = reaction;
				diff.preemptSpent = true;
			}
			UpdateTimerMtx.unlock();
		}
		} // perception interval
	}


	// slower update tick to make AIs reasonable to fight
	if (CanAct(actor))
	{
		// emergency hacks to get out of jail
		if (Settings::ExperimentalMode)
		{
			bool attacking = false;
			bool bashing = false;
			bool blocking = false;

			actor->GetGraphVariableBool("IsAttacking", attacking);
			actor->GetGraphVariableBool("IsBashing", bashing);
			actor->GetGraphVariableBool("IsBlocking", blocking);

			// attempted fix for weird in between state that enemies can get into after bashing



			if (!attacking && (actor->AsActorState()->actorState1.meleeAttackState > RE::ATTACK_STATE_ENUM::kNone
				&& actor->AsActorState()->actorState1.meleeAttackState < RE::ATTACK_STATE_ENUM::kBash))
			{
				//logger::info("speculative enemy fix3 {}", (int)actor->AsActorState()->actorState1.meleeAttackState);
				actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
			}
		}

		if (TargetDistSQ < JudgeDistance)
		{
			float CurrentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
			float MaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
			float CurrentStaminaRatio = CurrentStamina / MaxStamina;
			// every roll below fires once per decision tick, so it is rescaled
			// against this to keep frequency independent of the tier's cadence
			const float Tick = CalcUpdateTimer(actor);

			float EnemyCurrentStamina = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
			float EnemyMaxStamina = target->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
			float EnemyStaminaRatio = EnemyCurrentStamina / EnemyMaxStamina;

			// Actor::GetReach is the engine's own per-actor reach, measured from
			// the same origin GetPosition reports — so unlike a weapon record
			// multiplier or a bare collision capsule it can be compared against
			// TargetDistSQ directly. Recomputed per tick rather than cached, so
			// a weapon swap needs no invalidation.
			// Plus the lunge: attack animations carry the attacker forward, so
			// the distance an attack can LAND from is reach plus that travel.
			// Every consumer asks "can an attack connect", not "is the weapon
			// touching", so it is folded in here rather than at each site.
			const float OwnReach = actor->GetReach() + AttackLungeUnits;
			diff.CurrentWeaponLengthSQ = OwnReach * OwnReach;
			const float TargetReach = target->GetReach() + AttackLungeUnits;
			diff.targetReachSQ = TargetReach * TargetReach;
			const float PerceivedReachSQ = diff.targetReachSQ *
				diff.reachMisjudge * diff.reachMisjudge;

			// Spacing intent for the combat advance-radius hook.
			// Computed here, on the decision tick, because all the
			// state is already in hand; the per-frame ease lives in
			// the perception block so smoothing stays framerate-
			// independent regardless of tick rate.
			diff.spacingTarget = CalcSpacingTarget(actor, target, diff, CurrentStaminaRatio, EnemyStaminaRatio);

			// always attack if have perk
			if (DirHandler->IsUnblockable(actor) && TargetDistSQ < diff.CurrentWeaponLengthSQ)
			{
				AddAction(actor, AIHandler::Actions::Attack, Directions::TR, true);
				diff.defending = false;
				diff.numTimesDirectionsSwitched = 1;
				diff.numTimesDirectionSame = 0;
				// keep the once-per-swing edge detector fresh on
				// ticks that skip DirectionMatchTarget
				diff.lastCallForced = target->IsAttacking();
				//logger::info("try attacking! {} {}", actor->GetName(), TargetDistSQ);
			}
			// always follow up power attack with another attack
			else if (actor->IsAttacking())
			{
				//SwitchToNewDirection(actor, actor);
				SwitchToNextAttack(actor, true);
				if (IsPowerAttacking(actor))
				{
					AddAction(actor, AIHandler::Actions::Followup, Directions::TR, true);
				}
				// keep the once-per-swing edge detector fresh on
				// ticks that skip DirectionMatchTarget
				diff.lastCallForced = target->IsAttacking();
			}
			else
			{
				bool ShouldDirectionMatch = false;
				bool DontChangeDirection = false;
				bool targetStaggering = target->AsActorState()->actorState2.staggered;
				int mod = (int)CalcAndInsertDifficulty(actor);

				// Exit discipline, the complement to the entry gate in
				// CalcSpacingTarget. Inside measure the line is a guess, so an
				// actor that has just been put on the defensive there is in the
				// worst place to be: it cannot masterstrike once a blocked
				// power locks it out, dodging has no i-frames so it only works
				// from further out, and blocking simply feeds the attacker's
				// combo. Leaving is the answer, and spacing alone is a drift,
				// not an escape.
				//
				// Self-limiting: succeeding puts it out of range, so the
				// distance test is the only gate needed, and DodgeCooldown
				// covers the case where the target closes again. Only while it
				// can still afford the stamina — the existing retreat is a
				// panic bail below 0.30, which unlocks the escape at exactly
				// the point it can least pay for it.
				if (diff.defending)
				{
					const float DisengageFloor = std::clamp(
						DisengageStaminaFloor - diff.cautionMod * DisengageCautionScale,
						DisengageFloorMin, DisengageFloorMax);
					if (Settings::ActiveDodgeSystem != DodgeSystem::None &&
						!actor->IsBlocking() && diff.DodgeCooldown <= 0.f &&
						CurrentStaminaRatio > DisengageFloor &&
						TargetDistSQ < PerceivedReachSQ)
					{
						AddAction(actor, Actions::Dodge, Directions::TR, false, 0,
							DodgeDirection::Backward);
					}
				}

				// Neutral-game pre-block read. strongRead = the arc on the
				// line the target currently holds is high (I expect a repeat
				// attack from where they are). wantPreBlock = strongRead +
				// personality roll (cautious commits, reckless doesn't) +
				// stamina to hold. strongRead also pins the AI in defense
				// (suppresses the offense exit below) so a committed
				// pre-block isn't undone the same exchange.
				const Directions targetCurrentLine = DirHandler->GetCurrentDirection(target);
				const int beliefOnLine = diff.directionChangeChance[targetCurrentLine];
				const bool strongRead = beliefOnLine >= AISettings::PreBlockBeliefThreshold;
				const float preBlockChance = std::clamp(AISettings::PreBlockBaseChance + diff.cautionMod * AISettings::PreBlockCautionScale, 0.f, AISettings::PreBlockMaxChance);
				// Only pre-block an actual THREAT — a target that can't attack
				// (staggered, blocking, or locked out) is a free attack
				// opportunity, not something to defend against. Without this a
				// cautious NPC with a strong read would waste its opening
				// pre-blocking a helpless target instead of punishing it.
				const bool targetIsThreat = AttackHandler::GetSingleton()->CanAttack(target) &&
					!targetStaggering && !target->IsBlocking();
				const bool wantPreBlock = strongRead && targetIsThreat && !actor->IsBlocking() &&
					CurrentStaminaRatio > 0.4f &&
					RollTickScaled(preBlockChance, 100.f, Tick);

				// A swing thrown from well outside reach is not a threat, and
				// treating it as one lets a target pin the AI in defence by
				// flailing at nothing. The threshold is the AI's ESTIMATE of
				// the target's reach — reachMisjudge already puts a tier-scaled
				// random error on that — widened by caution. Everyone gets some
				// slack; caution only decides how much. Slack is linear, so it
				// squares to compare against the SQ pair.
				const float DefendSlack = std::clamp(
					DefendReachBase + diff.cautionMod * DefendReachCautionScale, 1.f, 1.6f);
				const bool TargetInThreatRange =
					TargetDistSQ < PerceivedReachSQ * DefendSlack * DefendSlack;

				// Step-in counter. They committed to a swing from beyond their
				// own reach, so it misses — but the swing's forward travel
				// carries them INTO measure, and they arrive committed to a
				// recovery. The band is one lunge wide:
				//
				//   theirReach + theirLunge  <  dist  <  ownReach + 2 lunges
				//
				// The upper bound counts the lunge twice on purpose. Their
				// travel closes the gap before this actor's swing connects, so
				// its effective reach is extended by THEIR movement as well as
				// its own — comparing against CurrentWeaponLengthSQ instead
				// prices the simultaneous case, where identical weapons make
				// the band empty and the branch can never fire.
				//
				// Falls out correctly against longer weapons too: if their
				// reach exceeds this actor's by more than a lunge the band
				// closes, which is why you can't whiff-punish a polearm without
				// the range to meet it.
				//
				// Bait-gated, because this is that disposition's whole idea —
				// let them commit to a mistake, then take it. Counter (1.0) and
				// Trickster (0.7) clear the threshold; everyone else declines.
				// Uses the raw reach rather than TargetInThreatRange, whose
				// DefendSlack widens generously to make DEFENDING safe — here
				// that same widening would shrink the band, backwards.
				const float CounterReach = OwnReach + AttackLungeUnits;
				if (target->IsAttacking() &&
					diff.baitTendency >= CounterBaitThreshold &&
					TargetDistSQ > diff.targetReachSQ &&
					TargetDistSQ < CounterReach * CounterReach &&
					CurrentStaminaRatio > AttackStaminaOffLine + StaminaReserve(diff.cautionMod) &&
					AttackHandler::GetSingleton()->CanAttack(actor) &&
					RollPerSecond(CounterRatePerSecond * diff.baitTendency, Tick))
				{
					AddAction(actor, Actions::Attack);
					DontChangeDirection = true;
				}
				// Most important case, attempt to defend
				else if (target->IsAttacking() && IsBlockableSwing(target) && TargetInThreatRange)
				{
					// Too winded to block reliably — retreat instead. A failed
					// block at low stamina breaks guard and stuns; a backward
					// dodge avoids the attack and creates space to recover.
					// Not forced — AddAction's duplicate-forced guard would drop
					// it anyway if a block was previously force-queued, and
					// without force it queues normally when the slot is open.
					// Gated on dodge system being enabled — otherwise we'd
					// queue a no-op dodge that still burns the cooldown,
					// stranding the actor in punching-bag mode.
					if (Settings::ActiveDodgeSystem != DodgeSystem::None &&
						CurrentStaminaRatio < 0.30f && diff.DodgeCooldown <= 0.f)
					{
						// Drop the block FIRST if we're holding one — a dodge
						// can't fire while blocking, so a locked-out AI that
						// pre-blocked would otherwise be trapped eating a flurry
						// into a stamina-break disarm. Threshold raised to 0.30
						// so it bails with enough stamina left to actually dodge
						// and recover, instead of blocking down to the break.
						if (actor->IsBlocking())
						{
							AddAction(actor, Actions::EndBlock);
						}
						else
						{
							AddAction(actor, Actions::Dodge, Directions::TR, false, 0, DodgeDirection::Backward);
						}
					}
					// Edge-of-reach evade. Dodging has no i-frames, so it only
					// beats blocking when it actually leaves the hitbox — i.e.
					// out at the rim of the attacker's reach. 
					// Reach is misjudged per actor so the AI isn't working off
					// a number the player can't see.
					else if (Settings::ActiveDodgeSystem != DodgeSystem::None &&
						!actor->IsBlocking() && !actor->IsAttacking() &&
						GetQueuedAction(actor) != Actions::Block &&
						PerceivedReachSQ > 0.f && diff.DodgeCooldown <= 0.f &&
						CurrentStaminaRatio > 0.35f &&
						TargetDistSQ < PerceivedReachSQ &&
						TargetDistSQ > PerceivedReachSQ * 0.7f &&
						// rolls per decision tick, so it compounds across an
						// attack — keep the per-tick odds low
						RollTickScaled(1.0f + diff.cautionMod * 3.0f, 40.f, Tick))
					{
						AddAction(actor, Actions::Dodge, Directions::TR, false, 0, DodgeDirection::Backward);
					}
					else
					{
						Actions action = GetQueuedAction(actor);
						// try to block or masterstrike
						if (action != Actions::Attack && action != Actions::Block)
						{

							// Aggression-modulated masterstrike chance: aggressive NPCs
							// go for the chamber-counter more readily instead of blocking.
							if (RollTickScaled(1.0f + diff.aggressionMod, 10.f, Tick) && DirHandler->HasBlockAngle(actor, target))
							{
								// masterstrike
								AddAction(actor, AIHandler::Actions::PowerAttack, Directions::TR);
							}
							// Wrong-line rule (see CanAnswerLine): inside the
							// floor with no angle it takes the mixup hit
							// honestly; past the floor it raises and the guard
							// follows.
							else if (CanAnswerLine(actor, target, diff))
							{
								AddAction(actor, Actions::Block, Directions::TR, true);
							}
						}
						if (DirHandler->HasBlockAngle(actor, target))
						{
							DontChangeDirection = true;
						}
						ShouldDirectionMatch = true;

						if (!diff.defending)
						{
							diff.defending = true;
							diff.numTimesDirectionsSwitched = 1;
							diff.numTimesDirectionSame = 0;
						}
					}
				}
				// Locked-out pre-block. A locked-out actor can't attack,
				// so defense is its only option and committing the guard
				// early has zero opportunity cost — pre-raise it rather
				// than tracking-and-waiting for a swing the reflex might
				// not catch (a fast follow-up beats the ~150ms reflex
				// raise; only an already-up guard blocks it). Aims at the
				// tracked line via ShouldDirectionMatch, so a feint still
				// beats it (guard's on the old line, switch punishes) —
				// which turns punishing a parry into a mixup instead of a
				// free hit. Only the proactive case: target-attacking is
				// handled reactively by the defend branch above; unblock-
				// able/bash aren't blockable so we let those through.
				else if (!AttackHandler::GetSingleton()->CanAttack(actor) &&
					!target->IsAttacking() && !actor->IsBlocking() &&
					CurrentStaminaRatio > 0.3f)
				{
					AddAction(actor, Actions::Block, Directions::TR, true);
					diff.defending = true;
					ShouldDirectionMatch = true;
					if (DirHandler->HasBlockAngle(actor, target))
					{
						DontChangeDirection = true;
					}
					if (Settings::VerboseLogging) logger::info("[preblock] {} locked-out pre-block", actor->GetName());
				}
				// Neutral-game pre-block: strong read on the target's held
				// line + a cautionMod roll to commit. 
				else if (wantPreBlock)
				{
					AddAction(actor, Actions::Block, Directions::TR, true);
					diff.defending = true;
					ShouldDirectionMatch = true;
					if (DirHandler->HasBlockAngle(actor, target))
					{
						DontChangeDirection = true;
					}
					if (Settings::VerboseLogging) logger::info("[preblock] {} neutral pre-block belief={} caution={:.2f}", actor->GetName(), beliefOnLine, diff.cautionMod);
				}
				// gated on our own lockout: a locked-out actor can't
				// actually attack, so "start attacking" would just
				// EndBlock the only defense it has
				// Close-range option against a target that isn't attacking.
				// Sits above the offense branch only for reachability: that one
				// catches IsBlocking() and routes to attacks, so with bash
				// underneath it a raised guard was the one thing bash could
				// never be used on. The 2s cooldown keeps it occasional.
				// Aggression-modulated: more aggressive NPCs bash more often.
				else if (TargetDistSQ < (BashDistanceSq + 1700) && RollTickScaled((mod + 1) * 0.5f + 2 + diff.aggressionMod * 1.5f, 8.f, Tick)
					&& AttackHandler::GetSingleton()->CanAttack(actor) && CurrentStaminaRatio > 0.3
					&& !target->IsAttacking() && AttackHandler::GetSingleton()->CanAttack(target) && !targetStaggering
					&& diff.BashCooldown <= 0.f)
				{
					AddAction(actor, Actions::Bash);
					ShouldDirectionMatch = true;
					DontChangeDirection = true;
				}
				else if (AttackHandler::GetSingleton()->CanAttack(actor) &&
					(!AttackHandler::GetSingleton()->CanAttack(target) || targetStaggering || target->IsBlocking()))
				{
					// target cant attack so start attacking
					if (diff.defending)
					{
						diff.defending = false;
						diff.numTimesDirectionsSwitched = 1;
						diff.numTimesDirectionSame = 0;
					}
								
					if (actor->IsBlocking())
					{
						AddAction(actor, Actions::EndBlock);
					}
					ShouldDirectionMatch = false;
					DontChangeDirection = false;
				}
				// uh oh, they might bash us! Caution-modulated: more cautious
				// NPCs are more likely to dodge a predicted bash.
				else if (Settings::ActiveDodgeSystem > static_cast<DodgeSystem>(0) && TargetDistSQ < (BashDistanceSq + 2000) && !actor->IsAttacking() && RollTickScaled((mod + 1) * 0.5f + 2 + diff.cautionMod * 1.5f, 8.f, Tick)
					&& AttackHandler::GetSingleton()->CanAttack(target) && AttackHandler::GetSingleton()->CanAttack(actor)
					&& diff.DodgeCooldown <= 0.f && !targetStaggering)
				{
					// add another check here because the enemy might be unable to bash
					if (actor->IsBlocking())
					{
						AddAction(actor, Actions::EndBlock);
					}
					else
					{
						// Random back-side dodge for variety. Picked here at
						// decision time; the Update execution path just uses
						// whatever direction was queued. Retreat / advance /
						// reactive dodges queue different directions from
						// their respective decision points.
						int rand = mt_rand() % 3;
						DodgeDirection dir = DodgeDirection::Backward;
						if (rand == 2) dir = DodgeDirection::BackwardRight;
						else if (rand == 1) dir = DodgeDirection::BackwardLeft;
						AddAction(actor, Actions::Dodge, Directions::TR, false, 0, dir);
					}
				}
				// too close, try to attack to prevent the bash. Aggression-
				// modulated: aggressive NPCs counter-attack more readily.
				else if (TargetDistSQ < (BashDistanceSq + 1500) && !actor->IsAttacking() && RollTickScaled((mod * 0.5f) + 2 + diff.aggressionMod * 2.0f, 14.f, Tick)
					&& AttackHandler::GetSingleton()->CanAttack(target) && AttackHandler::GetSingleton()->CanAttack(actor)
					&& CurrentStaminaRatio > 0.3)
				{

					DontChangeDirection = false;
					ShouldDirectionMatch = false;
					if (diff.defending)
					{
						diff.defending = false;
						diff.numTimesDirectionsSwitched = 1;
						diff.numTimesDirectionSame = 0;
					}
					if (actor->IsBlocking())
					{
						AddAction(actor, Actions::EndBlock);
					}
					else
					{
						AddAction(actor, Actions::Attack);

					}

				}
				// Target is out of weapon range but still within engage range —
				// close the gap with a forward dodge.
				else if (Settings::ActiveDodgeSystem != DodgeSystem::None &&
					!actor->IsAttacking() && !actor->IsBlocking() &&
					TargetDistSQ > (diff.CurrentWeaponLengthSQ + 2500.0f) &&
					CurrentStaminaRatio > std::clamp(0.75f - diff.aggressionMod * 0.25f + diff.cautionMod * 0.15f, 0.4f, 0.95f) &&
					diff.DodgeCooldown <= 0.f)
				{
					AddAction(actor, Actions::Dodge, Directions::TR, false, 0, DodgeDirection::Forward);
				}
				// Stop blocking to avoid burning stamina. Patience-modulated:
				// patient NPCs hold block longer (lower probability of releasing).
				// Never releases during our own attack lockout — the shell is
				// the only defense a locked-out actor has.
				else if (actor->IsBlocking() && AttackHandler::GetSingleton()->CanAttack(actor) &&
					diff.numTimesDirectionSame < 1 &&
					(RollTickScaled(3.0f - diff.patienceMod * 1.5f, 5.f, Tick) || CurrentStaminaRatio < 0.6))
				{
					AddAction(actor, Actions::EndBlock);
					DontChangeDirection = true;
					ShouldDirectionMatch = true;
				}
				// Probing dodge — lowest-priority distance management. When no
				// immediate threat or opportunity has fired above, occasionally
				// shift engagement with a random 8-way dodge. 
				// Probing dodge / active footwork. Both cautious AND aggressive
				// characters dodge frequently but for opposite reasons:
				//   - Cautious (Evasive): defensive footwork, dodges away/around
				//     to stay out of range and frustrate the attacker
				//   - Aggressive (Aggressor): offensive footwork, dodges forward
				//     to close distance and put on pressure
				// Probability is boosted by whichever extreme the personality
				// leans toward. Direction is biased to match: aggressive →
				// forward, cautious → backward/sides, neutral → any.
				else if (Settings::ActiveDodgeSystem != DodgeSystem::None &&
					!actor->IsAttacking() && !actor->IsBlocking() &&
					CurrentStaminaRatio > 0.9f &&
					TargetDistSQ > (BashDistanceSq + 2000.0f) &&
					diff.DodgeCooldown <= 0.f &&
					RollTickScaled(5.0f
						+ std::max(0.0f, diff.cautionMod) * 15.0f
						+ std::max(0.0f, diff.aggressionMod) * 15.0f, 100.f, Tick))
				{
					static constexpr DodgeDirection kForwardDirs[] = {
						DodgeDirection::Forward,
						DodgeDirection::ForwardLeft,
						DodgeDirection::ForwardRight,
					};
					static constexpr DodgeDirection kBackwardDirs[] = {
						DodgeDirection::Backward,
						DodgeDirection::BackwardLeft,
						DodgeDirection::BackwardRight,
						DodgeDirection::Left,
						DodgeDirection::Right,
					};
					// Probabilistic direction bias: aggression pushes toward
					// forward, caution toward backward/sides. Roll once,
					// cumulative thresholds split the [0, 100) range. Falls
					// through to "any direction" when both modifiers are
					// small or negative.
					const int roll = mt_rand() % 100;
					const int fwdThresh = static_cast<int>(std::max(0.0f, diff.aggressionMod) * 100.0f);
					const int backThresh = fwdThresh + static_cast<int>(std::max(0.0f, diff.cautionMod) * 100.0f);

					DodgeDirection probeDir;
					if (roll < fwdThresh)
					{
						probeDir = kForwardDirs[mt_rand() % std::size(kForwardDirs)];
					}
					else if (roll < backThresh)
					{
						probeDir = kBackwardDirs[mt_rand() % std::size(kBackwardDirs)];
					}
					else
					{
						probeDir = static_cast<DodgeDirection>(mt_rand() % 8);
					}
					AddAction(actor, Actions::Dodge, Directions::TR, false, 0, probeDir);
				}
				// always hard defend if cant attack (attack was parried)
				if (!AttackHandler::GetSingleton()->CanAttack(actor))
				{
					diff.defending = true;
				}
				if (diff.defending)
				{
					ShouldDirectionMatch = true;
					// Personality-paced offense exit, on a clock. Defence used
					// to also end on a switch-count overflow, but a count of
					// direction changes can't tell pressure from tracking — the
					// same number means the target is attacking hard or merely
					// mirroring — and it fired in about a second of active play,
					// which shadowed this entirely and erased the personality
					// spread. Patience sets how long defence runs, aggression
					// how readily it ends once open, and a strong read doubles
					// the duration rather than vetoing the exit, so a committed
					// pre-block isn't undone the same exchange but defence still
					// always terminates.
					// A whiff bypasses the clock outright. The patience timer is
					// for "nothing is happening, take a turn"; a miss is the
					// opposite — a known opening with a deadline. Without this
					// the punish window (0.6s) expires while the actor is still
					if (!target->IsAttacking() &&
						AttackHandler::GetSingleton()->CanAttack(actor) &&
						diff.defendTime >= AISettings::DefendPatienceSeconds *
							std::clamp(1.f + diff.patienceMod, 0.3f, 2.f) *
							(strongRead ? 2.f : 1.f) &&
						// floored: at aggression <= -0.67 the raw expression goes
						// non-positive and the exit can never fire, stranding
						// passive archetypes in defense
						RollTickScaled(std::max(1.f, 2.0f + diff.aggressionMod * 3.0f), 10.f, Tick))
					{
						diff.defending = false;
						// 2, not 1: this is the blocking-limbo grace countdown,
						// and the queued EndBlock needs ~1-2 ticks to execute —
						// with only 1 tick of grace the limbo re-latches
						// defending before the block has actually dropped
						diff.numTimesDirectionsSwitched = 2;
						diff.numTimesDirectionSame = 0;
						if (actor->IsBlocking())
						{
							AddAction(actor, Actions::EndBlock);
						}
						ShouldDirectionMatch = false;
					}
				}
				else
				{
					// Not defending
					if (actor->IsBlocking())
					{
						AddAction(actor, Actions::EndBlock);
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
						SwitchToNewDirection(actor, target, TargetDistSQ);
						//SwitchToNextAttack(actor);
						// keep the once-per-swing edge detector fresh on ticks
						// that skip DirectionMatchTarget — otherwise a swing in
						// flight when the AI leaves the defending state freezes
						// the flag at true and the next defend phase's first
						// swing never fires its belief spike
						diff.lastCallForced = target->IsAttacking();
					}
				}
				else
				{
					diff.lastCallForced = target->IsAttacking();
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
				diff.numTimesDirectionsSwitched = 1;
				diff.numTimesDirectionSame = 0;
				diff.defending = false;
				diff.lastCallForced = false;
			}

			SwitchToNextAttack(actor, false);
		}

		DidAct(actor);

		// Debug panel. Only for the actor actually fighting the player — that's
		// the one whose decisions you're trying to read, and it keeps the panel
		// to a single opponent without needing TDM.
		if (UISettings::ShowDebugOverlay && target->IsPlayerRef())
		{
			DebugSnapshot Snapshot;
			Snapshot.name = actor->GetName();
			Snapshot.archetype = diff.archetype;
			Snapshot.decisionKind = diff.lastDecisionKind;
			for (int i = 0; i < 4; ++i)
			{
				Snapshot.beliefs[i] = diff.directionChangeChance[static_cast<Directions>(i)];
			}
			Snapshot.conditioningStreak = diff.conditioningStreak;
			Snapshot.switchStreak = diff.numTimesDirectionsSwitched;
			Snapshot.sameStreak = diff.numTimesDirectionSame;
			Snapshot.defending = diff.defending;
			Snapshot.spacingMult = diff.spacingMult;
			Snapshot.targetDistSQ = TargetDistSQ;
			Snapshot.weaponLengthSQ = diff.CurrentWeaponLengthSQ;
			Snapshot.actorReach = actor->GetReach();
			Snapshot.targetActorReach = target->GetReach();
			Snapshot.powerWindup = diff.powerWindupEstimate;
			Snapshot.difficulty = static_cast<int>(diff.difficulty);
			Snapshot.guard = DirHandler->GetCurrentDirection(actor);
			Snapshot.targetGuard = DirHandler->GetCurrentDirection(target);
			Snapshot.valid = true;
			UI::SetDebugSnapshot(Snapshot);
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
	auto& diff = DifficultyMap[actor->GetHandle()];
	// stamina filter
	const float Reserve = StaminaReserve(diff.cautionMod);
	const float Commitment = DirectionHandler::GetSingleton()->GetComboProgress(actor) *
		ComboCommitmentDiscount;
	// Tapers instead of ending at a cliff. Without the top band an actor at 41%
	// attempted exactly as freely as one at full, so it would open combos it
	// had no stamina to finish. Left as odds rather than a can-I-finish check:
	// a lone opportunistic swing is often worth it, and that judgement is not
	// worth modelling.
	// Floor takes the reserve but not the commitment — no payoff is worth
	// ending up unable to attack or block.
	if (CurrentStaminaRatio < AttackStaminaFloor + Reserve)
	{
		return false;
	}
	else if (CurrentStaminaRatio < AttackStaminaLow + Reserve - Commitment)
	{
		if (static_cast<int>(mt_rand() % 100) >= AttackChanceLow)
		{
			return false;
		}
	}
	else if (CurrentStaminaRatio < AttackStaminaMid + Reserve - Commitment)
	{
		if (static_cast<int>(mt_rand() % 100) >= AttackChanceMid)
		{
			return false;
		}
	}
	else if (CurrentStaminaRatio < AttackStaminaHigh + Reserve - Commitment)
	{
		if (static_cast<int>(mt_rand() % 100) >= AttackChanceHigh)
		{
			return false;
		}
	}

	if (DirectionHandler::GetSingleton()->GetCurrentDirection(actor) ==
		diff.attackPattern[diff.currentAttackIdx])
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

	// some RNG to attack anyways
	if (mt_rand() % 10 < 1)
	{
		return true;
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
			// leaving the defending state mid-swing (riposte on a block) —
			// re-arm the once-per-swing spike edge detector so the next
			// defend phase's first swing isn't swallowed
			DifficultyMap[actor->GetHandle()].lastCallForced = false;
			if (CurrentStamina < TotalStamina * 0.4f)
			{
				ShouldFeint = false;
			}
			if (val < 6)
			{
				ShouldFeint = false;
			}
			if (ShouldFeint)
			{
				AddAction(actor, Actions::UnblockStartFeint);
			}
			else
			{
				AddAction(actor, Actions::UnblockRiposte);
			}
		}
		else
		{
			AIHandler::GetSingleton()->AddAction(actor, AIHandler::Actions::EndBlock);
		}

	}
	else
	{
		AIHandler::GetSingleton()->AddAction(actor, AIHandler::Actions::EndBlock);
	}
}

void AIHandler::TryBlockExternalCalled(RE::Actor* actor, RE::Actor* attacker)
{
	// In attack lockout, blocking is the actor's only defensive option —
	// the flinch is forced, not rolled.
	const bool lockedOut = !AttackHandler::GetSingleton()->CanAttack(actor);
	std::unique_lock lock(DifficultyMapMtx);
	int mod = (int)CalcAndInsertDifficulty(actor);
	mod += 3;
	mod *= 4;
	int val = mt_rand() % mod;
	DifficultyMap[actor->GetHandle()].defending = true;
	if (val > 0 || lockedOut)
	{
		Directions CurrentTargetDir = DirectionHandler::GetSingleton()->GetCurrentDirection(attacker);
		AddAction(actor, AIHandler::Actions::Block, CurrentTargetDir, lockedOut);
	}
}

// Total belief budget across the four directionChangeChance lines. The
// prediction cascade rolls mt_rand() % 100, so the SUM of the four values is
// the AI's entire probability space — keeping the sum at or under this
// budget preserves cascade fairness while letting a single heavily-invested
// line climb well past an even share. Accumulation
// past the budget drains the strongest OTHER line instead of clamping the
// investment (conservation): convincing the AI you'll attack from one line
// necessarily erodes its belief in the others.
static constexpr int BeliefBudget = 100;

// Ordinal preference for the line the target attacks from next: distinct lines
// advance the combo, same-side ones eat SameSideSpeedPenalty.
//
// False with no combo history — the caller must then skip the blend entirely,
// not treat it as uniform, or it erases learned belief in neutral.
static bool PredictedLinePreference(RE::Actor* target, int (&OutPref)[4])
{
	auto* Dir = DirectionHandler::GetSingleton();
	Directions last;
	if (!Dir->GetLastAttackDirection(target, last))
	{
		return false;
	}
	for (int i = 0; i < 4; ++i)
	{
		const Directions dir = static_cast<Directions>(i);
		if (Dir->IsInComboWindow(target, dir))
		{
			OutPref[i] = 0;  // cannot advance the combo
		}
		else
		{
			OutPref[i] = DirectionHandler::IsLeftSide(dir) ==
				DirectionHandler::IsLeftSide(last) ? 1 : 2;
		}
	}
	return true;
}

// force should not be abused, as it can cause actions to never be finished because they are constantly be overwritten
void AIHandler::DirectionMatchTarget(RE::Actor* actor, RE::Actor* target, bool force)
{
	int mod = (int)CalcAndInsertDifficulty(actor);
	auto& diff = DifficultyMap[actor->GetHandle()];

	// Conditioning-resistance divisor, capped at Normal's value so high tiers
	// stay fake-out-able. Keeps the low-tier gullibility gradient; above
	// Normal, difficulty expresses through reaction speed and decision
	// quality rather than conditioning immunity.
	const int condMod = std::clamp(mod, 1, 3);
	// Belief drains below are per tick, so they convert against this.
	const float Tick = CalcUpdateTimer(actor);

	// Save the direction the AI was tracking at the start of the call. The
	// cascade below may modify ToCounter; we need the original for the
	// mistake-accumulation step at the end (we're conditioning the direction
	// the player just LEFT, not whichever direction we end up guarding).
	const Directions PreviousTracked = diff.lastDirectionTracked;
	Directions ToCounter = PreviousTracked;
	Directions CurrentTargetDir = DirectionHandler::GetSingleton()->GetCurrentDirection(target);

	// Read map values into locals (sanity-clamped to the budget). All math
	// from here operates on these locals and writes back exactly once at the
	// end of the function. Individual lines may legitimately exceed 25 under
	// the belief-budget model — only the SUM is constrained, enforced by the
	// accumulate() helper below.
	int TR = std::min(diff.directionChangeChance[Directions::TR], BeliefBudget);
	int TL = std::min(diff.directionChangeChance[Directions::TL], BeliefBudget);
	int BL = std::min(diff.directionChangeChance[Directions::BL], BeliefBudget);
	int BR = std::min(diff.directionChangeChance[Directions::BR], BeliefBudget);
	if (Settings::ForHonorMode)
	{
		TL = 0;
	}
	// Helper: reference to the local for a given direction.
	auto chanceFor = [&](Directions dir) -> int& {
		switch (dir)
		{
		case Directions::TR: return TR;
		case Directions::TL: return TL;
		case Directions::BL: return BL;
		case Directions::BR: return BR;
		default:             return TR;
		}
	};

	// Panic: wounded fighters overreact to patterns. Recency bias amplifies
	// under stress, so belief accumulation scales up as health drops —
	// 1.0x at 40%+ health, ramping to ~1.75x at 10%. Free legibility through
	// the conditioning arcs (a rattled enemy's belief visibly converges
	// faster), and it cuts both ways: easier to condition-and-feint, but
	// snappier to lock onto a line the player genuinely favors.
	const float MaxHealth = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth);
	const float CurrentHealth = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
	const float HealthRatio = MaxHealth > 0.f ? CurrentHealth / MaxHealth : 1.f;
	const float PanicMult = 1.f + std::clamp((0.4f - HealthRatio) / 0.3f, 0.f, 1.f) * 0.75f;

	// Budget-conserving accumulation: invest into one line; if the total
	// spills past BeliefBudget, drain the strongest OTHER line point-by-point
	// until it fits. Deep commitment to one line is possible, but only by
	// visibly pulling belief away from the rest.
	auto accumulate = [&](Directions dir, int amount) {
		amount = static_cast<int>(static_cast<float>(amount) * PanicMult);
		int& invested = chanceFor(dir);
		invested += amount;
		int excess = (TR + TL + BL + BR) - BeliefBudget;
		while (excess > 0)
		{
			int* largest = nullptr;
			for (int* p : { &TR, &TL, &BL, &BR })
			{
				if (p == &invested)
				{
					continue;
				}
				if (*p > 0 && (largest == nullptr || *p > *largest))
				{
					largest = p;
				}
			}
			if (largest == nullptr)
			{
				// others already empty — clamp the invested line itself
				invested -= excess;
				break;
			}
			(*largest)--;
			excess--;
		}
	};

	// Set in the match branch to deny free mid-swing guard acquisition; the
	// belief/tracking bookkeeping above still runs when it's set.
	bool SuppressSwitch = false;

	// Diagnostic tag for VerboseLogging: which mechanism produced this tick's
	// guard decision. Used to discriminate "buildup problem" (cascade-wrong
	// dominating with high beliefs) from "observe-react loop problem"
	// (stay/gate-suppressed dominating with low beliefs) in playtest logs.
	const char* decisionKind = "hold";

	// Consume the perception layer: every switch the target completed since
	// the last decision tick, recorded per-frame in RunActor.
	//
	// Events condition; samples decide. Observed events drive ONLY belief
	// accumulation — branch selection and the tracking counters stay keyed
	// to the tick-sampled net change, because numTimesDirectionSame also
	// feeds the acquisition gate and the limbo countdown. Driving them
	// per-event let fidgety play churn the hold state.
	const int ObservedSwitches = std::min(diff.observedSwitchCount, static_cast<int>(diff.observedSwitchesFrom.size()));
	diff.observedSwitchCount = 0;

	// One accumulation per observed switch, to the line the target left.
	// False on an empty ring so the caller uses the sampled fallback instead.
	auto accumulateObserved = [&]() {
		if (ObservedSwitches <= 0)
		{
			return false;
		}
		const int DifficultyMod = AISettings::BeliefAccumBase * AISettings::AIMistakeRatio;
		const float baitDamping = 1.0f - std::max(0.0f, diff.baitTendency) * 0.7f;
		const int streak = std::max(1, diff.conditioningStreak);
		for (int i = 0; i < ObservedSwitches; ++i)
		{
			// float division BEFORE the multipliers — int division truncated
			// to zero at sub-default AIMistakeRatio values for high tiers
			accumulate(diff.observedSwitchesFrom[i], static_cast<int>(
				(static_cast<float>(DifficultyMod) / static_cast<float>(condMod)) *
				static_cast<float>(streak) *
				baitDamping + 0.5f));  // round, don't truncate — truncation
				                       // zeroed bait-archetype conditioning
				                       // at low streaks
		}
		return true;
	};

	// Belief blended toward the lines the combo makes likely. Computed every
	// call, before accumulation, so the cached copy the HUD reads stays current.
	{
		const int Belief[4] = { TR, TL, BL, BR };
		const int Total = TR + TL + BL + BR;
		int Pref[4] = {};
		const bool HasStructure = PredictedLinePreference(target, Pref);
		if (Settings::ForHonorMode)
		{
			// TL folds into TR in this mode, so it must not be reintroduced by
			// the structural term the way the belief above is already zeroed.
			Pref[static_cast<int>(Directions::TL)] = 0;
		}
		const int PrefSum = HasStructure ? Pref[0] + Pref[1] + Pref[2] + Pref[3] : 0;
		// Bait scales it: Counter waits for the commit, Aggressor never notices.
		// Zero in neutral, leaving learned belief untouched.
		// Reading combo structure is knowledge of the rules, not reflexes, so it
		// scales with tier as well as disposition. Bait keeps the character;
		// the tier sets the ceiling.
		constexpr int LowTier = static_cast<int>(Difficulty::VeryEasy);
		constexpr int HighTier = static_cast<int>(Difficulty::Legendary);
		const float TierT = std::clamp(
			static_cast<float>(static_cast<int>(diff.difficulty) - LowTier) /
			static_cast<float>(HighTier - LowTier), 0.f, 1.f);
		const float TierScale = AISettings::ComboReadLowTierScale +
			(1.f - AISettings::ComboReadLowTierScale) * TierT;
		const float k = HasStructure
			? std::clamp(AISettings::ComboReadStrength * diff.baitTendency * TierScale, 0.f, 1.f)
			: 0.f;
		for (int i = 0; i < 4; ++i)
		{
			const float structural = (PrefSum > 0)
				? static_cast<float>(Total) * static_cast<float>(Pref[i]) / static_cast<float>(PrefSum)
				: 0.f;
			// Round: the sum is the cascade's fire rate, and four truncations shed up to 3.
			diff.lastCascadeWeights[i] = static_cast<int>(
				static_cast<float>(Belief[i]) * (1.f - k) + structural * k + 0.5f);
		}
	}

	if (ToCounter != CurrentTargetDir)
	{
		// Target's net direction changed since last tick.
		const int DifficultyMod = AISettings::BeliefAccumBase * AISettings::AIMistakeRatio;
		diff.numTimesDirectionsSwitched++;
		diff.conditioningStreak = std::min(diff.conditioningStreak + 1, AISettings::MaxDirectionTracked);
		diff.numTimesDirectionSame = 0;

		// Cascade: roll against accumulated chances. The AI may pick a wrong
		// direction here based on conditioning
		int random = mt_rand() % 100;
		bool found = false;
		random -= diff.lastCascadeWeights[(int)Directions::TR];
		if (!found && random < 0) { ToCounter = Directions::TR; found = true; }
		random -= diff.lastCascadeWeights[(int)Directions::TL];
		if (!found && random < 0) { ToCounter = Directions::TL; found = true; }
		random -= diff.lastCascadeWeights[(int)Directions::BL];
		if (!found && random < 0) { ToCounter = Directions::BL; found = true; }
		random -= diff.lastCascadeWeights[(int)Directions::BR];
		if (!found && random < 0) { ToCounter = Directions::BR; found = true; }

		if (found)
		{
			// Wrong-pick fired — drain all chances by 5 ("used up" some
			// conditioning).
			const int drainAmount = AISettings::BeliefCascadeDrain;
			TR = std::max(0, TR - drainAmount);
			TL = std::max(0, TL - drainAmount);
			BL = std::max(0, BL - drainAmount);
			BR = std::max(0, BR - drainAmount);
			decisionKind = (ToCounter == CurrentTargetDir) ? "cascade-right" : "cascade-wrong";
		}

		else
		{
			// No conditioned pick — roll baseline track-or-stay. The low rate
			// (mt_rand()%50 < mod = 2-12%) is DELIBERATE: reactive tracking is
			// meant to be WEAK so the AI defends by READING (conditioning /
			// anticipation), not by reflex.
			const int rollBaseline = mt_rand() % 50;
			if (rollBaseline < mod)
			{
				ToCounter = CurrentTargetDir;
				decisionKind = "react-track";
			}
			else
			{
				decisionKind = "stay";
			}
			// Slow forgetting on all four.
			const int forgetDrain = DrainPerTick(ForgetDrainPerSecond, Tick, diff.beliefDrainRemainder);
			TR = std::max(0, TR - forgetDrain);
			TL = std::max(0, TL - forgetDrain);
			BL = std::max(0, BL - forgetDrain);
			BR = std::max(0, BR - forgetDrain);
		}

		// Sampled fallback for the first tick against a target, before the ring
		// has anything in it. After the drain, so it isn't immediately undone.
		if (!accumulateObserved())
		{
			const float baitDamping = 1.0f - std::max(0.0f, diff.baitTendency) * 0.7f;
			accumulate(PreviousTracked, static_cast<int>(
				(static_cast<float>(DifficultyMod) / static_cast<float>(condMod)) *
				static_cast<float>(std::max(1, diff.conditioningStreak)) *
				baitDamping + 0.5f));  // round, don't truncate
		}
	}
	else
	{
		// Target stayed in same direction. AI is "tracking."
		// Drain non-matched directions every tick during the hold (ebb), and
		// reinforce the matched direction ONLY on the first tick of a new
		// hold (the transition event from mismatch → match). Without the
		// event-gate, accumulation fires every tick during a hold and
		// quickly saturates the matched direction at the cap. Tying it to
		// the transition keeps the conditioning "ebb and flow with action."
		diff.numTimesDirectionsSwitched = std::max(diff.numTimesDirectionsSwitched - 1, 0);
		diff.conditioningStreak = std::max(diff.conditioningStreak - 1, 0);
		diff.numTimesDirectionSame++;
		// Drain continues every tick of the hold, no upper bound on hold length.
		// Not scaled by mod: difficulty in the conditioning economy lives in
		// the buildup divisor instead. It IS scaled by tick length — the note
		// that drains are tick-paced while buildup is action-paced was right,
		// but leaving it per-tick is what made higher tiers forget faster.
		// 13.3/s, up from the equivalent of 6.7: with the perception layer
		// feeding accumulation from every observed switch, beliefs sat high and
		// stale at the lower rate — the AI guarded its predictions instead of
		// the actual attack line.
		const int holdDrain = DrainPerTick(HoldDrainPerSecond, Tick, diff.beliefDrainRemainder);
		TR = std::max(0, TR - holdDrain);
		TL = std::max(0, TL - holdDrain);
		BL = std::max(0, BL - holdDrain);
		BR = std::max(0, BR - holdDrain);
		if (diff.numTimesDirectionSame == 1)
		{
			const int DifficultyMod = AISettings::BeliefAccumBase * AISettings::AIMistakeRatio;
			// floor at 1 (when learning is enabled at all) so low ini values
			// don't integer-truncate this path to zero at high tiers
			if (DifficultyMod > 0)
			{
				accumulate(ToCounter, std::max(1, DifficultyMod / condMod));
			}
		}
		// Mid-swing acquisition gate, TIME-based: the guard may HOLD against
		// an attack but may not ARRIVE at the correct counter within the
		// commit window of the line change — a deterministic one-tempo
		// answer to switch-and-attack would invalidate the commit
		// 50/50. Conditioning on the line the guard currently covers extends
		// the floor (fixation): a deeply-conditioned AI abandons its
		// expectation late. Beyond floor + fixation the gate opens and the
		// match branch's tracking is guaranteed — on a long enough attack
		// the AI ALWAYS adjusts; it never sits in a wrong-line block for a
		// full heavy. (The old tick-count gate scaled with tier tick length
		// and made low tiers unable to adjust within any attack's duration.)
		//
		// Probabilistic within the window: the AI beats the gate at the
		// baseline reaction odds (mod/50 — 2% VeryEasy, 12% Legendary).
		if (force)
		{
			const Directions guardDir = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
			const Directions coveredLine = DirectionHandler::GetCounterDirection(guardDir);
			const float fixation = (static_cast<float>(chanceFor(coveredLine)) / static_cast<float>(BeliefBudget)) * AISettings::ConditionedFixationSeconds;
			if (diff.timeSinceLineChange < CommitWindow(diff) + fixation)
			{
				const int gateRoll = static_cast<int>(mt_rand() % 50);
				if (gateRoll >= mod)
				{
					SuppressSwitch = true;
					decisionKind = "gate-suppressed";
				}
				else
				{
					decisionKind = "gate-beaten";
				}
			}
		}

		// Net-zero wiggles (switch away and back between ticks) still
		// condition — that's the perception layer's point — they just don't
		// perturb the hold/streak state above.
		accumulateObserved();
	}

	// Force bump: target is actively attacking. Focus AI attention on the
	// attacking direction (big spike) and de-emphasize the other three
	// directions (faster drain). Represents the AI "locking on" once a
	// commit happens — when the player has thrown an attack from a specific
	// guard, prediction converges hard on that direction and forgets prior
	// mix-up conditioning. Budget-conserving, so sustained commitment keeps
	// paying past an even share by draining the other lines.
	if (force)
	{
		const int DifficultyMod = AISettings::BeliefAccumBase * AISettings::AIMistakeRatio;
		// 3x the normal accumulation bump, but only on the FIRST tick of a
		// swing (edge-detected via lastCallForced): each attack teaches the
		// AI the line once. Per-tick spiking made slow attacks teach 4-5x
		// more than fast ones and, with the belief budget, let one or two
		// attacks saturate a line before the exchange even resolved.
		if (!diff.lastCallForced && DifficultyMod > 0)
		{
			accumulate(CurrentTargetDir, std::max(1, (DifficultyMod * 3) / condMod));
		}

		// Drain the other three directions faster than the regular tick drain
		// so accumulated bias on non-attacking directions doesn't fight the
		// spike on the attacking one.
		constexpr int forceDrain = 3;
		if (CurrentTargetDir != Directions::TR) TR = std::max(0, TR - forceDrain);
		if (CurrentTargetDir != Directions::TL) TL = std::max(0, TL - forceDrain);
		if (CurrentTargetDir != Directions::BL) BL = std::max(0, BL - forceDrain);
		if (CurrentTargetDir != Directions::BR) BR = std::max(0, BR - forceDrain);
	}

	// Single writeback to the map.
	diff.directionChangeChance[Directions::TR] = TR;
	diff.directionChangeChance[Directions::TL] = TL;
	diff.directionChangeChance[Directions::BL] = BL;
	diff.directionChangeChance[Directions::BR] = BR;

	diff.lastDecisionKind = decisionKind;

	if (Settings::VerboseLogging)
	{
		logger::info("[dmt] {} kind={} targetDir={} guard->{} force={} beliefs[TR:{} TL:{} BL:{} BR:{}] streak={} same={} obs={}",
			actor->GetName(), decisionKind, (int)CurrentTargetDir,
			(int)DirectionHandler::GetCounterDirection(ToCounter), force,
			TR, TL, BL, BR,
			diff.numTimesDirectionsSwitched, diff.numTimesDirectionSame, ObservedSwitches);
	}

	if (!SuppressSwitch)
	{
		const Directions ToSwitch = DirectionHandler::GetCounterDirection(ToCounter);
		QueueDirectionSwitch(actor, ToSwitch, force);

		if (force && ToCounter == CurrentTargetDir &&
			IsPreparedToBlock(actor, diff) && IsBlockableSwing(target) &&
			!CanAnswerLine(actor, target, diff))
		{
			AddAction(actor, Actions::Block, Directions::TR, true);
			if (Settings::VerboseLogging)
			{
				logger::info("[dmt] {} read-commit block carry ({})", actor->GetName(), decisionKind);
			}
		}
	}

	diff.lastDirectionTracked = CurrentTargetDir;
	diff.lastCallForced = force;

}

void AIHandler::GetGuardConditioningExternalCalled(RE::Actor* actor, std::array<int, 4>& outConditioning, float& outConfidence)
{
	outConditioning.fill(0);
	outConfidence = 0.f;
	if (!actor)
	{
		return;
	}
	// Pure read under shared lock; no DirectionHandler locks taken in here
	// (GetCounterDirection is a static pure function) so this is safe to call
	// from the render-prep path without nesting locks across handlers.
	std::shared_lock lock(DifficultyMapMtx);
	auto iter = DifficultyMap.find(actor->GetHandle());
	if (iter == DifficultyMap.end())
	{
		return;
	}
	// Positive mistakeRatio = this AI has been winning recent exchanges;
	// normalize against the clamp range for the HUD. Negative (rattled)
	// reads as 0 — the arcs only warm, never cool.
	outConfidence = std::clamp(iter->second.mistakeRatio / MaxMistakeRange, 0.f, 1.f);
	// directionChangeChance is keyed by the TARGET line the AI expects an
	// attack from; the HUD wants guard space — the marker this belief pulls
	// the guard toward — so map through the counter direction.
	// Cached weights, not raw belief, so the arcs show what the AI rolled against.
	for (int i = 0; i < 4; ++i)
	{
		const Directions dir = static_cast<Directions>(i);
		const int value = std::clamp(iter->second.lastCascadeWeights[i], 0, BeliefBudget);
		outConditioning[(int)DirectionHandler::GetCounterDirection(dir)] = value;
	}
}

void AIHandler::SwitchToNewDirection(RE::Actor* actor, RE::Actor* target, float TargetDistSQ)
{
	// This will queue up this event if you cant switch instead
	RE::ActorHandle ActorHandle = actor->GetHandle();
	CalcAndInsertDifficulty(actor);
	auto& diff = DifficultyMap[ActorHandle];
	Directions CurrentDirection = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
	Directions TargetDirection = DirectionHandler::GetSingleton()->GetCurrentDirection(target);
	// This will queue up this event if you cant switch instead
	Directions CounterDirection = DirectionHandler::GetCounterDirection(TargetDirection);
	unsigned idx = diff.currentAttackIdx;

	float MaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
	float CurrentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	float CurrentStaminaRatio = CurrentStamina / MaxStamina;
	int mod = (int)CalcAndInsertDifficulty(actor);
	const float Reserve = StaminaReserve(diff.cautionMod);
	const float Tick = CalcUpdateTimer(actor);
	const float Commitment = DirectionHandler::GetSingleton()->GetComboProgress(actor) *
		ComboCommitmentDiscount;
	if (diff.attackPattern[idx] != CounterDirection)
	{
		SwitchToNextAttack(actor, true);
		// Floor is the only hard gate; AttackStaminaOffLine is now where the
		// rate reaches full rather than a cliff below which nothing happens.
		if (CurrentStaminaRatio > AttackStaminaFloor + Reserve && TargetDistSQ < diff.CurrentWeaponLengthSQ)
		{
			// Difficulty- and aggression-scaled attack frequency, as a rate so
			// the tier's update timer isn't a second hidden multiplier on top
			// of mod. This is the dominant difficulty axis in play — a tier
			// that barely attacks reads as easy however fast it answers.
			const float StaminaScale = StaminaAttackScale(
				CurrentStaminaRatio, AttackStaminaOffLine, Reserve, Commitment);
			if (RollPerSecond((mod + diff.aggressionMod * 1.5f) * AttackRatePerMod * StaminaScale, Tick))
			{
				// the attack lands from the queued line if one is still in
				// transit, otherwise from the one already held
				DirectionHandler* DirHandler = DirectionHandler::GetSingleton();
				Directions AttackDir;
				if (!DirHandler->HasQueuedDirection(actor, AttackDir))
				{
					AttackDir = DirHandler->GetCurrentDirection(actor);
				}
				// Spend the finisher as a power attack
				if (DirHandler->WouldCompleteCombo(actor, AttackDir) &&
					CurrentStaminaRatio > ComboFinisherStaminaRatio + Reserve)
				{
					AddAction(actor, AIHandler::Actions::PowerAttack);
				}
				// PowerAttackTendency-modulated attack-type choice: NPCs that
				// favor power attacks pick them more often over light attacks.
				else if (mt_rand() % 3 < (2.0f - diff.powerAttackTendency * 1.5f))
				{
					AddAction(actor, AIHandler::Actions::Attack);
				}
				else
				{
					AddAction(actor, AIHandler::Actions::PowerAttack);
				}
			}
		}
	}
	else
	{
		// if the attack direction is where my enemy is blocking, try feinting.
		// Feint-tendency-modulated: feint-leaning NPCs are more likely to
		// feint instead of swinging straight into a block.
		bool ShouldFeint = mt_rand() % 3 < (2.0f + diff.feintTendency * 1.0f);

		if (CurrentStaminaRatio > AttackStaminaFloor + Reserve)
		{
			// Same gate as the off-line branch above, tapered the same way.
			// Keeps its own full-rate point: this branch is already on the
			// counter line, so it reaches full rate earlier.
			const float StaminaScale = StaminaAttackScale(
				CurrentStaminaRatio, AttackStaminaFeint, Reserve, Commitment);
			if (RollPerSecond((mod + diff.aggressionMod * 1.5f) * AttackRatePerMod * StaminaScale, Tick))
			{
				if (ShouldFeint && TargetDistSQ < diff.CurrentWeaponLengthSQ)
				{
					SwitchToNextAttack(actor, false);
					int i = mt_rand() % 3;

					if (i == 2)
					{
						AddAction(actor, AIHandler::Actions::StartFeint);
					}
					else if (i == 1)
					{
						AddAction(actor, AIHandler::Actions::PowerAttack);
					}
					else
					{
						AddAction(actor, AIHandler::Actions::Attack);
					}
				}
			}


		}
		else
		{
			ShouldFeint = false;
		}
		if (!ShouldFeint)
		{
			// switch between actively changing directions if our current direction is matching
			if (mt_rand() % 4 < 2)
			{
				Directions ToAvoid = DirectionHandler::GetSingleton()->GetCurrentDirection(target);
				QueueDirectionSwitch(actor, ToAvoid, false);
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
				QueueDirectionSwitch(actor, ToSwitch, false);
			}
		}
		
	}
}

void AIHandler::ReduceDifficulty(RE::Actor* actor)
{
	if (DifficultyMap.contains(actor->GetHandle()))
	{
		auto& diff = DifficultyMap[actor->GetHandle()];
		if (diff.mistakeRatio <= 0.01f && diff.mistakeRatio >= -0.01f)
		{
			diff.mistakeRatio = 0.f;
		}
		else
		{
			// was *= 0.5 per tick — a sub-second wipe on any disengage, which
			// made composure (now visible as the arcs' red tint) effectively
			// binary. 0.9 gives ~a 1.5-3s half-life: brief measure breaks
			// keep the streak alive, a real disengage still resets it.
			diff.mistakeRatio *= 0.9f;
		}
		diff.numTimesDirectionsSwitched =
			std::max(diff.numTimesDirectionsSwitched - 1, 0);
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
		auto& diff = DifficultyMap[actor->GetHandle()];
		diff.directionChangeChance[Directions::TR] = 0;
		diff.directionChangeChance[Directions::TL] = 0;
		diff.directionChangeChance[Directions::BL] = 0;
		diff.directionChangeChance[Directions::BR] = 0;

		diff.mistakeRatio = 0.f;

	}
	else
	{
		logger::error("couldn't find in map!");
	}
}

bool AIHandler::TryAttack(RE::Actor* actor, bool force)
{


	// since this is a forced attack, it happens outside of the normal AI attack loop so we need to add checks here as well
	if (AttackHandler::GetSingleton()->CanInitiateAttack(actor))
	{

		AttackHandler::GetSingleton()->DoAttack(actor);
		return true;
		/*
		
			// load attack data into actor to ensure attacks register correctly
		if (LoadCachedAttack(actor, force))
		{
			// set actor state to show that actor is now attacking
			actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kSwing;
			actor->NotifyAnimationGraph("attackStart");
			return true;
		}	
		*/

	}
	return false;
}

bool AIHandler::TryPowerAttack(RE::Actor* actor)
{

	// since this is a forced attack, it happens outside of the normal AI attack loop so we need to add checks here as well
	if (AttackHandler::GetSingleton()->CanInitiateAttack(actor) && !actor->IsBlocking())
	{
		// not sure why this doesnt work
		//AttackHandler::GetSingleton()->DoPowerAttack(actor);
		//return true;
		
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

bool AIHandler::HasPendingDirectionSwitch(RE::Actor* actor) const
{
	{
		std::shared_lock Lock(DirectionQueueMtx);
		if (DirectionQueue.contains(actor->GetHandle()))
		{
			return true;
		}
	}
	Directions Queued;
	return DirectionHandler::GetSingleton()->HasQueuedDirection(actor, Queued);
}

void AIHandler::QueueDirectionSwitch(RE::Actor* actor, Directions dir, bool force)
{
	std::unique_lock DirLock(DirectionQueueMtx);
	// force means the target is mid-swing, so this is a defensive correction,
	// not deliberate repositioning.
	// Any pending deliberate switch is dropped rather than left to land after
	// and drag the guard back off the line being defended.
	if (force)
	{
		DirectionQueue.erase(actor->GetHandle());
		DirectionHandler::GetSingleton()->WantToSwitchTo(actor, dir, true);
		return;
	}
	auto Iter = DirectionQueue.find(actor->GetHandle());
	if (Iter == DirectionQueue.end())
	{
		DirectionQueue[actor->GetHandle()] = { dir, force, GuardInputSeconds };
		return;
	}
	// Deciding the same line again is not a new movement. Re-arming it every
	// tick would stall the switch forever once the decision interval dropped
	// below GuardInputSeconds.
	if (Iter->second.dir == dir)
	{
		return;
	}
	// Redirecting mid-reach kinda weird tbh
	const bool Opposite =
		((static_cast<int>(Iter->second.dir) + 2) % 4) == static_cast<int>(dir);
	Iter->second.dir = dir;
	Iter->second.force = force;
	if (Opposite)
	{
		Iter->second.timeLeft = GuardInputSeconds;
	}
}

void AIHandler::SwitchToNextAttack(RE::Actor* actor, bool force)
{
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		CalcAndInsertDifficulty(actor);
	}
	auto& diff = DifficultyMap[actor->GetHandle()];
	diff.numTimesDirectionSame = 0;
	int idx = diff.currentAttackIdx;
	if (idx < diff.attackPattern.size())
	{
		Directions dir = diff.attackPattern[idx];
		if ((int)dir > 3)
		{
			if (Settings::VerboseLogging) logger::info("[ai] {} had error in attack pattern to {}", actor->GetName(), (int)dir);
		}
		QueueDirectionSwitch(actor, dir, false);
	}
	else
	{
		diff.currentAttackIdx = 0;
	}
}

Directions AIHandler::GetNextAttack(RE::Actor* actor)
{
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		CalcAndInsertDifficulty(actor);
	}
	auto& diff = DifficultyMap[actor->GetHandle()];
	int idx = diff.currentAttackIdx;
	if (idx >= diff.attackPattern.size())
	{
		diff.currentAttackIdx = 0;
		idx = 0;
	}
	return diff.attackPattern[idx];
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
	auto& diff = DifficultyMap[actor->GetHandle()];
	if (!diff.cachedBasicAttackData)
	{
		return false;
	}
	if (!force)
	{
		if (!actor->GetActorRuntimeData().currentProcess->high->attackData)
		{
			actor->GetActorRuntimeData().currentProcess->high->attackData = diff.cachedBasicAttackData;
			return true;
		}
	}
	else
	{
		actor->GetActorRuntimeData().currentProcess->high->attackData = diff.cachedBasicAttackData;
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
		auto& diff = DifficultyMap[actor->GetHandle()];
		if (!diff.cachedPowerAttackData)
		{
			return false;
		}
		actor->GetActorRuntimeData().currentProcess->high->attackData = diff.cachedPowerAttackData;
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
		if (Settings::VerboseLogging) logger::info("[ai] {} got difficulty level {}", actor->GetName(), (int)ret);
		AIDifficulty aidiff = { ret, 0.f };
		DifficultyMap[actor->GetHandle()] = aidiff;
		DifficultyMap[actor->GetHandle()].lastDirectionsEncountered.reserve(MaxDirs);

		// iterate until we found the attackstart

		auto AttackData = FindActorAttackData(actor);
		//default
		DifficultyMap[actor->GetHandle()].lastDirectionTracked = Directions::TR;
		if (AttackData)
		{
			if (Settings::VerboseLogging) logger::info("[ai] {} has basic attack data", actor->GetName());
			DifficultyMap[actor->GetHandle()].cachedBasicAttackData = AttackData;
		}

		auto PowerAttackData = FindActorPowerAttackData(actor);
		if (PowerAttackData)
		{
			if (Settings::VerboseLogging) logger::info("[ai] {} has power attack data", actor->GetName());
			DifficultyMap[actor->GetHandle()].cachedPowerAttackData = PowerAttackData;
		}

		// generate attack patterns ahead of time
		// instead of using rand, use their native handle as that is unique per actor and is the same between saves
		// so you can learn after dying
		uint32_t handle = actor->GetHandle().native_handle();
		int Idx = handle % TotalAttackCombos;
		for (unsigned i = 0; i < AttackComboLength; ++i)
		{
			DifficultyMap[actor->GetHandle()].attackPattern.push_back(AIAttackCombo[Idx][i]);
		}


		// create modifier to see how good or bad the AI is at judging distance
		DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched = 0;
		DifficultyMap[actor->GetHandle()].currentAttackIdx = 0;
		DifficultyMap[actor->GetHandle()].defending = false;

		// seed with deterministic input
		DifficultyMap[actor->GetHandle()].npcRand.seed(handle);

		{
			// Seed only — RunActor recomputes this every decision tick from
			// Actor::GetReach, which needs no Precision and survives a weapon
			// swap without invalidation. Includes the lunge, same as there.
			const float CurrentWeaponLength = actor->GetReach() + AttackLungeUnits;
			DifficultyMap[actor->GetHandle()].CurrentWeaponLengthSQ =
				CurrentWeaponLength * CurrentWeaponLength;
		}

		DifficultyMap[actor->GetHandle()].DodgeCooldown = 0.f;
		DifficultyMap[actor->GetHandle()].BashCooldown = 0.f;

		// Personality: pick an archetype based on the actor's native handle,
		// add small per-NPC jitter so two NPCs with the same archetype still
		// feel slightly different. Archetype values are defined at file scope
		// (see kPersonalityArchetypes near the top of this file) for tuning
		// visibility. Handle is stable within a session; archetype assignment
		// is deterministic per-NPC.
		{
			std::uint32_t seed = actor->GetHandle().native_handle();
			seed ^= seed >> 16;
			seed *= 0x7feb352dU;
			seed ^= seed >> 15;
			seed *= 0x846ca68bU;
			seed ^= seed >> 16;

			const auto& arch = kPersonalityArchetypes[seed % kNumPersonalityArchetypes];

			// Jitter is small (±0.1) so the archetype identity dominates but
			// no two NPCs of the same archetype play identically.
			auto jitter = [&](unsigned shift) -> float {
				const std::uint32_t byte = (seed >> shift) & 0xFFu;
				return ((static_cast<float>(byte) / 127.5f) - 1.0f) * 0.1f;
			};

			auto& d = DifficultyMap[actor->GetHandle()];
			d.archetype           = arch.name;
			d.aggressionMod       = arch.aggression  + jitter(0);
			d.patienceMod         = arch.patience    + jitter(8);
			d.baitTendency        = arch.bait        + jitter(16);
			d.cautionMod          = arch.caution     + jitter(24);
			std::uint32_t seed2 = seed * 0x9E3779B1U;
			seed2 ^= seed2 >> 16;
			d.powerAttackTendency = arch.powerAttack + (((static_cast<float>(seed2 & 0xFFu) / 127.5f) - 1.0f) * 0.1f);
			d.feintTendency       = arch.feint       + (((static_cast<float>((seed2 >> 8) & 0xFFu) / 127.5f) - 1.0f) * 0.1f);

			// Distance judgement: +-25% at VeryEasy down to ~+-4% at Legendary.
			const float judgeSpread = 0.25f / static_cast<float>(std::max(1, (int)ret));
			d.reachMisjudge = 1.f + (((static_cast<float>((seed2 >> 16) & 0xFFu) / 127.5f) - 1.0f) * judgeSpread);

			if (Settings::VerboseLogging)
			{
				logger::info("{} archetype={} (agg={:.2f} pat={:.2f} bait={:.2f} caut={:.2f} pow={:.2f} feint={:.2f})",
					actor->GetName(), arch.name,
					d.aggressionMod, d.patienceMod, d.baitTendency, d.cautionMod, d.powerAttackTendency, d.feintTendency);
			}
		}
	}

	return ret;
}

// Both disconfirmation paths share this so the severity lives in one place.
static void DisconfirmLine(int& chance)
{
	const int Loss = std::max(AISettings::BeliefDisconfirmFloor,
		static_cast<int>(chance * AISettings::BeliefDisconfirmFraction));
	chance = std::max(0, chance - Loss);
}

void AIHandler::SignalWrongLineBlockExternalCalled(RE::Actor* actor)
{
	if (!actor)
	{
		return;
	}
	// Read guard state before taking the difficulty lock (lock-order hygiene).
	const Directions guardDir = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
	const Directions failedLine = DirectionHandler::GetCounterDirection(guardDir);
	std::unique_lock lock(DifficultyMapMtx);
	auto iter = DifficultyMap.find(actor->GetHandle());
	if (iter == DifficultyMap.end())
	{
		return;
	}
	// The belief that put the guard on the wrong line just failed in the
	// world — drain it proportionally, harder than any passive decay. Deep
	// wrong convictions shatter hardest; repeated feints into the same false
	// line stop paying forever.
	DisconfirmLine(iter->second.directionChangeChance[failedLine]);
}

void AIHandler::SignalBadThingExternalCalled(RE::Actor* actor, Directions attackDir)
{
	// Read guard state before taking the difficulty lock — GetCurrentDirection
	// takes ActiveDirectionsMtx and we avoid nesting locks across handlers.
	const bool wasBlocking = actor->IsBlocking();
	const Directions guardDir = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);

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

	// Disconfirmation FALLBACK only: the primary wrong-line drain fires from
	// HandleBlock's strip at the prehit hook (SignalWrongLineBlockExternalCalled),
	// because the strip clears IsBlocking before this hit event runs — this
	// branch survives for the rare paths where a block reaches the hit event
	// intact (audit finding: it is otherwise dead for its designed trigger).
	if (wasBlocking && guardDir != DirectionHandler::GetCounterDirection(attackDir))
	{
		const Directions failedLine = DirectionHandler::GetCounterDirection(guardDir);
		DisconfirmLine(Iter->second.directionChangeChance[failedLine]);
	}

	// increase percentage of blocking this location
	IncreaseBlockChance(actor, attackDir, mt_rand() % 10 + 5, AISettings::BeliefSpreadModifierHit);

	float NewMistakeRatio = DifficultyMap[actor->GetHandle()].mistakeRatio - AISettings::AIGrowthFactor;
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
	IncreaseBlockChance(actor, attackedDir, mt_rand() % 5 + 3, AISettings::BeliefSpreadModifierBlock);


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
	auto& diff = DifficultyMap[actor->GetHandle()];
	diff.directionChangeChance[dir] += percent;
	diff.directionChangeChance[dir] = std::min(BeliefBudget, diff.directionChangeChance[dir]);
	switch (dir)
	{
	case Directions::TR:
		diff.directionChangeChance[Directions::TL] -= percent / modifier;
		diff.directionChangeChance[Directions::BL] -= percent / modifier;
		diff.directionChangeChance[Directions::BR] -= percent / modifier;
		break;
	case Directions::TL:
		diff.directionChangeChance[Directions::TR] -= percent / modifier;
		diff.directionChangeChance[Directions::BL] -= percent / modifier;
		diff.directionChangeChance[Directions::BR] -= percent / modifier;
		break;
	case Directions::BL:
		diff.directionChangeChance[Directions::TL] -= percent / modifier;
		diff.directionChangeChance[Directions::TR] -= percent / modifier;
		diff.directionChangeChance[Directions::BR] -= percent / modifier;
		break;
	case Directions::BR:
		diff.directionChangeChance[Directions::TL] -= percent / modifier;
		diff.directionChangeChance[Directions::BL] -= percent / modifier;
		diff.directionChangeChance[Directions::TR] -= percent / modifier;
		break;
	}
	diff.directionChangeChance[Directions::TR] = std::max(0, diff.directionChangeChance[Directions::TR]);
	diff.directionChangeChance[Directions::TL] = std::max(0, diff.directionChangeChance[Directions::TL]);
	diff.directionChangeChance[Directions::BL] = std::max(0, diff.directionChangeChance[Directions::BL]);
	diff.directionChangeChance[Directions::BR] = std::max(0, diff.directionChangeChance[Directions::BR]);

	// Enforce the shared belief budget (see BeliefBudget above): the cascade
	// rolls % 100, so the sum must stay within it. Drain the strongest line
	// other than the one just boosted until the total fits.
	constexpr Directions AllDirs[4] = { Directions::TR, Directions::TL, Directions::BL, Directions::BR };
	int total = 0;
	for (Directions d : AllDirs)
	{
		total += diff.directionChangeChance[d];
	}
	int excess = total - BeliefBudget;
	while (excess > 0)
	{
		Directions largest = dir;
		int largestVal = 0;
		for (Directions d : AllDirs)
		{
			if (d == dir)
			{
				continue;
			}
			if (diff.directionChangeChance[d] > largestVal)
			{
				largestVal = diff.directionChangeChance[d];
				largest = d;
			}
		}
		if (largestVal <= 0)
		{
			diff.directionChangeChance[dir] -= excess;
			break;
		}
		diff.directionChangeChance[largest]--;
		excess--;
	}
}

float AIHandler::CalcUpdateTimer(RE::Actor* actor)
{
	std::shared_lock lock(AIHandlerDataMtx);
	DifficultyUpdateTimerMtx.lock_shared();
	Difficulty mod = CalcAndInsertDifficulty(actor);
	assert(DifficultyUpdateTimer.contains(mod));
	float base = DifficultyUpdateTimer.at(mod);
	DifficultyUpdateTimerMtx.unlock_shared();
	if (mod < Difficulty::VeryHard && NumPlayerAttackers > 3)
	{
		base += NumPlayerAttackers * 0.05;
	}
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
	std::shared_lock lock(AIHandlerDataMtx);
	DifficultyActionTimerMtx.lock_shared();
	const Difficulty mod = CalcAndInsertDifficulty(actor);
	assert(DifficultyActionTimer.contains(mod));
	float base = DifficultyActionTimer.at(mod);
	DifficultyActionTimerMtx.unlock_shared();
	// find, not at(): this function never holds DifficultyMapMtx, so RemoveActor
	// can erase between the insert above and this read. A miss means the actor
	// left combat mid-call, and skipping its mistake-ratio adjustment is correct.
	auto DiffIter = DifficultyMap.find(actor->GetHandle());
	if (DiffIter != DifficultyMap.end())
	{
		float adjust = DiffIter->second.mistakeRatio / MaxMistakeRange;  // -1..1
		if (adjust < 0.f)
		{
			adjust *= 0.5f;
		}
		base *= 1.f + adjust * 0.1f;
	}
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

	DirectionQueueMtx.lock();
	DirectionQueue.clear();
	DirectionQueueMtx.unlock();

	UpdateTimerMtx.lock();
	UpdateTimer.clear();
	UpdateTimerMtx.unlock();

	DifficultyMapMtx.lock();
	DifficultyMap.clear();
	DifficultyMapMtx.unlock();
}


void AIHandler::Update(float delta)
{
	// Cooldown writes are deferred until after the queue lock releases:
	// taking DifficultyMapMtx while holding ActionQueueMtx forms an ABBA
	// cycle with every path that holds DifficultyMapMtx and calls AddAction
	// (RunActor's decision branches, the reflex block, and the hit-thread
	// ExternalCalled family) — a cross-thread deadlock waiting on timing.
	std::vector<RE::ActorHandle> DeferredBashCooldowns;
	std::vector<RE::ActorHandle> DeferredDodgeCooldowns;

	// queue for directionc hanges only
	{
		std::unique_lock DirLock(DirectionQueueMtx);
		auto DirIter = DirectionQueue.begin();
		while (DirIter != DirectionQueue.end())
		{
			RE::Actor* DirActor = DirIter->first ? DirIter->first.get().get() : nullptr;
			if (!DirActor)
			{
				DirIter = DirectionQueue.erase(DirIter);
				continue;
			}
			DirIter->second.timeLeft -= delta;
			if (DirIter->second.timeLeft <= 0.f)
			{
				DirectionHandler::GetSingleton()->WantToSwitchTo(
					DirActor, DirIter->second.dir, DirIter->second.force);
				DirIter = DirectionQueue.erase(DirIter);
				continue;
			}
			++DirIter;
		}
	}

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
			// reset flags, follow up actions may set them again
			ActionQueueIter->second.wasForced = false;
			ActionQueueIter->second.priority = 0;
			if (ActionQueueIter->second.toDo == Actions::Attack)
			{
				// sit in queue until we can attack again, and until the hand
				// has finished moving to the attack line — swinging first makes
				// CanSwitch park the pending change and the attack comes out
				// from the OLD direction, which is the one the combo records.
				if (ActionQueueIter->second.waitedForDir < MaxAttackDirWait &&
					HasPendingDirectionSwitch(actor))
				{
					ActionQueueIter->second.waitedForDir += delta;
				}
				else if (TryAttack(actor, false))
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
				bool staggering = actor->AsActorState()->actorState2.staggered;
				if (staggering)
				{
					// A staggered fighter can't raise the guard, but the
					// INTENT persists — retry as soon as recovery allows.
					ActionQueueIter->second.timeLeft = 0.1f;
				}
				else
				{
					if (!actor->IsBlocking())
					{
						actor->NotifyAnimationGraph("blockStart");
					}
					ActionQueueIter->second.toDo = Actions::None;
				}
			}
			else if (ActionQueueIter->second.toDo == Actions::UnblockRiposte)
			{
				bool staggering = actor->AsActorState()->actorState2.staggered;
				if (!staggering)
				{
					actor->AsActorState()->actorState2.wantBlocking = 0;
					actor->NotifyAnimationGraph("blockStop");
					ActionQueueIter->second.toDo = Actions::Attack;
					ActionQueueIter->second.timeLeft = 0.1f;
				}
				else
				{
					ActionQueueIter->second.toDo = Actions::None;
				}
			}
			else if (ActionQueueIter->second.toDo == Actions::UnblockStartFeint)
			{
				actor->AsActorState()->actorState2.wantBlocking = 0;
				actor->NotifyAnimationGraph("blockStop");
				ActionQueueIter->second.toDo = Actions::StartFeint;
				ActionQueueIter->second.timeLeft = 0.1f;
			}
			else if (ActionQueueIter->second.toDo == Actions::Bash)
			{
				// dont start if we cannot bash
				bool staggering = actor->AsActorState()->actorState2.staggered;
				if (!actor->IsAttacking() && !staggering && AttackHandler::GetSingleton()->CanAttack(actor))
				{
					actor->NotifyAnimationGraph("bashStart");
					actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kBash;
					ActionQueueIter->second.toDo = Actions::ReleaseBash;
					ActionQueueIter->second.timeLeft = LowestTime;
					DeferredBashCooldowns.push_back(ActionQueueIter->first);
				}
				else
				{
					ActionQueueIter->second.toDo = Actions::None;
				}
			}
			else if (ActionQueueIter->second.toDo == Actions::ReleaseBash)
			{
				actor->NotifyAnimationGraph("bashRelease");
				ActionQueueIter->second.toDo = Actions::ResetState;
				ActionQueueIter->second.timeLeft = 0.1f;
			}
			// speculative fix/hack for bashes getting npc stuck
			else if (ActionQueueIter->second.toDo == Actions::ResetState)
			{
				//actor->NotifyAnimationGraph("bashRelease");
				//actor->NotifyAnimationGraph("attackStop");
				if (IsBashing(actor))
				{
					actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;
				}
				
				ActionQueueIter->second.toDo = Actions::None;
			}
			else if (ActionQueueIter->second.toDo == Actions::EndBlock)
			{
				actor->AsActorState()->actorState2.wantBlocking = 0;
				actor->NotifyAnimationGraph("blockStop");
				ActionQueueIter->second.toDo = Actions::None;
			}
			else if (ActionQueueIter->second.toDo == Actions::StartFeint)
			{
				if (actor->IsBlocking())
				{
					actor->AsActorState()->actorState2.wantBlocking = 0;
					actor->NotifyAnimationGraph("blockStop");
				}
				if (TryAttack(actor, false))
				{
					ActionQueueIter->second.toDo = Actions::EndFeint;
					ActionQueueIter->second.timeLeft = DifficultySettings::FeintWindowTime - (ActionQueueIter->second.baseTimer * .5f);
					ActionQueueIter->second.wasForced = true;
					ActionQueueIter->second.priority = 2;
				}


			}
			else if (ActionQueueIter->second.toDo == Actions::EndFeint)
			{
				if (!actor->IsBlocking())
				{
					AttackHandler::GetSingleton()->HandleFeint(actor);

					// queue another attack
					ActionQueueIter->second.toDo = Actions::Attack;
					ActionQueueIter->second.timeLeft = 0.18f;
					ActionQueueIter->second.wasForced = true;
					ActionQueueIter->second.priority = 2;
				}
				else
				{
					// we tried but they attacked anyway
					ActionQueueIter->second.toDo = Actions::None;
				}

			}
			else if (ActionQueueIter->second.toDo == Actions::PowerAttack)
			{
				bool staggering = actor->AsActorState()->actorState2.staggered;
				// sit in queue until we can attack again
				if (!staggering && TryPowerAttack(actor))
				{
					ActionQueueIter->second.toDo = Actions::None;
				}

			}
			else if (ActionQueueIter->second.toDo == Actions::Dodge)
			{
				if (!actor->IsAttacking() && !actor->IsBlocking())
				{
					actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;

					// Direction was picked at queue time (in RunActor). TriggerDodge
					// routes to whichever dodge system is active.
					if (DodgeHandler::GetSingleton()->CanDodge(actor))
					{
						DodgeHandler::GetSingleton()->TriggerDodge(actor, ActionQueueIter->second.dodgeDir);
						DeferredDodgeCooldowns.push_back(ActionQueueIter->first);
					}

					ActionQueueIter->second.toDo = Actions::None;
				}

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

	// apply the deferred cooldowns now that the queue lock is released
	if (!DeferredBashCooldowns.empty() || !DeferredDodgeCooldowns.empty())
	{
		std::unique_lock lock(DifficultyMapMtx);
		for (auto& handle : DeferredBashCooldowns)
		{
			auto iter = DifficultyMap.find(handle);
			if (iter != DifficultyMap.end())
			{
				iter->second.BashCooldown = 2.f;
			}
		}
		for (auto& handle : DeferredDodgeCooldowns)
		{
			auto iter = DifficultyMap.find(handle);
			if (iter != DifficultyMap.end())
			{
				iter->second.DodgeCooldown = 4.f;
			}
		}
	}

	UpdateTimerMtx.lock();
	// spread out AI actions to control difficulty
	auto UpdateTimerIter = UpdateTimer.begin();
	int NumActorsTargettingPlayer = 0;
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
		RE::Actor* currentTarget = actor->GetActorRuntimeData().currentCombatTarget.get().get();
		if (currentTarget && currentTarget->IsPlayer() && actor->IsHostileToActor(currentTarget))
		{
			NumActorsTargettingPlayer++;
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
	AIHandlerDataMtx.lock();
	NumPlayerAttackers = NumActorsTargettingPlayer;
	AIHandlerDataMtx.unlock();
	UpdateTimerMtx.unlock();

	
}


