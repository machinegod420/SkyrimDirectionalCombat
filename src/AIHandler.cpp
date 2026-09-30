#include "AIHandler.h"
#include <cassert>
#include "DirectionHandler.h"
#include "SettingsLoader.h"
#include "AttackHandler.h"
#include "BlockHandler.h"
#include "DodgeHandler.h"

#include <random>

// this is really fucking complicated at this point
// 4000 lines of hell

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
// Stagger worth an immediate swing; lighter ones only hand over the initiative.
constexpr float OpportunityStaggerMagnitude = 0.5f;
// Dash strike: earliest after the dodge starts, then polled until it ends.
constexpr float DashSwingDelay = 0.4f;
constexpr float DashPollSeconds = 0.05f;
// Settle time after a guard drop or dodge exit, before the attackStart.
constexpr float TransitionSettleSeconds = 0.1f;
// Longest dodge clip (MCO_Dodge-F-1.hkx).
constexpr float DodgeClipSeconds = 1.f;
// How long a bash may take to reach its hold before the release is sent anyway.
constexpr float BashReleaseWaitMax = 0.5f;
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
// Kept under the real travel: a swing from too close still lands, one from
// too far never does.
constexpr float AttackLungeUnits = 70.f;
// The target's travel when judging whether its swing is a threat: the defender isn't
// backing off, so the full step-in counts.
constexpr float TargetStepInUnits = 90.f;
// Median forward travel to the hit of the direction folders' mco_attack1 / mco_powerattack1;
// the lunge table adds (mult - 1) of it to a swing's reach.
constexpr float LightLungeUnits = 58.f;
constexpr float PowerLungeUnits = 63.f;
static float LungeExtra(Directions line, bool chained, bool power)
{
	return (AttackHandler::LungeMultFor(line, chained, power) - 1.f) * (power ? PowerLungeUnits : LightLungeUnits);
}
// The NPC's own attack gates only: a cut barely travels once it starts, unlike the
// thrust, so cuts thrown past weapon reach whiffed about half the time.
constexpr float CutLungeUnits = 0.f;
// Offense gate reach for a line; only a light on the thrust line lunges.
static float GateReach(RE::Actor* actor, Directions line, bool chained, bool power)
{
	const bool Thrust = !power && line == AttackHandler::PokeLine(actor);
	return AttackHandler::LineReach(actor, line, power) + (Thrust ? AttackLungeUnits : CutLungeUnits) +
		LungeExtra(line, chained, power);
}
// Their reach on a line: the model, raised by their furthest connection. Leans far on purpose.
float AIHandler::TargetReachEstimate(RE::Actor* target, const AIDifficulty& diff, Directions line, bool power) const
{
	const float Weapon = AttackHandler::LineReach(target, line, power);
	float Estimate = Weapon + TargetStepInUnits +
		LungeExtra(line, DirectionHandler::GetSingleton()->GetComboStep(target) > 0, power);
	const int L = static_cast<int>(line);
	if (AISettings::LearnReach && L >= 0 && L < 4)
	{
		Estimate = std::max(Estimate, diff.targetReachLearned[power ? 1 : 0][L]);
	}
	return std::max(Estimate, Weapon);
}
// Distance a backpedalling target opens during the windup: its travel over ~0.34s.
constexpr float BackpedalLeadUnits = 40.f;
// Slack past the estimated target reach before a swing is worth defending. Never
// below 1: a wrong "out" takes a hit, a wrong "in" only wastes a block.
constexpr float DefendReachBase = 1.0f;
constexpr float DefendReachCautionScale = 0.2f;

// Extra stamina a cautious actor keeps in hand before attacking, added to every
// attack threshold so the whole ladder shifts together. 
static float StaminaReserve(float CautionMod)
{
	return std::clamp(CautionMod * 0.2f, 0.f, 0.2f);
}

// Stamina gates lean with personality: offensive spends come later for the
// cautious, defensive spends earlier. Up to ~0.17 across the archetypes.
constexpr float StaminaGatePersonalityScale = 0.1f;
static float StaminaGate(float Base, float Lean, float CautionMod, float AggressionMod)
{
	return std::clamp(Base + (CautionMod - AggressionMod) * Lean * StaminaGatePersonalityScale, 0.15f, 0.95f);
}

// Odds an actor reads a swing's type, power or light, out of the animation in
// time to aim its parry. A failed read falls back on the opponent's power habit.
// Recognising one is trained, not learned mid-fight, so it is gated by tier alone
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

// Parry placement error by tier; too slow for the power window means no defense.
// Extra seconds to contact per unit of gap: the 200 ms window over ~200 units.
constexpr float ContactDistanceSlope = 0.001f;

// Kept tight on purpose: an opponent who can't parry makes the guard choice free.
// Reaction scale on a combo-advancing swing vs a repeat: the prune, and its price.
constexpr float AdvanceReactionScale = 0.85f;
constexpr float RepeatReactionScale = 1.15f;
constexpr float ParryTimingErrorMin = 0.04f;  // Legendary
constexpr float ParryTimingErrorMax = 0.12f;  // VeryEasy
static float ParryTimingError(AIHandler::Difficulty difficulty)
{
	const int Tier = std::clamp(static_cast<int>(difficulty),
		static_cast<int>(AIHandler::Difficulty::VeryEasy),
		static_cast<int>(AIHandler::Difficulty::Legendary));
	const int Span = static_cast<int>(AIHandler::Difficulty::Legendary) -
		static_cast<int>(AIHandler::Difficulty::VeryEasy);
	const float T = static_cast<float>(Tier - static_cast<int>(AIHandler::Difficulty::VeryEasy)) /
		static_cast<float>(Span);
	return ParryTimingErrorMax + (ParryTimingErrorMin - ParryTimingErrorMax) * T;
}

// Every roll here runs once per decision tick, so rates are per second and
// converted here; the tier's timer then cancels out instead of scaling everything.
static bool RollPerSecond(float RatePerSecond, float TickLength)
{
	const float P = std::clamp(RatePerSecond * TickLength, 0.f, 1.f);
	return (mt_rand() % 10000u) < static_cast<unsigned>(P * 10000.f);
}

// A unit, not a knob: the tick length the original per-tick odds were written
// against, which is what turns "3 in 8 per tick" into 2.5/sec
constexpr float ReferenceTick = 0.15f;
static bool RollTickScaled(float Numerator, float Denominator, float TickLength)
{
	return RollPerSecond((Numerator / Denominator) / ReferenceTick, TickLength);
}

// Drain in points per second, carried fractionally, so memory length is
// independent of the tier's tick and tunable between whole points per tick.
static int DrainPerTick(float PerSecond, float TickLength, float& Remainder)
{
	Remainder += PerSecond * TickLength;
	const int Whole = static_cast<int>(Remainder);
	Remainder -= static_cast<float>(Whole);
	return Whole;
}
// Points per second bled from every line: a belief worth N points lasts N/rate
// seconds. Higher lives in the moment; lower leaves stale reads pinning the AI.
constexpr float HoldDrainPerSecond = 8.f;
constexpr float ForgetDrainPerSecond = 4.f;
// The cascade rolls mt_rand() % 100, so the sum of the four lines is the AI's
// whole probability space. Past the budget, accumulation drains the strongest
// other line instead of clamping.
static constexpr int BeliefBudget = 100;
// Chain graph, in belief points: the edge a swing walks gains, its siblings
// give some back, and a blocked one loses twice the gain.
constexpr int ChainEdgeGain = 20;
constexpr int ChainEdgeSiblingLoss = 5;
// Pseudo-count per legal line when the combo rule ranks beliefs, so a few
// stray points can't become a certain pick.
constexpr int StructuralRankPrior = 10;
// Player habit profile. Each transition decays its row first, so recent habits
// win (half-life ~13 swings from the same line).
constexpr float PlayerHabitDecay = 0.95f;
// Decayed transitions from a line before its row seeds at full strength.
constexpr float PlayerHabitFullCount = 10.f;
// Edge points a certain, fully learned habit seeds at Legendary: about three
// observed transitions' worth.
constexpr float PlayerHabitSeedMax = 60.f;
// Share of the profile the lowest tier starts with.
constexpr float PlayerHabitSeedFloor = 0.1f;
// Points per second the held line gains at full guard charge.
constexpr float HoldAccrualPerSecond = 10.f;

constexpr float AttackStaminaFloor = 0.25f;  // never attacks below this

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
// Floor on (tier + aggression * 1.5): a Turtle at VeryEasy sits near zero and
// would never attack at all.
constexpr float AttackRateBaseFloor = 0.5f;
// Subtracted from the full-rate point in proportion to combo progress; sized
// against the taper width so stamina still matters mid-combo.
constexpr float ComboCommitmentDiscount = 0.10f;
// Attack rate gained at full combo progress, so a landed hit invites the
// follow-up while the combo window is open. 1.0 doubles it one hit from done.
constexpr float ComboRateBoost = 1.0f;
// Aggression shift from the fight situation: each advantage (own ratio minus the
// target's) times its weight, clamped, eased in over a few seconds.
constexpr float ShiftHealthWeight = 0.2f;
constexpr float ShiftStaminaWeight = 0.1f;
constexpr float ShiftMax = 0.2f;
constexpr float ShiftEaseSeconds = 2.f;
// The AI's own attack decision, separate from the engine-driven ladder above.
// Full rate down to these, then a taper to the floor: two to four fresh swings.
constexpr float AttackStaminaOffLine = 0.55f;  // attacking off the guarded line
constexpr float AttackStaminaFeint = 0.45f;    // the feint branch

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
constexpr float DisengageStaminaFloor = 0.65f;
// Caution moves the floor, so giving ground is a personality tell; clamped
// above what a dodge costs.
constexpr float DisengageCautionScale = 0.4f;
constexpr float DisengageFloorMin = 0.25f;
constexpr float DisengageFloorMax = 0.85f;
// Share of its own reach a fully cautious actor may be out-reached by and still
// disengage. At full caution 15%, which covers a sword against a greataxe.
constexpr float DisengageCautionReach = 0.15f;
// Seconds between an AI's dodges; caution cuts it, so the mobile fighter moves
// more often. 3 s at no caution, 2 s at full.
constexpr float DodgeCooldownSeconds = 3.f;
constexpr float DodgeCooldownCautionCut = 1.f;
// Dodging has no i-frames, so a backward dodge only beats blocking from the
// outer part of the attacker's reach, where it actually leaves the hitbox.
// Fraction of squared reach; shared by the disengage and the evade.
constexpr float DodgeRimFraction = 0.7f;
// Floor on the rim evade's roll numerator: at Aggressor and Brute caution it
// goes negative and they would never evade at all. Rare, not impossible.
constexpr float RimEvadeRollFloor = 0.25f;

// Learned power-attack windup. New samples fold in at this weight, so a weapon
// swap converges in a few swings without one odd reading throwing it.
constexpr float PowerWindupSmoothing = 0.35f;
// Weight of each swing in the power habit, about three to four swings of
// memory. Much shorter and strict power/light alternation beats it every time.
constexpr float PowerHabitWeight = 0.3f;
// Same pace for the share of the target's powers that were feints.
constexpr float FeintHabitWeight = 0.3f;
// Chamber odds per swing, from bait; a swing type it has seen feinted is trusted less.
constexpr float MasterstrikeBaseChance = 0.1f;
constexpr float MasterstrikeBaitScale = 0.25f;
constexpr float MasterstrikeMaxChance = 0.35f;

// Per-actor tier jitter in percent, on top of the level-relative base: one below,
// the base, one above, remainder two above. Weighted up — an opponent who can't
// answer you teaches nothing.
constexpr int TierJitterDown = 10;
constexpr int TierJitterFlat = 45;
constexpr int TierJitterUp = 30;
// A swing that hasn't connected in this long was whiffed, cancelled, or blocked
// by terrain — drop it rather than record an inflated windup. Overestimating is
// the one direction that hurts, since it presses the guard late.
constexpr float PowerWindupTimeout = 2.0f;

// Step-in counter, bait-gated: 0.6 admits Counter and Trickster only, and the
// rate scales by the same trait.
constexpr float CounterBaitThreshold = 0.6f;
// Chance, from bait, to swing the same line again. A landed repeat resets the
// combo run; the price of not letting the defender prune the repeat.
constexpr float RepeatBase = 0.10f;
constexpr float RepeatBaitScale = 0.15f;
constexpr float RepeatMax = 0.3f;
constexpr float CounterRatePerSecond = 5.f;
// Probe from the band at full bait; a probe is rarer than a punish.
constexpr float PokeRatePerSecond = 3.f;
// Riposte odds: the tier's 75-83%, leaned by bait.
constexpr float RiposteBaitScale = 0.15f;
constexpr float RiposteChanceMin = 0.5f;
constexpr float RiposteChanceMax = 0.95f;


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
// Where giving ground to a low bar starts; ramps to full at empty.
constexpr float SpacingWindedStart = 0.6f;
// Out of the target's reach, the actor holds there until stamina is back over
// this mark. Caution raises it, aggression lowers it.
constexpr float SpacingReengageStamina = 0.85f;
constexpr float SpacingReengagePersonalityScale = 0.1f;
constexpr float SpacingReengageMin = 0.7f;
constexpr float SpacingReengageMax = 0.9f;
constexpr float SpacingMinBandWidth = 64.f;      // keep outer meaningfully beyond inner
constexpr float SpacingEaseRate = 4.f;           // exponential ease, ~0.25s to 63%

// How often the perception layer samples the target.
constexpr float PerceptionInterval = 0.02f;

// todo replace with reading game setting
float BashDistance = 110;
float BashDistanceSq = BashDistance * BashDistance;
// Beyond this the actor only recovers. Sized so the widest threat range still
// fits for weapon reach up to ~2.3, which covers long modded spears.
constexpr float JudgeRadius = 600.f;
// Mental fatigue from time in measure: none before the onset, linear to full at the horizon.
static float Fatigue(float fightSeconds)
{
	const float Ramp = AISettings::FatigueHorizonSeconds - AISettings::FatigueOnsetSeconds;
	if (AISettings::FatigueHorizonSeconds <= 0.f || Ramp <= 0.f)
	{
		return 0.f;
	}
	return std::clamp((fightSeconds - AISettings::FatigueOnsetSeconds) / Ramp, 0.f, 1.f);
}
// Ground a forward dodge covers, the low end of measured dodges (121-147).
constexpr float GapCloseDodgeUnits = 120.f;
// Half-angle of a player swing's arc, the threat test when the swinger is the
// player and there is no trusted combat target to compare against.
constexpr float ThreatConeDegrees = 60.f;

// Archetypes, assigned by handle hash (CalcAndInsertDifficulty) with ±0.1 jitter.
struct AIPersonalityArchetype
{
	const char* name;
	float aggression;    // attack frequency, retreat reluctance, counter-attack readiness
	float patience;      // willingness to hold block vs. release
	float bait;          // holds same direction longer to bait commits
	float caution;       // dodge/block frequency, anticipation threshold
	float powerAttack;   // prefers power attacks over light
	float feint;         // probability of feinting on an attack
};
static constexpr AIPersonalityArchetype kPersonalityArchetypes[] = {
	// name           agg    pat    bait   caut   pow    feint   reads as
	{ "Aggressor",   +1.0f, -0.8f, -0.5f, -0.5f, +0.6f,  0.0f }, // charges in, swings constantly, rarely dodges
	{ "Turtle",      -0.6f, +1.0f, +0.5f, +0.8f, -0.4f, -0.5f }, // holds block, waits, hard to break
	{ "Trickster",   +0.3f, +0.3f, +0.7f, +0.2f, -0.3f, +1.0f }, // feints constantly, mind games heavy
	{ "Counter",     -0.1f, +0.8f, +1.0f, +0.5f, +0.3f, +0.4f }, // patient, waits for masterstrike opportunity
	{ "Brute",       +0.6f, -0.3f, -0.4f, -0.2f, +1.0f, -0.7f }, // power-attack-heavy heavy hitter
	{ "Evasive",     -0.1f,  0.0f, -0.2f, +1.0f, -0.5f, +0.3f }, // dodges everything, mobile fighter
	{ "Balanced",     0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f }, // no lean, every gate at its base value
	{ "Balanced",     0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f }, // 2 balanced archetypes because they should be the most common
};
constexpr std::size_t kNumPersonalityArchetypes = std::size(kPersonalityArchetypes);

