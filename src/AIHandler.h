#pragma once

#include <unordered_map>
#include <vector>
#include <array>
#include <shared_mutex>
#include "Direction.h"
#include "DodgeHandler.h"
#include "3rdparty/PrecisionAPI.h"
#include "Utils.h"
#include "parallel_hashmap/phmap.h"


// This is an important note about the philosophy of the mod and why the ai is so stupidly fucking complicated
// The point of this AI isn't to be an RNG number "you attack and randomly it succeeds or fails". It's RNG based
// but it attempts to model human reaction, perception, and conditioning, because you should be able to outsmart
// the opponent or figure out its attack patterns. Thats why theres so much shit, the "did i block this attack"
// isnt just a direct dice roll, but downstream of a lot of variables that determines how the ai reacts to the attack


class AIHandler
{
public:
	enum class Difficulty : uint16_t
	{
		Uninitialized = 0,
		VeryEasy,
		Easy,
		Normal,
		Hard,
		VeryHard,
		Legendary
	};
	void InitializeValues(PRECISION_API::IVPrecision3* precision);
	enum class Actions
	{
		None,
		Attack,
		Block,
		UnblockRiposte,
		UnblockStartFeint,
		StartFeint,
		EndFeint, 
		Bash,
		ReleaseBash,
		EndBlock,
		PowerAttack,
		Dodge,
		Followup,
		ResetState,
		FeintFollowup,
		OpportunityAttack,
		DashAttack
	};

	static AIHandler* GetSingleton()
	{
		static AIHandler obj;
		return std::addressof(obj);
	}
	void Update(float delta);
	void RunActor(RE::Actor* actor, float delta);

	// This block is called from other thread
	// this means that they should lock!
	void TryRiposteExternalCalled(RE::Actor* actor, RE::Actor* attacker);
	void TryBlockExternalCalled(RE::Actor* actor, RE::Actor* attacker);
	// Whether a vanilla-initiated attack may go ahead. False in a directional
	// fight, where this AI swings on its own schedule.
	bool ShouldAttackExternalCalled(RE::Actor* actor, RE::Actor* target);
	void SignalGoodThingExternalCalled(RE::Actor* actor, Directions attackedDir);
	void SignalBadThingExternalCalled(RE::Actor* actor, Directions attackedDir);
	// Disconfirmation drain for a wrong-line block. Called from HandleBlock's strip, which
	// clears IsBlocking before SignalBadThing could see the wrong line.
	void SignalWrongLineBlockExternalCalled(RE::Actor* actor);
	// Called instead of the decision tick while another mod holds this actor out
	// of the fight, so the first tick after release can undo what it was left in.
	void NotifyHeldOff(RE::Actor* actor);
	// The target's swing connected with this actor: records that swing's windup, power or light.
	// timeIt false: met a counter before its hit frame, so its elapsed isn't a windup.
	void NotifyPowerAttackHitExternalCalled(RE::Actor* actor, RE::Actor* attacker, bool timeIt = true);
	// A swing of this actor's is queued to fire shortly.
	bool HasSwingQueued(RE::Actor* actor);

	void DidAttackExternalCalled(RE::Actor* actor);
	// Copies this actor's per-direction conditioning
	void GetGuardConditioningExternalCalled(RE::Actor* actor, std::array<int, 4>& outConditioning, float& outConfidence);
	// Rewrites the engine's combat advance radii (the inner/outer engagement
	// band the vanilla AI paths to) with this actor's dynamic spacing intent.
	void ApplySpacingExternalCalled(RE::Actor* actor, float* a_inOutInner, float* a_inOutOuter);
	//

	void SwitchToNewDirection(RE::Actor* actor, RE::Actor* target, float TargetDistSQ);
	bool TryAttack(RE::Actor* actor);
	bool TryPowerAttack(RE::Actor* actor);

	
	bool CanAct(RE::Actor* actor) const;


