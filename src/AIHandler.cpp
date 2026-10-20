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
// How often a warhammer in a chain picks the opposite of its last swing type.
// simulates human mouse or contorller input to change guard
constexpr float GuardInputSeconds = 0.15f;
// How long a guard switch made against a swing counts as a commitment.
constexpr float GuardCommitSeconds = 0.6f;
// Mirrors DirectionHandler's TimeBetweenChanges todo keep it in a central constexpr file to avoid this horrible hack
constexpr float GuardTransitionSeconds = 0.133f;
// Decision to actually standing on the new line. Nothing shorter than this is
// worth chasing — the answer arrives after the target has already moved on.
constexpr float GuardCommitLatency = GuardInputSeconds + GuardTransitionSeconds;
constexpr float MaxAttackDirWait = GuardCommitLatency + 0.02f;
// A queued swing that still can't start this long after its timer ran out is dropped.
constexpr float StaleSwingSeconds = 1.f;
// The target's guard sits on the counter of this attack line (pre-covered; up or not).
static bool LineCovered(RE::Actor* target, Directions line)
{
	return DirectionHandler::FoldDirection(DirectionHandler::GetSingleton()->GetCurrentDirection(target)) ==
		DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(line));
}

// An enabled attack line the target's guard isn't covering, other than the one held; the
// held line when every other is covered.
static Directions OpenLineAgainst(RE::Actor* actor, RE::Actor* target)
{
	const Directions Held = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
	Directions Open[4];
	int Count = 0;
	const uint8_t Mask = DirectionHandler::EnabledDirections();
	for (Directions Line : { Directions::TR, Directions::TL, Directions::BL, Directions::BR })
	{
		if ((Mask & (1u << static_cast<int>(Line))) && Line != Held && !LineCovered(target, Line))
		{
			Open[Count++] = Line;
		}
	}
	return Count > 0 ? Open[static_cast<unsigned>(GetRand()) % Count] : Held;
}

// Binary entropy of a share: 0 when it is always or never, 1 bit at a coin flip.
static float Entropy2(float p)
{
	p = std::clamp(p, 0.f, 1.f);
	if (p <= 0.f || p >= 1.f)
	{
		return 0.f;
	}
	return -(p * std::log2(p) + (1.f - p) * std::log2(1.f - p));
}
// Hick's law on the kind axis: how unpredictable the opponent's swing kind is, from the
// power and feint shares this actor has watched. Pure lights that never feint: 0 bits;
// half powers and a third feints: ~1.9.
static float KindBits(float powerShare, float feintLight, float feintPower)
{
	const float Feint = feintLight * (1.f - powerShare) + feintPower * powerShare;
	return Entropy2(powerShare) + Entropy2(Feint);
}
#define KIND_BITS(diff) KindBits((diff).powerHabit, (diff).feintHabit[0], (diff).feintHabit[1])
// Hick's law on the action timer: a read picks among the enabled lines, log2(n+1) bits, 2
// bits on three lines, which the action timers are calibrated to, plus the kind bits; a
// press on a line the guard already covers is one bit plus the kind bits.
static float ReadBitsScale(float kindBits)
{
	int Lines = 0;
	for (uint8_t Mask = DirectionHandler::EnabledDirections(); Mask; Mask >>= 1)
	{
		Lines += Mask & 1;
	}
	return (std::log2(static_cast<float>(Lines + 1)) + kindBits) / 2.f;
}
constexpr float PressBitsScale = 0.5f;
// Recognition varies swing to swing: uniform within half the tier's timer either way, so a
// swing the read just beats is blocked most of the time, not every time. Floored where
// perception itself bottoms out, so the fast tail stays human.
constexpr float ProcessingFloor = 0.08f;
static float RollProcessing(float Base, std::mt19937& Rand)
{
	std::uniform_real_distribution<float> Spread(0.5f, 1.5f);
	return std::max(ProcessingFloor, Base * Spread(Rand));
}
// Share of NPC feint follow-ups thrown on the feinted line; one that always crosses is a read.
constexpr float FeintSameLineShare = 0.5f;
// ForceAttackStop leaves the attack through a 0.167 s blend (1hm_behavior #3544). An attackStart
// inside it is accepted and never plays: 4 of 6 follow-ups fired at 0.16 s vanished (10-12 log).
constexpr float FeintFollowupSettle = 0.18f;
// Charged openers: a light opener held on its line until the guard charge speeds it past a
// reaction. Patience waits, aggression doesn't, low stamina welcomes the rest.
constexpr float ChargeShareBase = 0.2f;
constexpr float ChargeSharePatience = 0.4f;
constexpr float ChargeShareAggression = 0.2f;
constexpr float ChargeShareLowStamina = 0.3f;
constexpr float ChargeShareMax = 0.8f;
constexpr float ChargeGoalMin = 0.4f;
// Past the full charge time the hold ends whatever the ratio (a block zeroed it).
constexpr float ChargeHoldMargin = 0.5f;
// The hold ends when the target gets this far outside the swing's reach.
constexpr float ChargeReachSlack = 1.3f;

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
// A back-dodge's travel t seconds after the decision. Speed 5 with the ease-out over 0.5 s
// measured 97-120 units end to end; the first frame goes to dispatch.
constexpr float EscapeTravelUnits = 110.f;
constexpr float EscapeDuration = 0.5f;
constexpr float EscapeStartup = 0.05f;
constexpr float EscapeMarginUnits = 20.f;
constexpr float DefaultLightWindup = 0.45f;
constexpr float DefaultPowerWindup = 0.85f;
static float EscapeTravel(float seconds)
{
	const float T = std::clamp((seconds - EscapeStartup) / EscapeDuration, 0.f, 1.f);
	// Integral of the (1 - t^2) speed profile, normalised to the full travel.
	return EscapeTravelUnits * 1.5f * (T - T * T * T / 3.f);
}
// Will a back-dodge started now be out of their reach when the swing lands? Their reach
// already carries the step-in and lunge; the windup is the one learned for the kind.
bool AIHandler::EscapeClearsSwing(RE::Actor* target, const AIDifficulty& diff, Directions line, bool power, float distSq) const
{
	const float Threat = TargetReachEstimate(target, diff, line, power) * diff.reachMisjudge;
	float Windup = power ? diff.powerWindupEstimate : diff.lightWindupEstimate;
	if (Windup <= 0.f)
	{
		Windup = power ? DefaultPowerWindup : DefaultLightWindup;
	}
	const float TimeToHit = std::max(0.f, Windup / std::max(0.5f, diff.swingSpeed) - std::max(0.f, diff.swingElapsed));
	return std::sqrt(distSq) + EscapeTravel(TimeToHit) > Threat + EscapeMarginUnits;
}
// Distance a backpedalling target opens during the windup: its travel over ~0.34s.
constexpr float BackpedalLeadUnits = 40.f;
// Slack past the estimated target reach before a swing is worth defending. Never
// below 1: a wrong "out" takes a hit, a wrong "in" only wastes a block.
constexpr float DefendReachBase = 1.0f;
constexpr float DefendReachCautionScale = 0.2f;
// The reflex block's reach gate, over that threat. Measured 2026-10-01: no landed swing started past 0.82x.
constexpr float ReflexThreatMargin = 1.25f;
// A line change in reach is taken as an attack telegraph with this chance, plus their guard
// charge (a loaded line) and this actor's caution. Measured 10-12: 73% of the player's switches
// had no swing behind them, so predicting on every one moved the guard three times per swing.
constexpr float SwitchThreatBase = 0.35f;
constexpr float SwitchThreatChargeScale = 0.4f;
constexpr float SwitchThreatCautionScale = 0.25f;
constexpr float SwitchThreatFloor = 0.1f;
// Swinging into the line their switch left, from where the actor stands: by aggression, only
// when the actor reaches them. Carved out of the cover roll so a switch gets one answer.
constexpr float PunishBase = 0.3f;
constexpr float PunishAggressionScale = 0.4f;
constexpr float PunishMax = 0.8f;

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
// Edge points a certain, fully learned habit seeds at Legendary: four observed
// transitions' worth, never certain. The player is a new opponent to every NPC;
// the seed stands for experience against people, not knowledge of this one.
constexpr float PlayerHabitSeedMax = 80.f;
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
// Floor on (tier + aggression * 1.5): a Turtle at VeryEasy sits near zero and
// would never attack at all.
constexpr float InitiativeRateBaseFloor = 0.5f;
// Attempts per second to start an exchange from neutral (a swing, or a press into the guard),
// at full stamina outside a combo. Counters don't use it. Rate and offset are ini
// (InitiativeRatePerTier, InitiativeRateTierOffset).
static float BaseInitiativeRate(int tier, float aggression)
{
	return std::max(InitiativeRateBaseFloor, tier + AISettings::InitiativeRateTierOffset + aggression * 1.5f) *
		AISettings::InitiativeRatePerTier;
}
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
// A neutral pre-block stays up this long with no swing to answer; an answered swing's
// block stays up this long after it, so a chain doesn't drop and re-raise it.
constexpr float PreBlockHoldSeconds = 1.0f;
constexpr float AnsweredHoldSeconds = 0.3f;
// A block pressed with the read holds until the chained swing it was pressed for is due.
constexpr float FlickHoldSeconds = 0.7f;
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
constexpr float SpacingUnknownWeight = 0.35f;    // no read on them -> mixup range is unsafe
constexpr float SpacingKnownSum = 40.f;          // expectation chance that counts as a read
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
// A line change this close before a swing start is the attack's own switch.
constexpr float SwitchWithSwingSeconds = 0.06f;

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
	{ "Aggressor",   +1.0f, -0.8f, -0.5f, -0.5f, +0.6f,  +0.2f }, // charges in, swings constantly, rarely dodges
	{ "Turtle",      -0.3f, +1.0f, +0.5f, +0.8f,  0.0f,  0.0f }, // holds block, waits, hard to break
	{ "Trickster",   +0.6f, +0.3f, +0.7f, +0.2f, -0.1f, +1.0f }, // feints constantly, mind games heavy
	{ "Counter",     +0.2f, +0.8f, +1.0f, +0.5f, +0.4f, +0.4f }, // patient, waits for masterstrike opportunity
	{ "Brute",       +0.8f, -0.3f, -0.4f, -0.2f, +1.0f, -0.5f }, // power-attack-heavy heavy hitter
	{ "Evasive",      0.0f,  0.0f, -0.2f, +1.0f,  0.2f, +0.5f }, // dodges everything, mobile fighter
	{ "Balanced",     0.3f,  0.0f,  0.0f,  0.0f,  0.3f,  0.3f }, // no lean, every gate at its base value
	{ "Balanced",     0.3f,  0.0f,  0.0f,  0.0f,  0.3f,  0.3f }, // 2 balanced archetypes because they should be the most common
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

	// time to register a stimulus before acting on it
	DifficultyProcessingTimer[Difficulty::VeryEasy] = AISettings::VeryEasyProcessingTimer;
	DifficultyProcessingTimer[Difficulty::Easy] = AISettings::EasyProcessingTimer;
	DifficultyProcessingTimer[Difficulty::Normal] = AISettings::NormalProcessingTimer;
	DifficultyProcessingTimer[Difficulty::Hard] = AISettings::HardProcessingTimer;
	DifficultyProcessingTimer[Difficulty::VeryHard] = AISettings::VeryHardProcessingTimer;
	DifficultyProcessingTimer[Difficulty::Legendary] = AISettings::LegendaryProcessingTimer;
	for (auto& iter : DifficultyProcessingTimer)
	{
		iter.second = std::max(iter.second * AISettings::AIDifficultyMult, 0.02f);
	}


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
	const bool Riposte = toDo == Actions::UnblockRiposte || toDo == Actions::UnblockPowerRiposte ||
		toDo == Actions::UnblockStartFeint;
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
		DiffIter->second.reflexSpent = false;
		DiffIter->second.switchCameWithSwing = false;
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