void AIHandler::InitializeValues(PRECISION_API::IVPrecision3* precision)
{
	Precision = precision;
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
		// 70ms should be the absolute limit of a persons physical reaction time
		iter.second = std::max(iter.second, 0.07f);
	}
	logger::info("Finished reinitializing difficulty");
}

void AIHandler::AddAction(RE::Actor* actor, Actions toDo, bool force, int priority, DodgeDirection dodgeDir, float TargetDelay)
{
	std::unique_lock lock(ActionQueueMtx);
	auto Iter = ActionQueue.find(actor->GetHandle());
	// A pending action is replaced only by a higher priority. An unranked one also gives
	// way once its timer has run out; a ranked one holds until it runs. Forced is priority 1.
	priority = std::max(priority, force ? 1 : 0);
	const bool Riposte = toDo == Actions::UnblockRiposte || toDo == Actions::UnblockStartFeint;
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
	if (Iter != ActionQueue.end() && Iter->second.toDo != Actions::None &&
		(Iter->second.timeLeft > 0.f || Iter->second.priority > 0) && priority <= Iter->second.priority)
	{
		return;
	}
	// hack to prevent anything from getting in the way of resetting state
	if (Iter != ActionQueue.end() && (Iter->second.toDo == Actions::ResetState ||
		Iter->second.toDo == Actions::ReleaseBash || Iter->second.toDo == Actions::EndFeint))
	{
		return;
	}
	// short circuit here because this causes issues where the actor will block forever.
	// A riposte drops the guard itself, so it may replace the pending EndBlock.
	if (Iter != ActionQueue.end() && actor->IsBlocking() && Iter->second.toDo == Actions::EndBlock && !Riposte)
	{
		return;
	}


	// if no action or time has expired
	if (Iter == ActionQueue.end() || priority > Iter->second.priority || Iter->second.timeLeft <= 0.f || Iter->second.toDo == Actions::None)
	{
		Action action;
		// Floored by the action timer: a deadline can hold the hand back, never
		// move it faster than the actor can act. When the deadline is already
		// past, this collapses to normal behaviour
		const float actionTime = std::max(CalcActionTimer(actor), TargetDelay);
		action.timeLeft = actionTime;
		action.baseTimer = actionTime;
		action.toDo = toDo;
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
		std::uniform_real_distribution<float> Roll(0.f, 1.f);
		diff.repeatNext = Roll(diff.npcRand) <
			std::clamp(RepeatBase + diff.baitTendency * RepeatBaitScale, 0.f, RepeatMax);
		if (!diff.repeatNext)
		{
			idx++;
		}

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

bool AIHandler::IsSwingingAt(RE::Actor* attacker, RE::Actor* defender)
{
	return attacker->IsAttacking() &&
		(attacker->IsPlayerRef() ?
			attacker->GetHeadingAngle(defender->GetPosition(), true) < ThreatConeDegrees :
			attacker->GetActorRuntimeData().currentCombatTarget == defender->GetHandle());
}

bool AIHandler::IsIncomingSwing(RE::Actor* attacker, RE::Actor* defender)
{
	const auto State = attacker->AsActorState()->actorState1.meleeAttackState;
	return (State == RE::ATTACK_STATE_ENUM::kDraw || State == RE::ATTACK_STATE_ENUM::kSwing) &&
		IsSwingingAt(attacker, defender) && IsBlockableSwing(attacker);
}

// Reflex-block ready: hands free, wind to hold, and in stance or still outside own reach.
bool AIHandler::IsPreparedToBlock(RE::Actor* actor, RE::Actor* target, const AIDifficulty& diff) const
{
	const bool Approaching = TorsoDistanceSq(actor, target) > diff.CurrentWeaponLengthSQ;
	if ((!diff.defending && !Approaching) || actor->IsBlocking() || actor->IsAttacking())
	{
		return false;
	}
	const float Stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	const float MaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
	return MaxStamina > 0.f && (Stamina / MaxStamina) >= 0.2f;
}

// Closing the mover added along the line to the other since `from`: positive toward them.
static float ClosingSince(RE::Actor* mover, RE::Actor* other, const RE::NiPoint3& from)
{
	RE::NiPoint3 Toward = other->GetPosition() - from;
	Toward.z = 0.f;
	if (Toward.Length() <= 0.f)
	{
		return 0.f;
	}
	Toward.Unitize();
	RE::NiPoint3 Moved = mover->GetPosition() - from;
	Moved.z = 0.f;
	return Moved.Dot(Toward);
}

void AIHandler::NotifyPowerAttackHitExternalCalled(RE::Actor* actor, RE::Actor* attacker, bool timeIt)
{
	std::unique_lock lock(DifficultyMapMtx);
	auto Iter = DifficultyMap.find(actor->GetHandle());
	if (Iter == DifficultyMap.end())
	{
		return;
	}
	auto& diff = Iter->second;
	// The gap this sample landed at, to transfer the estimate to the next.
	float SampleDist = 0.f;
	// The same target RunActor watches, so the swing it armed is the one credited.
	RE::Actor* TargetActor = GetCombatTarget(actor);
	if (TargetActor)
	{
		SampleDist = std::sqrt(TorsoDistanceSq(actor, TargetActor));
	}
	// swingWasPower was read at the start edge and already excludes bashes.
	if (timeIt)
	{
		RecordWindupSample(diff, SampleDist, diff.swingWasPower);
	}
	// Their reach floor: the furthest gap they connected from, less my closing. Theirs only.
	if (AISettings::LearnReach && TargetActor && attacker == TargetActor && diff.swingStartGap >= 0.f && diff.swingLine >= 0)
	{
		const float Sample = diff.swingStartGap - ClosingSince(actor, TargetActor, diff.swingStartOwnPos);
		float& Learned = diff.targetReachLearned[diff.swingWasPower ? 1 : 0][diff.swingLine];
		Learned = std::max(Learned, Sample);
		if (Settings::VerboseLogging)
		{
			logger::info("[reach] {} learns {}'s {} from line {} reaches {:.0f} (sample {:.0f})", actor->GetName(),
				TargetActor->GetName(), diff.swingWasPower ? "power" : "light", diff.swingLine, Learned, Sample);
		}
		diff.swingStartGap = -1.f;
	}
}

bool AIHandler::HasSwingQueued(RE::Actor* actor)
{
	const Actions Queued = GetQueuedAction(actor);
	return Queued == Actions::Attack || Queued == Actions::PowerAttack || Queued == Actions::Followup ||
		Queued == Actions::FeintFollowup || Queued == Actions::OpportunityAttack || Queued == Actions::DashAttack;
}

void AIHandler::RecordWindupSample(AIDifficulty& diff, float sampleDist, bool power) const
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
	// Learned at base speed: a swing at speed m took base / m.
	const float Base = sample * diff.swingSpeed;
	float& Estimate = power ? diff.powerWindupEstimate : diff.lightWindupEstimate;
	float& Dist = power ? diff.powerWindupDist : diff.lightWindupDist;
	const bool First = Estimate <= 0.f;
	Estimate = First ? Base : Estimate + (Base - Estimate) * PowerWindupSmoothing;
	if (sampleDist > 0.f)
	{
		Dist = (First || Dist <= 0.f) ? sampleDist : Dist + (sampleDist - Dist) * PowerWindupSmoothing;
	}
}

float AIHandler::CommitWindow(const AIDifficulty& diff) const
{
	std::shared_lock lock(DifficultyUpdateTimerMtx);
	auto Iter = DifficultyUpdateTimer.find(diff.difficulty);
	const float base = (Iter != DifficultyUpdateTimer.end()) ? Iter->second : ReferenceTick;

	// Irregular switching, not the line itself, is what costs a person; the
	// switch streak stands in for it and bleeds off once the target settles.
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
	// (target, actor): their line against my guard. Symmetric between two
	// guards, but a creature has no guard, and the unblockable and full-shield
	// cases must read from the defending side.
	return DirectionHandler::GetSingleton()->HasBlockAngle(target, actor) ||
		diff.timeSinceLineChange >= CommitWindow(diff);
}

// Beyond the target's reach, refilling costs only time, so an actor there below
// its re-engage mark recovers instead of fighting. Only true out of range, so
// once the actor has committed this never pulls it back out.
static bool IsRecovering(float TargetDistSQ, float PerceivedReachSQ, float StaminaRatio, float CautionMod, float AggressionMod)
{
	const float ReengageMark = std::clamp(
		SpacingReengageStamina + (CautionMod - AggressionMod) * SpacingReengagePersonalityScale,
		SpacingReengageMin, SpacingReengageMax);
	return TargetDistSQ > PerceivedReachSQ && StaminaRatio < ReengageMark;
}

float AIHandler::CalcSpacingTarget(RE::Actor* actor, RE::Actor* target, const AIDifficulty& diff,
	float ownStaminaRatio, float enemyStaminaRatio, float targetDistSQ) const
{
	// Recovering: hold just outside the target's reach. The engine stands at
	// bodyRadius + inner, so inner at the reach leaves the body as margin.
	const float PerceivedReachSQ = diff.targetReachSQ * diff.reachMisjudge * diff.reachMisjudge;
	if (IsRecovering(targetDistSQ, PerceivedReachSQ, ownStaminaRatio, diff.cautionMod, diff.aggressionMod))
	{
		return std::clamp(1.f + std::sqrt(PerceivedReachSQ) / SpacingStandoffScale, SpacingMultMin, SpacingMultMax);
	}

	// Personality sets how often the actor takes its turn, never how close it
	// stands. Out-reached, it defends while closing instead of standing off.
	const bool Outreached = diff.CurrentWeaponLengthSQ < diff.targetReachSQ * diff.reachMisjudge * diff.reachMisjudge;
	if (!diff.defending || Outreached)
	{
		// Entering measure is the decision the stamina read gates, scaled by
		// difficulty: VeryEasy still charges, Legendary waits for the edge.
		const float deficit = std::clamp(enemyStaminaRatio - ownStaminaRatio, 0.f, 1.f);
		const float discipline = std::clamp(
			static_cast<float>(static_cast<int>(diff.difficulty)) /
				static_cast<float>(static_cast<int>(Difficulty::Legendary)), 0.f, 1.f);
		return SpacingOffenseMult + deficit * SpacingEntryDiscipline * discipline;
	}

	float spacing = 1.f;

	// withdraw when low on stam
	spacing += std::clamp((SpacingWindedStart - ownStaminaRatio) / SpacingWindedStart, 0.f, 1.f) * SpacingStaminaWeight;

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

	// Stamina advantage
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

	// A baiter parks where their light, with its step, falls short of its own travel.
	if (diff.baitTendency >= CounterBaitThreshold)
	{
		auto* Dir = DirectionHandler::GetSingleton();
		const Directions Line = Dir->GetCurrentDirection(actor);
		const float Travel = GateReach(actor, Line, Dir->GetComboStep(actor) > 0, false) -
			AttackHandler::LineReach(actor, Line, false);
		const float TheirReach = TargetReachEstimate(target, diff, Dir->GetCurrentDirection(target), false);
		spacing = std::max(spacing, 1.f + (TheirReach + Travel) / SpacingStandoffScale);
	}

	return std::clamp(spacing, SpacingMultMin, SpacingMultMax);
}

void AIHandler::NotifyHeldOff(RE::Actor* actor)
{
	if (!actor)
	{
		return;
	}
	std::unique_lock lock(DifficultyMapMtx);
	// Seed first: RunActor only seeds when the key is absent, so an entry left by
	// operator[] would cost this actor its tier and personality.
	CalcAndInsertDifficulty(actor);
	auto Iter = DifficultyMap.find(actor->GetHandle());
	if (Iter != DifficultyMap.end())
	{
		if (!Iter->second.heldOff && Settings::VerboseLogging)
		{
			logger::info("[ai] {} {:08X} held off: attacking-disabled flag set, AI paused", actor->GetName(), actor->GetFormID());
		}
		Iter->second.heldOff = true;
	}
}

void AIHandler::ApplySpacingExternalCalled(RE::Actor* actor, float* a_inOutInner, float* a_inOutOuter)
{
	if (!actor || !a_inOutInner || !a_inOutOuter)
	{
		return;
	}
	// we're forcing disabled attackers to stay way outta range for now
	if (IsAttackingDisabled(actor))
	{
		*a_inOutInner = std::max(*a_inOutInner, 400.f);
		*a_inOutOuter = std::max(*a_inOutOuter, *a_inOutInner + SpacingMinBandWidth);
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

	// Additive on inner (vanilla's is 0), multiplicative on outer; max against
	// the incoming value so another mod's hold-back is only ever added to.
	const float inner = std::max(*a_inOutInner, std::max(0.f, (mult - 1.f) * SpacingStandoffScale));
	const float outer = std::max(*a_inOutOuter * mult, inner + SpacingMinBandWidth);
	*a_inOutInner = inner;
	*a_inOutOuter = outer;
}

// i abandoned good coding conventions a long time ago
void AIHandler::RunActor(RE::Actor* actor, float delta)
{
	DirectionHandler* DirHandler = DirectionHandler::GetSingleton();
	RE::Actor* target = GetCombatTarget(actor);
	if (!target)
	{
		return;
	}

	if (!DirHandler->IsDirectionalOpponent(target))
	{
		// no guard and no line to read: step the pattern, let vanilla swing
		if (CanAct(actor))
		{
			// Held here so the order stays DifficultyMapMtx -> AIHandlerDataMtx/ActionQueueMtx.
			std::unique_lock DiffLock(DifficultyMapMtx);
			SwitchToNextAttack(actor);
			// Not fencing: release the last exchange, the spacing mult and any raised block.
			auto& diff = DifficultyMap[actor->GetHandle()];
			ReduceDifficulty(actor);
			diff.numTimesDirectionsSwitched = 1;
			diff.numTimesDirectionSame = 0;
			diff.defending = false;
			diff.sawSwingLastTick = false;
			diff.spacingMult = 1.f;
			diff.spacingTarget = 1.f;
			diff.aggressionShift = 0.f;
			diff.aggressionMod = diff.baseAggression;
			diff.cautionMod = diff.baseCaution;
			// Perception paused: a swing seen before it must not claim a hit after it.
			diff.swingStartGap = -1.f;
			if (actor->IsBlocking())
			{
				AddAction(actor, Actions::EndBlock);
			}
			// This branch already drops the guard, so the release is handled.
			diff.heldOff = false;
			DidAct(actor);
		}
		return;
	}

	// Actions that occur outside of the normal tick (such as reactions) happen here
	float TargetDistSQ = TorsoDistanceSq(actor, target);
	std::unique_lock DiffLock(DifficultyMapMtx);
	if (!DifficultyMap.contains(actor->GetHandle()))
	{
		CalcAndInsertDifficulty(actor);
	}
	// Cached reference: the key exists and nothing below inserts, so no rehash.
	auto& diff = DifficultyMap[actor->GetHandle()];
	// Just released: drop the guard imposed on it while it waited, or the actor
	// rotating in spends its first seconds blocking.
	if (diff.heldOff)
	{
		if (Settings::VerboseLogging)
		{
			logger::info("[ai] {} {:08X} no longer held off", actor->GetName(), actor->GetFormID());
		}
		diff.heldOff = false;
		diff.defending = false;
		diff.defendTime = 0.f;
		diff.swingStartGap = -1.f;
		if (actor->IsBlocking())
		{
			AddAction(actor, Actions::EndBlock);
		}
	}
	AttackHandler::GetSingleton()->ClearStuckAttack(actor, diff.graphIdleAttacking, delta);
	// tick cooldown
	if (diff.DodgeCooldown >= 0)
	{
		diff.DodgeCooldown -= delta;
	}
	if (diff.BashCooldown >= 0)
	{
		diff.BashCooldown -= delta;
	}

	// Diagnostic: once a second, where this actor stands by its own measure.
	if (Settings::VerboseLogging)
	{
		diff.statusLogTimer -= delta;
		if (diff.statusLogTimer <= 0.f)
		{
			diff.statusLogTimer = 1.f;
			const float Stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) /
				std::max(1.f, actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina));
			logger::info("[tick] {} {:08X} ({} tier {}) -> {} dist {:.0f} own reach {:.0f} target reach {:.0f} judge {} stamina {:.2f} defending {} canAct {} state {} spacing {:.2f} base attack rate {:.2f}/s aggression shift {:+.2f}",
				actor->GetName(), actor->GetFormID(), diff.archetype ? diff.archetype : "?", static_cast<int>(diff.difficulty), target->GetName(), std::sqrt(TargetDistSQ),
				actor->GetReach() + AttackLungeUnits, (target->GetReach() + AttackLungeUnits) * diff.reachMisjudge,
				TargetDistSQ < JudgeRadius * JudgeRadius,
				Stamina, diff.defending, CanAct(actor), static_cast<int>(actor->AsActorState()->GetAttackState()), diff.spacingTarget,
				std::max(AttackRateBaseFloor, static_cast<int>(diff.difficulty) + diff.aggressionMod * 1.5f) * AttackRatePerMod,
				diff.aggressionShift);
		}
	}

	// Perception layer: observe the target's guard EVERY frame
	{
		bool salientEvent = false;
		// Perception is per-target: reset on retarget, or the old target's
		// remembered guard reads as a switch by the new one.
		const uint32_t targetId = target->GetHandle().native_handle();
		if (diff.observedTargetId != targetId)
		{
			diff.observedTargetId = targetId;
			diff.hasObservation = false;
			// The shift measured the old matchup.
			diff.aggressionShift = 0.f;
			diff.aggressionMod = diff.baseAggression;
			diff.cautionMod = diff.baseCaution;
			diff.observedSwitchCount = 0;
			diff.lastObservedAttackState = 0;
			diff.timeSinceLineChange = 0.f;
			// Windup is a property of the opponent's weapon and animations, and
			// the habit of their choices, so neither means anything against a
			// different one.
			diff.powerWindupEstimate = 0.f;
			diff.powerWindupDist = 0.f;
			diff.lightWindupEstimate = 0.f;
			diff.lightWindupDist = 0.f;
			for (auto& Row : diff.targetReachLearned)
			{
				std::fill(std::begin(Row), std::end(Row), 0.f);
			}
			diff.swingStartGap = -1.f;
			diff.powerHabit = 0.f;
			diff.feintHabit[0] = 0.f;
			diff.feintHabit[1] = 0.f;
			diff.swingElapsed = -1.f;
			for (auto& Row : diff.chainEdges)
			{
				std::fill(std::begin(Row), std::end(Row), 0);
			}
			diff.lastChainFrom = -1;
			diff.chainSeeded = false;
		}
		// Against the player, the graph starts from what fighters have seen of
		// the player's style, scaled by tier. The lowest tiers meet the player cold,
		// and so does everyone with LearnAcrossFights off.
		if (!diff.chainSeeded && target->IsPlayerRef())
		{
			diff.chainSeeded = true;
			if (diff.difficulty > Difficulty::Easy && AISettings::LearnAcrossFights)
			{
				// The player's weapon at first contact; a mid-fight swap keeps this seed.
				const WeaponSet PlayerSet = DirHandler->AnimationSet(target);
				SeedChainEdges(diff, PlayerSet);
				if (Settings::VerboseLogging)
				{
					logger::info("[habit] {} {:08X} seeded from the player profile (tier {}, {})",
						actor->GetName(), actor->GetFormID(), static_cast<int>(diff.difficulty), WeaponSetName(PlayerSet));
				}
			}
		}
		diff.timeSinceLineChange += delta;
		diff.defendTime = diff.defending ? diff.defendTime + delta : 0.f;
		// Pacing for the player: only time spent fighting them counts.
		if (target->IsPlayerRef() && TargetDistSQ < JudgeRadius * JudgeRadius)
		{
			diff.fightSeconds += delta;
		}
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
			// Not a preempt: a switch is information, not a threat, and spending
			// the reaction on it leaves none for the swing that follows.
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
			// A new swing's first tick may spike, even mid-chain where the
			// target never stops swinging between swings.
			diff.sawSwingLastTick = false;

			// Arm the swing lifecycle: timed from here to the hit for the windup estimates.
			diff.swingElapsed = 0.f;
			diff.swingWasPower = IsPowerAttacking(target);
			// For the reach learner: where their swing started, and where I stood.
			diff.swingStartGap = std::sqrt(TargetDistSQ);
			diff.swingStartOwnPos = actor->GetPosition();
			diff.swingLine = static_cast<int>(seen) < 4 ? static_cast<int>(seen) : -1;
			// Non-positive would divide by zero below; the required speed fix makes 1.0 the base.
			const float Speed = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWeaponSpeedMult);
			diff.swingSpeed = Speed > 0.f ? Speed : 1.f;
			// Chain graph: walk the edge from the target's last landed line to
			// this swing's. No landed line in the ring walks the opener row.
			diff.lastChainFrom = -1;
			Directions ChainFrom;
			const bool Chained = DirectionHandler::GetSingleton()->GetLastAttackDirection(target, ChainFrom);
			if (targetAttackState != RE::ATTACK_STATE_ENUM::kBash &&
				(!Chained || static_cast<int>(ChainFrom) < 4) && static_cast<int>(seen) < 4)
			{
				const int From = Chained ? static_cast<int>(ChainFrom) : OpenerRow;
				const int To = static_cast<int>(seen);
				for (int i = 0; i < 4; ++i)
				{
					int& Edge = diff.chainEdges[From][i];
					Edge = i == To ? std::min(Edge + ChainEdgeGain, BeliefBudget) :
						std::max(0, Edge - ChainEdgeSiblingLoss);
				}
				diff.lastChainFrom = From;
				diff.lastChainTo = To;
			}
			if (Settings::VerboseLogging)
			{
				// Why a swing went unanswered: out of the threat band, or the
				// line too fresh for the acquisition gate.
				const float Slack = std::clamp(DefendReachBase + diff.cautionMod * DefendReachCautionScale, 1.f, 1.6f);
				const float Threat = TargetReachEstimate(target, diff, seen, diff.swingWasPower) * diff.reachMisjudge * Slack;
				logger::info("[dmt] {} {:08X} sees {} swing from {} at {:.0f} (threat {:.0f}), line seen {:.0f}ms ago, window {:.0f}ms",
					actor->GetName(), actor->GetFormID(), target->GetName(), static_cast<int>(seen), std::sqrt(TargetDistSQ), Threat,
					diff.timeSinceLineChange * 1000.f, CommitWindow(diff) * 1000.f);
			}

			// Reflex, once per swing: chamber it or block it, paying only the action timer.
			const bool Prepared = IsPreparedToBlock(actor, target, diff) && IsBlockableSwing(target) &&
				CanAnswerLine(actor, target, diff);
			bool Chambered = false;
			if (Prepared)
			{
				// Chamber: a power aimed at the window's centre, if it can fire before the window
				// closes. The queue needs their line and their hit frame still ahead at fire time.
				const float Windup = diff.swingWasPower ? diff.powerWindupEstimate : diff.lightWindupEstimate;
				const float WindupDist = diff.swingWasPower ? diff.powerWindupDist : diff.lightWindupDist;
				float StrikeDelay = -1.f;
				if (Windup > 0.f)
				{
					const float DistAdjust = WindupDist > 0.f ?
						(std::sqrt(TargetDistSQ) - WindupDist) * ContactDistanceSlope : 0.f;
					StrikeDelay = (Windup + DistAdjust) / diff.swingSpeed -
						DifficultySettings::ChamberWindowTime * 0.5f;
					const float Error = ParryTimingError(diff.difficulty);
					std::uniform_real_distribution<float> Spread(-Error, Error);
					StrikeDelay += Spread(diff.npcRand);
				}
				const float FeintHabit = diff.feintHabit[diff.swingWasPower ? 1 : 0];
				const float StrikeChance = std::clamp(MasterstrikeBaseChance + diff.baitTendency * MasterstrikeBaitScale,
					0.f, MasterstrikeMaxChance) * (1.f - FeintHabit);
				// Only a swing that reaches, with the power's stamina in hand.
				if (StrikeDelay >= 0.f &&
					CalcActionTimer(actor) <= StrikeDelay + DifficultySettings::ChamberWindowTime * 0.5f &&
					TargetDistSQ < diff.targetReachSQ * diff.reachMisjudge * diff.reachMisjudge &&
					DirectionHandler::GetSingleton()->HasDirectionalPerks(target) &&
					AttackHandler::GetSingleton()->CanAttack(actor) &&
					actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) >=
						AttackHandler::GetSingleton()->SwingStaminaCost(actor, true) &&
					static_cast<float>(mt_rand() % 10000u) < StrikeChance * 10000.f)
				{
					// Priority 2 outranks the tick's forced block. Read back: a refused queue still blocks.
					AddAction(actor, Actions::PowerAttack, true, 2, DodgeDirection::Backward, StrikeDelay);
					Chambered = GetQueuedAction(actor) == Actions::PowerAttack;
					if (Chambered && Settings::VerboseLogging)
					{
						logger::info("[dmt] {} masterstrike in {:.2f}s ({} windup {:.2f}, feint habit {:.2f})",
							actor->GetName(), StrikeDelay, diff.swingWasPower ? "power" : "light", Windup, FeintHabit);
					}
				}
			}
			if (Prepared && !Chambered)
			{
				// Full action timer: "prepared" is worth the skipped decision stage.
				// Against an expected power the press waits out the learned windup.
				const bool ExpectPower = RollPowerParryRead(diff.difficulty) ?
					diff.swingWasPower :
					static_cast<float>(mt_rand() % 10000u) < diff.powerHabit * 10000.f;
				float TargetDelay = -1.f;
				if (ExpectPower && diff.powerWindupEstimate > 0.f)
				{
					// Shift the estimate to this swing's gap.
					const float DistAdjust = diff.powerWindupDist > 0.f ?
						(std::sqrt(TargetDistSQ) - diff.powerWindupDist) * ContactDistanceSlope : 0.f;
					TargetDelay = (diff.powerWindupEstimate + DistAdjust) / diff.swingSpeed -
						DifficultySettings::TimedBlockStartup -
						DifficultySettings::TimedBlockActiveTime * 0.5f;
					// Aimed at the middle of the window, missed by the tier's
					// margin. Per attempt, so sloppiness is not a fixed bias.
					const float Error = ParryTimingError(diff.difficulty);
					std::uniform_real_distribution<float> Spread(-Error, Error);
					TargetDelay += Spread(diff.npcRand);
				}
				AddAction(actor, Actions::Block, true, 0,
					DodgeDirection::Backward, TargetDelay);
				if (Settings::VerboseLogging)
				{
					logger::info("[dmt] {} {:08X} reflex block, delay {:.0f}ms (expects power {}, swing is power {})",
						actor->GetName(), actor->GetFormID(), std::max(TargetDelay, 0.f) * 1000.f, ExpectPower, diff.swingWasPower);
				}
			}
			// Learned after the guess, so the guess only uses past swings.
			if (targetAttackState != RE::ATTACK_STATE_ENUM::kBash)
			{
				diff.powerHabit += ((diff.swingWasPower ? 1.f : 0.f) - diff.powerHabit) * PowerHabitWeight;
			}
		}
		// Swing ended: fold its feint into the habit and close the lifecycle.
		else if (!inSwingPhase && wasSwingPhase)
		{
			// A swing ends either real (hit frame, or interrupted) or feinted during it.
			// lastObservedAttackState still holds the ending swing's state: bashes don't count.
			if (diff.swingElapsed >= 0.f && diff.lastObservedAttackState != static_cast<int>(RE::ATTACK_STATE_ENUM::kBash))
			{
				const bool Feinted = AttackHandler::GetSingleton()->SecondsSinceFeint(target) <= diff.swingElapsed;
				float& Habit = diff.feintHabit[diff.swingWasPower ? 1 : 0];
				Habit += ((Feinted ? 1.f : 0.f) - Habit) * FeintHabitWeight;
			}
			// swingStartGap stays armed: the hit can land after this state change.
			diff.swingElapsed = -1.f;
		}
		diff.lastObservedAttackState = static_cast<int>(targetAttackState);

		// Attention capture: the clock restarts from the stimulus, so a swing is
		// answered one update timer later wherever it fell in the tick grid.
		if (salientEvent && !diff.preemptSpent)
		{
			// Hick's law against the four-line baseline, so the AI keeps pace with
			// a human whose reaction shortens with fewer lines. Three is ~0.86.
			const int Lines = std::popcount(DirectionHandler::EnabledDirections());
			const float Hick = std::log2(static_cast<float>(Lines + 1)) / std::log2(5.f);
			// Cheap prune: a swing on a line that advances the target's combo is
			// the one being watched for, the repeat is the one dropped from the set.
			float Prune = 1.f;
			Directions LastDir;
			auto* Dir = DirectionHandler::GetSingleton();
			if (Dir->GetLastAttackDirection(target, LastDir) && Dir->GetRepeatCount(target) == 0)
			{
				Prune = Dir->IsInComboWindow(target, Dir->GetCurrentDirection(target)) ?
					RepeatReactionScale : AdvanceReactionScale;
				// Rule knowledge, so it scales with tier like the structural read.
				const float TierT = std::clamp(
					static_cast<float>(static_cast<int>(diff.difficulty) - static_cast<int>(Difficulty::VeryEasy)) /
					static_cast<float>(static_cast<int>(Difficulty::Legendary) - static_cast<int>(Difficulty::VeryEasy)), 0.f, 1.f);
				Prune = 1.f + (Prune - 1.f) * (AISettings::ComboReadLowTierScale + (1.f - AISettings::ComboReadLowTierScale) * TierT);
			}
			const float reaction = std::max(LowestTime, CalcUpdateTimer(actor) * Hick * Prune);
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
		float CurrentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		float MaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		float CurrentStaminaRatio = MaxStamina > 0.f ? CurrentStamina / MaxStamina : 1.f;
		// every roll below fires once per decision tick, so it is rescaled
		// against this to keep frequency independent of the tier's cadence
		const float Tick = CalcUpdateTimer(actor);

		float EnemyCurrentStamina = target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		float EnemyMaxStamina = target->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		float EnemyStaminaRatio = EnemyMaxStamina > 0.f ? EnemyCurrentStamina / EnemyMaxStamina : 1.f;

		// Press a lead, turn cautious when behind.
		{
			const float OwnMaxHealth = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth);
			const float TargetMaxHealth = target->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth);
			const float OwnHealthRatio = OwnMaxHealth > 0.f ?
				actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) / OwnMaxHealth : 1.f;
			const float TargetHealthRatio = TargetMaxHealth > 0.f ?
				target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) / TargetMaxHealth : 1.f;
			const float ShiftTarget = std::clamp(
				(OwnHealthRatio - TargetHealthRatio) * ShiftHealthWeight +
				(CurrentStaminaRatio - EnemyStaminaRatio) * ShiftStaminaWeight,
				-ShiftMax, ShiftMax);
			diff.aggressionShift += (ShiftTarget - diff.aggressionShift) * (1.f - std::exp(-Tick / ShiftEaseSeconds));
			diff.aggressionMod = diff.baseAggression + diff.aggressionShift;
			diff.cautionMod = diff.baseCaution - diff.aggressionShift;
		}

		const float OwnReach = actor->GetReach() + AttackLungeUnits;
		diff.CurrentWeaponLengthSQ = OwnReach * OwnReach;
		// Flat: feeds spacing and the dodges, which must not move with the guard line.
		const float TargetReach = target->GetReach() + AttackLungeUnits;
		diff.targetReachSQ = TargetReach * TargetReach;
		// Per line, for the block threat range only.
		const Directions TargetLine = DirHandler->GetCurrentDirection(target);
		const bool TargetPower = IsPowerAttacking(target);
		const float ThreatReach = TargetReachEstimate(target, diff, TargetLine, TargetPower);
		const float ThreatReachSQ = ThreatReach * ThreatReach * diff.reachMisjudge * diff.reachMisjudge;
		const float PerceivedReachSQ = diff.targetReachSQ *
			diff.reachMisjudge * diff.reachMisjudge;
		const bool TargetSwingingAtMe = IsSwingingAt(target, actor);

		// Set at any distance, or a value last set inside judge range would freeze.
		diff.spacingTarget = CalcSpacingTarget(actor, target, diff, CurrentStaminaRatio, EnemyStaminaRatio, TargetDistSQ);
		// Refilling out of reach: nothing below may spend stamina or hold a block.
		const bool Recovering = IsRecovering(TargetDistSQ, PerceivedReachSQ, CurrentStaminaRatio, diff.cautionMod, diff.aggressionMod);

		if (TargetDistSQ < JudgeRadius * JudgeRadius)
		{

			// always attack if have perk
			if (DirHandler->IsUnblockable(actor) && TargetDistSQ < diff.CurrentWeaponLengthSQ)
			{
				AddAction(actor, AIHandler::Actions::Attack, true);
				diff.defending = false;
				diff.numTimesDirectionsSwitched = 1;
				diff.numTimesDirectionSame = 0;
			}
			// always follow up power attack with another attack
			else if (actor->IsAttacking())
			{
				SwitchToNextAttack(actor);
				if (IsPowerAttacking(actor))
				{
					AddAction(actor, AIHandler::Actions::Followup, true);
				}
			}
			else
			{
				bool ShouldDirectionMatch = false;
				bool DontChangeDirection = false;
				bool targetStaggering = target->AsActorState()->actorState2.staggered;
				int mod = (int)CalcAndInsertDifficulty(actor);

				// Inside measure a fresh defender has no good answer, so it dodges
				// out; a pre-block queued this tick outranks it. Not when out-reached,
				// though caution tolerates some of that: the mobile fighter still backs
				// off a slightly longer weapon.
				const float DisengageReach = std::sqrt(diff.CurrentWeaponLengthSQ) *
					(1.f + std::max(0.f, diff.cautionMod) * DisengageCautionReach);
				if (diff.defending && DisengageReach * DisengageReach >= PerceivedReachSQ)
				{
					const float DisengageFloor = std::clamp(
						DisengageStaminaFloor - diff.cautionMod * DisengageCautionScale,
						DisengageFloorMin, DisengageFloorMax);
					if (Settings::ActiveDodgeSystem != DodgeSystem::None &&
						!actor->IsBlocking() && diff.DodgeCooldown <= 0.f &&
						CurrentStaminaRatio > DisengageFloor &&
						TargetDistSQ < PerceivedReachSQ &&
						(!TargetSwingingAtMe || TargetDistSQ > PerceivedReachSQ * DodgeRimFraction))
					{
						AddAction(actor, Actions::Dodge, false, 0,
							DodgeDirection::Backward);
					}
				}

				// Neutral pre-block: strongRead = the arc on their held line is high.
				// It also pins the AI in defense so the commit isn't undone this exchange.
				const Directions targetCurrentLine = DirHandler->GetCurrentDirection(target);
				const int beliefOnLine = diff.directionChangeChance[targetCurrentLine];
				const bool strongRead = beliefOnLine >= AISettings::PreBlockBeliefThreshold;
				const float preBlockChance = std::clamp(AISettings::PreBlockBaseChance + diff.cautionMod * AISettings::PreBlockCautionScale, 0.f, AISettings::PreBlockMaxChance);
				// Only pre-block a target that can attack; a helpless one is an opening.
				const bool targetIsThreat = AttackHandler::GetSingleton()->CanAttack(target) &&
					!targetStaggering && !target->IsBlocking();

				// A swing from well outside reach is not a threat, or flailing could
				// pin the AI in defence. Estimated reach widened by caution; linear
				// slack, squared to compare.
				const float DefendSlack = std::clamp(
					DefendReachBase + diff.cautionMod * DefendReachCautionScale, 1.f, 1.6f);
				const bool TargetInThreatRange =
					TargetDistSQ < ThreatReachSQ * DefendSlack * DefendSlack;
				// A held block stops regen, so only pre-block what can reach.
				const bool wantPreBlock = strongRead && targetIsThreat && TargetInThreatRange && !Recovering && !actor->IsBlocking() &&
					CurrentStaminaRatio > StaminaGate(0.55f, -1.f, diff.cautionMod, diff.aggressionMod) &&
					RollTickScaled(preBlockChance, 100.f, Tick);

				// Step-in counter from the current line: their swing must fall short of my travel,
				// mine must reach them after their step. Only while holding ground.
				const bool OwnChained = DirHandler->GetComboStep(actor) > 0;
				const Directions PunishLine = DirHandler->GetCurrentDirection(actor);
				const float PunishReach = GateReach(actor, PunishLine, OwnChained, false);
				const float OwnTravel = PunishReach - AttackHandler::LineReach(actor, PunishLine, false);
				const float PunishFrom = ThreatReach + OwnTravel;
				// I reach them after their step: their reach beyond the weapon itself is that step.
				const float PunishTo = PunishReach + std::max(0.f, ThreatReach - AttackHandler::LineReach(target, TargetLine, TargetPower));
				const Directions Poke = AttackHandler::PokeLine(actor);
				const float PokeReach = GateReach(actor, Poke, OwnChained, false);
				// Reach of the next pattern swing, and how far out closing is worth.
				float NextReach = 0.f;
				if (!diff.attackPattern.empty())
				{
					const Directions NextLine = DirectionHandler::FoldDirection(
						diff.attackPattern[diff.currentAttackIdx % diff.attackPattern.size()]);
					NextReach = std::max(GateReach(actor, NextLine, OwnChained, false), GateReach(actor, NextLine, OwnChained, true));
				}
				const float CloseInFrom = std::max(NextReach + GapCloseDodgeUnits, std::sqrt(PerceivedReachSQ));
				if (TargetSwingingAtMe && diff.defending &&
					diff.baitTendency >= CounterBaitThreshold &&
					TargetDistSQ > PunishFrom * PunishFrom &&
					TargetDistSQ < PunishTo * PunishTo &&
					CurrentStaminaRatio > StaminaGate(0.35f, 1.f, diff.cautionMod, diff.aggressionMod) &&
					AttackHandler::GetSingleton()->CanAttack(actor) &&
					RollPerSecond(CounterRatePerSecond * diff.baitTendency, Tick))
				{
					AddAction(actor, Actions::Attack);
					DontChangeDirection = true;
				}
				// Poke: they can't reach me on the line they hold, I can reach them on
				// mine, and their guard isn't answering it. Opportunistic, bait-scaled.
				else if (!TargetSwingingAtMe && !Recovering && diff.baitTendency > 0.f &&
					TargetDistSQ > ThreatReachSQ &&
					TargetDistSQ < PokeReach * PokeReach &&
					(!IsGuardUp(target) || DirHandler->GetCurrentDirection(target) !=
						DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(Poke))) &&
					CurrentStaminaRatio > AttackStaminaOffLine + StaminaReserve(diff.cautionMod) &&
					AttackHandler::GetSingleton()->CanAttack(actor) &&
					RollPerSecond(PokeRatePerSecond * diff.baitTendency, Tick))
				{
					DirHandler->WantToSwitchTo(actor, Poke, true);
					AddAction(actor, Actions::Attack);
					DontChangeDirection = true;
				}
				// Most important case, attempt to defend
				else if (TargetSwingingAtMe && IsBlockableSwing(target) && TargetInThreatRange)
				{
					// Too winded to block safely: back out instead. Unforced, and only
					// with a dodge system, or the cooldown burns on a no-op.
					if (Settings::ActiveDodgeSystem != DodgeSystem::None &&
						CurrentStaminaRatio < StaminaGate(0.30f, 1.f, diff.cautionMod, diff.aggressionMod) && diff.DodgeCooldown <= 0.f)
					{
						// Drop the block first: a dodge can't fire while blocking, and
						// bailing at 0.30 leaves enough stamina to dodge and recover.
						if (actor->IsBlocking())
						{
							AddAction(actor, Actions::EndBlock);
						}
						else
						{
							AddAction(actor, Actions::Dodge, false, 0, DodgeDirection::Backward);
						}
					}
					// Edge-of-reach evade: no i-frames, so a dodge only beats a block
					// when it leaves the hitbox. Reach is misjudged per actor.
					else if (Settings::ActiveDodgeSystem != DodgeSystem::None &&
						!actor->IsBlocking() && !actor->IsAttacking() &&
						GetQueuedAction(actor) != Actions::Block &&
						PerceivedReachSQ > 0.f && diff.DodgeCooldown <= 0.f &&
						CurrentStaminaRatio > StaminaGate(0.45f, -1.f, diff.cautionMod, diff.aggressionMod) &&
						TargetDistSQ < PerceivedReachSQ &&
						TargetDistSQ > PerceivedReachSQ * DodgeRimFraction &&
						// rolls per decision tick, so it compounds across an
						// attack — keep the per-tick odds low
						RollTickScaled(std::max(RimEvadeRollFloor, 1.0f + diff.cautionMod * 3.0f), 40.f, Tick))
					{
						AddAction(actor, Actions::Dodge, false, 0, DodgeDirection::Backward);
					}
					else
					{
						Actions action = GetQueuedAction(actor);
						// Try to block; a chosen dodge or masterstrike keeps the slot.
						if (action != Actions::Attack && action != Actions::FeintFollowup && action != Actions::Block &&
							action != Actions::Dodge && action != Actions::PowerAttack)
						{
							// Wrong-line rule: inside the floor it takes the mixup hit;
							// past it the guard follows (see CanAnswerLine).
							if (CanAnswerLine(actor, target, diff))
							{
								AddAction(actor, Actions::Block, true);
							}
						}
						if (DirHandler->HasBlockAngle(target, actor))
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
				// Locked out, so defense is free: pre-raise on the tracked line rather
				// than wait for a swing the reflex might miss. A feint still beats it.
				else if (!AttackHandler::GetSingleton()->CanAttack(actor) &&
					!TargetSwingingAtMe && TargetInThreatRange && !actor->IsBlocking() && !Recovering &&
					CurrentStaminaRatio > 0.3f)
				{
					AddAction(actor, Actions::Block, true);
					diff.defending = true;
					ShouldDirectionMatch = true;
					if (DirHandler->HasBlockAngle(target, actor))
					{
						DontChangeDirection = true;
					}
					if (Settings::VerboseLogging) logger::info("[preblock] {} locked-out pre-block", actor->GetName());
				}
				// Neutral-game pre-block: strong read on the target's held
				// line + a cautionMod roll to commit. 
				else if (wantPreBlock)
				{
					AddAction(actor, Actions::Block, true);
					diff.defending = true;
					ShouldDirectionMatch = true;
					if (DirHandler->HasBlockAngle(target, actor))
					{
						DontChangeDirection = true;
					}
					if (Settings::VerboseLogging) logger::info("[preblock] {} neutral pre-block belief={} caution={:.2f}", actor->GetName(), beliefOnLine, diff.cautionMod);
				}
				// Close-range bash at a target that isn't attacking. Above the offense
				// branch only so a raised guard can be bashed at all; the cooldown
				// keeps it rare.
				else if (TargetDistSQ < (BashDistanceSq + 1700) && RollTickScaled((mod + 1) * 0.5f + 2 + diff.aggressionMod * 1.5f, 8.f, Tick)
					&& AttackHandler::GetSingleton()->CanAttack(actor) && CurrentStaminaRatio > StaminaGate(0.45f, 1.f, diff.cautionMod, diff.aggressionMod)
					&& !TargetSwingingAtMe && AttackHandler::GetSingleton()->CanAttack(target) && !targetStaggering
					&& diff.BashCooldown <= 0.f)
				{
					AddAction(actor, Actions::Bash);
					ShouldDirectionMatch = true;
					DontChangeDirection = true;
				}
				// Out of own reach: close with a forward dodge. Ahead of the next branch, or a
				// raised guard holds it out there.
				else if (Settings::ActiveDodgeSystem != DodgeSystem::None && NextReach > 0.f &&
					!actor->IsAttacking() && !actor->IsBlocking() && !Recovering && !TargetSwingingAtMe &&
					TargetDistSQ > NextReach * NextReach &&
					TargetDistSQ < CloseInFrom * CloseInFrom &&
					CurrentStaminaRatio > std::clamp(0.75f - diff.aggressionMod * 0.25f + diff.cautionMod * 0.15f, 0.4f, 0.95f) &&
					diff.DodgeCooldown <= 0.f)
				{
					AddAction(actor, Actions::Dodge, false, 0, DodgeDirection::Forward);
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
								
					// A hard stagger is an opening: swing at once from the current line.
					bool Opportunity = false;
					if (targetStaggering)
					{
						float TargetStagger = 0.f;
						const bool HasMagnitude = target->GetGraphVariableFloat("StaggerMagnitude", TargetStagger);
						const bool Hard = HasMagnitude && TargetStagger >= OpportunityStaggerMagnitude;
						Opportunity = Hard && TargetDistSQ < diff.CurrentWeaponLengthSQ;
						if (Opportunity)
						{
							AddAction(actor, Actions::OpportunityAttack, false, 1);
						}
						if (Settings::VerboseLogging)
						{
							logger::info("[opportunity] {} sees {} staggered, magnitude {}: {}", actor->GetName(), target->GetName(),
								HasMagnitude ? std::format("{:.2f}", TargetStagger) : "n/a",
								Opportunity ? "swing" : Hard ? "out of reach" : "initiative only");
						}
					}
					if (!Opportunity && actor->IsBlocking())
					{
						AddAction(actor, Actions::EndBlock);
					}
					ShouldDirectionMatch = false;
					DontChangeDirection = Opportunity;
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
						// Random back-side dodge for variety, picked at decision time.
						int rand = mt_rand() % 3;
						DodgeDirection dir = DodgeDirection::Backward;
						if (rand == 2) dir = DodgeDirection::BackwardRight;
						else if (rand == 1) dir = DodgeDirection::BackwardLeft;
						AddAction(actor, Actions::Dodge, false, 0, dir);
					}
				}
				// too close, try to attack to prevent the bash. Aggression-
				// modulated: aggressive NPCs counter-attack more readily.
				else if (TargetDistSQ < (BashDistanceSq + 1500) && !actor->IsAttacking() && RollTickScaled((mod * 0.5f) + 2 + diff.aggressionMod * 2.0f, 14.f, Tick)
					&& AttackHandler::GetSingleton()->CanAttack(target) && AttackHandler::GetSingleton()->CanAttack(actor)
					&& CurrentStaminaRatio > StaminaGate(0.4f, 1.f, diff.cautionMod, diff.aggressionMod))
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
				// Release the block to save stamina, patience-modulated; never
				// during our own attack lockout, when the shell is all we have.
				else if (actor->IsBlocking() && AttackHandler::GetSingleton()->CanAttack(actor) &&
					diff.numTimesDirectionSame < 1 &&
					(RollTickScaled(3.0f - diff.patienceMod * 1.5f, 5.f, Tick) || CurrentStaminaRatio < 0.6))
				{
					AddAction(actor, Actions::EndBlock);
					DontChangeDirection = true;
					ShouldDirectionMatch = true;
				}
				// Probing footwork when nothing above fired: aggressive leans forward,
				// cautious leans back or sideways, neutral goes anywhere.
				else if (Settings::ActiveDodgeSystem != DodgeSystem::None &&
					!actor->IsAttacking() && !actor->IsBlocking() && !Recovering &&
					CurrentStaminaRatio > 0.9f &&
					TargetDistSQ > (BashDistanceSq + 2000.0f) &&
					diff.DodgeCooldown <= 0.f &&
					RollTickScaled(5.0f
						+ std::max(0.0f, diff.baseCaution) * 15.0f
						+ std::max(0.0f, diff.baseAggression) * 15.0f, 100.f, Tick))
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
					// Direction bias: aggression toward forward, caution toward
					// back/sides; one roll over cumulative thresholds, else any.
					const int roll = mt_rand() % 100;
					const int fwdThresh = static_cast<int>(std::max(0.0f, diff.baseAggression) * 100.0f);
					const int backThresh = fwdThresh + static_cast<int>(std::max(0.0f, diff.baseCaution) * 100.0f);

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
					AddAction(actor, Actions::Dodge, false, 0, probeDir);
				}
				// always hard defend if cant attack (attack was parried)
				if (!AttackHandler::GetSingleton()->CanAttack(actor))
				{
					diff.defending = true;
				}
				if (diff.defending)
				{
					ShouldDirectionMatch = true;
					// Offense exit on a clock: patience sets it, aggression ends it,
					// a strong read doubles it; recovering skips it.
					if (!TargetSwingingAtMe &&
						AttackHandler::GetSingleton()->CanAttack(actor) &&
						(Recovering ||
						(diff.defendTime >= AISettings::DefendPatienceSeconds *
							std::clamp(1.f + diff.patienceMod, 0.3f, 2.f) *
							(strongRead ? 2.f : 1.f) &&
						// floored: at aggression <= -0.67 the raw expression goes
						// non-positive and the exit can never fire, stranding
						// passive archetypes in defense
						RollTickScaled(std::max(1.f, 2.0f + diff.aggressionMod * 3.0f), 10.f, Tick))))
					{
						diff.defending = false;
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
					// Tracking a target that can't reach me only spends switches; a real swing still answers.
					if (ShouldDirectionMatch && (TargetSwingingAtMe || TargetInThreatRange))
					{
						DirectionMatchTarget(actor, target, TargetSwingingAtMe);
					}
					else
					{
						// Lead a retreating target: test the swing against where it
						// will be, not where it is.
						float AttackDist = std::sqrt(TargetDistSQ);
						if (target->AsActorState()->actorState1.movingBack)
						{
							AttackDist += BackpedalLeadUnits;
						}
						SwitchToNewDirection(actor, target, AttackDist * AttackDist);
					}
				}
			}
						

		}
		else
		{
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
			}

			SwitchToNextAttack(actor);
		}

		// Read by next tick's DirectionMatchTarget: a swing spikes only on its first tick.
		diff.sawSwingLastTick = TargetDistSQ < JudgeRadius * JudgeRadius && TargetSwingingAtMe;

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

