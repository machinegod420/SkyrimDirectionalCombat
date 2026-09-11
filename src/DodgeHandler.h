#pragma once
#include "Utils.h"
#include "Direction.h"
#include <shared_mutex>
#include "parallel_hashmap/phmap.h"

enum class DodgeDirection
{
	Forward,
	Backward,
	Left,
	Right,
	ForwardLeft,
	ForwardRight,
	BackwardLeft,
	BackwardRight
};

class DodgeHandler
{
public:
	DodgeHandler()
	{
	}

	static DodgeHandler* GetSingleton()
	{
		static DodgeHandler obj;
		return std::addressof(obj);
	}

	void Initialize();
	void Cleanup();

	bool CanDodge(RE::Actor* actor);
	bool IsDodging(RE::Actor* actor);
	float GetDodgeCost(RE::Actor* actor);


	void ApplyImpulse(RE::Actor* actor, DodgeDirection direction);

	// High-level dodge dispatcher. Routes to whichever dodge system is active
	// (Custom → ApplyImpulse, DMCO → graph events, None → no-op). Callers
	// (player input, AI, etc.) use this so they don't have to know about each
	// system's specifics.
	void TriggerDodge(RE::Actor* actor, DodgeDirection direction);

	// called from bhkCharacterStateOnGround::SimulateStatePhysics hook (per physics tick)
	void ApplyOnGround(RE::bhkCharacterController* controller);
	// called from bhkCharacterStateInAir::SimulateStatePhysics hook to abort airborne dodges
	void CancelDodgeOnAir(RE::bhkCharacterController* controller);

private:
	struct DodgeState
	{
		DodgeDirection direction;
		float speed;
		float animSpeedPeak;      // SpeedSampled peak for this dodge (per-direction)
		float timeTotal;
		float elapsed;
		float rampEnd;            // ramp portion (fraction of timeTotal); longer when starting from idle
		bool active;
		RE::NiPoint3 startPos;
		float startSpeedSampled;  // pre-dodge value, used as decay target
		bool footstepFired;       // one-shot guard so the dodge fires exactly one footstep
	};

	phmap::flat_hash_map<RE::ActorHandle, DodgeState> ActiveDodges;
	std::shared_mutex DodgeMtx;

	// Post-dodge slow: writes a temporary-slot delta to SpeedMult AV during the
	// slow window. m_appliedSlowDelta tracks the exact value we wrote so we can
	// reverse it precisely. Cleanup reverts unconditionally if delta != 0,
	// covering save-mid-slow desyncs where in-memory state is lost.
	bool m_slowActive = false;
	float m_slowRemaining = 0.0f;
	float m_appliedSlowDelta = 0.0f;
	std::mutex m_slowMtx;

	void StartPostDodgeSlow();
	void TickPostDodgeSlow(float dt);
};