// Reflex-block ready: hands free, wind to hold, and the target's swing can reach. Without
// that last part the guard went up at any range and held the actor off instead of closing.
// Any posture: a swing is answered whether the actor was defending or lining up its own.
float AIHandler::SwitchCoverWeight(RE::Actor* target, const AIDifficulty& diff, float distSq, Directions line, bool comboLive)
{
	if (diff.swingElapsed >= 0.f || comboLive)
	{
		return 1.f;
	}
	const float Slack = std::clamp(DefendReachBase + diff.cautionMod * DefendReachCautionScale, 1.f, 1.6f);
	const float Reach = TargetReachEstimate(target, diff, line, false) * diff.reachMisjudge * Slack * ReflexThreatMargin;
	if (distSq >= Reach * Reach)
	{
		return 0.f;
	}
	return std::clamp(SwitchThreatBase +
		SwitchThreatChargeScale * DirectionHandler::GetSingleton()->GetGuardChargeRatio(target) +
		SwitchThreatCautionScale * diff.cautionMod, SwitchThreatFloor, 1.f);
}

bool AIHandler::SwitchLooksLikeAttack(RE::Actor* actor, RE::Actor* target, AIDifficulty& diff, float distSq, Directions line, bool comboLive)
{
	UNUSED(actor);
	std::uniform_real_distribution<float> Roll(0.f, 1.f);
	return Roll(diff.npcRand) < SwitchCoverWeight(target, diff, distSq, line, comboLive);
}

AIHandler::SwitchAnswer AIHandler::AnswerSwitch(RE::Actor* actor, RE::Actor* target, AIDifficulty& diff, float distSq, Directions was, Directions seen, bool comboLive)
{
	// Their swing or combo: nothing to punish, the guard answers.
	if (diff.swingElapsed >= 0.f || comboLive)
	{
		return SwitchAnswer::Cover;
	}
	const float Cover = SwitchCoverWeight(target, diff, distSq, seen, comboLive);
	float Attack = 0.f;
	// Own guard in transit: the swing would come out from a line not judged here.
	if (!actor->IsAttacking() && GetQueuedAction(actor) == Actions::None && !HasPendingDirectionSwitch(actor))
	{
		auto* Dir = DirectionHandler::GetSingleton();
		const Directions Held = Dir->GetCurrentDirection(actor);
		// The line they left: covered by the old guard, open to the new one.
		const bool WasCovered = DirectionHandler::FoldDirection(was) ==
			DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(Held));
		const float Reach = GateReach(actor, Held, Dir->GetComboStep(actor) > 0, false);
		if (WasCovered && !LineCovered(target, Held) && distSq < Reach * Reach &&
			AttackHandler::GetSingleton()->CanInitiateAttack(actor))
		{
			Attack = std::clamp(PunishBase + PunishAggressionScale * diff.aggressionMod, 0.f, PunishMax);
		}
	}
	// Ignore is the cover roll's remainder, so with no swing possible this is that roll unchanged.
	const float Ignore = 1.f - Cover;
	std::uniform_real_distribution<float> Roll(0.f, Attack + Cover + Ignore);
	const float R = Roll(diff.npcRand);
	return R < Attack ? SwitchAnswer::Attack : R < Attack + Cover ? SwitchAnswer::Cover : SwitchAnswer::Ignore;
}

bool AIHandler::IsPreparedToBlock(RE::Actor* actor, RE::Actor* target, const AIDifficulty& diff) const
{
	const float DistSQ = TorsoDistanceSq(actor, target);
	if (actor->IsBlocking() || actor->IsAttacking())
	{
		return false;
	}
	const float Slack = std::clamp(DefendReachBase + diff.cautionMod * DefendReachCautionScale, 1.f, 1.6f);
	const float Reach = TargetReachEstimate(target, diff, DirectionHandler::GetSingleton()->GetCurrentDirection(target),
		IsPowerAttacking(target)) * diff.reachMisjudge * Slack * ReflexThreatMargin;
	if (DistSQ >= Reach * Reach)
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
			logger::info("[reach] {} learns {}'s {} from line {} reaches {:.0f} (sample {:.0f})", Who(actor),
				Who(TargetActor), diff.swingWasPower ? "power" : "light", diff.swingLine, Learned, Sample);
		}
		diff.swingStartGap = -1.f;
	}
}