bool AIHandler::ShouldAttackExternalCalled(RE::Actor* actor, RE::Actor* target)
{
	// This AI sends its own attackStart against a directional opponent, so a
	// vanilla swing there would double up. Against anything else RunActor
	// only steps the pattern and never swings, so vanilla keeps the decision.
	UNUSED(actor);
	return !DirectionHandler::GetSingleton()->IsDirectionalOpponent(target);
}

void AIHandler::TryRiposteExternalCalled(RE::Actor* actor, RE::Actor*)
{
	if (!AttackHandler::GetSingleton()->CanAttack(actor))
	{
		return;
	}
	std::unique_lock lock(DifficultyMapMtx);
	const int Slots = (static_cast<int>(CalcAndInsertDifficulty(actor)) + 7) / 2;
	const float Chance = std::clamp(1.f - 1.f / static_cast<float>(Slots) +
		DifficultyMap[actor->GetHandle()].baitTendency * RiposteBaitScale, RiposteChanceMin, RiposteChanceMax);
	// force block stop to avoid weird stamina issues
	if (static_cast<float>(mt_rand() % 10000u) < Chance * 10000.f)
	{

		SwitchToNextAttack(actor);
		// A blind feint: a guess that they'll commit to defending the riposte. Their guard
		// here is just the line they swung from, so there's nothing of theirs to read.
		bool ShouldFeint = !DirectionHandler::GetSingleton()->IsUnblockable(actor);
		float TotalStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		float CurrentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		if (CurrentStamina > TotalStamina * 0.15f)
		{
			DifficultyMap[actor->GetHandle()].defending = false;
			// The next tick may spike the blocked swing again if it's still running.
			DifficultyMap[actor->GetHandle()].sawSwingLastTick = false;
			if (CurrentStamina < TotalStamina * 0.4f)
			{
				ShouldFeint = false;
			}
			// Rare, and only for feint-leaning personalities: feinting here gives up the
			// riposte turn, landing after the attacker's lockout has run out.
			const float FeintShare = std::clamp(0.3f * DifficultyMap[actor->GetHandle()].feintTendency, 0.f, 1.f);
			if (static_cast<float>(mt_rand() % 10000u) >= FeintShare * 10000.f)
			{
				ShouldFeint = false;
			}
			// Priority 1, so a neutral pre-block can't wipe the counter before it comes out.
			if (ShouldFeint)
			{
				AddAction(actor, Actions::UnblockStartFeint, false, 1);
			}
			else
			{
				AddAction(actor, Actions::UnblockRiposte, false, 1);
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

void AIHandler::TryBlockExternalCalled(RE::Actor* actor, RE::Actor*)
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
		AddAction(actor, AIHandler::Actions::Block, lockedOut);
	}
}

// Ordinal preference for the target's next attack line. False with no combo
// history — the caller must skip the blend, or it erases learned belief in neutral.
static bool PredictedLinePreference(RE::Actor* target, int (&OutPref)[4])
{
	auto* Dir = DirectionHandler::GetSingleton();
	Directions last;
	// Fool me once: a landed repeat means the structure isn't what they're
	// playing, so fall back to learned belief until they change line.
	if (!Dir->GetLastAttackDirection(target, last) || Dir->GetRepeatCount(target) > 0)
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
			// Same-side is a four-line notion; on three every fresh line is equal.
			OutPref[i] = DirectionHandler::EnabledDirections() == 0xF &&
				DirectionHandler::IsLeftSide(dir) == DirectionHandler::IsLeftSide(last) ? 1 : 2;
		}
	}
	return true;
}