	// These five expect the caller to hold DifficultyMapMtx.
	Difficulty CalcAndInsertDifficulty(RE::Actor* actor);
	void DirectionMatchTarget(RE::Actor* actor, RE::Actor* target, bool force);
	inline void IncreaseBlockChance(RE::Actor* actor, Directions dir, int percent, int modifier);
	void ReduceDifficulty(RE::Actor* actor);
	void SwitchToNextAttack(RE::Actor* actor);
	// Chain graph row for a swing with no landed line in the ring: the opener.
	static constexpr int OpenerRow = 4;
	// Player habit profile: the player's own chain transitions (last landed line,
	// or OpenerRow, to the next swing's line), per weapon set, learned across every
	// fight and used to seed each NPC's chain graph when it engages the player.
	void RecordPlayerChainTransition(WeaponSet a_set, int a_from, Directions a_to);
	// Kept in the co-save, so a reload rewinds what the AI learned since.
	static constexpr std::uint32_t PlayerHabitRecord = 'HABT';
	static constexpr std::uint32_t PlayerHabitVersion = 1;
	void SavePlayerHabit(SKSE::SerializationInterface* a_intfc);
	void LoadPlayerHabit(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void ResetPlayerHabit();
	// Player swing stats, to read in the log only: nothing decides on them.
	static constexpr std::uint32_t PlayerStatsRecord = 'PSTA';
	static constexpr std::uint32_t PlayerStatsVersion = 1;
	static constexpr std::uint32_t PlayerStaminaRecord = 'PSTM';
	static constexpr std::uint32_t PlayerStaminaVersion = 1;
	static constexpr std::uint32_t PlayerFeintRecord = 'PSFT';
	static constexpr std::uint32_t PlayerFeintVersion = 1;
	// A feint that fired, on a swing at a_step (as RecordPlayerSwing counts it).
	void RecordPlayerFeint(WeaponSet a_set, int a_step, bool a_power);
	void LoadPlayerFeintStats(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	// How the player's swings end and how the player's defence fares. Saved values: append only.
	static constexpr std::uint32_t PlayerOutcomeRecord = 'PSOC';
	static constexpr std::uint32_t PlayerOutcomeVersion = 1;
	enum class SwingOutcome : int { Landed, Blocked, Clashed, Masterstruck, Feinted };
	enum class DefenseOutcome : int { Blocked, Parried, GuardMissed, NoGuard, Unblockable, Countered };
	// Opens the player's current swing; the first outcome resolves it, later ones are ignored.
	void BeginPlayerSwing(WeaponSet a_set, int a_step, Directions a_line);
	void ResolvePlayerSwing(SwingOutcome a_outcome);
	// An NPC swing at the player; a_chained: the NPC's combo had a landed step.
	void RecordPlayerDefense(WeaponSet a_playerSet, bool a_power, bool a_chained, DefenseOutcome a_outcome);
	void LoadPlayerOutcomeStats(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	// a_step: 0 the opener, else landed hits in the combo run. a_powerAffordable: stamina
	// covered a full power's cost at swing start.
	void RecordPlayerSwing(WeaponSet a_set, int a_step, bool a_power, float a_distance, float a_staminaRatio, bool a_powerAffordable);
	// Writes the stats, stamina and feint records.
	void SavePlayerStats(SKSE::SerializationInterface* a_intfc);
	void LoadPlayerStats(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void LoadPlayerStaminaStats(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version, std::uint32_t a_length);
	void ResetPlayerStats();
	void LogPlayerStats(const char* a_when);



	// if something stopped combat, make sure they get removed from the map
	void RemoveActor(RE::ActorHandle actor)
	{
		DifficultyMapMtx.lock();
		DifficultyMap.erase(actor);
		DifficultyMapMtx.unlock();

		UpdateTimerMtx.lock();
		UpdateTimer.erase(actor);
		UpdateTimerMtx.unlock();

		ActionQueueMtx.lock();
		ActionQueue.erase(actor);
		ActionQueueMtx.unlock();

		DirectionQueueMtx.lock();
		DirectionQueue.erase(actor);
		DirectionQueueMtx.unlock();
	}



	void Cleanup();
private:
	// TargetDelay: fire this many seconds from now instead of at the actor's
	// normal action timer. Floored by that timer, so it can only ever DELAY an
	// action, never rush one — a computed deadline, not a discount. Negative
	// means "as soon as the action timer allows", which is every other caller.
	void AddAction(RE::Actor* actor, Actions toDo, bool force = false, int priority = 0, DodgeDirection dodgeDir = DodgeDirection::Backward, float TargetDelay = -1.f);
	float CalcUpdateTimer(RE::Actor* actor);
	float CalcActionTimer(RE::Actor* actor);
	void DidAct(RE::Actor* actor);

	Actions GetQueuedAction(RE::Actor* actor);

	struct Action
	{
		Actions toDo;
		float timeLeft;
		int priority;
		// 8-way direction for Actions::Dodge
		DodgeDirection dodgeDir = DodgeDirection::Backward;
		// CalcActionTimer's result at queue time. Kept so Update can reuse it
		// without touching DifficultyMap, which it cannot lock while holding
		// ActionQueueMtx.
		float baseTimer = 0.f;
		// How long this attack has been held back waiting for a guard change to
		// land. Bounded, because an actor that keeps re-deciding its line would
		// otherwise never swing at all.
		float waitedForDir = 0.f;
	};

	struct AIDifficulty
	{
		Difficulty difficulty = Difficulty::Uninitialized;
		// this exists because depending on the circumstances of the fight, we may want to make the AI 
		// more challenging or less challenging based on its mistake ratio
		// Increment we do a successful action (landing an attack, blocking)
		// Decrement if we fail an action (missing a block, attack was blocked)
		float mistakeRatio = 0.f;
		// keep a list of the last 5 directions attacked by 
		// if there is an obvious pattern, become harder or easier
		// todo: circular array
		std::vector<Directions> lastDirectionsEncountered;

		// which direction we last saw, we try to match that
		Directions lastDirectionTracked;
		// Recent line changes seen on decision ticks (+1 per change, -1 per hold): the
		// commit window's churn.
		int numTimesDirectionsSwitched = 0;
		// Belief accumulation multiplier: how consistently this target has been switching
		// lines. Kept apart from numTimesDirectionsSwitched, which every mode change resets.
		int conditioningStreak = 0;
		// Holds in a row seen on decision ticks; the release-block branch needs 0.
		int numTimesDirectionSame = 0;
		// Carries the sub-point part of a tick's belief drain. Beliefs are
		// integers and a tick is short, so rounding each tick quantized the
		// drain to multiples of 1/tick — at Legendary the only reachable rates
		// were 6.7, 13.3 and 20/s, with nothing usable in between.
		float beliefDrainRemainder = 0.f;
		float holdAccrualRemainder = 0.f;
		// Weights the cascade last rolled against. Cached so the HUD renders
		// what the AI acted on rather than recomputing and drifting from it.
		int lastCascadeWeights[4] = {};
		// Chain flow graph: [a][b] is how often the target, with a landed hit
		// from a still in its combo ring, swings next from b. Row OpenerRow is
		// the ring-empty swing, on top of directionChangeChance.
		int chainEdges[OpenerRow + 1][4] = {};
		// The edge the target's current swing walked, for the lose-shift on a block.
		int lastChainFrom = -1;
		int lastChainTo = -1;
		// chainEdges started from the player habit profile for this target.
		bool chainSeeded = false;
		// Learned draw-to-hit for this target's power attacks at base attack speed,
		// seconds; 0 until one has connected.
		float powerWindupEstimate = 0.f;
		// Distance the samples behind that estimate landed at, smoothed the same
		// way. Contact comes later the further out a swing connects, so the
		// estimate only transfers to the current gap once that is corrected for.
		float powerWindupDist = 0.f;
		// The same pair for lights, learned the same way. Feeds the masterstrike
		// aim; the parry can't use it, since a light's parry window is inside reaction.
		float lightWindupEstimate = 0.f;
		float lightWindupDist = 0.f;
		// Running share of this opponent's swings that were power attacks. When
		// the tier's read fails, the block is timed off this instead of the truth.
		float powerHabit = 0.f;
		// Running share of this opponent's swings that were feinted, [light, power].
		// Scales the masterstrike roll down, so a feinter can't bait it all fight.
		float feintHabit[2] = {};
		// The target's current swing, from its start edge to its end edge: timed for the
		// windup estimates and the masterstrike, closed with the feint habit.
		float swingElapsed = -1.f;   // negative when not tracking
		bool swingWasPower = false;  // set from IsPowerAttacking at the start edge
		// The target's WeaponSpeedMult at the start edge. Windups are learned at base
		// speed (elapsed x this) and predicted for a swing as base / its speed.
		float swingSpeed = 1.f;
		bool defending = false;
		// Held out of the fight since the last decision tick.
		bool heldOff = false;
		// seconds spent in the defending state; zeroed on leaving it. Drives
		// the personality-paced offense exit.
		float defendTime = 0.f;
		// Seconds inside the judge radius this combat, never reset.
		float fightSeconds = 0.f;
		float graphIdleAttacking = 0.f;
		// Last DirectionMatchTarget outcome, for the debug overlay. String
		// literal, never freed.
		const char* lastDecisionKind = "none";

		// percent chance it may switch to a specific direction to emulate anticipation of attacks
		phmap::flat_hash_map<Directions, int> directionChangeChance;

		// This NPC's habit: a loop of attack lines, fixed by its handle.
		std::vector<Directions> attackPattern;
		unsigned currentAttackIdx = 0u;
		// The current entry is a deliberate repeat, so the landed-line skip lets it through.
		bool repeatNext = false;

		// Each NPC has its own rand implementation to ensure that each NPC acts deterministically
		std::mt19937 npcRand;

		float CurrentWeaponLengthSQ = 0.f;

		float DodgeCooldown = 0.f;

		float BashCooldown = 0.f;

		// Combat personality modifiers see cpp file
		float aggressionMod = 0.f;       // attack frequency, retreat reluctance
		float baseAggression = 0.f;      // aggressionMod before the fight-situation shift
		float aggressionShift = 0.f;     // eased toward the health and stamina advantage
		float patienceMod = 0.f;         // willingness to wait vs. press
		float baitTendency = 0.f;        // holds same direction longer to bait
		float cautionMod = 0.f;          // dodge/block frequency
		float baseCaution = 0.f;         // cautionMod before the shift, which it takes negated
		float powerAttackTendency = 0.f; // prefers power attacks over light
		float feintTendency = 0.f;       // probability of feinting on an attack
		// points into kPersonalityArchetypes (static storage). Debug display only.
		const char* archetype = "?";

		// Whether the previous decision tick, in judge range, saw the target swinging
		// at this actor. The force belief-spike fires only on a swing's first tick:
		// per-tick spiking made a single attack teach the AI far too fast.
		bool sawSwingLastTick = false;

		// Perception: the target's guard sampled every 20 ms in RunActor and read by
		// DirectionMatchTarget on its tick, so switches between ticks still condition.
		Directions lastObservedDirection = Directions::TR;
		bool hasObservation = false;
		std::array<Directions, 4> observedSwitchesFrom{};
		int observedSwitchCount = 0;
		// The target's attack state at the last sample, for its swing-start edge.
		// meleeAttackState, not IsAttacking, which stays on across a chain.
		int lastObservedAttackState = 0;
		float perceptionAccum = 0.f;
		// One preempt per decision cycle. A second stimulus arriving while a
		// decision is pending waits for it rather than restarting the clock.
		bool preemptSpent = false;
		// Seconds since the target last changed line; the commit window counts from it.
		// Starts at 0 against a new target.
		float timeSinceLineChange = 0.f;
		// Whose guard the perception state describes; a retarget resets it.
		uint32_t observedTargetId = 0;

		// Dynamic spacing. spacingTarget is recomputed on the decision tick from
		// combat state and personality; 
		float spacingTarget = 1.f;
		float spacingMult = 1.f;
		// Diagnostic: seconds until the next [tick] status line.
		float statusLogTimer = 0.f;

		// Multiplier on the TARGET's reach as this actor perceives it. Players
		// eyeball reach; the AI can read the exact number, so it gets a
		// deterministic error instead. Tightens with difficulty.
		float reachMisjudge = 1.f;
		// target's reach squared, from Actor::GetReach each decision tick
		float targetReachSQ = 0.f;
		// Their learned reach per [power][line]: the furthest connection, 0 = unlearned.
		float targetReachLearned[2][4] = {};
		// Their last swing: start gap (-1 once it connected), my position then, its line.
		float swingStartGap = -1.f;
		RE::NiPoint3 swingStartOwnPos{};
		int swingLine = -1;

	};

	// Named decision predicates shared by the block-decision sites (reflex
	// block, defend-branch raise, read-commit carry).
	float CalcSpacingTarget(RE::Actor* actor, RE::Actor* target, const AIDifficulty& diff,
		float ownStaminaRatio, float enemyStaminaRatio, float targetDistSQ) const;

	static bool IsBlockableSwing(RE::Actor* target);
	// A companion's target is usually swinging at the player, so a player swing
	// only counts when the defender is in its arc.
	static bool IsSwingingAt(RE::Actor* attacker, RE::Actor* defender);
	// A blockable swing at the defender that hasn't reached its hit frame. Past it
	// the swing is recovery, which is an opening, not a threat.
	static bool IsIncomingSwing(RE::Actor* attacker, RE::Actor* defender);
	bool IsPreparedToBlock(RE::Actor* actor, RE::Actor* target, const AIDifficulty& diff) const;
	float TargetReachEstimate(RE::Actor* target, const AIDifficulty& diff, Directions line, bool power) const;
	bool CanAnswerLine(RE::Actor* actor, RE::Actor* target, const AIDifficulty& diff) const;
	// Seconds a target's guard switch is protected for, derived from this
	// actor's decision latency rather than set independently. Reads the tier
	// timer off diff.difficulty instead of CalcUpdateTimer so it stays const
	// and skips the map insert — callers already hold DifficultyMapMtx.
	float CommitWindow(const AIDifficulty& diff) const;
	// Folds a completed swing observation, and the gap it landed at, into the
	// power or light windup estimate. Caller must hold DifficultyMapMtx.
	void RecordWindupSample(AIDifficulty& diff, float sampleDist, bool power) const;

	phmap::flat_hash_map<RE::ActorHandle,Action> ActionQueue;
	mutable std::shared_mutex ActionQueueMtx;

	// we've split up guard changes from the action timer to simulate human parallel input types
	struct DirectionAction
	{
		Directions dir;
		bool force;
		float timeLeft;
	};
	phmap::flat_hash_map<RE::ActorHandle, DirectionAction> DirectionQueue;
	mutable std::shared_mutex DirectionQueueMtx;
	// EVERY AI-side guard change goes through here. Two paths with different
	// latencies would both write DirectionTimers, so a slow one could overwrite
	// a fast one that had already committed — the AI picking an attack line and
	// then being dragged off it by an older defensive decision.
	void QueueDirectionSwitch(RE::Actor* actor, Directions dir, bool force);
	// True while a guard change is still in flight, in EITHER phase: the hand
	// still moving (our queue) or the guard mid-transition (DirectionHandler's
	// timer).
	bool HasPendingDirectionSwitch(RE::Actor* actor) const;

	phmap::flat_hash_map<RE::ActorHandle, float> UpdateTimer;
	mutable std::shared_mutex UpdateTimerMtx;

	phmap::flat_hash_map<RE::ActorHandle, AIDifficulty> DifficultyMap;
	mutable std::shared_mutex DifficultyMapMtx;

	phmap::flat_hash_map<Difficulty, float> DifficultyUpdateTimer;
	mutable std::shared_mutex DifficultyUpdateTimerMtx;

	phmap::flat_hash_map<Difficulty, float> DifficultyActionTimer;
	mutable std::shared_mutex DifficultyActionTimerMtx;

	// Decayed transition counts, [set][from][to], for the player habit profile.
	// Lock order: DifficultyMapMtx before PlayerHabitMtx.
	float PlayerHabit[NumWeaponSets][OpenerRow + 1][4] = {};
	std::mutex PlayerHabitMtx;

	// Co-save layout v1, frozen: a new layout is a new struct and version, and
	// LoadPlayerStats keeps converting this one.
	struct PlayerStatsV1
	{
		// [weapon set][step]: 0 the opener, 1-3 landed hits in the combo run (3 = 3 or more).
		std::uint32_t swings[5][4];
		std::uint32_t powers[5][4];
		// [weapon set][light, power][torso distance at swing start, 25 units a bucket, the last 400+]
		std::uint32_t distance[5][2][17];
	};
	PlayerStatsV1 PlayerStats = {};
	// Its own record, so the stats record above stays v1. Frozen the same way.
	struct PlayerStaminaStatsV1
	{
		// [weapon set][light with a full power affordable, light without, power][stamina ratio at swing start, 10% a bucket]
		std::uint32_t stamina[5][3][10];
	};
	PlayerStaminaStatsV1 PlayerStamina = {};
	// Its own record too, frozen the same way.
	struct PlayerFeintStatsV1
	{
		// [weapon set][step, as in PlayerStatsV1][light, power]: feints that fired.
		std::uint32_t feints[5][4][2];
	};
	PlayerFeintStatsV1 PlayerFeints = {};
	// Its own record too, frozen the same way.
	struct PlayerOutcomeStatsV1
	{
		// Player swings with a target: [weapon set][step, as in PlayerStatsV1][line], then how they
		// ended by SwingOutcome. Swings minus all outcomes = no contact (whiffed or interrupted).
		std::uint32_t swings[5][4][4];
		std::uint32_t outcomes[5][4][4][5];
		// NPC swings at the player: [player's weapon set][light, power][NPC opener, NPC chained][DefenseOutcome].
		std::uint32_t defense[5][2][2][6];
	};
	PlayerOutcomeStatsV1 PlayerOutcomes = {};
	struct OpenPlayerSwing
	{
		bool open = false;
		int set = 0;
		int step = 0;
		int line = 0;
	};
	OpenPlayerSwing CurrentPlayerSwing;
	// Guards PlayerStats, PlayerStamina, PlayerFeints, PlayerOutcomes and CurrentPlayerSwing.
	std::mutex PlayerStatsMtx;
	void SeedChainEdges(AIDifficulty& diff, WeaponSet a_set);

	public:
	PRECISION_API::IVPrecision3* Precision = nullptr;
};