bool AIHandler::HasSwingQueued(RE::Actor* actor)
{
	const Actions Queued = GetQueuedAction(actor);
	return Queued == Actions::Attack || Queued == Actions::PowerAttack || Queued == Actions::Followup ||
		Queued == Actions::FeintFollowup || Queued == Actions::OpportunityAttack || Queued == Actions::DashAttack ||
		Queued == Actions::StartFeint || Queued == Actions::EndFeint ||
		Queued == Actions::UnblockRiposte || Queued == Actions::UnblockPowerRiposte ||
		Queued == Actions::UnblockStartFeint;
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
	// Pressing is having the wind and the right to attack, not a posture flag:
	// the standoff below is for the winded and the locked out.
	const bool Outreached = diff.CurrentWeaponLengthSQ < diff.targetReachSQ * diff.reachMisjudge * diff.reachMisjudge;
	const bool Pressing = ownStaminaRatio > AttackStaminaOffLine &&
		AttackHandler::GetSingleton()->CanAttack(actor);
	if (Pressing || Outreached)
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
	// Unknown opponent: with no expectation to fall back on, a switch-and-attack is a
	// coin flip, so the band moves out until the habits are learned. Belief accrues
	// 1/condMod per tier, scaled back to one footing; aggression ignores the unknown,
	// caution can at most fully respect it.
	{
		const int Sum = diff.lastCascadeWeights[0] + diff.lastCascadeWeights[1] +
			diff.lastCascadeWeights[2] + diff.lastCascadeWeights[3];
		const int CondMod = std::clamp(static_cast<int>(diff.difficulty), 1, 3);
		const float Known = std::clamp(static_cast<float>(Sum * CondMod) / (3.f * SpacingKnownSum), 0.f, 1.f);
		const float Respect = std::clamp(1.f + diff.cautionMod - diff.aggressionMod, 0.f, 1.f);
		spacing += (1.f - Known) * SpacingUnknownWeight * Respect;
	}

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
// How hard a tier acts on its read: 1 picks lines in proportion to their weight, the way a
// novice matches the odds; higher commits to the favourite, the way an expert maximizes.
static float ReadDecisiveness(AIHandler::Difficulty tier)
{
	return 1.f + 0.5f * static_cast<float>(std::max(0, static_cast<int>(tier) - static_cast<int>(AIHandler::Difficulty::VeryEasy)));
}

// The expectation roll, shared by the reflex and the tick: whether it fires is the raw weight
// sum; which line it picks is the weights raised to the tier's decisiveness.
static bool RollExpectation(const int weights[4], float decisiveness, Directions& outPick)
{
	const int Sum = weights[0] + weights[1] + weights[2] + weights[3];
	if (Sum <= 0 || static_cast<int>(mt_rand() % 100) >= Sum)
	{
		return false;
	}
	float Sharp[4];
	float Total = 0.f;
	for (int i = 0; i < 4; ++i)
	{
		Sharp[i] = weights[i] > 0 ? std::pow(static_cast<float>(weights[i]), decisiveness) : 0.f;
		Total += Sharp[i];
	}
	float Draw = static_cast<float>(mt_rand() % 10000u) / 10000.f * Total;
	for (int i = 0; i < 4; ++i)
	{
		Draw -= Sharp[i];
		if (Draw < 0.f && Sharp[i] > 0.f)
		{
			outPick = static_cast<Directions>(i);
			return true;
		}
	}
	for (int i = 3; i >= 0; --i)
	{
		if (Sharp[i] > 0.f)
		{
			outPick = static_cast<Directions>(i);
			return true;
		}
	}
	return false;
}

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
			// Held here so the order stays DifficultyMapMtx -> ActionQueueMtx.
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
			diff.statusLogTimer = 2.f;
			const float Stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) /
				std::max(1.f, actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina));
			logger::info("[tick] {} {:08X} ({} tier {}) -> {} dist {:.0f} own reach {:.0f} target reach {:.0f} judge {} stamina {:.2f} defending {} canAct {} state {} spacing {:.2f} initiative rate {:.2f}/s aggression shift {:+.2f}",
				actor->GetName(), actor->GetFormID(), diff.archetype ? diff.archetype : "?", static_cast<int>(diff.difficulty), Who(target), std::sqrt(TargetDistSQ),
				actor->GetReach() + AttackLungeUnits, (target->GetReach() + AttackLungeUnits) * diff.reachMisjudge,
				TargetDistSQ < JudgeRadius * JudgeRadius,
				Stamina, diff.defending, CanAct(actor), static_cast<int>(actor->AsActorState()->GetAttackState()), diff.spacingTarget,
				BaseInitiativeRate(static_cast<int>(diff.difficulty), diff.aggressionMod),
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
			diff.lastSeenSwingLine = -1;
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
		// A weapon swap changes the windup and reach; the old estimates would mislead.
		RE::TESForm* const TargetWeapon = target->GetEquippedObject(false);
		if (diff.observedTargetWeapon != TargetWeapon)
		{
			if (Settings::VerboseLogging && (diff.powerWindupEstimate > 0.f || diff.lightWindupEstimate > 0.f))
			{
				logger::info("[dmt] {} clears windups and reach: {} swapped weapons", Who(actor), Who(target));
			}
			diff.observedTargetWeapon = TargetWeapon;
			diff.powerWindupEstimate = 0.f;
			diff.powerWindupDist = 0.f;
			diff.lightWindupEstimate = 0.f;
			diff.lightWindupDist = 0.f;
			for (auto& Row : diff.targetReachLearned)
			{
				std::fill(std::begin(Row), std::end(Row), 0.f);
			}
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
		diff.holdBlockLeft = std::max(0.f, diff.holdBlockLeft - delta);
		// Pacing for the player: only time spent fighting them counts.
		if (target->IsPlayerRef() && TargetDistSQ < JudgeRadius * JudgeRadius)
		{
			diff.fightSeconds += delta;
		}
		// The target's combo wears off: the next swing is an opener again.
		if (diff.lastSeenSwingLine >= 0)
		{
			diff.sinceLastSwing += delta;
			if (diff.sinceLastSwing >= DifficultySettings::ComboResetTimer)
			{
				if (Settings::VerboseLogging)
				{
					logger::info("[chain] {} drops {}'s line {} as the node: {:.1f}s without a swing", Who(actor), Who(target),
						diff.lastSeenSwingLine, diff.sinceLastSwing);
				}
				diff.lastSeenSwingLine = -1;
			}
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
		if (diff.guardMovedAgo >= 0.f)
		{
			diff.guardMovedAgo += delta;
			if (diff.guardMovedAgo > GuardCommitSeconds)
			{
				diff.guardMovedAgo = -1.f;
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
		// The guard they left, for the switch answer below.
		const Directions Was = diff.lastObservedDirection;
		bool lineChanged = false;
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
			lineChanged = true;
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
			// A switch that arrives with the swing is the attack, not a show: it must not
			// condition the line left behind.
			if (targetAttackState != RE::ATTACK_STATE_ENUM::kBash && diff.observedSwitchCount > 0 &&
				diff.timeSinceLineChange <= SwitchWithSwingSeconds)
			{
				diff.observedSwitchCount--;
				diff.switchCameWithSwing = true;
			}

			// Arm the swing lifecycle: timed from here to the hit for the windup estimates.
			diff.swingElapsed = 0.f;
			diff.sinceLastSwing = 0.f;
			diff.swingCorrectionLogged = false;
			diff.answerKind = diff.reflexPickValid ? "expect-pre" : "none";
			diff.answerLater = nullptr;
			diff.answerAt = -1.f;
			diff.pressAt = -1.f;
			diff.swingContact = false;
			diff.swingChained = DirectionHandler::GetSingleton()->GetComboStep(target) > 0;
			diff.swingWasPower = IsPowerAttacking(target);
			// For the reach learner: where their swing started, and where I stood.
			diff.swingStartGap = std::sqrt(TargetDistSQ);
			diff.swingStartOwnPos = actor->GetPosition();
			diff.swingLine = static_cast<int>(seen) < 4 ? static_cast<int>(seen) : -1;
			// The clip's playback rate (AV x record speed, two-handers x1.5): a windup learned
			// at "base speed" is then in clip time and survives their weapon swaps and a retune.
			diff.swingSpeed = AttackHandler::EffectiveAttackSpeed(target);
			// Chain graph: walk the edge from the last swing line seen to this one. Keyed by
			// what was seen, not the combo ring, so the sequence read survives blocks,
			// finishers and pauses; the first swing seen walks the opener row.
			diff.lastChainFrom = -1;
			if (targetAttackState != RE::ATTACK_STATE_ENUM::kBash && static_cast<int>(seen) < 4)
			{
				const int From = diff.lastSeenSwingLine >= 0 ? diff.lastSeenSwingLine : OpenerRow;
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
			// Chain escape: a chained swing is a mixup, so a cautious fighter leaves instead of
			// guessing the line. Decided here, on the frame the swing is seen: from the tick a
			// dodge starts ~190 ms late and no longer clears the hitbox. Caution is the rate.
			if (Settings::ActiveDodgeSystem != DodgeSystem::None &&
				targetAttackState != RE::ATTACK_STATE_ENUM::kBash && !actor->IsAttacking() &&
				diff.DodgeCooldown <= 0.f && IsSwingingAt(target, actor) &&
				DirectionHandler::GetSingleton()->GetComboStep(target) > 0)
			{
				const float Threat = TargetReachEstimate(target, diff, seen, diff.swingWasPower) * diff.reachMisjudge;
				const float Stamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina) /
					std::max(1.f, actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina));
				// Decided at their switch when one was seen (the flick pressed the block instead when
				// it chose to stay); undecided swings, a same-line chain, roll here.
				const bool Chosen = diff.chainAnswer == 2;
				const bool Go = diff.chainAnswer != 0 ? Chosen :
					static_cast<float>(mt_rand() % 10000u) / 10000.f < std::clamp(diff.cautionMod, 0.f, 1.f);
				diff.chainAnswer = 0;
				// Only when the dodge is out of reach by the hit frame: inside a sword's reach it
				// never is, and a dodge that gets hit has dropped the guard for nothing.
				const bool Clears = EscapeClearsSwing(target, diff, seen, diff.swingWasPower, TargetDistSQ);
				if (TargetDistSQ < Threat * Threat && Stamina > 0.3f && Go && Clears)
				{
					// The dodge can't fire through a raised guard; drop it now, the queue takes the dodge
					// the frame the graph lets go. Priority 2 so the defend branch's block can't displace it.
					const bool GuardWasUp = actor->IsBlocking();
					if (GuardWasUp)
					{
						actor->AsActorState()->actorState2.wantBlocking = 0;
						actor->NotifyAnimationGraph("blockStop");
					}
					AddAction(actor, Actions::Dodge, true, 2, DodgeDirection::Backward);
					// Measured: through the action timer the dodge started ~200-240 ms after this frame
					// and had moved ~50 units by a light's hit frame, so 4 of 5 escapes were still hit.
					if (GetQueuedAction(actor) == Actions::Dodge)
					{
						ExpediteQueuedAction(actor);
					}
					if (Settings::VerboseLogging)
					{
						logger::info("[escape] {} leaves {}'s chained swing from {} at {:.0f} (caution {:.2f}, guard was {})",
							Who(actor), Who(target), static_cast<int>(seen), std::sqrt(TargetDistSQ), diff.cautionMod, GuardWasUp);
					}
				}
				else if (Chosen && !actor->IsBlocking())
				{
					// The escape decided at their switch displaced the block press; it can't go, so
					// the block is pressed now instead of standing on nothing.
					AddAction(actor, Actions::Block, true);
					diff.holdBlockLeft = std::max(diff.holdBlockLeft, FlickHoldSeconds);
					if (Settings::VerboseLogging)
					{
						logger::info("[escape] {} can't clear {}'s chained swing from {} at {:.0f}, blocks instead",
							Who(actor), Who(target), static_cast<int>(seen), std::sqrt(TargetDistSQ));
					}
				}
			}
			if (Settings::VerboseLogging)
			{
				// Why a swing went unanswered: out of the threat band, or the
				// line too fresh for the acquisition gate.
				const float Slack = std::clamp(DefendReachBase + diff.cautionMod * DefendReachCautionScale, 1.f, 1.6f);
				const float Threat = TargetReachEstimate(target, diff, seen, diff.swingWasPower) * diff.reachMisjudge * Slack;
				logger::info("[dmt] {} {:08X} sees {} swing from {} at {:.0f} (threat {:.0f}), line seen {:.0f}ms ago, window {:.0f}ms",
					actor->GetName(), actor->GetFormID(), Who(target), static_cast<int>(seen), std::sqrt(TargetDistSQ), Threat,
					diff.timeSinceLineChange * 1000.f, CommitWindow(diff) * 1000.f);
			}

			// Reflex, once per swing: chamber it or block it, paying only the action timer.
			// The line is the read's business; the guard comes up regardless.
			const bool Prepared = IsPreparedToBlock(actor, target, diff) && IsBlockableSwing(target);
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
							Who(actor), StrikeDelay, diff.swingWasPower ? "power" : "light", Windup, FeintHabit);
					}
				}
			}
			if (Prepared && !Chambered)
			{
				// A one-bit reaction: register the swing, then half the action timer (the guard
				// already covers the line, so there is no line to choose). AddAction floors it at
				// the action timer. Against an expected power the press waits out the learned windup.
				const bool ExpectPower = RollPowerParryRead(diff.difficulty) ?
					diff.swingWasPower :
					static_cast<float>(mt_rand() % 10000u) < diff.powerHabit * 10000.f;
				// The press chooses now / later / hold: their kind mixup is its extra bits.
				float TargetDelay = RollProcessing(CalcProcessingTimer(actor), diff.npcRand) +
					CalcActionTimer(actor) * (PressBitsScale + KIND_BITS(diff) * 0.5f);
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
				diff.pressAt = std::max(CalcActionTimer(actor), TargetDelay);
				if (Settings::VerboseLogging)
				{
					logger::info("[dmt] {} {:08X} reflex block, delay {:.0f}ms (expects power {}, swing is power {})",
						actor->GetName(), actor->GetFormID(), std::max(TargetDelay, 0.f) * 1000.f, ExpectPower, diff.swingWasPower);
				}
			}
			// The read, decided on the frame that sees the swing: the guard is off their line and
			// no expectation is already carrying it there. Priced as a human reaction, recognition
			// plus the action timer for the choice of line, plus the fixation when the guard
			// already moved this swing (a wrong prediction). A bash is spacing's problem.
			if (targetAttackState != RE::ATTACK_STATE_ENUM::kBash && static_cast<int>(seen) < 4 &&
				TargetDistSQ < JudgeRadius * JudgeRadius && !actor->IsAttacking() && IsSwingingAt(target, actor))
			{
				const Directions Counter = DirectionHandler::GetCounterDirection(seen);
				const bool OnAngle = DirectionHandler::GetSingleton()->HasBlockAngle(target, actor);
				// The guard already covers the swing: a prediction still moving it elsewhere was wrong,
				// and the swing in flight is the proof. Hold the line instead.
				if (OnAngle && HasPendingDirectionSwitch(actor) && !IsSwitchUnderway(actor, Counter))
				{
					CancelSwitchUnderway(actor);
					diff.answerKind = "hold-cancel";
					diff.answerAt = 0.f;
					if (Settings::VerboseLogging)
					{
						logger::info("[dmt] {} holds its line against {}'s swing from {}: the pending switch was wrong", Who(actor), Who(target), static_cast<int>(seen));
					}
				}
				else if (!OnAngle && !IsSwitchUnderway(actor, Counter))
				{
					const Directions Covered = DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(
						DirectionHandler::GetSingleton()->GetCurrentDirection(actor)));
					const float Fixation = static_cast<float>(diff.directionChangeChance[Covered]) /
						static_cast<float>(BeliefBudget) * AISettings::ConditionedFixationSeconds;
					const float Commit = diff.guardMovedAgo >= 0.f ? Fixation : 0.f;
					const float Input = RollProcessing(CalcProcessingTimer(actor), diff.npcRand) +
						CalcActionTimer(actor) * ReadBitsScale(KIND_BITS(diff)) + Commit;
					QueueDirectionSwitch(actor, Counter, true, Input);
					diff.guardMovedAgo = 0.f;
					diff.swingCorrectionLogged = true;
					diff.answerKind = "read";
					diff.answerAt = Input + GuardTransitionSeconds;
					if (Settings::VerboseLogging)
					{
						logger::info("[dmt] {} reads {}'s swing from {}: guard counts in {:.0f}ms (commit cost {:.0f}ms)",
							Who(actor), Who(target), static_cast<int>(seen), (Input + GuardTransitionSeconds) * 1000.f, Commit * 1000.f);
					}
				}
				else if (OnAngle)
				{
					// Already on their line: nothing to decide, the press carries it.
					diff.answerKind = "hold";
					diff.answerAt = 0.f;
				}
				else
				{
					// A switch to the counter is already in flight from an earlier decision.
					diff.answerKind = "underway";
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
				// Exchange record for a swing that never connected.
				if (!diff.swingContact && target->IsPlayerRef())
				{
					LogExchangeLine(target, actor, &diff, Feinted ? "feinted" : "whiff", 0.f);
				}
			}
			// The chain node moves on only once the swing is over: while it is in flight the
			// row keyed by the previous swing is the one that predicts it.
			if (diff.swingLine >= 0 && diff.lastObservedAttackState != static_cast<int>(RE::ATTACK_STATE_ENUM::kBash))
			{
				diff.lastSeenSwingLine = diff.swingLine;
			}
			// swingStartGap stays armed: the hit can land after this state change.
			diff.swingElapsed = -1.f;
			// A chain answer decided for a swing that has now passed is spent.
			diff.chainAnswer = 0;
		}
		diff.lastObservedAttackState = static_cast<int>(targetAttackState);

		// Reflex expectation: the line changed, so a defending guard goes to the expected
		// counter now, paying only the input. The tick keeps this pick instead of rolling
		// again, and its read still comes due at its own time. The move is a commitment.
		Directions Pick = Directions::TR;
		// Gated on the actor's own swing, not the defend flag: a switch is the stimulus
		// whether the actor was defending, closing or lining up a poke.
		const bool ComboLive = lineChanged && DirectionHandler::GetSingleton()->GetComboStep(target) > 0;
		// One answer per switch: swing into the line they left, move the guard, or nothing.
		const SwitchAnswer Answer = lineChanged ? AnswerSwitch(actor, target, diff, TargetDistSQ, Was, seen, ComboLive) : SwitchAnswer::Ignore;
		if (Answer == SwitchAnswer::Attack)
		{
			// See-and-go from the held line: one stimulus, one choice, so press-sized bits. Priority
			// 0, so a forced block from their swing start replaces it. From a raised guard it takes
			// the riposte's path (blockStop, settle, swing), which keeps rank into the swing.
			AddAction(actor, actor->IsBlocking() ? Actions::UnblockRiposte : Actions::Attack, false, 0, DodgeDirection::Backward,
				RollProcessing(CalcProcessingTimer(actor), diff.npcRand) + CalcActionTimer(actor) * PressBitsScale);
			if (Settings::VerboseLogging)
			{
				logger::info("[punish] {} swings into {}'s switch from {} to {}: line {} opened at {:.0f} (aggression {:.2f})",
					Who(actor), Who(target), static_cast<int>(Was), static_cast<int>(seen),
					static_cast<int>(DirectionHandler::GetSingleton()->GetCurrentDirection(actor)), std::sqrt(TargetDistSQ), diff.aggressionMod);
			}
		}
		// A pick still held from an earlier switch doesn't disarm this one: the newest read wins
		// (the match function that spends a pick is skipped while an attack is queued).
		// A swing already in flight on this frame is read, not predicted: the start edge above
		// queued the switch to its actual line, and a forced prediction would overwrite it.
		const bool AttackLike = Answer == SwitchAnswer::Cover;
		const bool ReflexArmed = lineChanged && !actor->IsAttacking() && TargetDistSQ < JudgeRadius * JudgeRadius &&
			!diff.reflexSpent && !(inSwingPhase && !wasSwingPhase) && AttackLike;
		if (ReflexArmed)
		{
			// The row as the chain node stands this frame, not the tick's copy: the node moved on
			// the hit frame, and a chain switch lands before the next tick refreshes the cache.
			ComputeCascade(target, diff);
		}
		const bool ReflexFired = ReflexArmed && RollExpectation(diff.lastCascadeWeights, ReadDecisiveness(diff.difficulty), Pick);
		if (ReflexFired)
		{
			for (Directions Dir : { Directions::TR, Directions::TL, Directions::BL, Directions::BR })
			{
				diff.directionChangeChance[Dir] = std::max(0, diff.directionChangeChance[Dir] - AISettings::BeliefCascadeDrain);
			}
			diff.reflexPick = Pick;
			diff.reflexPickValid = true;
			diff.reflexSpent = true;
			const Directions ToSwitch = DirectionHandler::GetCounterDirection(Pick);
			// Forced between their chained swings too: a correction still pending there is for
			// a swing that has already landed, and it was refusing the read's switch.
			QueueDirectionSwitch(actor, ToSwitch, diff.swingElapsed >= 0.f || ComboLive, GuardInputSeconds);
			if (DirectionHandler::FoldDirection(ToSwitch) != DirectionHandler::GetSingleton()->GetCurrentDirection(actor))
			{
				diff.guardMovedAgo = 0.f;
			}
			// The flick: with their combo live the next swing is due within ~1 s, and a block
			// pressed on seeing it goes live ~0.4 s later, a coin flip. So the press rides with
			// the read, as the player's does. Not expedited: the switch request must reach the
			// handler before the guard is up, or the landing kills the parry window. A cautious
			// fighter that will escape the swing keeps its guard down instead: one answer per swing.
			if (ComboLive && !actor->IsBlocking())
			{
				// Decided with the escape's own gates: a chained swing is a light until it isn't.
				const bool Escapes = Settings::ActiveDodgeSystem != DodgeSystem::None && diff.DodgeCooldown <= 0.f &&
					static_cast<float>(mt_rand() % 10000u) / 10000.f < std::clamp(diff.cautionMod, 0.f, 1.f) &&
					EscapeClearsSwing(target, diff, seen, false, TargetDistSQ);
				diff.chainAnswer = Escapes ? 2 : 1;
				if (!Escapes)
				{
					AddAction(actor, Actions::Block, true);
					// Outlives the tail's "nothing to answer" release until the swing is due.
					diff.holdBlockLeft = std::max(diff.holdBlockLeft, FlickHoldSeconds);
					if (Settings::VerboseLogging)
					{
						logger::info("[flick] {} presses with the read on {}'s chain (switch to {})", Who(actor), Who(target), static_cast<int>(seen));
					}
				}
			}
			if (Settings::VerboseLogging)
			{
				logger::info("[dmt] {} {:08X} reflex expects {} on {}'s switch to {}: {} (weights {} {} {} {})",
					actor->GetName(), actor->GetFormID(), static_cast<int>(Pick), Who(target), static_cast<int>(seen),
					Pick == seen ? "right" : "wrong",
					diff.lastCascadeWeights[0], diff.lastCascadeWeights[1], diff.lastCascadeWeights[2], diff.lastCascadeWeights[3]);
			}
		}
		else if (Settings::VerboseLogging && ComboLive && TargetDistSQ < JudgeRadius * JudgeRadius)
		{
			// Why a chained switch got no expectation: a closed gate, or the roll on these
			// weights (the tick's copy when the gate was closed).
			logger::info("[dmt] {} {:08X} reflex passes on {}'s switch to {}: armed {} (answer {}, attacking {}, spent {}, pick held {}, dist {:.0f}) weights {} {} {} {}",
				actor->GetName(), actor->GetFormID(), Who(target), static_cast<int>(seen), ReflexArmed, static_cast<int>(Answer),
				actor->IsAttacking(), diff.reflexSpent, diff.reflexPickValid, std::sqrt(TargetDistSQ),
				diff.lastCascadeWeights[0], diff.lastCascadeWeights[1], diff.lastCascadeWeights[2], diff.lastCascadeWeights[3]);
		}

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
				// Poke from outside their reach: free, at the full rate, since a longer weapon
				// abuses its reach. Inside their reach it is rolled in the tail, on a tick the
				// pattern passed on.
				else if (!TargetSwingingAtMe && !Recovering &&
					TargetDistSQ > ThreatReachSQ &&
					TargetDistSQ < PokeReach * PokeReach &&
					(!IsGuardUp(target) || DirHandler->GetCurrentDirection(target) !=
						DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(Poke))) &&
					CurrentStaminaRatio > AttackStaminaOffLine + StaminaReserve(diff.cautionMod) &&
					AttackHandler::GetSingleton()->CanAttack(actor) &&
					RollPerSecond(PokeRatePerSecond, Tick))
				{
					DirHandler->WantToSwitchTo(actor, Poke, true);
					AddAction(actor, Actions::Attack);
					DontChangeDirection = true;
					if (Settings::VerboseLogging)
					{
						logger::info("[poke] {} thrusts at {} from {} at {:.0f} (outside their reach)", Who(actor), Who(target),
							static_cast<int>(Poke), std::sqrt(TargetDistSQ));
					}
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
						EscapeClearsSwing(target, diff, DirHandler->GetCurrentDirection(target), diff.swingWasPower, TargetDistSQ) &&
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
					diff.holdBlockLeft = DifficultySettings::AttackTimeoutTime;
					ShouldDirectionMatch = true;
					if (DirHandler->HasBlockAngle(target, actor))
					{
						DontChangeDirection = true;
					}
					if (Settings::VerboseLogging) logger::info("[preblock] {} locked-out pre-block", Who(actor));
				}
				// Neutral-game pre-block: strong read on the target's held
				// line + a cautionMod roll to commit. 
				else if (wantPreBlock)
				{
					AddAction(actor, Actions::Block, true);
					diff.holdBlockLeft = PreBlockHoldSeconds;
					ShouldDirectionMatch = true;
					if (DirHandler->HasBlockAngle(target, actor))
					{
						DontChangeDirection = true;
					}
					if (Settings::VerboseLogging) logger::info("[preblock] {} neutral pre-block belief={} caution={:.2f}", Who(actor), beliefOnLine, diff.cautionMod);
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
							logger::info("[opportunity] {} sees {} staggered, magnitude {}: {}", Who(actor), Who(target),
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
				// Never into a dagger, whose bash beats the attack.
				else if (TargetDistSQ < (BashDistanceSq + 1500) && !actor->IsAttacking() && !HoldsDagger(target) &&
					RollTickScaled((mod * 0.5f) + 2 + diff.aggressionMod * 2.0f, 14.f, Tick)
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
					diff.numTimesDirectionSame < 1 && diff.chainAnswer != 1 &&
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
				// Defending means answering a swing in flight, nothing more: it ends with the swing.
				// The block outlives it briefly, so a chain doesn't drop and re-raise it.
				if (diff.defending && !TargetSwingingAtMe)
				{
					diff.defending = false;
					diff.numTimesDirectionsSwitched = 2;
					diff.numTimesDirectionSame = 0;
					diff.holdBlockLeft = std::max(diff.holdBlockLeft, AnsweredHoldSeconds);
				}
				// A block with nothing to answer comes down, unless a pre-block is holding it.
				if (!diff.defending && actor->IsBlocking() && diff.holdBlockLeft <= 0.f)
				{
					AddAction(actor, Actions::EndBlock);
					DontChangeDirection = false;
				}

				if (!DontChangeDirection)
				{
					// Offense is decided every tick no swing is being answered, except while a chosen
					// swing is still queued: a new pick would replace it before it could start (its
					// protection is one action timer; its line switch or chain window takes longer).
					// The runner drops a swing that never gets its window, so this can't park the offense.
					if (!diff.defending && AttackHandler::GetSingleton()->CanAttack(actor) && !HasSwingQueued(actor))
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
					// The guard stays on them in every posture, out to the judge radius so it
					// is set as they close. Only a swing that already asked for its line keeps
					// it, and that swing waits for the line to land.
					const Actions Queued = GetQueuedAction(actor);
					bool AttackQueued = Queued == Actions::Attack || Queued == Actions::PowerAttack ||
						Queued == Actions::FeintFollowup || Queued == Actions::StartFeint ||
						Queued == Actions::DashAttack || Queued == Actions::OpportunityAttack ||
						Queued == Actions::Followup || Queued == Actions::UnblockRiposte ||
						Queued == Actions::UnblockPowerRiposte || Queued == Actions::UnblockStartFeint;
					// Inside their reach the thrust is the turn taken between their swings, on a
					// tick the pattern passed on: the loops, powers and feints decide first.
					if (!AttackQueued && !diff.defending && !TargetSwingingAtMe && !Recovering &&
						TargetDistSQ <= ThreatReachSQ && TargetDistSQ < PokeReach * PokeReach &&
						(!IsGuardUp(target) || DirHandler->GetCurrentDirection(target) !=
							DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(Poke))) &&
						CurrentStaminaRatio > AttackStaminaOffLine + StaminaReserve(diff.cautionMod) &&
						AttackHandler::GetSingleton()->CanAttack(actor) &&
						RollPerSecond(PokeRatePerSecond * std::clamp(0.5f + diff.aggressionMod + diff.baitTendency, 0.25f, 1.5f), Tick))
					{
						// Queue first: a pending EndBlock refuses the attack this tick, and a thrust
						// that was refused must not move the guard or claim the tick.
						AddAction(actor, Actions::Attack);
						if (GetQueuedAction(actor) == Actions::Attack)
						{
							DirHandler->WantToSwitchTo(actor, Poke, true);
							AttackQueued = true;
							if (Settings::VerboseLogging)
							{
								logger::info("[poke] {} thrusts at {} from {} at {:.0f} (inside their reach)", Who(actor), Who(target),
									static_cast<int>(Poke), std::sqrt(TargetDistSQ));
							}
						}
					}
					// Mid-chain the hand stays on its own next line, or every chained swing
					// would pay a switch back; a swing coming in still gets its read.
					bool PrevPower = false;
					const bool InChain = AttackHandler::GetSingleton()->InAttackChain(actor, PrevPower);
					if (!AttackQueued && !actor->IsAttacking() && (TargetSwingingAtMe || !InChain) &&
						(TargetSwingingAtMe || TargetDistSQ < JudgeRadius * JudgeRadius))
					{
						DirectionMatchTarget(actor, target, TargetSwingingAtMe);
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
		float TotalStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
		float CurrentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
		if (CurrentStamina > TotalStamina * 0.15f)
		{
			AIDifficulty& Diff = DifficultyMap[actor->GetHandle()];
			Diff.defending = false;
			// The next tick may spike the blocked swing again if it's still running.
			Diff.sawSwingLastTick = false;
			// The riposte carries the personality's kind roll, same shares as the open-line pick:
			// the attacker is locked out, so this is where a feint or a power bites hardest. The
			// unblockable finisher stays a plain riposte; both heavier kinds need a power's stamina.
			const bool PowerAffordable = !DirectionHandler::GetSingleton()->IsUnblockable(actor) &&
				CurrentStamina >= AttackHandler::GetSingleton()->SwingStaminaCost(actor, true);
			const float FeintShare = std::clamp((1.f + Diff.feintTendency) / 3.f, 0.f, 1.f);
			const float DieShare = std::clamp((1.f + 1.5f * Diff.powerAttackTendency) / 3.f, 0.f, 1.f);
			Actions Pick = Actions::UnblockRiposte;
			if (PowerAffordable && CurrentStamina >= TotalStamina * 0.4f &&
				static_cast<float>(mt_rand() % 10000u) / 10000.f < FeintShare)
			{
				Pick = Actions::UnblockStartFeint;
			}
			else if (PowerAffordable && static_cast<float>(mt_rand() % 10000u) / 10000.f < DieShare)
			{
				Pick = Actions::UnblockPowerRiposte;
			}
			if (Settings::VerboseLogging)
			{
				logger::info("[riposte] {} picks a {} riposte (feint share {:.2f}, power share {:.2f}, stamina {:.2f})", Who(actor),
					Pick == Actions::UnblockStartFeint ? "feint" : Pick == Actions::UnblockPowerRiposte ? "power" : "light",
					FeintShare, DieShare, CurrentStamina / std::max(1.f, TotalStamina));
			}
			// Priority 1, so a neutral pre-block can't wipe the counter before it comes out.
			AddAction(actor, Pick, false, 1);
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
// The cascade: the belief prior plus the learned edges out of the last swing line seen
// (the opener row before any), blended with the combo-rule term by tier. Rebuilt on the
// tick and again on the frame a reflex rolls, since the chain node moves between ticks.
void AIHandler::ComputeCascade(RE::Actor* target, AIDifficulty& diff) const
{
	int Belief[4] = {};
	for (int i = 0; i < 4; ++i)
	{
		const Directions Dir = static_cast<Directions>(i);
		// A mode that folds a line must never pick it.
		Belief[i] = DirectionHandler::DirectionEnabled(Dir) ? std::min(diff.directionChangeChance[Dir], BeliefBudget) : 0;
	}
	const int ChainNode = diff.lastSeenSwingLine >= 0 && diff.lastSeenSwingLine < 4 ? diff.lastSeenSwingLine : OpenerRow;
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
	// archetypes predicting the habit line right after it landed, the one
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

void AIHandler::DirectionMatchTarget(RE::Actor* actor, RE::Actor* target, bool force)
{
	int mod = (int)CalcAndInsertDifficulty(actor);
	auto& diff = DifficultyMap[actor->GetHandle()];

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

	// A swing in flight answered by sight: the switch waits for the reaction time,
	// plus CommitCost when a guard that already moved this swing has to move again.
	bool Read = false;
	float CommitCost = 0.f;
	// The price of leaving the line the guard covers: its belief share, in seconds.
	// Folded: beliefs live on the lines attacks actually come from.
	const Directions CoveredLine = DirectionHandler::FoldDirection(
		DirectionHandler::GetCounterDirection(DirectionHandler::GetSingleton()->GetCurrentDirection(actor)));
	const float Fixation = (static_cast<float>(chanceFor(CoveredLine)) / static_cast<float>(BeliefBudget)) *
		AISettings::ConditionedFixationSeconds;

	// Diagnostic tag for VerboseLogging: which mechanism produced this guard decision.
	const char* decisionKind = "hold";

	// The perception ring is flushed per tick; a switch no longer conditions the line
	// left behind. What follows a line is the chain graph's to say.
	diff.observedSwitchCount = 0;

	// Belief blended toward the lines the combo makes likely. Computed every
	// call, before accumulation, so the cached copy the HUD reads stays current.
	ComputeCascade(target, diff);

	if (ToCounter != CurrentTargetDir)
	{
		// Target's net direction changed since last tick.
		diff.numTimesDirectionsSwitched++;
		diff.conditioningStreak = std::min(diff.conditioningStreak + 1, AISettings::MaxDirectionTracked);
		diff.numTimesDirectionSame = 0;

		// First tier: expectation. The cascade fires from habits with no reaction
		// wait, right or wrong; being wrong is what conditioning buys the attacker.
		// Not against a swing already in flight: its line is visible, the start edge has
		// answered it, and re-forcing a stale pick here moved right guards onto wrong lines.
		bool found = false;
		if (force)
		{
			decisionKind = "swing-seen";
		}
		else if (diff.reflexPickValid)
		{
			// The reflex rolled, drained and moved on the frame the line changed; keep its pick.
			ToCounter = diff.reflexPick;
			diff.reflexPickValid = false;
			found = true;
			decisionKind = (ToCounter == CurrentTargetDir) ? "expect-right" : "expect-wrong";
		}
		else if (SwitchLooksLikeAttack(actor, target, diff, TorsoDistanceSq(actor, target), CurrentTargetDir,
					 DirectionHandler::GetSingleton()->GetComboStep(target) > 0) &&
				 RollExpectation(diff.lastCascadeWeights, ReadDecisiveness(diff.difficulty), ToCounter))
		{
			found = true;
			// The pick spends some conditioning.
			const int drainAmount = AISettings::BeliefCascadeDrain;
			TR = std::max(0, TR - drainAmount);
			TL = std::max(0, TL - drainAmount);
			BL = std::max(0, BL - drainAmount);
			BR = std::max(0, BR - drainAmount);
			decisionKind = (ToCounter == CurrentTargetDir) ? "expect-right" : "expect-wrong";
		}
		else
		{
			// No expectation. Between swings a weak reflex track, so positioning
			// comes from reading; against a swing the read below takes over.
			const int rollBaseline = mt_rand() % 50;
			if (!force && rollBaseline < mod)
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
		// Second tier: recognition. The swing-start frame normally decides the read; this is
		// the fallback for a swing whose start it did not answer (not yet pending).
		if (force && !found && !IsSwitchUnderway(actor, DirectionHandler::GetCounterDirection(CurrentTargetDir)))
		{
			ToCounter = CurrentTargetDir;
			Read = true;
			// A guard still committed from the last swing (a feint's first half) pays to leave it.
			CommitCost = diff.guardMovedAgo >= 0.f ? Fixation : 0.f;
			decisionKind = "read";
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
		// belief there grows with the target's charge instead. Not when the held
		// line is just where the last swing came from: that is a leftover, not a
		// show, and the chain graph already says what follows it.
		if (static_cast<int>(ToCounter) != diff.lastSeenSwingLine)
		{
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
		}
		// Recognition on a swing in flight: a guard off the line corrects toward it,
		// due at the reaction time plus, if a switch already committed this swing,
		// the fixation on the line it committed to.
		if (force && !DirectionHandler::GetSingleton()->HasBlockAngle(target, actor) &&
			!IsSwitchUnderway(actor, DirectionHandler::GetCounterDirection(CurrentTargetDir)))
		{
			ToCounter = CurrentTargetDir;
			Read = true;
			CommitCost = diff.guardMovedAgo >= 0.f ? Fixation : 0.f;
			decisionKind = "correct";
		}

	}

	// No force bump: an attack teaches the chain graph what follows it, not a belief
	// that the next one comes from the same line.

	// Single writeback to the map.
	diff.directionChangeChance[Directions::TR] = TR;
	diff.directionChangeChance[Directions::TL] = TL;
	diff.directionChangeChance[Directions::BL] = BL;
	diff.directionChangeChance[Directions::BR] = BR;

	diff.lastDecisionKind = decisionKind;

	{
		const Directions ToSwitch = DirectionHandler::GetCounterDirection(ToCounter);
		// The swing-start read owns the answer to a swing in flight: while its switch to the
		// counter is underway the tick neither re-decides the line nor "stays" over it, since a
		// forced request for any other line (a stay is one) would overwrite the read in the
		// one-slot queue. The press below still runs.
		const Directions SwingCounter = DirectionHandler::GetCounterDirection(CurrentTargetDir);
		const bool ReadUnderway = force && IsSwitchUnderway(actor, SwingCounter) &&
			DirectionHandler::FoldDirection(ToSwitch) != DirectionHandler::FoldDirection(SwingCounter);
		if (ReadUnderway)
		{
			decisionKind = "read-pending";
			diff.lastDecisionKind = decisionKind;
		}
		else
		{
			// A read is a reaction: the tier's processing time to register the swing, the action
			// timer for the choice of line, measured from the swing's start.
			float Input = GuardInputSeconds;
			if (Read)
			{
				const float Elapsed = std::max(0.f, diff.swingElapsed);
				Input = std::max(GuardInputSeconds, RollProcessing(CalcProcessingTimer(actor), diff.npcRand) +
					CalcActionTimer(actor) * ReadBitsScale(KIND_BITS(diff)) + CommitCost - Elapsed);
			}
			QueueDirectionSwitch(actor, ToSwitch, force, Input);
			// A switch against a swing is a commitment for a short while, feint follow-ups included.
			if (force && DirectionHandler::FoldDirection(ToSwitch) != DirectionHandler::GetSingleton()->GetCurrentDirection(actor))
			{
				if (Settings::VerboseLogging && Read && !diff.swingCorrectionLogged)
				{
					diff.swingCorrectionLogged = true;
					logger::info("[dmt] {} {} toward {} swing from {}: guard counts in {:.0f}ms (commit cost {:.0f}ms)",
						Who(actor), decisionKind, Who(target), static_cast<int>(CurrentTargetDir),
						(Input + GuardTransitionSeconds) * 1000.f, CommitCost * 1000.f);
				}
				if (diff.answerAt < 0.f)
				{
					diff.answerAt = diff.swingElapsed + Input + GuardTransitionSeconds;
				}
				diff.guardMovedAgo = 0.f;
			}
		}

		// Every tick of a swing in flight: a guard that isn't up yet comes up, whichever
		// line it is on. AddAction drops the repeat once it's queued or blocking.
		if (force && IsPreparedToBlock(actor, target, diff) && IsBlockableSwing(target))
		{
			AddAction(actor, Actions::Block, true);
		}
	}

	// Exchange record: the swing's first answer, or a later move that overrode it.
	if (force)
	{
		const bool Unset = std::strcmp(diff.answerKind, "none") == 0 || std::strcmp(diff.answerKind, "expect-pre") == 0;
		const bool Moves = std::strcmp(decisionKind, "correct") == 0 || std::strcmp(decisionKind, "react-track") == 0 ||
			std::strcmp(decisionKind, "read") == 0 || std::strncmp(decisionKind, "expect", 6) == 0;
		if (Unset)
		{
			diff.answerKind = decisionKind;
		}
		else if (Moves && std::strcmp(decisionKind, diff.answerKind) != 0)
		{
			diff.answerLater = decisionKind;
		}
	}
	// Once per swing: what the guard did about it, and why.
	if (Settings::VerboseLogging && force && !diff.sawSwingLastTick)
	{
		logger::info("[dmt] {} {:08X} answers {} swing from {}: {} (guard {} -> {}, line seen {:.0f}ms, window {:.0f}ms)",
			actor->GetName(), actor->GetFormID(), Who(target), static_cast<int>(CurrentTargetDir), decisionKind,
			static_cast<int>(DirectionHandler::GetSingleton()->GetCurrentDirection(actor)),
			static_cast<int>(DirectionHandler::GetCounterDirection(ToCounter)),
			diff.timeSinceLineChange * 1000.f, CommitWindow(diff) * 1000.f);
	}
	// A reflex pick only lives until the next decision; a wiggle must not hand it to a later change.
	diff.reflexPickValid = false;
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
	// Tint = how fixated this AI is on the player's swing kind: 1 (red) when it has only
	// seen lights and reacts at full speed, 0 (yellow) at two kind bits, the full mixup tax.
	// Replaced the fight-record tint (mistakeRatio / MaxMistakeRange) 2026-10-12.
	outConfidence = std::clamp(1.f - KIND_BITS(iter->second) * 0.5f, 0.f, 1.f);
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
	// A dash swings from the line it dodged in on: any switch here would spend its
	// charge, and the dash entry becomes the swing itself.
	if (GetQueuedAction(actor) == Actions::DashAttack)
	{
		return;
	}
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
	// A charge built while parked (out of reach, standing off, waiting on a line) is spent
	// before it tapers: the swing it speeds up is the one to take now.
	const float ChargeRatio = DirectionHandler::GetSingleton()->GetGuardChargeRatio(actor);
	const float ChargeBoost = 1.f + ChargeRatio;
	// Folded on both sides: in a 3-line mode the two lows (or tops) are one
	// line, so an entry on the folded twin is into the guard too.
	if (DirectionHandler::FoldDirection(diff.attackPattern[idx]) != DirectionHandler::FoldDirection(CounterDirection))
	{
		// The pattern line is taken only when a swing is chosen, below: until then the
		// guard is the defender's, steered by DirectionMatchTarget.
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
			if (RollPerSecond(BaseInitiativeRate(mod, diff.aggressionMod) * StaminaScale * ComboRate * ChargeBoost, Tick))
			{
				// Ask for the pattern line now; the swing waits for it to land.
				SwitchToNextAttack(actor);
				// the attack lands from the queued line if one is still in
				// transit, otherwise from the one already held
				DirectionHandler* DirHandler = DirectionHandler::GetSingleton();
				Directions AttackDir;
				if (!DirHandler->HasQueuedDirection(actor, AttackDir))
				{
					AttackDir = DirHandler->GetCurrentDirection(actor);
				}
				// A warhammer in a chain alternates light and power: each switch speeds the next swing.
				// It breaks the alternation on a read, not a roll: a guard already answering the
				// line gets the power (a blocked power locks the defender out and advances the
				// combo), an open guard gets the fast light.
				bool PrevPower = false;
				const bool Alternating = DirHandler->AnimationSet(actor) == WeaponSet::Warhammer &&
					AttackHandler::GetSingleton()->InAttackChain(actor, PrevPower);
				const bool Answered = IsGuardUp(target) && DirHandler->GetCurrentDirection(target) ==
					DirectionHandler::FoldDirection(DirectionHandler::GetCounterDirection(AttackDir));
				const bool PowerAffordable = PowerInReach &&
					CurrentStamina >= AttackHandler::GetSingleton()->SwingStaminaCost(actor, true);
				// Spend the finisher as a power attack
				if (PowerInReach && DirHandler->WouldCompleteCombo(actor, AttackDir) &&
					CurrentStaminaRatio > ComboFinisherStaminaRatio + Reserve)
				{
					AddAction(actor, AIHandler::Actions::PowerAttack);
				}
				else if (Alternating && PrevPower && Answered && PowerAffordable)
				{
					AddAction(actor, AIHandler::Actions::PowerAttack);
					if (Settings::VerboseLogging) logger::info("[hammer] {} breaks for a power: {}'s guard answers the line", Who(actor), Who(target));
				}
				else if (Alternating && !PrevPower && !Answered && LightInReach)
				{
					AddAction(actor, AIHandler::Actions::Attack);
					if (Settings::VerboseLogging) logger::info("[hammer] {} breaks for a light: {}'s guard is open", Who(actor), Who(target));
				}
				else if (Alternating && PrevPower && LightInReach)
				{
					AddAction(actor, AIHandler::Actions::Attack);
				}
				else if (Alternating && !PrevPower && PowerAffordable)
				{
					AddAction(actor, AIHandler::Actions::PowerAttack);
				}
				// This line is open: the feint takes its own share first (a power windup, so it
				// needs the power's reach), then the light is the anti-open tool and keeps its
				// archetype share of the rest at every tier; the remainder is a real power.
				else
				{
					const float FeintShare = std::clamp((1.f + diff.feintTendency) / 3.f, 0.f, 1.f);
					const float DieShare = std::clamp((1.f + 1.5f * diff.powerAttackTendency) / 3.f, 0.f, 1.f);
					if (PowerInReach && static_cast<float>(mt_rand() % 10000u) / 10000.f < FeintShare)
					{
						AddAction(actor, AIHandler::Actions::StartFeint);
					}
					else if (LightInReach && (!PowerInReach ||
						static_cast<float>(mt_rand() % 10000u) / 10000.f >= DieShare))
					{
						AddAction(actor, AIHandler::Actions::Attack);
						// Charged opener: hold the attack line for guard charge, then swing it fast or
						// re-aim at an open line inside the carry window. Openers only; a chain has
						// tempo to keep. Defence interrupts the hold for free, as it does the player's.
						if (!Chained)
						{
							const float ChargeShare = std::clamp(ChargeShareBase + ChargeSharePatience * diff.patienceMod -
								ChargeShareAggression * diff.aggressionMod + (CurrentStaminaRatio < 0.5f ? ChargeShareLowStamina : 0.f),
								0.f, ChargeShareMax);
							if (static_cast<float>(mt_rand() % 10000u) / 10000.f < ChargeShare)
							{
								const float Goal = ChargeGoalMin + static_cast<float>(mt_rand() % 10000u) / 10000.f * (1.f - ChargeGoalMin);
								if (SetQueuedChargeGoal(actor, Goal) && Settings::VerboseLogging)
								{
									logger::info("[charge] {} holds line {} for a {:.2f} charge (share {:.2f}: patience {:.2f}, aggression {:.2f}, stamina {:.2f})",
										Who(actor), static_cast<int>(AttackDir), Goal, ChargeShare, diff.patienceMod, diff.aggressionMod, CurrentStaminaRatio);
								}
							}
						}
					}
					else
					{
						AddAction(actor, AIHandler::Actions::PowerAttack);
					}
				}
				if (Settings::VerboseLogging)
				{
					// What the queue holds after the pick: a refused add shows as neither.
					const Actions Picked = GetQueuedAction(actor);
					logger::info("[pattern] {} picks {} from {} at {:.0f}: blocking {}, light in reach {}, power in reach {}, chained {}, tendency {:.2f}, feint share {:.2f}, charge {:.2f}",
						Who(actor), Picked == Actions::PowerAttack ? "power" : Picked == Actions::Attack ? "light" :
						Picked == Actions::StartFeint ? "feint" : "nothing",
						static_cast<int>(AttackDir), std::sqrt(TargetDistSQ), actor->IsBlocking(), LightInReach, PowerInReach, Chained,
						diff.powerAttackTendency, std::clamp((1.f + diff.feintTendency) / 3.f, 0.f, 1.f), ChargeRatio);
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
			if (RollPerSecond(BaseInitiativeRate(mod, diff.aggressionMod) * StaminaScale * ComboRate * ChargeBoost, Tick))
			{
				// Flat reach: a feint redirects to a cut, whatever line it starts on.
				const float CutReach = actor->GetReach() + CutLungeUnits;
				if (ShouldFeint && TargetDistSQ < CutReach * CutReach)
				{
					SwitchToNextAttack(actor);
					// A third of these are feints with no lean; the tendency scales that share.
					// The rest is the anti-block read: into a covered line a light only locks the
					// attacker out, so the power takes half of the rest at VeryEasy and all of it at
					// Legendary.
					const float FeintShare = std::clamp((1.f + diff.feintTendency) / 3.f, 0.f, 1.f);
					const float TierT = std::clamp(
						static_cast<float>(mod - static_cast<int>(Difficulty::VeryEasy)) /
							static_cast<float>(static_cast<int>(Difficulty::Legendary) - static_cast<int>(Difficulty::VeryEasy)), 0.f, 1.f);
					const float Pick = static_cast<float>(mt_rand() % 10000u) / 10000.f;

					if (Pick < FeintShare)
					{
						AddAction(actor, AIHandler::Actions::StartFeint);
					}
					else if (Pick < FeintShare + (1.f - FeintShare) * (0.5f + 0.5f * TierT))
					{
						AddAction(actor, AIHandler::Actions::PowerAttack);
					}
					else
					{
						AddAction(actor, AIHandler::Actions::Attack);
					}
					if (Settings::VerboseLogging)
					{
						const Actions Picked = GetQueuedAction(actor);
						logger::info("[pattern] {} presses {} into {}'s guard at {:.0f}: blocking {}, feint share {:.2f}, charge {:.2f}",
							Who(actor), Picked == Actions::PowerAttack ? "power" : Picked == Actions::Attack ? "light" :
							Picked == Actions::StartFeint ? "feint" : "nothing",
							Who(target), std::sqrt(TargetDistSQ), actor->IsBlocking(), FeintShare, ChargeRatio);
					}
				}
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

void AIHandler::RecordPlayerSwingLine(WeaponSet a_set, Directions a_to)
{
	// Off, the saved profile stays as it was.
	if (!AISettings::LearnAcrossFights)
	{
		return;
	}
	const int Set = static_cast<int>(a_set);
	const int To = static_cast<int>(a_to);
	if (Set < 0 || Set >= NumWeaponSets)
	{
		return;
	}
	std::lock_guard Lock(PlayerHabitMtx);
	// The NPCs' chain node, mirrored: the last swing line until the combo timer would
	// have worn off; a new weapon set starts over, an unblockable swing keeps the node.
	const auto Now = std::chrono::steady_clock::now();
	const bool Chained = PlayerLastSwingLine >= 0 && PlayerLastSwingSet == Set &&
		std::chrono::duration<float>(Now - PlayerLastSwingAt).count() < DifficultySettings::ComboResetTimer;
	const int From = Chained ? PlayerLastSwingLine : OpenerRow;
	if (To >= 0 && To < 4)
	{
		PlayerLastSwingLine = To;
	}
	else if (!Chained)
	{
		PlayerLastSwingLine = -1;
	}
	PlayerLastSwingSet = Set;
	PlayerLastSwingAt = Now;
	if (To < 0 || To > 3)
	{
		return;
	}
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
		logger::info("[stats] {}", Line);
		std::string Feints = std::format("[stats] {} feints:", SetName);
		for (int Step = 0; Step < Steps; ++Step)
		{
			const std::uint32_t Powers = PlayerStats.powers[Set][Step];
			const std::uint32_t Lights = PlayerStats.swings[Set][Step] - Powers;
			const std::string Label = Step == 0 ? "opener" : std::format("step {}{}", Step, Step == Steps - 1 ? "+" : "");
			Feints += std::format(" {} light {}/{} power {}/{}", Label,
				PlayerFeints.feints[Set][Step][0], Lights, PlayerFeints.feints[Set][Step][1], Powers);
		}
		logger::info("[stats] {}", Feints);
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
				logger::info("[stats] {}", Dist);
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
				logger::info("[stats] {}", Stam);
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
			logger::info("[stats] {}, no contact {}", Out, Swings - std::min(Swings, Resolved));
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
					logger::info("[stats] {} (of {})", Def, Total);
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

// Staggered, inside a dodge clip, or inside a feint's stop blend: an attackStart is
// accepted but never plays.
static bool InTransition(RE::Actor* actor)
{
	auto* Dodges = DodgeHandler::GetSingleton();
	const float SinceDodge = Dodges->SecondsSinceDodge(actor);
	return actor->AsActorState()->actorState2.staggered || Dodges->IsDodging(actor) ||
		(SinceDodge >= 0.f && SinceDodge < DodgeClipSeconds) ||
		AttackHandler::GetSingleton()->SecondsSinceFeint(actor) < FeintFollowupSettle;
}

// Out of a straight back dodge the only swing is the poke. NPCs can swing once
// the dodge clip ends, and for this long after.
constexpr float BackDodgePokeSeconds = 0.3f;
static bool InBackDodgeFollowUp(RE::Actor* actor)
{
	const float Since = DodgeHandler::GetSingleton()->SecondsSinceBackDodge(actor);
	return Since >= 0.f && Since < DodgeClipSeconds + BackDodgePokeSeconds;
}

bool AIHandler::TryAttack(RE::Actor* actor)
{
	// Turn to the poke line first; the queue waits for the switch. A raised guard opts
	// out: turning it is a guard switch, with its parry and stamina cost.
	if (InBackDodgeFollowUp(actor) && !actor->IsBlocking())
	{
		auto* DirHandler = DirectionHandler::GetSingleton();
		const Directions Poke = DirectionHandler::FoldDirection(AttackHandler::PokeLine(actor));
		if (DirHandler->GetCurrentDirection(actor) != Poke)
		{
			Directions Queued;
			if (Settings::VerboseLogging && (!DirHandler->HasQueuedDirection(actor, Queued) || Queued != Poke))
			{
				logger::info("[ai] {} swings out of a back dodge: turning to the poke line {}", Who(actor), static_cast<int>(Poke));
			}
			DirHandler->WantToSwitchTo(actor, Poke, true);
			return false;
		}
	}
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
	// Out of a back dodge a power comes out as the poke; the blocking gate below still holds.
	if (InBackDodgeFollowUp(actor))
	{
		return !actor->IsBlocking() && TryAttack(actor);
	}

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

void AIHandler::QueueDirectionSwitch(RE::Actor* actor, Directions dir, bool force, float inputDelay)
{
	const float Delay = inputDelay < 0.f ? GuardInputSeconds : inputDelay;
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
		DirectionQueue[actor->GetHandle()] = { dir, force, Delay };
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
	// An explicit delay is a floor: a read redirecting a pending guess must not inherit its head start.
	if (inputDelay >= 0.f)
	{
		Iter->second.timeLeft = std::max(Iter->second.timeLeft, Delay);
	}
	else if (Opposite)
	{
		Iter->second.timeLeft = Delay;
	}
}

bool AIHandler::IsSwitchPending(RE::Actor* actor, Directions dir) const
{
	std::shared_lock DirLock(DirectionQueueMtx);
	auto Iter = DirectionQueue.find(actor->GetHandle());
	return Iter != DirectionQueue.end() &&
		DirectionHandler::FoldDirection(Iter->second.dir) == DirectionHandler::FoldDirection(dir);
}

void AIHandler::CancelSwitchUnderway(RE::Actor* actor)
{
	{
		std::unique_lock DirLock(DirectionQueueMtx);
		DirectionQueue.erase(actor->GetHandle());
	}
	DirectionHandler::GetSingleton()->CancelQueuedDirection(actor);
}

bool AIHandler::IsSwitchUnderway(RE::Actor* actor, Directions dir) const
{
	if (IsSwitchPending(actor, dir))
	{
		return true;
	}
	Directions Queued;
	return DirectionHandler::GetSingleton()->HasQueuedDirection(actor, Queued) &&
		DirectionHandler::FoldDirection(Queued) == DirectionHandler::FoldDirection(dir);
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
			if (Settings::VerboseLogging) logger::info("[ai] {} had error in attack pattern to {}", Who(actor), (int)dir);
		}
		QueueDirectionSwitch(actor, dir, false);
	}
	else
	{
		diff.currentAttackIdx = 0;
	}
}

bool AIHandler::SetQueuedChargeGoal(RE::Actor* actor, float goal)
{
	std::unique_lock lock(ActionQueueMtx);
	auto Iter = ActionQueue.find(actor->GetHandle());
	if (Iter == ActionQueue.end() || Iter->second.toDo != Actions::Attack)
	{
		return false;
	}
	Iter->second.chargeGoal = goal;
	Iter->second.heldForCharge = 0.f;
	return true;
}

void AIHandler::ExpediteQueuedAction(RE::Actor* actor)
{
	std::unique_lock lock(ActionQueueMtx);
	auto Iter = ActionQueue.find(actor->GetHandle());
	if (Iter != ActionQueue.end())
	{
		// The runner dispatches once this is negative.
		Iter->second.timeLeft = -0.001f;
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
			Who(actor), (int)ret, (int)Base, Jitter);
		AIDifficulty aidiff = { ret, 0.f };
		DifficultyMap[actor->GetHandle()] = aidiff;
		DifficultyMap[actor->GetHandle()].lastDirectionsEncountered.reserve(MaxDirs);
		//default
		DifficultyMap[actor->GetHandle()].lastDirectionTracked = Directions::TR;

		// generate attack patterns ahead of time, from the seed rather than rand so
		// you can learn after dying
		std::array<Directions, 4> Lines{};
		int NumLines = 0;
		for (Directions Dir : { Directions::TR, Directions::TL, Directions::BL, Directions::BR })
		{
			if (DirectionHandler::DirectionEnabled(Dir))
			{
				Lines[NumLines++] = Dir;
			}
		}
		auto& Pattern = DifficultyMap[actor->GetHandle()].attackPattern;
		// Every loop this mode can run. Three lines: the table rows fold into repeats, so
		// loops over the real lines, never the same line back-to-back so combos still
		// complete. Nine distinct: 3 by favourite line (length 4; swapping Y and Z only
		// shifts that loop) and 6 by ordering (length 5). Four lines: the table rows.
		std::vector<std::vector<Directions>> Pool;
		if (NumLines == 3)
		{
			for (uint32_t Pick = 0; Pick < 9; ++Pick)
			{
				const uint32_t Perm = Pick < 3 ? Pick * 2 : Pick - 3;
				std::array<int, 3> Order{ 0, 1, 2 };
				for (uint32_t i = 0; i < Perm; ++i)
				{
					std::next_permutation(Order.begin(), Order.end());
				}
				const Directions X = Lines[Order[0]], Y = Lines[Order[1]], Z = Lines[Order[2]];
				if (Pick < 3)
				{
					Pool.push_back({ X, Y, X, Z });     // one favourite line
				}
				else
				{
					Pool.push_back({ X, Y, X, Y, Z });  // one rare line
				}
			}
		}
		else
		{
			for (const auto& Row : AIAttackCombo)
			{
				Pool.emplace_back(std::begin(Row), std::end(Row));
			}
		}
		const FollowUpAxis Axis = DirectionHandler::GetSingleton()->GetFollowUpAxis(actor);
		if (Axis == FollowUpAxis::None)
		{
			Pattern = Pool[ActorSeed % Pool.size()];
		}
		else
		{
			// Swords and polearms lean on their fast follow-up without all playing it. Four
			// lines also offer the loops over all four, two of which cross one axis every
			// step. The pick is weighted by the square of the share of steps that cross the
			// weapon's axis, then started at a seeded point.
			if (NumLines == 4)
			{
				constexpr Directions Loops[6][4] = {
					{ Directions::TR, Directions::TL, Directions::BL, Directions::BR },  // square
					{ Directions::TR, Directions::BR, Directions::BL, Directions::TL },
					{ Directions::TR, Directions::TL, Directions::BR, Directions::BL },  // Z: sides every step
					{ Directions::TR, Directions::BL, Directions::BR, Directions::TL },
					{ Directions::TR, Directions::BR, Directions::TL, Directions::BL },  // N: heights every step
					{ Directions::TR, Directions::BL, Directions::TL, Directions::BR },
				};
				for (const auto& Loop : Loops)
				{
					Pool.emplace_back(std::begin(Loop), std::end(Loop));
				}
			}
			std::uint32_t Hash = ActorSeed * 0x9E3779B1u;
			Hash ^= Hash >> 15;
			std::vector<float> Weights;
			float Total = 0.f;
			for (const auto& Loop : Pool)
			{
				int Cross = 0;
				for (size_t i = 0; i < Loop.size(); ++i)
				{
					Cross += DirectionHandler::AxisHalf(Loop[i], Axis) !=
						DirectionHandler::AxisHalf(Loop[(i + 1) % Loop.size()], Axis) ? 1 : 0;
				}
				const float Share = static_cast<float>(Cross) / static_cast<float>(Loop.size());
				Weights.push_back(Share * Share);
				Total += Share * Share;
			}
			float PoolRoll = static_cast<float>(Hash % 10000u) / 10000.f * Total;
			size_t Chosen = 0;
			while (Chosen + 1 < Pool.size() && PoolRoll >= Weights[Chosen])
			{
				PoolRoll -= Weights[Chosen];
				++Chosen;
			}
			Pattern = Pool[Chosen];
			std::rotate(Pattern.begin(), Pattern.begin() + (Hash >> 16) % Pattern.size(), Pattern.end());
		}
		if (Settings::VerboseLogging)
		{
			std::string Text;
			for (Directions Dir : Pattern)
			{
				Text += std::format(" {}", static_cast<int>(Dir));
			}
			logger::info("[ai] {} attack pattern:{} ({})", Who(actor), Text,
				Axis == FollowUpAxis::Side ? "leans side to side" : Axis == FollowUpAxis::Height ? "leans up and down" : "seeded");
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
				logger::info("[ai] {} archetype={} (agg={:.2f} pat={:.2f} bait={:.2f} caut={:.2f} pow={:.2f} feint={:.2f})",
					Who(actor), arch.name,
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

	// A hit no longer credits its own line: what follows it is the chain graph's to say.
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
	// A blocked hit no longer credits its own line either; the lose-shift below is the read.
	// Lose-shift: a transition that just got blocked is one the target is less
	// likely to try again from there. One sighting's worth: at two, a habit that
	// kept coming eroded until it was blocked only half the time.
	auto& Chain = Iter->second;
	if (Chain.lastChainFrom >= 0 && Chain.lastChainTo == static_cast<int>(attackedDir))
	{
		int& Edge = Chain.chainEdges[Chain.lastChainFrom][Chain.lastChainTo];
		Edge = std::max(0, Edge - ChainEdgeGain);
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
	DifficultyUpdateTimerMtx.lock_shared();
	Difficulty mod = CalcAndInsertDifficulty(actor);
	assert(DifficultyUpdateTimer.contains(mod));
	float base = DifficultyUpdateTimer.at(mod);
	DifficultyUpdateTimerMtx.unlock_shared();
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

float AIHandler::CalcProcessingTimer(RE::Actor* actor)
{
	float base = AISettings::NormalProcessingTimer;
	{
		std::shared_lock lock(DifficultyProcessingTimerMtx);
		const Difficulty mod = CalcAndInsertDifficulty(actor);
		auto It = DifficultyProcessingTimer.find(mod);
		if (It != DifficultyProcessingTimer.end())
		{
			base = It->second;
		}
	}
	// Tiredness slows the registering and deciding too. find, not at(): see CalcActionTimer.
	auto DiffIter = DifficultyMap.find(actor->GetHandle());
	if (DiffIter != DifficultyMap.end())
	{
		base += AISettings::FatigueProcessingSeconds * Fatigue(DiffIter->second.fightSeconds);
	}
	return base;
}

float AIHandler::CalcActionTimer(RE::Actor* actor)
{
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
				// A charged opener holds its line until the charge reaches its goal. A target
				// that leaves reach ends it; the hold is bounded by the full charge time, so a
				// block that zeroed the charge still lets the swing go.
				else if (ActionQueueIter->second.chargeGoal > 0.f)
				{
					Action& Act = ActionQueueIter->second;
					RE::Actor* Target = GetCombatTarget(actor);
					const float Reach = (actor->GetReach() + AttackLungeUnits) * ChargeReachSlack;
					const float Ratio = DirectionHandler::GetSingleton()->GetGuardChargeRatio(actor);
					Act.heldForCharge += delta;
					if (!Target || TorsoDistanceSq(actor, Target) > Reach * Reach)
					{
						if (Settings::VerboseLogging)
						{
							logger::info("[charge] {} drops its charged opener at {:.2f}: target out of reach", Who(actor), Ratio);
						}
						Act.toDo = Actions::None;
					}
					else if (Ratio >= Act.chargeGoal || Act.heldForCharge >= DifficultySettings::GuardChargeFull + ChargeHoldMargin)
					{
						// Go. Their guard on the line: re-aim at an open one inside the carry window,
						// the switch landing before the swing as for any attack.
						Act.chargeGoal = 0.f;
						const Directions Line = DirectionHandler::GetSingleton()->GetCurrentDirection(actor);
						const Directions Open = LineCovered(Target, Line) ? OpenLineAgainst(actor, Target) : Line;
						if (Open != Line)
						{
							DirectionHandler::GetSingleton()->WantToSwitchTo(actor, Open, true);
							Act.waitedForDir = 0.f;
						}
						if (Settings::VerboseLogging)
						{
							logger::info("[charge] {} swings at {:.2f} charge after {:.2f}s: line {} {}", Who(actor), Ratio, Act.heldForCharge,
								static_cast<int>(Line), Open != Line ? std::format("covered, re-aims to {}", static_cast<int>(Open)) : "open");
						}
					}
				}
				else if (TryAttack(actor))
				{
					ActionQueueIter->second.toDo = Actions::None;
				}
				else
				{
					// Unranked while it can't start, so the defence can displace it; dropped after
					// StaleSwingSeconds so a swing that never gets its window can't park the offense.
					ActionQueueIter->second.priority = 0;
					ActionQueueIter->second.timeLeft -= delta;
					if (ActionQueueIter->second.timeLeft < -StaleSwingSeconds)
					{
						ActionQueueIter->second.toDo = Actions::None;
					}
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
					ActionQueueIter->second.timeLeft -= delta;
					if (ActionQueueIter->second.timeLeft < -StaleSwingSeconds)
					{
						ActionQueueIter->second.toDo = Actions::None;
					}
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
			else if (ActionQueueIter->second.toDo == Actions::UnblockRiposte ||
				ActionQueueIter->second.toDo == Actions::UnblockPowerRiposte)
			{
				bool staggering = actor->AsActorState()->actorState2.staggered;
				if (!staggering)
				{
					actor->AsActorState()->actorState2.wantBlocking = 0;
					actor->NotifyAnimationGraph("blockStop");
					ActionQueueIter->second.toDo = ActionQueueIter->second.toDo == Actions::UnblockPowerRiposte ?
						Actions::PowerAttack : Actions::Attack;
					ActionQueueIter->second.timeLeft = TransitionSettleSeconds;
					// The riposte keeps its priority into the swing.
					ActionQueueIter->second.priority = 1;
					if (Settings::VerboseLogging)
					{
						logger::info("[riposte] {} drops its guard to riposte", Who(actor));
					}
				}
				else
				{
					if (Settings::VerboseLogging)
					{
						logger::info("[riposte] {} drops its riposte: staggered", Who(actor));
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
					const bool Bashed = AttackHandler::GetSingleton()->DoBash(actor);
					if (Settings::VerboseLogging)
					{
						logger::info("[bash] {} {}", Who(actor), Bashed ? "starts a bash" : "bash refused by the engine");
					}
					if (Bashed)
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
					if (Settings::VerboseLogging)
					{
						logger::info("[bash] {} drops its bash: attacking {}, in transition {}, can attack {}", Who(actor),
							actor->IsAttacking(), InTransition(actor), AttackHandler::GetSingleton()->CanAttack(actor));
					}
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
					if (Settings::VerboseLogging)
					{
						logger::info("[bash] {} releases: graph bashing {}, waited {:.2f}s, state {}", Who(actor), Bashing,
							ActionQueueIter->second.waitedForDir, static_cast<int>(actor->AsActorState()->GetAttackState()));
					}
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
					// The cancel fires at the feint point; EndFeint waits for it. A refusal
					// here means the engine took no swing, and QueueFeint logged it.
					if (AttackHandler::GetSingleton()->QueueFeint(actor))
					{
						ActionQueueIter->second.toDo = Actions::EndFeint;
						ActionQueueIter->second.timeLeft = 0.f;
						ActionQueueIter->second.priority = 2;
					}
					else
					{
						ActionQueueIter->second.toDo = Actions::None;
					}
				}
				else
				{
					ActionQueueIter->second.priority = 0;
					ActionQueueIter->second.timeLeft -= delta;
					if (ActionQueueIter->second.timeLeft < -StaleSwingSeconds)
					{
						ActionQueueIter->second.toDo = Actions::None;
					}
				}


			}
			else if (ActionQueueIter->second.toDo == Actions::EndFeint)
			{
				auto* Attacks = AttackHandler::GetSingleton();
				if (Attacks->TakeFeintResolved(actor))
				{
					RE::Actor* FeintTarget = GetCombatTarget(actor);
					// Already countered: hold the line the counter is aimed at and let the
					// defence block or masterstrike it.
					if (FeintTarget && IsIncomingSwing(FeintTarget, actor))
					{
						if (Settings::VerboseLogging)
						{
							logger::info("[feint] {} holds its line: {} is already countering", Who(actor), Who(FeintTarget));
						}
						ActionQueueIter->second.toDo = Actions::None;
					}
					else
					{
						// A feint only cancels; the follow-up crosses or stays by the coin, and waits
						// out the stop's blend either way.
						if (static_cast<float>(static_cast<unsigned>(GetRand()) % 10000u) / 10000.f >= FeintSameLineShare)
						{
							Attacks->HandleFeintChangeDirection(actor);
						}
						ActionQueueIter->second.toDo = Actions::FeintFollowup;
						ActionQueueIter->second.timeLeft = FeintFollowupSettle;
						// Unranked: a forced block or dodge may take the slot during the settle;
						// only swings must wait it out (InTransition).
						ActionQueueIter->second.priority = 0;
					}
				}
				else if (Attacks->FeintPending(actor))
				{
					// Waiting for the feint point (under a second at any plausible weapon speed).
					ActionQueueIter->second.timeLeft -= delta;
					if (ActionQueueIter->second.timeLeft < -2.f * StaleSwingSeconds)
					{
						if (Settings::VerboseLogging)
						{
							logger::info("[feint] {} planned feint never reached its point", Who(actor));
						}
						ActionQueueIter->second.toDo = Actions::None;
					}
				}
				else
				{
					// Refused at the point (no stamina, swing cut short) or dropped at the
					// hit: the original swing goes through.
					if (Settings::VerboseLogging)
					{
						logger::info("[feint] {} planned feint didn't fire", Who(actor));
					}
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
						logger::info("[feint] {} drops its follow-up: {} is countering", Who(actor), Who(FeintTarget));
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
						logger::info("[opportunity] {} drops its swing: the opening closed", Who(actor));
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
						// Which of TryAttack's checks refused.
						auto* Attacks = AttackHandler::GetSingleton();
						const char* Why = Swung ? "" :
							InTransition(actor) ? ": in transition" :
							IsAttackingDisabled(actor) ? ": attacking disabled" :
							!Attacks->CanAttack(actor) ? ": locked out" :
							Attacks->FeintPending(actor) ? ": feint pending" :
							Attacks->LacksAttackStamina(actor) ? ": no stamina" : ": mid-swing";
						logger::info("[opportunity] {} {} {}{}", Who(actor), Swung ? "swings at" : "couldn't swing at", Who(Opening), Why);
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
					// Same stale rule as the light: unranked while it can't start, dropped after
					// StaleSwingSeconds.
					ActionQueueIter->second.priority = 0;
					ActionQueueIter->second.timeLeft -= delta;
					if (ActionQueueIter->second.timeLeft < -StaleSwingSeconds)
					{
						ActionQueueIter->second.toDo = Actions::None;
					}
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
						// On the guard it dodged in on: a switch still in flight would spend the charge.
						{
							std::unique_lock DirLock(DirectionQueueMtx);
							DirectionQueue.erase(ActionQueueIter->first);
						}
						DirectionHandler::GetSingleton()->CancelQueuedDirection(actor);
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
			UpdateTimerIter->second -= delta;
		}
		UpdateTimerIter++;


	}
	UpdateTimerMtx.unlock();


}

void AIHandler::LogExchange(RE::Actor* attacker, RE::Actor* defender, const char* outcome, float damage)
{
	if (!Settings::VerboseLogging || !attacker || !defender || (!attacker->IsPlayerRef() && !defender->IsPlayerRef()))
	{
		return;
	}
	// The defender's record is about its own target's swing; anyone else's hit is untracked.
	const bool Tracked = GetCombatTarget(defender) == attacker;
	std::unique_lock Lock(DifficultyMapMtx);
	auto It = DifficultyMap.find(defender->GetHandle());
	AIDifficulty* Diff = (It != DifficultyMap.end() && Tracked) ? &It->second : nullptr;
	if (Diff)
	{
		Diff->swingContact = true;
	}
	LogExchangeLine(attacker, defender, Diff, outcome, damage);
}

void AIHandler::LogExchangeLine(RE::Actor* attacker, RE::Actor* defender, const AIDifficulty* diff, const char* outcome, float damage) const
{
	if (!Settings::VerboseLogging)
	{
		return;
	}
	auto* Dir = DirectionHandler::GetSingleton();
	const bool Live = diff && diff->swingElapsed >= 0.f;
	const int Line = Live && diff->swingLine >= 0 ? diff->swingLine : static_cast<int>(Dir->GetCurrentDirection(attacker));
	const bool Power = Live ? diff->swingWasPower : IsPowerAttacking(attacker);
	const bool Landed = std::strcmp(outcome, "hit") == 0 || std::strcmp(outcome, "unblockable") == 0;
	const bool Chained = Live ? diff->swingChained : Dir->GetComboStep(attacker) > (Landed ? 1 : 0);
	std::string Answer = "-";
	if (diff)
	{
		Answer = diff->answerKind;
		if (diff->answerAt >= 0.f)
		{
			Answer += std::format(" due +{:.0f}ms", diff->answerAt * 1000.f);
		}
		if (diff->pressAt >= 0.f)
		{
			Answer += std::format(", press +{:.0f}ms", diff->pressAt * 1000.f);
		}
		if (diff->answerLater)
		{
			Answer += std::format(", then {}", diff->answerLater);
		}
	}
	logger::info("[xchg] {} -> {}: line {} {}{}{} speed {:.2f} | answer {} | guard {} {} | {}{}",
		Who(attacker), Who(defender), Line, Power ? "power" : "light",
		Dir->IsUnblockable(attacker) ? " unblockable" : "", Chained ? " chained" : "",
		AttackHandler::EffectiveAttackSpeed(attacker), Answer,
		IsGuardUp(defender) ? "up" : "down", Dir->HasBlockAngle(attacker, defender) ? "on angle" : "off angle",
		outcome, damage > 0.f ? std::format(" dmg {:.1f}", damage) : std::string());
}


