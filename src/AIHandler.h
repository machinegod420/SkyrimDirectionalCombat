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
	AIHandler()
	{
		EnableRaceKeyword = nullptr;
	}
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
		ResetState
	};

	static AIHandler* GetSingleton()
	{
		static AIHandler obj;
		return std::addressof(obj);
	}
	bool RaceForcedDirectionalCombat(RE::Actor* actor)
	{
		// can cache value in <race,bool> dictionary
		return (actor->GetRace()->HasKeyword(EnableRaceKeyword));
	}
	void Update(float delta);
	void RunActor(RE::Actor* actor, float delta);

	// This block is called from other thread
	// this means that they should lock!
	void TryRiposteExternalCalled(RE::Actor* actor, RE::Actor* attacker);
	void TryBlockExternalCalled(RE::Actor* actor, RE::Actor* attacker);
	bool ShouldAttackExternalCalled(RE::Actor* actor, RE::Actor* target);
	void SignalGoodThingExternalCalled(RE::Actor* actor, Directions attackedDir);
	void SignalBadThingExternalCalled(RE::Actor* actor, Directions attackedDir);
	// Disconfirmation drain for a wrong-line block, called from HandleBlock's
	// wrong-angle strip (the prehit hook). It must live there: the strip
	// clears IsBlocking before the hit event reaches SignalBadThing, so a
	// wasBlocking check at hit time can never see the wrong-line case
	// (audit-confirmed dead path).
	void SignalWrongLineBlockExternalCalled(RE::Actor* actor);
	void SwitchTargetExternalCalled(RE::Actor* actor, RE::Actor* newTarget);
	// function for us to help estimate how long it takes for a power attack to land for timing conditioning
	void NotifyPowerAttackHitExternalCalled(RE::Actor* actor);

	void DidAttackExternalCalled(RE::Actor* actor);
	// Copies this actor's per-direction conditioning
	void GetGuardConditioningExternalCalled(RE::Actor* actor, std::array<int, 4>& outConditioning, float& outConfidence);
	// Rewrites the engine's combat advance radii (the inner/outer engagement
	// band the vanilla AI paths to) with this actor's dynamic spacing intent.
	void ApplySpacingExternalCalled(RE::Actor* actor, float* a_inOutInner, float* a_inOutOuter);
	//

	void SwitchToNewDirection(RE::Actor* actor, RE::Actor* target, float TargetDistSQ);
	bool TryAttack(RE::Actor* actor, bool force);
	bool TryPowerAttack(RE::Actor* actor);

	
	bool CanAct(RE::Actor* actor) const;


	// WARNING - lower level function do not lock
	Difficulty CalcAndInsertDifficulty(RE::Actor* actor);
	void DirectionMatchTarget(RE::Actor* actor, RE::Actor* target, bool force);
	inline void IncreaseBlockChance(RE::Actor* actor, Directions dir, int percent, int modifier);
	void ReduceDifficulty(RE::Actor* actor);
	void ResetDifficulty(RE::Actor* actor);
	void SwitchToNextAttack(RE::Actor* actor, bool force);
	Directions GetNextAttack(RE::Actor* actor);

	// Load attackdata so our attackstart event is processed correctly as an attack
	bool LoadCachedAttack(RE::Actor* actor, bool force);
	bool LoadCachedPowerAttack(RE::Actor* actor);
	// WARNING



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
	RE::BGSKeyword* EnableRaceKeyword = nullptr;
	RE::BGSAction* RightPowerAttackAction = nullptr;
	// TargetDelay: fire this many seconds from now instead of at the actor's
	// normal action timer. Floored by that timer, so it can only ever DELAY an
	// action, never rush one — a computed deadline, not a discount. Negative
	// means "as soon as the action timer allows", which is every other caller.
	void AddAction(RE::Actor* actor, Actions toDo, Directions attackedDir = Directions::TR, bool force = false, int priority = 0, DodgeDirection dodgeDir = DodgeDirection::Backward, float TargetDelay = -1.f);
	float CalcUpdateTimer(RE::Actor* actor);
	float CalcActionTimer(RE::Actor* actor);
	void DidAct(RE::Actor* actor);

	Actions GetQueuedAction(RE::Actor* actor);

	struct Action
	{
		Actions toDo;
		float timeLeft;
		// for pro block
		Directions targetDir;
		bool wasForced;
		int priority;
		// 8-way direction for Actions::Dodge — set at queue time so the
		// decision (retreat/advance/reactive) lives in RunActor rather than
		// at execution. Default Backward matches legacy "dodge to retreat"
		// intent.
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

	enum class AIState
	{
		Attacking,
		Defending
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
		std::vector<Directions> lastDirectionsTracked;
		// check to see how much times we had to try to match target direction
		// as a confustion factor
		int numTimesDirectionsSwitched = 0;
		// Belief accumulation multiplier: how consistently this target has been
		// switching lines. Deliberately separate from numTimesDirectionsSwitched,
		// which is also the offense-exit guard and the block-limbo countdown —
		// leaving/entering the defending state reset the multiplier and pinned
		// conditioning at its slowest rate.
		int conditioningStreak = 0;
		int numTimesDirectionSame = 0;
		// Carries the sub-point part of a tick's belief drain. Beliefs are
		// integers and a tick is short, so rounding each tick quantized the
		// drain to multiples of 1/tick — at Legendary the only reachable rates
		// were 6.7, 13.3 and 20/s, with nothing usable in between.
		float beliefDrainRemainder = 0.f;
		// Weights the cascade last rolled against. Cached so the HUD renders
		// what the AI acted on rather than recomputing and drifting from it.
		int lastCascadeWeights[4] = {};
		// Learned draw-to-hit for this target's power attacks, seconds. 0 until
		// one has connected. Measured rather than assumed because windup is an
		// animation property and attack speed is ini-configurable, so there is
		// no number to look up — but it is the same for a given opponent, so it
		// can be learned from the very attacks it is failing to answer.
		float powerWindupEstimate = 0.f;
		// Lifecycle of the target's CURRENT swing, armed at the observed start
		// edge and closed at the end edge. Serves two readers: the windup
		// measurement above, and whiff detection — a swing that ends without
		// having connected is a committed miss, which is the moment entering
		// measure was a mistake.
		float swingElapsed = -1.f;   // negative when not tracking
		bool swingWasPower = false;  // set from IsPowerAttacking at the start edge
		// Seconds left of the opening a whiffed swing bought. Biases the attack
		// rate rather than granting a free hit
		RE::NiPointer<RE::BGSAttackData> cachedBasicAttackData = nullptr;
		RE::NiPointer<RE::BGSAttackData> cachedPowerAttackData = nullptr;

		// this stores the amount of time since the target last switched guards
		// if the target does not switch enough, then we can change guards to try to snipe
		//float targetSwitchTimer = 0.f;
		Directions targetLastDir;
		bool defending = false;
		// seconds spent in the defending state; zeroed on leaving it. Drives
		// the personality-paced offense exit.
		float defendTime = 0.f;
		// Last DirectionMatchTarget outcome, for the debug overlay. String
		// literal, never freed.
		const char* lastDecisionKind = "none";

		// percent chance it may switch to a specific direction to emulate anticipation of attacks
		phmap::flat_hash_map<Directions, int> directionChangeChance;

		// AI should attack in specific patterns, just like people do
		// todo: use uint16_t, every 2 bits represents a direction for fast access and generation of patterns
		// the actual random generation for this would be difficult as you would want a spread of bits versus true random
		std::vector<Directions> attackPattern;
		unsigned currentAttackIdx = 0u;

		// Each NPC has its own rand implementation to ensure that each NPC acts deterministically
		std::mt19937 npcRand;

		float CurrentWeaponLengthSQ = 0.f;

		float DodgeCooldown = 0.f;

		float BashCooldown = 0.f;

		// Combat personality modifiers see cpp file
		float aggressionMod = 0.f;       // attack frequency, retreat reluctance
		float patienceMod = 0.f;         // willingness to wait vs. press
		float baitTendency = 0.f;        // holds same direction longer to bait
		float cautionMod = 0.f;          // dodge/block frequency
		float powerAttackTendency = 0.f; // prefers power attacks over light
		float feintTendency = 0.f;       // probability of feinting on an attack
		// points into kPersonalityArchetypes (static storage). Debug display only.
		const char* archetype = "?";

		// Whether the previous DirectionMatchTarget call had force set (target
		// mid-attack). Edge-detects the start of a swing so the force
		// belief-spike fires once per attack instead of once per tick —
		// per-tick spiking compounded under the belief-budget model and made
		// a single attack teach the AI far too fast.
		bool lastCallForced = false;

		// Perception layer: update almoste every frame in RunActor, consumed by
		// DirectionMatchTarget on the decision tick. Humans see continuously
		// and decide discretely — sampling the target's guard only on
		// decision ticks made sub-tick mixups invisible to the conditioning
		// system (switch away and back between ticks = never happened).
		Directions lastObservedDirection = Directions::TR;
		bool hasObservation = false;
		std::array<Directions, 4> observedSwitchesFrom{};
		int observedSwitchCount = 0;
		// edge detector for the target starting a swing (attention capture +
		// reflex block + per-swing spike re-arm). Tracks meleeAttackState,
		// NOT IsAttacking: combo chains never drop IsAttacking between
		// swings, so a boolean edge fired once per CHAIN and every follow-up
		// attack arrived with no capture, no reflex, and no spike.
		int lastObservedAttackState = 0;
		float perceptionAccum = 0.f;
		// One preempt per decision cycle. A second stimulus arriving while a
		// decision is pending waits for it rather than restarting the clock.
		bool preemptSpent = false;
		// wall-clock seconds since the target last changed guard line,
		// accumulated per-frame by the perception layer. Drives the
		// time-based acquisition gate: adjustment is suppressed within the
		// commit window of a line change, guaranteed after floor + fixation.
		// Starts at 0 (per-target reset does the same): acquiring a new
		// opponent is an attention shift and pays the commit window.
		float timeSinceLineChange = 0.f;
		// whose guard the perception state describes — on retarget the whole
		// perception block resets, otherwise the first frame against a new
		// target fabricates a phantom switch from the OLD target's guard
		uint32_t observedTargetId = 0;

		// Dynamic spacing. spacingTarget is recomputed on the decision tick from
		// combat state and personality; 
		float spacingTarget = 1.f;
		float spacingMult = 1.f;

		// Multiplier on the TARGET's reach as this actor perceives it. Players
		// eyeball reach; the AI can read the exact number, so it gets a
		// deterministic error instead. Tightens with difficulty.
		float reachMisjudge = 1.f;
		// target's reach squared, from Actor::GetReach each decision tick
		float targetReachSQ = 0.f;

	};

	// Named decision predicates shared by the block-decision sites (reflex
	// block, defend-branch raise, read-commit carry).
	float CalcSpacingTarget(RE::Actor* actor, RE::Actor* target, const AIDifficulty& diff,
		float ownStaminaRatio, float enemyStaminaRatio) const;

	static bool IsBlockableSwing(RE::Actor* target);
	bool IsPreparedToBlock(RE::Actor* actor, const AIDifficulty& diff) const;
	bool CanAnswerLine(RE::Actor* actor, RE::Actor* target, const AIDifficulty& diff) const;
	// Seconds a target's guard switch is protected for, derived from this
	// actor's decision latency rather than set independently. Reads the tier
	// timer off diff.difficulty instead of CalcUpdateTimer so it stays const
	// and skips the map insert — callers already hold DifficultyMapMtx.
	float CommitWindow(const AIDifficulty& diff) const;
	// Folds a completed power-swing observation into powerWindupEstimate.
	// Caller must hold DifficultyMapMtx.
	void RecordPowerWindupSample(AIDifficulty& diff) const;

	phmap::flat_hash_map<RE::TESRace*, RE::NiPointer<RE::BGSAttackData>> RaceToNormalAttack;
	phmap::flat_hash_map<RE::TESRace*, RE::NiPointer<RE::BGSAttackData>> RaceToPowerAttack;

	RE::NiPointer<RE::BGSAttackData> FindActorAttackData(RE::Actor* actor);
	RE::NiPointer<RE::BGSAttackData> FindActorPowerAttackData(RE::Actor* actor);

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



	int NumPlayerAttackers;
	mutable std::shared_mutex AIHandlerDataMtx;

	public:
	PRECISION_API::IVPrecision3* Precision = nullptr;
};