// force should not be abused, as it can cause actions to never be finished because they are constantly be overwritten
void AIHandler::DirectionMatchTarget(RE::Actor* actor, RE::Actor* target, bool force)
{
	int mod = (int)CalcAndInsertDifficulty(actor);
	auto& diff = DifficultyMap[actor->GetHandle()];

	// Conditioning-resistance divisor, capped at Normal so high tiers stay fake-out-able.
	const int condMod = std::clamp(mod, 1, 3);
	// Belief drains below are per tick, so they convert against this.
	const float Tick = CalcUpdateTimer(actor);

	// The line tracked at entry: the cascade may change ToCounter, but the
	// mistake step conditions the line the player just left.
	const Directions PreviousTracked = diff.lastDirectionTracked;
	Directions ToCounter = PreviousTracked;
	Directions CurrentTargetDir = DirectionHandler::GetSingleton()->GetCurrentDirection(target);

	// Locals, written back once at the end. Only the SUM is budget-constrained
	// (accumulate() below); a single line may exceed 25.
	int TR = std::min(diff.directionChangeChance[Directions::TR], BeliefBudget);
	int TL = std::min(diff.directionChangeChance[Directions::TL], BeliefBudget);
	int BL = std::min(diff.directionChangeChance[Directions::BL], BeliefBudget);
	int BR = std::min(diff.directionChangeChance[Directions::BR], BeliefBudget);
	// A mode that folds a line must never pick it.
	if (!DirectionHandler::DirectionEnabled(Directions::TR)) TR = 0;
	if (!DirectionHandler::DirectionEnabled(Directions::TL)) TL = 0;
	if (!DirectionHandler::DirectionEnabled(Directions::BL)) BL = 0;
	if (!DirectionHandler::DirectionEnabled(Directions::BR)) BR = 0;
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

	// Panic: belief accumulation scales up as health drops, 1.0x at 40%+ to
	// ~1.75x at 10%. Cuts both ways: easier to feint, quicker to lock on.
	const float MaxHealth = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth);
	const float CurrentHealth = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth);
	const float HealthRatio = MaxHealth > 0.f ? CurrentHealth / MaxHealth : 1.f;
	const float PanicMult = 1.f + std::clamp((0.4f - HealthRatio) / 0.3f, 0.f, 1.f) * 0.75f;

	// Budget-conserving: invest in one line, drain the strongest other until
	// the total fits, so deep commitment visibly pulls belief from the rest.
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

	// Diagnostic tag for VerboseLogging: which mechanism produced this guard decision.
	const char* decisionKind = "hold";

	// Switches the target completed since the last tick, recorded per frame.
	// Events condition belief only; branch selection stays on the tick sample,
	// or fidgety play churns the hold state.
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
	int ChainNode = -1;
	{
		// The start row is the prior and the learned edges out of the target's
		// last landed line, or the opener row with none, add to it.
		int Belief[4] = { TR, TL, BL, BR };
		Directions LastLanded;
		if (!DirectionHandler::GetSingleton()->GetLastAttackDirection(target, LastLanded))
		{
			ChainNode = OpenerRow;
		}
		else if (static_cast<int>(LastLanded) < 4)
		{
			ChainNode = static_cast<int>(LastLanded);
		}
		if (ChainNode >= 0)
		{
			for (int i = 0; i < 4; ++i)
			{
				Belief[i] += diff.chainEdges[ChainNode][i];
			}
		}
		int Total = Belief[0] + Belief[1] + Belief[2] + Belief[3];
		// Scaled back into the budget, or the lottery truncates the last lines.
		if (Total > BeliefBudget)
		{
			for (int& B : Belief)
			{
				B = B * BeliefBudget / Total;
			}
			Total = Belief[0] + Belief[1] + Belief[2] + Belief[3];
		}
		int Pref[4] = {};
		const bool HasStructure = PredictedLinePreference(target, Pref);
		// Folded lines are zeroed in the belief above; the structural term must
		// not reintroduce them.
		for (int i = 0; i < 4; ++i)
		{
			if (!DirectionHandler::DirectionEnabled(static_cast<Directions>(i)))
			{
				Pref[i] = 0;
			}
		}
		// The rule masks and the beliefs rank: legal lines keep their learned
		// order instead of being spread evenly over.
		int Ranked[4] = {};
		int RankedSum = 0;
		if (HasStructure)
		{
			for (int i = 0; i < 4; ++i)
			{
				Ranked[i] = Pref[i] * (Belief[i] + StructuralRankPrior);
				RankedSum += Ranked[i];
			}
		}
		// Tier only: reading combo structure is rule knowledge every fighter has,
		// not a personality trait. Bait used to gate it, which left the non-bait
		// archetypes predicting the habit line right after it landed — the one
		// line the combo rule says can't come next.
		constexpr int LowTier = static_cast<int>(Difficulty::VeryEasy);
		constexpr int HighTier = static_cast<int>(Difficulty::Legendary);
		const float TierT = std::clamp(
			static_cast<float>(static_cast<int>(diff.difficulty) - LowTier) /
			static_cast<float>(HighTier - LowTier), 0.f, 1.f);
		const float TierScale = AISettings::ComboReadLowTierScale +
			(1.f - AISettings::ComboReadLowTierScale) * TierT;
		const float k = HasStructure
			? std::clamp(AISettings::ComboReadStrength * TierScale, 0.f, 1.f)
			: 0.f;
		for (int i = 0; i < 4; ++i)
		{
			const float structural = (RankedSum > 0)
				? static_cast<float>(Total) * static_cast<float>(Ranked[i]) / static_cast<float>(RankedSum)
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
			// No conditioned pick: baseline track-or-stay. Deliberately weak, so
			// the AI defends by reading, not by reflex.
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
		diff.numTimesDirectionsSwitched = std::max(diff.numTimesDirectionsSwitched - 1, 0);
		diff.conditioningStreak = std::max(diff.conditioningStreak - 1, 0);
		diff.numTimesDirectionSame++;
		// The other three lines ebb every tick of the hold. The held line is a
		// loaded attack — guard charge speeds the next swing from it — so
		// belief there grows with the target's charge instead.
		const int holdDrain = DrainPerTick(HoldDrainPerSecond, Tick, diff.beliefDrainRemainder);
		int& held = chanceFor(ToCounter);
		for (int* p : { &TR, &TL, &BL, &BR })
		{
			if (p != &held)
			{
				*p = std::max(0, *p - holdDrain);
			}
		}
		const float Charge = DirectionHandler::GetSingleton()->GetGuardChargeRatio(target);
		const int holdAccrual = DrainPerTick(HoldAccrualPerSecond * Charge, Tick, diff.holdAccrualRemainder);
		if (holdAccrual > 0)
		{
			accumulate(ToCounter, holdAccrual);
		}
		// Acquisition gate: the guard can't arrive at the counter inside the
		// commit window plus the fixation floor; past it, it always adjusts.
		if (force)
		{
			const Directions guardDir = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
			// Folded: beliefs live on the lines attacks actually come from.
			const Directions coveredLine = DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(guardDir));
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

	// Force bump: the target is attacking, so belief spikes on that line and
	// the others drain faster — locking on once a commit happens.
	if (force)
	{
		const int DifficultyMod = AISettings::BeliefAccumBase * AISettings::AIMistakeRatio;
		// 3x the normal bump, but only on the swing's first tick: each attack
		// teaches the line once, so slow attacks don't teach more than fast ones.
		if (!diff.sawSwingLastTick && DifficultyMod > 0)
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

	if (!SuppressSwitch)
	{
		const Directions ToSwitch = DirectionHandler::GetCounterDirection(ToCounter);
		QueueDirectionSwitch(actor, ToSwitch, force);

		if (force && ToCounter == CurrentTargetDir &&
			IsPreparedToBlock(actor, target, diff) && IsBlockableSwing(target) &&
			!CanAnswerLine(actor, target, diff))
		{
			AddAction(actor, Actions::Block, true);
		}
	}

	// Once per swing: what the guard did about it, and why.
	if (Settings::VerboseLogging && force && !diff.sawSwingLastTick)
	{
		logger::info("[dmt] {} {:08X} answers {} swing from {}: {} (guard {} -> {}, line seen {:.0f}ms, window {:.0f}ms)",
			actor->GetName(), actor->GetFormID(), target->GetName(), static_cast<int>(CurrentTargetDir), decisionKind,
			static_cast<int>(DirectionHandler::GetSingleton()->GetCurrentDirection(actor)),
			SuppressSwitch ? -1 : static_cast<int>(DirectionHandler::GetCounterDirection(ToCounter)),
			diff.timeSinceLineChange * 1000.f, CommitWindow(diff) * 1000.f);
	}
	diff.lastDirectionTracked = CurrentTargetDir;

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
	// Belief is keyed by the line the attack comes from; the HUD wants guard
	// space, so map through the counter. Cached weights, so the arcs show the roll.
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
	Directions TargetDirection = DirectionHandler::GetSingleton()->GetCurrentDirection(target);
	// This will queue up this event if you cant switch instead
	Directions CounterDirection = DirectionHandler::GetCounterDirection(TargetDirection);
	unsigned idx = diff.currentAttackIdx;
	// A landed poke sits in the combo ring; skip a pattern entry that would repeat it,
	// unless the repeat is deliberate.
	if (!diff.repeatNext && !diff.attackPattern.empty() && idx < diff.attackPattern.size() &&
		DirectionHandler::GetSingleton()->IsInComboWindow(actor, DirectionHandler::FoldDirection(diff.attackPattern[idx])))
	{
		diff.currentAttackIdx = (idx + 1) % static_cast<unsigned>(diff.attackPattern.size());
		idx = diff.currentAttackIdx;
	}

	float MaxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
	float CurrentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	float CurrentStaminaRatio = CurrentStamina / MaxStamina;
	int mod = (int)CalcAndInsertDifficulty(actor);
	const float Reserve = StaminaReserve(diff.cautionMod);
	const float Tick = CalcUpdateTimer(actor);
	const float ComboProgress = DirectionHandler::GetSingleton()->GetComboProgress(actor);
	const float Commitment = ComboProgress * ComboCommitmentDiscount;
	const float ComboRate = 1.f + ComboProgress * ComboRateBoost;
	// Folded on both sides: in a 3-line mode the two lows (or tops) are one
	// line, so an entry on the folded twin is into the guard too.
	if (DirectionHandler::FoldDirection(diff.attackPattern[idx]) != DirectionHandler::FoldDirection(CounterDirection))
	{
		// A queued swing already asked for its line (a poke's may differ from
		// the pattern's); re-asking here would turn it before it comes out.
		const Actions Queued = GetQueuedAction(actor);
		if (Queued != Actions::Attack && Queued != Actions::FeintFollowup)
		{
			SwitchToNextAttack(actor);
		}
		// Per-line reach of this swing; a kind out of reach isn't picked.
		const Directions GateLine = DirectionHandler::FoldDirection(diff.attackPattern[idx]);
		const bool Chained = DirectionHandler::GetSingleton()->GetComboStep(actor) > 0;
		const float LightReach = GateReach(actor, GateLine, Chained, false);
		const float PowerReach = GateReach(actor, GateLine, Chained, true);
		const bool LightInReach = TargetDistSQ < LightReach * LightReach;
		const bool PowerInReach = TargetDistSQ < PowerReach * PowerReach;
		// Floor is the only hard gate; AttackStaminaOffLine is now where the
		// rate reaches full rather than a cliff below which nothing happens.
		if (CurrentStaminaRatio > AttackStaminaFloor + Reserve && (LightInReach || PowerInReach))
		{
			// Attack frequency as a rate, so the tier's timer isn't a hidden
			// multiplier. The dominant difficulty axis in play.
			const float StaminaScale = StaminaAttackScale(
				CurrentStaminaRatio, AttackStaminaOffLine, Reserve, Commitment);
			if (RollPerSecond(std::max(AttackRateBaseFloor, mod + diff.aggressionMod * 1.5f) * AttackRatePerMod * StaminaScale * ComboRate, Tick))
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
				if (PowerInReach && DirHandler->WouldCompleteCombo(actor, AttackDir) &&
					CurrentStaminaRatio > ComboFinisherStaminaRatio + Reserve)
				{
					AddAction(actor, AIHandler::Actions::PowerAttack);
				}
				// PowerAttackTendency-modulated attack-type choice: NPCs that
				// favor power attacks pick them more often over light attacks.
				else if (LightInReach && (!PowerInReach || mt_rand() % 3 < (2.0f - diff.powerAttackTendency * 1.5f)))
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
		const float PressChance = std::clamp((2.0f + diff.feintTendency) / 3.f, 0.f, 1.f);
		bool ShouldFeint = static_cast<float>(mt_rand() % 10000u) < PressChance * 10000.f;

		if (CurrentStaminaRatio > AttackStaminaFloor + Reserve)
		{
			// Same gate as the off-line branch above, tapered the same way.
			// Keeps its own full-rate point: this branch is already on the
			// counter line, so it reaches full rate earlier.
			const float StaminaScale = StaminaAttackScale(
				CurrentStaminaRatio, AttackStaminaFeint, Reserve, Commitment);
			if (RollPerSecond(std::max(AttackRateBaseFloor, mod + diff.aggressionMod * 1.5f) * AttackRatePerMod * StaminaScale * ComboRate, Tick))
			{
				// Flat reach: a feint redirects to a cut, whatever line it starts on.
				const float CutReach = actor->GetReach() + CutLungeUnits;
				if (ShouldFeint && TargetDistSQ < CutReach * CutReach)
				{
					SwitchToNextAttack(actor);
					// A third of these are feints with no lean; the tendency scales that
					// share, and power and light split the rest.
					const float FeintShare = std::clamp((1.f + diff.feintTendency) / 3.f, 0.f, 1.f);
					const float Pick = static_cast<float>(mt_rand() % 10000u) / 10000.f;

					if (Pick < FeintShare)
					{
						AddAction(actor, AIHandler::Actions::StartFeint);
					}
					else if (Pick < FeintShare + (1.f - FeintShare) * 0.5f)
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
			// 0.9 per tick is a 1.5-3s half-life: a brief measure break keeps
			// the streak, a real disengage resets it. (0.5 made composure binary.)
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

void AIHandler::RecordPlayerChainTransition(WeaponSet a_set, int a_from, Directions a_to)
{
	// Off, the saved profile stays as it was.
	if (!AISettings::LearnAcrossFights)
	{
		return;
	}
	const int Set = static_cast<int>(a_set);
	const int From = a_from;
	const int To = static_cast<int>(a_to);
	if (Set < 0 || Set >= NumWeaponSets || From < 0 || From > OpenerRow || To < 0 || To > 3)
	{
		return;
	}
	std::lock_guard Lock(PlayerHabitMtx);
	float (&Row)[4] = PlayerHabit[Set][From];
	for (float& Count : Row)
	{
		Count *= PlayerHabitDecay;
	}
	Row[To] += 1.f;
	if (Settings::VerboseLogging)
	{
		if (From == OpenerRow)
		{
			logger::info("[habit] player opens {}, row now [TR:{:.1f} TL:{:.1f} BL:{:.1f} BR:{:.1f}] ({})", To,
				Row[0], Row[1], Row[2], Row[3], WeaponSetName(a_set));
		}
		else
		{
			logger::info("[habit] player {} -> {}, row now [TR:{:.1f} TL:{:.1f} BL:{:.1f} BR:{:.1f}] ({})", From, To,
				Row[0], Row[1], Row[2], Row[3], WeaponSetName(a_set));
		}
	}
}

void AIHandler::SavePlayerHabit(SKSE::SerializationInterface* a_intfc)
{
	std::lock_guard Lock(PlayerHabitMtx);
	if (!a_intfc->WriteRecord(PlayerHabitRecord, PlayerHabitVersion,
			static_cast<const void*>(PlayerHabit), static_cast<std::uint32_t>(sizeof(PlayerHabit))))
	{
		logger::error("[habit] couldn't write the player profile to the co-save");
	}
}

// Saved layouts are frozen. A new layout gets a new version, and every older reader
// stays below to convert its saves.
// v1: [5 weapon sets][5 rows][4 lines] floats, indexed by WeaponSet and Directions.
using PlayerHabitV1 = float[5][5][4];

void AIHandler::LoadPlayerHabit(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
{
	switch (a_version)
	{
	case 1:
	{
		PlayerHabitV1 Loaded;
		if (a_length != sizeof(Loaded) || a_intfc->ReadRecordData(Loaded, sizeof(Loaded)) != sizeof(Loaded))
		{
			logger::error("[habit] couldn't read the v1 player profile ({} bytes)", a_length);
			return;
		}
		// The length check can't see damaged contents; a count settles at 20.
		for (const auto& Set : Loaded)
		{
			for (const auto& Row : Set)
			{
				for (const float Count : Row)
				{
					if (!(Count >= 0.f && Count <= 1000.f))
					{
						logger::error("[habit] the saved player profile holds an impossible count; starting it empty");
						return;
					}
				}
			}
		}
		// Fails when the live table changes shape: write v2 and convert v1 here.
		static_assert(std::is_same_v<decltype(PlayerHabit), PlayerHabitV1> && PlayerHabitVersion == 1);
		std::lock_guard Lock(PlayerHabitMtx);
		std::memcpy(PlayerHabit, Loaded, sizeof(Loaded));
		break;
	}
	default:
		// Written by a newer build.
		logger::info("[habit] skipped a player profile of unknown version {}", a_version);
		return;
	}
	logger::info("[habit] player profile loaded from the save (version {})", a_version);
}

void AIHandler::ResetPlayerHabit()
{
	std::lock_guard Lock(PlayerHabitMtx);
	std::memset(PlayerHabit, 0, sizeof(PlayerHabit));
}

constexpr float PlayerStatsBucketUnits = 25.f;

void AIHandler::RecordPlayerSwing(WeaponSet a_set, int a_step, bool a_power, float a_distance, float a_staminaRatio, bool a_powerAffordable)
{
	constexpr int Sets = static_cast<int>(std::extent_v<decltype(PlayerStatsV1::swings), 0>);
	constexpr int Steps = static_cast<int>(std::extent_v<decltype(PlayerStatsV1::swings), 1>);
	constexpr int Buckets = static_cast<int>(std::extent_v<decltype(PlayerStatsV1::distance), 2>);
	constexpr int StaminaBuckets = static_cast<int>(std::extent_v<decltype(PlayerStaminaStatsV1::stamina), 2>);
	const int Set = static_cast<int>(a_set);
	if (Set < 0 || Set >= Sets)
	{
		return;
	}
	const int Step = std::clamp(a_step, 0, Steps - 1);
	std::lock_guard Lock(PlayerStatsMtx);
	++PlayerStats.swings[Set][Step];
	if (a_power)
	{
		++PlayerStats.powers[Set][Step];
	}
	if (a_distance >= 0.f)
	{
		const int Bucket = std::min(static_cast<int>(a_distance / PlayerStatsBucketUnits), Buckets - 1);
		++PlayerStats.distance[Set][a_power ? 1 : 0][Bucket];
	}
	const int Kind = a_power ? 2 : (a_powerAffordable ? 0 : 1);
	const int StaminaBucket = std::clamp(static_cast<int>(a_staminaRatio * StaminaBuckets), 0, StaminaBuckets - 1);
	++PlayerStamina.stamina[Set][Kind][StaminaBucket];
}

void AIHandler::SavePlayerStats(SKSE::SerializationInterface* a_intfc)
{
	std::lock_guard Lock(PlayerStatsMtx);
	if (!a_intfc->WriteRecord(PlayerStatsRecord, PlayerStatsVersion,
			static_cast<const void*>(&PlayerStats), static_cast<std::uint32_t>(sizeof(PlayerStats))))
	{
		logger::error("[stats] couldn't write the player swing stats to the co-save");
	}
	if (!a_intfc->WriteRecord(PlayerStaminaRecord, PlayerStaminaVersion,
			static_cast<const void*>(&PlayerStamina), static_cast<std::uint32_t>(sizeof(PlayerStamina))))
	{
		logger::error("[stats] couldn't write the player stamina stats to the co-save");
	}
	if (!a_intfc->WriteRecord(PlayerFeintRecord, PlayerFeintVersion,
			static_cast<const void*>(&PlayerFeints), static_cast<std::uint32_t>(sizeof(PlayerFeints))))
	{
		logger::error("[stats] couldn't write the player feint stats to the co-save");
	}
	if (!a_intfc->WriteRecord(PlayerOutcomeRecord, PlayerOutcomeVersion,
			static_cast<const void*>(&PlayerOutcomes), static_cast<std::uint32_t>(sizeof(PlayerOutcomes))))
	{
		logger::error("[stats] couldn't write the player outcome stats to the co-save");
	}
}

void AIHandler::BeginPlayerSwing(WeaponSet a_set, int a_step, Directions a_line)
{
	constexpr int Sets = static_cast<int>(std::extent_v<decltype(PlayerOutcomeStatsV1::swings), 0>);
	constexpr int Steps = static_cast<int>(std::extent_v<decltype(PlayerOutcomeStatsV1::swings), 1>);
	constexpr int Lines = static_cast<int>(std::extent_v<decltype(PlayerOutcomeStatsV1::swings), 2>);
	const int Set = static_cast<int>(a_set);
	const int Line = static_cast<int>(a_line);
	std::lock_guard Lock(PlayerStatsMtx);
	// A swing that opens before the last one resolved leaves that one as no contact.
	CurrentPlayerSwing.open = false;
	if (Set < 0 || Set >= Sets || Line < 0 || Line >= Lines)
	{
		return;
	}
	CurrentPlayerSwing = { true, Set, std::clamp(a_step, 0, Steps - 1), Line };
	++PlayerOutcomes.swings[Set][CurrentPlayerSwing.step][Line];
}

void AIHandler::ResolvePlayerSwing(SwingOutcome a_outcome)
{
	std::lock_guard Lock(PlayerStatsMtx);
	if (!CurrentPlayerSwing.open)
	{
		return;
	}
	CurrentPlayerSwing.open = false;
	// An outcome added after v1 has no column in it.
	const int Outcome = static_cast<int>(a_outcome);
	if (Outcome < 0 || Outcome >= static_cast<int>(std::extent_v<decltype(PlayerOutcomeStatsV1::outcomes), 3>))
	{
		return;
	}
	++PlayerOutcomes.outcomes[CurrentPlayerSwing.set][CurrentPlayerSwing.step][CurrentPlayerSwing.line][Outcome];
}

void AIHandler::RecordPlayerDefense(WeaponSet a_playerSet, bool a_power, bool a_chained, DefenseOutcome a_outcome)
{
	constexpr int Sets = static_cast<int>(std::extent_v<decltype(PlayerOutcomeStatsV1::defense), 0>);
	constexpr int Kinds = static_cast<int>(std::extent_v<decltype(PlayerOutcomeStatsV1::defense), 3>);
	const int Set = static_cast<int>(a_playerSet);
	const int Kind = static_cast<int>(a_outcome);
	if (Set < 0 || Set >= Sets || Kind < 0 || Kind >= Kinds)
	{
		return;
	}
	std::lock_guard Lock(PlayerStatsMtx);
	++PlayerOutcomes.defense[Set][a_power ? 1 : 0][a_chained ? 1 : 0][Kind];
}

void AIHandler::LoadPlayerOutcomeStats(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
{
	switch (a_version)
	{
	case 1:
	{
		PlayerOutcomeStatsV1 Loaded;
		if (a_length != sizeof(Loaded) || a_intfc->ReadRecordData(&Loaded, sizeof(Loaded)) != sizeof(Loaded))
		{
			logger::error("[stats] couldn't read the v1 player outcome stats ({} bytes)", a_length);
			return;
		}
		// Fails when the live stats change layout: write v2 and convert v1 here.
		static_assert(std::is_same_v<decltype(PlayerOutcomes), PlayerOutcomeStatsV1> && PlayerOutcomeVersion == 1);
		static_assert(sizeof(PlayerOutcomeStatsV1) == 2400);
		std::lock_guard Lock(PlayerStatsMtx);
		PlayerOutcomes = Loaded;
		break;
	}
	default:
		// Written by a newer build.
		logger::info("[stats] skipped player outcome stats of unknown version {}", a_version);
		return;
	}
}

void AIHandler::RecordPlayerFeint(WeaponSet a_set, int a_step, bool a_power)
{
	constexpr int Sets = static_cast<int>(std::extent_v<decltype(PlayerFeintStatsV1::feints), 0>);
	constexpr int Steps = static_cast<int>(std::extent_v<decltype(PlayerFeintStatsV1::feints), 1>);
	const int Set = static_cast<int>(a_set);
	if (Set < 0 || Set >= Sets)
	{
		return;
	}
	std::lock_guard Lock(PlayerStatsMtx);
	++PlayerFeints.feints[Set][std::clamp(a_step, 0, Steps - 1)][a_power ? 1 : 0];
}

void AIHandler::LoadPlayerFeintStats(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
{
	switch (a_version)
	{
	case 1:
	{
		PlayerFeintStatsV1 Loaded;
		if (a_length != sizeof(Loaded) || a_intfc->ReadRecordData(&Loaded, sizeof(Loaded)) != sizeof(Loaded))
		{
			logger::error("[stats] couldn't read the v1 player feint stats ({} bytes)", a_length);
			return;
		}
		// Fails when the live stats change layout: write v2 and convert v1 here.
		static_assert(std::is_same_v<decltype(PlayerFeints), PlayerFeintStatsV1> && PlayerFeintVersion == 1);
		static_assert(sizeof(PlayerFeintStatsV1) == 160);
		std::lock_guard Lock(PlayerStatsMtx);
		PlayerFeints = Loaded;
		break;
	}
	default:
		// Written by a newer build.
		logger::info("[stats] skipped player feint stats of unknown version {}", a_version);
		return;
	}
}

void AIHandler::LoadPlayerStaminaStats(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
{
	switch (a_version)
	{
	case 1:
	{
		PlayerStaminaStatsV1 Loaded;
		if (a_length != sizeof(Loaded) || a_intfc->ReadRecordData(&Loaded, sizeof(Loaded)) != sizeof(Loaded))
		{
			logger::error("[stats] couldn't read the v1 player stamina stats ({} bytes)", a_length);
			return;
		}
		// Fails when the live stats change layout: write v2 and convert v1 here.
		static_assert(std::is_same_v<decltype(PlayerStamina), PlayerStaminaStatsV1> && PlayerStaminaVersion == 1);
		static_assert(sizeof(PlayerStaminaStatsV1) == 600);
		std::lock_guard Lock(PlayerStatsMtx);
		PlayerStamina = Loaded;
		break;
	}
	default:
		// Written by a newer build.
		logger::info("[stats] skipped player stamina stats of unknown version {}", a_version);
		return;
	}
}

void AIHandler::LoadPlayerStats(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length)
{
	switch (a_version)
	{
	case 1:
	{
		PlayerStatsV1 Loaded;
		if (a_length != sizeof(Loaded) || a_intfc->ReadRecordData(&Loaded, sizeof(Loaded)) != sizeof(Loaded))
		{
			logger::error("[stats] couldn't read the v1 player swing stats ({} bytes)", a_length);
			return;
		}
		// Fails when the live stats change layout: write v2 and convert v1 here.
		static_assert(std::is_same_v<decltype(PlayerStats), PlayerStatsV1> && PlayerStatsVersion == 1);
		static_assert(sizeof(PlayerStatsV1) == 840);
		std::lock_guard Lock(PlayerStatsMtx);
		PlayerStats = Loaded;
		break;
	}
	default:
		// Written by a newer build.
		logger::info("[stats] skipped player swing stats of unknown version {}", a_version);
		return;
	}
}

void AIHandler::ResetPlayerStats()
{
	std::lock_guard Lock(PlayerStatsMtx);
	PlayerStats = {};
	PlayerStamina = {};
	PlayerFeints = {};
	PlayerOutcomes = {};
	CurrentPlayerSwing = {};
}

void AIHandler::LogPlayerStats(const char* a_when)
{
	if (!Settings::VerboseLogging)
	{
		return;
	}
	constexpr int Sets = static_cast<int>(std::extent_v<decltype(PlayerStatsV1::swings), 0>);
	constexpr int Steps = static_cast<int>(std::extent_v<decltype(PlayerStatsV1::swings), 1>);
	constexpr int Buckets = static_cast<int>(std::extent_v<decltype(PlayerStatsV1::distance), 2>);
	std::lock_guard Lock(PlayerStatsMtx);
	logger::info("[stats] player swing stats, {}", a_when);
	for (int Set = 0; Set < Sets; ++Set)
	{
		std::uint32_t Total = 0;
		for (int Step = 0; Step < Steps; ++Step)
		{
			Total += PlayerStats.swings[Set][Step];
		}
		if (Total == 0)
		{
			continue;
		}
		const char* SetName = WeaponSetName(static_cast<WeaponSet>(Set));
		std::string Line = std::format("[stats] {} powers:", SetName);
		for (int Step = 0; Step < Steps; ++Step)
		{
			const std::uint32_t Swings = PlayerStats.swings[Set][Step];
			const std::uint32_t Powers = PlayerStats.powers[Set][Step];
			const std::string Label = Step == 0 ? "opener" : std::format("step {}{}", Step, Step == Steps - 1 ? "+" : "");
			Line += std::format(" {} {}/{} ({:.0f}%)", Label, Powers, Swings, Swings ? 100.f * Powers / Swings : 0.f);
		}
		logger::info("{}", Line);
		std::string Feints = std::format("[stats] {} feints:", SetName);
		for (int Step = 0; Step < Steps; ++Step)
		{
			const std::uint32_t Powers = PlayerStats.powers[Set][Step];
			const std::uint32_t Lights = PlayerStats.swings[Set][Step] - Powers;
			const std::string Label = Step == 0 ? "opener" : std::format("step {}{}", Step, Step == Steps - 1 ? "+" : "");
			Feints += std::format(" {} light {}/{} power {}/{}", Label,
				PlayerFeints.feints[Set][Step][0], Lights, PlayerFeints.feints[Set][Step][1], Powers);
		}
		logger::info("{}", Feints);
		for (int Kind = 0; Kind < 2; ++Kind)
		{
			std::string Dist = std::format("[stats] {} {} start distance:", SetName, Kind ? "power" : "light");
			bool Any = false;
			for (int Bucket = 0; Bucket < Buckets; ++Bucket)
			{
				const std::uint32_t Count = PlayerStats.distance[Set][Kind][Bucket];
				if (Count == 0)
				{
					continue;
				}
				Any = true;
				const int From = static_cast<int>(Bucket * PlayerStatsBucketUnits);
				Dist += Bucket == Buckets - 1 ? std::format(" {}+:{}", From, Count) :
					std::format(" {}-{}:{}", From, From + static_cast<int>(PlayerStatsBucketUnits), Count);
			}
			if (Any)
			{
				logger::info("{}", Dist);
			}
		}
		constexpr int StaminaBuckets = static_cast<int>(std::extent_v<decltype(PlayerStaminaStatsV1::stamina), 2>);
		constexpr const char* StaminaKinds[] = { "light, power affordable", "light, power not affordable", "power" };
		for (int Kind = 0; Kind < 3; ++Kind)
		{
			std::string Stam = std::format("[stats] {} {} start stamina:", SetName, StaminaKinds[Kind]);
			bool Any = false;
			for (int Bucket = 0; Bucket < StaminaBuckets; ++Bucket)
			{
				const std::uint32_t Count = PlayerStamina.stamina[Set][Kind][Bucket];
				if (Count > 0)
				{
					Any = true;
					Stam += std::format(" {}-{}%:{}", Bucket * 10, Bucket * 10 + 10, Count);
				}
			}
			if (Any)
			{
				logger::info("{}", Stam);
			}
		}
		// Outcomes summed two ways: by step over every line, by line over every step.
		constexpr int OutcomeKinds = static_cast<int>(std::extent_v<decltype(PlayerOutcomeStatsV1::outcomes), 3>);
		constexpr int Lines = static_cast<int>(std::extent_v<decltype(PlayerOutcomeStatsV1::swings), 2>);
		constexpr const char* OutcomeNames[] = { "landed", "blocked", "clashed", "masterstruck", "feinted" };
		auto LogOutcomes = [&](const std::string& Label, int StepFrom, int StepTo, int LineFrom, int LineTo)
		{
			std::uint32_t Swings = 0;
			std::uint32_t Counts[OutcomeKinds] = {};
			for (int Step = StepFrom; Step <= StepTo; ++Step)
			{
				for (int Line = LineFrom; Line <= LineTo; ++Line)
				{
					Swings += PlayerOutcomes.swings[Set][Step][Line];
					for (int Kind = 0; Kind < OutcomeKinds; ++Kind)
					{
						Counts[Kind] += PlayerOutcomes.outcomes[Set][Step][Line][Kind];
					}
				}
			}
			if (Swings == 0)
			{
				return;
			}
			std::uint32_t Resolved = 0;
			std::string Out = std::format("[stats] {} swing outcomes, {}: {} swings", SetName, Label, Swings);
			for (int Kind = 0; Kind < OutcomeKinds; ++Kind)
			{
				Resolved += Counts[Kind];
				Out += std::format(", {} {}", OutcomeNames[Kind], Counts[Kind]);
			}
			logger::info("{}, no contact {}", Out, Swings - std::min(Swings, Resolved));
		};
		for (int Step = 0; Step < Steps; ++Step)
		{
			LogOutcomes(Step == 0 ? "opener" : std::format("step {}{}", Step, Step == Steps - 1 ? "+" : ""), Step, Step, 0, Lines - 1);
		}
		constexpr const char* LineNames[] = { "TR", "TL", "BL", "BR" };
		static_assert(std::size(LineNames) == Lines);
		for (int Line = 0; Line < Lines; ++Line)
		{
			LogOutcomes(std::format("line {}", LineNames[Line]), 0, Steps - 1, Line, Line);
		}
		constexpr int DefenseKinds = static_cast<int>(std::extent_v<decltype(PlayerOutcomeStatsV1::defense), 3>);
		constexpr const char* DefenseNames[] = { "blocked", "parried", "guard missed", "no guard", "unblockable", "countered" };
		for (int Power = 0; Power < 2; ++Power)
		{
			for (int Chained = 0; Chained < 2; ++Chained)
			{
				std::uint32_t Total = 0;
				std::string Def = std::format("[stats] {} defence vs NPC {} {}:", SetName, Power ? "power" : "light", Chained ? "chained" : "opener");
				for (int Kind = 0; Kind < DefenseKinds; ++Kind)
				{
					const std::uint32_t Count = PlayerOutcomes.defense[Set][Power][Chained][Kind];
					Total += Count;
					Def += std::format(" {} {}{}", DefenseNames[Kind], Count, Kind + 1 < DefenseKinds ? "," : "");
				}
				if (Total > 0)
				{
					logger::info("{} (of {})", Def, Total);
				}
			}
		}
	}
}

void AIHandler::SeedChainEdges(AIDifficulty& diff, WeaponSet a_set)
{
	constexpr int LowTier = static_cast<int>(Difficulty::VeryEasy);
	constexpr int HighTier = static_cast<int>(Difficulty::Legendary);
	const float TierT = std::clamp(
		static_cast<float>(static_cast<int>(diff.difficulty) - LowTier) /
		static_cast<float>(HighTier - LowTier), 0.f, 1.f);
	const float TierScale = PlayerHabitSeedFloor + (1.f - PlayerHabitSeedFloor) * TierT;
	const int Set = static_cast<int>(a_set);
	if (Set < 0 || Set >= NumWeaponSets)
	{
		return;
	}
	std::lock_guard Lock(PlayerHabitMtx);
	for (int From = 0; From <= OpenerRow; ++From)
	{
		const float (&Row)[4] = PlayerHabit[Set][From];
		const float Total = Row[0] + Row[1] + Row[2] + Row[3];
		if (Total <= 0.f)
		{
			continue;
		}
		const float Confidence = std::min(1.f, Total / PlayerHabitFullCount);
		for (int To = 0; To < 4; ++To)
		{
			diff.chainEdges[From][To] = static_cast<int>(
				Row[To] / Total * PlayerHabitSeedMax * TierScale * Confidence + 0.5f);
		}
	}
}

// Staggered or inside a dodge clip: an attackStart is accepted but never plays.
static bool InTransition(RE::Actor* actor)
{
	auto* Dodges = DodgeHandler::GetSingleton();
	const float SinceDodge = Dodges->SecondsSinceDodge(actor);
	return actor->AsActorState()->actorState2.staggered || Dodges->IsDodging(actor) ||
		(SinceDodge >= 0.f && SinceDodge < DodgeClipSeconds);
}

bool AIHandler::TryAttack(RE::Actor* actor)
{
	// since this is a forced attack, it happens outside of the normal AI attack loop so we need to add checks here as well
	// Mid-swing only in the chain window; a queued step retries until then.
	if (!InTransition(actor) && AttackHandler::GetSingleton()->CanInitiateAttack(actor) &&
		(!actor->IsAttacking() || DirectionHandler::GetSingleton()->InAttackWindow(actor)))
	{
		AttackHandler::GetSingleton()->DoAttack(actor);
		return true;
	}
	return false;
}

bool AIHandler::TryPowerAttack(RE::Actor* actor)
{

	// since this is a forced attack, it happens outside of the normal AI attack loop so we need to add checks here as well
	// Same chain-window rule as TryAttack.
	if (!InTransition(actor) && AttackHandler::GetSingleton()->CanInitiateAttack(actor) && !actor->IsBlocking() &&
		(!actor->IsAttacking() || DirectionHandler::GetSingleton()->InAttackWindow(actor)))
	{
		// Through the action system with the event filled, as vanilla attacks are.
		AttackHandler::GetSingleton()->DoPowerAttack(actor);
		return true;
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
	// force means override: a defensive correction replaces a pending switch and
	// a deliberate one can't replace it. It still pays the input delay.
	auto Iter = DirectionQueue.find(actor->GetHandle());
	if (Iter == DirectionQueue.end())
	{
		// A forced hold is re-decided every tick of a swing; parking a no-op
		// switch would hold back the riposte that waits on pending switches.
		if (force && DirectionHandler::GetSingleton()->GetCurrentDirection(actor) == DirectionHandler::FoldDirection(dir))
		{
			return;
		}
		DirectionQueue[actor->GetHandle()] = { dir, force, GuardInputSeconds };
		return;
	}
	// Deciding the same line again is not a new movement. Re-arming it every
	// tick would stall the switch forever once the decision interval dropped
	// below GuardInputSeconds.
	if (Iter->second.dir == dir)
	{
		Iter->second.force = Iter->second.force || force;
		return;
	}
	if (!force && Iter->second.force)
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

void AIHandler::SwitchToNextAttack(RE::Actor* actor)
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
		// The level delta only carries to Hard. Past it the old mapping saturated,
		// so every late-game opponent was Legendary and the tier stopped meaning
		// anything. Above Hard is the jitter's to give.
		const Difficulty Base = std::min(ret, Difficulty::Hard);
		// Every per-NPC draw keys off the FormID: the save keeps it, where the
		// runtime handle can change between sessions.
		const std::uint32_t ActorSeed = actor->GetFormID();
		// Stable per actor like the personality draw, so it survives a reload and
		// you can learn a particular NPC, but mixed differently so competence and
		// archetype don't correlate.
		std::uint32_t JitterSeed = ActorSeed + 0x9E3779B9U;
		JitterSeed ^= JitterSeed >> 15;
		JitterSeed *= 0x2c1b3c6dU;
		JitterSeed ^= JitterSeed >> 12;
		const int Roll = static_cast<int>(JitterSeed % 100u);
		const int Jitter = Roll < TierJitterDown ? -1 :
			Roll < TierJitterDown + TierJitterFlat ? 0 :
			Roll < TierJitterDown + TierJitterFlat + TierJitterUp ? 1 : 2;
		ret = static_cast<Difficulty>(std::clamp(static_cast<int>(Base) + Jitter,
			static_cast<int>(Difficulty::VeryEasy), static_cast<int>(Difficulty::Legendary)));
		if (Settings::VerboseLogging) logger::info("[ai] {} got difficulty level {} (base {}, jitter {:+})",
			actor->GetName(), (int)ret, (int)Base, Jitter);
		AIDifficulty aidiff = { ret, 0.f };
		DifficultyMap[actor->GetHandle()] = aidiff;
		DifficultyMap[actor->GetHandle()].lastDirectionsEncountered.reserve(MaxDirs);
		//default
		DifficultyMap[actor->GetHandle()].lastDirectionTracked = Directions::TR;

		// generate attack patterns ahead of time, from the seed rather than rand so
		// you can learn after dying
		std::array<Directions, 3> Lines{};
		int NumLines = 0;
		for (Directions Dir : { Directions::TR, Directions::TL, Directions::BL, Directions::BR })
		{
			if (DirectionHandler::DirectionEnabled(Dir) && NumLines < 3)
			{
				Lines[NumLines++] = Dir;
			}
		}
		if (NumLines == 3)
		{
			// Three lines: the table rows fold into repeats, so build a loop over the real lines.
			// Never the same line back-to-back, so combos still complete.
			// Nine distinct loops, evenly: 3 by favourite line (length 4; swapping Y and Z
			// only shifts that loop) and 6 by ordering (length 5).
			const uint32_t Pick = ActorSeed % 9;
			const uint32_t Perm = Pick < 3 ? Pick * 2 : Pick - 3;
			std::array<int, 3> Order{ 0, 1, 2 };
			for (uint32_t i = 0; i < Perm; ++i)
			{
				std::next_permutation(Order.begin(), Order.end());
			}
			const Directions X = Lines[Order[0]], Y = Lines[Order[1]], Z = Lines[Order[2]];
			auto& Pattern = DifficultyMap[actor->GetHandle()].attackPattern;
			if (Pick < 3)
			{
				Pattern = { X, Y, X, Z };     // one favourite line
			}
			else
			{
				Pattern = { X, Y, X, Y, Z };  // one rare line
			}
		}
		else
		{
			int Idx = ActorSeed % TotalAttackCombos;
			for (unsigned i = 0; i < AttackComboLength; ++i)
			{
				DifficultyMap[actor->GetHandle()].attackPattern.push_back(AIAttackCombo[Idx][i]);
			}
		}


		// create modifier to see how good or bad the AI is at judging distance
		DifficultyMap[actor->GetHandle()].numTimesDirectionsSwitched = 0;
		DifficultyMap[actor->GetHandle()].currentAttackIdx = 0;
		DifficultyMap[actor->GetHandle()].defending = false;

		// seed with deterministic input
		DifficultyMap[actor->GetHandle()].npcRand.seed(ActorSeed);

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

		// Personality: pick an archetype from the seed, add small per-NPC jitter
		// so two NPCs with the same archetype still feel slightly different.
		{
			std::uint32_t seed = ActorSeed;
			seed ^= seed >> 16;
			seed *= 0x7feb352dU;
			seed ^= seed >> 15;
			seed *= 0x846ca68bU;
			seed ^= seed >> 16;

			const auto& arch = kPersonalityArchetypes[seed % kNumPersonalityArchetypes];
			//const auto& arch = kPersonalityArchetypes[3];
			// Jitter is small (±0.1) so the archetype identity dominates but
			// no two NPCs of the same archetype play identically.
			auto jitter = [&](unsigned shift) -> float {
				const std::uint32_t byte = (seed >> shift) & 0xFFu;
				return ((static_cast<float>(byte) / 127.5f) - 1.0f) * 0.1f;
			};

			auto& d = DifficultyMap[actor->GetHandle()];
			d.archetype           = arch.name;
			d.aggressionMod       = arch.aggression  + jitter(0);
			d.baseAggression      = d.aggressionMod;
			d.patienceMod         = arch.patience    + jitter(8);
			d.baitTendency        = arch.bait        + jitter(16);
			d.cautionMod          = arch.caution     + jitter(24);
			d.baseCaution         = d.cautionMod;
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
	const Directions failedLine = DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(guardDir));
	std::unique_lock lock(DifficultyMapMtx);
	auto iter = DifficultyMap.find(actor->GetHandle());
	if (iter == DifficultyMap.end())
	{
		return;
	}
	// The belief that put the guard on the wrong line just failed in the
	// world — drain it proportionally, harder than any passive decay. 
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

	// Disconfirmation fallback only: the primary drain fires from HandleBlock's
	// strip at the prehit, which clears IsBlocking before this hit event runs.
	if (wasBlocking && guardDir != DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(attackDir)))
	{
		const Directions failedLine = DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(guardDir));
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
	// Lose-shift: a transition that just got blocked is one the target is less
	// likely to try again from there.
	auto& Chain = Iter->second;
	if (Chain.lastChainFrom >= 0 && Chain.lastChainTo == static_cast<int>(attackedDir))
	{
		int& Edge = Chain.chainEdges[Chain.lastChainFrom][Chain.lastChainTo];
		Edge = std::max(0, Edge - 2 * ChainEdgeGain);
		Chain.lastChainFrom = -1;
	}


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
	// find, not at(): see CalcActionTimer.
	auto DiffIter = DifficultyMap.find(actor->GetHandle());
	if (DiffIter != DifficultyMap.end())
	{
		base += AISettings::FatigueUpdateSeconds * Fatigue(DiffIter->second.fightSeconds);
	}
	// Floored so direction switching can't flicker.
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
	// left combat mid-call, and skipping its fatigue is correct.
	auto DiffIter = DifficultyMap.find(actor->GetHandle());
	if (DiffIter != DifficultyMap.end())
	{
		base += AISettings::FatigueActionSeconds * Fatigue(DiffIter->second.fightSeconds);
	}

	base = std::max(base, LowestTime);

	return base;
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
	// Cooldown writes are deferred past the queue lock: taking DifficultyMapMtx
	// under ActionQueueMtx is an ABBA cycle with every AddAction caller.
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
			// Priority stays until the action runs. A step that can't start drops it, so a
			// stuck swing can't hold off the defence.
			if (ActionQueueIter->second.toDo == Actions::Attack)
			{
				// Wait for the hand to reach the attack line: swinging first parks the
				// switch and the attack comes out from the old direction.
				if (ActionQueueIter->second.waitedForDir < MaxAttackDirWait &&
					HasPendingDirectionSwitch(actor))
				{
					ActionQueueIter->second.waitedForDir += delta;
				}
				else if (TryAttack(actor))
				{
					ActionQueueIter->second.toDo = Actions::None;
				}
				else
				{
					ActionQueueIter->second.priority = 0;
				}

			}
			else if (ActionQueueIter->second.toDo == Actions::Followup)
			{
				// Chains off the power attack that queued it, so it waits for the chain window.
				if (DirectionHandler::GetSingleton()->CanSwitch(actor) && TryAttack(actor))
				{
					ActionQueueIter->second.toDo = Actions::None;
				}
				else
				{
					ActionQueueIter->second.priority = 0;
				}
			}
			else if (ActionQueueIter->second.toDo == Actions::Block)
			{
				bool staggering = actor->AsActorState()->actorState2.staggered;
				if (staggering || actor->IsAttacking())
				{
					// A blockStart mid-swing cuts it short of its stop event: retry once recovered.
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
					ActionQueueIter->second.timeLeft = TransitionSettleSeconds;
					// The riposte keeps its priority into the swing.
					ActionQueueIter->second.priority = 1;
					if (Settings::VerboseLogging)
					{
						logger::info("[riposte] {} drops its guard to riposte", actor->GetName());
					}
				}
				else
				{
					if (Settings::VerboseLogging)
					{
						logger::info("[riposte] {} drops its riposte: staggered", actor->GetName());
					}
					ActionQueueIter->second.toDo = Actions::None;
				}
			}
			else if (ActionQueueIter->second.toDo == Actions::UnblockStartFeint)
			{
				actor->AsActorState()->actorState2.wantBlocking = 0;
				actor->NotifyAnimationGraph("blockStop");
				ActionQueueIter->second.toDo = Actions::StartFeint;
				ActionQueueIter->second.timeLeft = 0.1f;
				ActionQueueIter->second.priority = 1;
			}
			else if (ActionQueueIter->second.toDo == Actions::Bash)
			{
				// dont start if we cannot bash
				if (!actor->IsAttacking() && !InTransition(actor) && AttackHandler::GetSingleton()->CanAttack(actor))
				{
					DeferredBashCooldowns.push_back(ActionQueueIter->first);
					if (AttackHandler::GetSingleton()->DoBash(actor))
					{
						ActionQueueIter->second.toDo = Actions::ReleaseBash;
						ActionQueueIter->second.timeLeft = LowestTime;
						ActionQueueIter->second.waitedForDir = 0.f;
					}
					else
					{
						ActionQueueIter->second.toDo = Actions::None;
					}
				}
				else
				{
					ActionQueueIter->second.toDo = Actions::None;
				}
			}
			else if (ActionQueueIter->second.toDo == Actions::ReleaseBash)
			{
				// A release sent into the wind-up is dropped and the bash held for good: wait for the graph.
				bool Bashing = false;
				actor->GetGraphVariableBool("IsBashing", Bashing);
				if (Bashing || ActionQueueIter->second.waitedForDir >= BashReleaseWaitMax)
				{
					actor->NotifyAnimationGraph("bashRelease");
					ActionQueueIter->second.toDo = Actions::ResetState;
					ActionQueueIter->second.timeLeft = 0.1f;
				}
				else if (!actor->IsAttacking())
				{
					ActionQueueIter->second.toDo = Actions::None;
				}
				else
				{
					ActionQueueIter->second.waitedForDir += DashPollSeconds;
					ActionQueueIter->second.timeLeft = DashPollSeconds;
				}
			}
			// A short hold after the bash release that AddAction won't replace.
			else if (ActionQueueIter->second.toDo == Actions::ResetState)
			{
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
				// NPCs feint only out of a power attack, so a light is always real:
				// the one read a player can rely on.
				if (TryPowerAttack(actor))
				{
					ActionQueueIter->second.toDo = Actions::EndFeint;
					ActionQueueIter->second.timeLeft = DifficultySettings::FeintWindowTime - (ActionQueueIter->second.baseTimer * .5f);
					ActionQueueIter->second.priority = 2;
				}
				else
				{
					ActionQueueIter->second.priority = 0;
				}


			}
			else if (ActionQueueIter->second.toDo == Actions::EndFeint)
			{
				// Refused (window missed or no stamina): the original swing goes through.
				const bool Feinted = !actor->IsBlocking() && AttackHandler::GetSingleton()->HandleFeint(actor);
				if (!Feinted && Settings::VerboseLogging && !actor->IsBlocking())
				{
					logger::info("[feint] {} planned feint didn't fire (window open {})",
						actor->GetName(), AttackHandler::GetSingleton()->InFeintWindow(actor));
				}
				RE::Actor* FeintTarget = Feinted ? GetCombatTarget(actor) : nullptr;
				// Already countered: hold the line the counter is aimed at and let the
				// defence block or masterstrike it.
				if (FeintTarget && IsIncomingSwing(FeintTarget, actor))
				{
					if (Settings::VerboseLogging)
					{
						logger::info("[feint] {} holds its line: {} is already countering", actor->GetName(), FeintTarget->GetName());
					}
					ActionQueueIter->second.toDo = Actions::None;
				}
				else if (Feinted)
				{
					// A feint only cancels now; the NPC's follow-up still comes off the other side.
					AttackHandler::GetSingleton()->HandleFeintChangeDirection(actor);
					ActionQueueIter->second.toDo = Actions::FeintFollowup;
					ActionQueueIter->second.timeLeft = 0.18f;
					ActionQueueIter->second.priority = 2;
				}
				else
				{
					// we tried but they attacked anyway
					ActionQueueIter->second.toDo = Actions::None;
				}

			}
			else if (ActionQueueIter->second.toDo == Actions::FeintFollowup)
			{
				// Countered since the feint: leave it to the defence.
				RE::Actor* FeintTarget = GetCombatTarget(actor);
				if (FeintTarget && IsIncomingSwing(FeintTarget, actor))
				{
					if (Settings::VerboseLogging)
					{
						logger::info("[feint] {} drops its follow-up: {} is countering", actor->GetName(), FeintTarget->GetName());
					}
					ActionQueueIter->second.toDo = Actions::None;
				}
				else
				{
					// The Attack branch swings it once the line change has landed. Unranked,
					// so a counter during that wait can still be answered.
					ActionQueueIter->second.toDo = Actions::Attack;
					ActionQueueIter->second.priority = 0;
				}
			}
			else if (ActionQueueIter->second.toDo == Actions::OpportunityAttack)
			{
				// Rechecked at fire: holds rank through own swing, drops the guard, swings from the current line.
				RE::Actor* Opening = GetCombatTarget(actor);
				if (!Opening || !Opening->AsActorState()->actorState2.staggered ||
					actor->AsActorState()->actorState2.staggered)
				{
					if (Settings::VerboseLogging)
					{
						logger::info("[opportunity] {} drops its swing: the opening closed", actor->GetName());
					}
					ActionQueueIter->second.toDo = Actions::None;
				}
				else if (actor->IsBlocking())
				{
					actor->AsActorState()->actorState2.wantBlocking = 0;
					actor->NotifyAnimationGraph("blockStop");
					ActionQueueIter->second.timeLeft = TransitionSettleSeconds;
				}
				else if (!actor->IsAttacking() || DirectionHandler::GetSingleton()->InAttackWindow(actor))
				{
					// One try: an opening doesn't wait for stamina or a lockout to clear.
					const bool Swung = TryAttack(actor);
					if (Settings::VerboseLogging)
					{
						logger::info("[opportunity] {} {} {}", actor->GetName(), Swung ? "swings at" : "couldn't swing at", Opening->GetName());
					}
					ActionQueueIter->second.toDo = Actions::None;
				}
			}
			else if (ActionQueueIter->second.toDo == Actions::DashAttack)
			{
				if (DodgeHandler::GetSingleton()->IsDodging(actor))
				{
					ActionQueueIter->second.timeLeft = DashPollSeconds;
				}
				else
				{
					RE::Actor* Target = GetCombatTarget(actor);
					const float Reach = actor->GetReach() + AttackLungeUnits;
					if (Target && !actor->IsAttacking() && !IsIncomingSwing(Target, actor) &&
						TorsoDistanceSq(actor, Target) < Reach * Reach)
					{
						ActionQueueIter->second.toDo = Actions::Attack;
						ActionQueueIter->second.timeLeft = TransitionSettleSeconds;
					}
					else
					{
						ActionQueueIter->second.toDo = Actions::None;
					}
				}
			}
			else if (ActionQueueIter->second.toDo == Actions::PowerAttack)
			{
				bool staggering = actor->AsActorState()->actorState2.staggered;
				// A masterstrike (priority 2) fires only onto their line before their hit frame.
				const bool Strike = ActionQueueIter->second.priority >= 2;
				RE::Actor* Target = Strike ? GetCombatTarget(actor) : nullptr;
				const bool StrikeOpen = !Strike || (Target &&
					DirectionHandler::GetSingleton()->HasBlockAngle(Target, actor) && IsIncomingSwing(Target, actor));
				// sit in queue until we can attack again, and until the guard
				// has reached the attack line — same rule as the light attack
				if (ActionQueueIter->second.waitedForDir < MaxAttackDirWait &&
					HasPendingDirectionSwitch(actor))
				{
					ActionQueueIter->second.waitedForDir += delta;
				}
				else if (StrikeOpen && !staggering && TryPowerAttack(actor))
				{
					ActionQueueIter->second.toDo = Actions::None;
				}
				else if (Strike)
				{
					// Only worth its window: dropped, not kept as a stray power.
					ActionQueueIter->second.toDo = Actions::None;
				}
				else
				{
					ActionQueueIter->second.priority = 0;
				}

			}
			else if (ActionQueueIter->second.toDo == Actions::Dodge)
			{
				// A dodge queued before something took this actor out of the
				// fight would fire it out of whatever pattern it is being held in.
				if (!actor->IsAttacking() && !actor->IsBlocking() && !IsAttackingDisabled(actor))
				{
					actor->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kNone;

					// Direction was picked at queue time (in RunActor). TriggerDodge
					// routes to whichever dodge system is active.
					bool Dashing = false;
					if (DodgeHandler::GetSingleton()->CanDodge(actor))
					{
						DodgeHandler::GetSingleton()->TriggerDodge(actor, ActionQueueIter->second.dodgeDir);
						DeferredDodgeCooldowns.push_back(ActionQueueIter->first);
						// Only the mod's own dodge reports when it lands.
						Dashing = ActionQueueIter->second.dodgeDir == DodgeDirection::Forward &&
							Settings::ActiveDodgeSystem == DodgeSystem::Custom;
					}
					// A straight dash in arrives swinging: the strike is due as the dodge lands.
					if (Dashing)
					{
						ActionQueueIter->second.toDo = Actions::DashAttack;
						ActionQueueIter->second.timeLeft = DashSwingDelay;
					}
					else
					{
						ActionQueueIter->second.toDo = Actions::None;
					}
				}
				else
				{
					ActionQueueIter->second.priority = 0;
				}

			}

			// once we have executed, the timeleft should be negative and this should be no action
			// this is what we use to determine if we are done with actions for this actor

			// do not erase every time for perf reasons

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
				iter->second.DodgeCooldown = DodgeCooldownSeconds -
					std::clamp(iter->second.cautionMod, 0.f, 1.f) * DodgeCooldownCautionCut;
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
			UpdateTimerIter->second -= delta;
		}
		UpdateTimerIter++;


	}
	AIHandlerDataMtx.lock();
	NumPlayerAttackers = NumActorsTargettingPlayer;
	AIHandlerDataMtx.unlock();
	UpdateTimerMtx.unlock();

	
}


