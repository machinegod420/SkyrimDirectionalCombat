#include "DodgeHandler.h"
#include "SettingsLoader.h"

namespace
{
	// All dodge tuning. Tune empirically — see CanDodge probe log for SpeedSampled values.

	// --- shared timing ---
	// Total duration of the dodge in seconds.
	constexpr float kDodgeDuration     = 0.5f;
	// Idle-start dodge runs longer so the locomotion graph has wall-clock time
	// to cross-fade idle clip → run clip without looking like a sudden break
	// into a run. Triggered when startSpeedSampled is below kIdleStartThreshold.
	constexpr float kDodgeDurationIdle = 0.6f;

	// --- post-dodge slow (recovery) ---
	// Player movement is multiplied by this for kPostDodgeSlowDuration seconds
	// after a dodge ends. 1.0 = no slow, 0.5 = half speed, etc.
	constexpr float kPostDodgeSlowMult     = 0.5f;
	constexpr float kPostDodgeSlowDuration = 0.2f;
	// Seconds after a player dodge ends before the next one. AI dodges use the
	// cooldown in AIHandler instead.
	constexpr float kPlayerDodgeCooldown = 1.0f;
	// Shortest a slowed dodge gets, as a share of full distance.
	constexpr float kDodgeSlowFloor = 0.6f;
	// Trapezoid shape for SpeedSampled over the dodge:
	//   [0, rampEnd]                ramp from startSpeedSampled to peak (ease-out)
	//   [rampEnd, kDecayStart]      hold at peak
	//   [kDecayStart, 1]            ease back down to startSpeedSampled (ease-in)
	// rampEnd is per-dodge (longer when starting from idle, see below).
	constexpr float kRampEnd            = 0.15f;
	constexpr float kRampEndIdle        = 0.30f;
	constexpr float kDecayStart         = 0.5f;
	// SpeedSampled below this counts as "starting from idle" — picks the longer
	// ramp + duration to give the graph blend room to engage.
	constexpr float kIdleStartThreshold = 50.0f;

	// --- per-direction tuning ---
	// speed         = peak velocityMod injected into the controller (~7-15 range)
	// animSpeedPeak = peak SpeedSampled written to the locomotion graph (~130 run, ~350 sprint)
	// Forward/backward read as "flat" with the same values as sides because the
	// legs and body move the same direction (no contrast). Bumping speed and
	// animSpeed for those is what differentiates them visually from "fast running."
	struct DodgeDirectionTuning
	{
		float speed;
		float animSpeedPeak;
	};
	// Indexed by DodgeDirection enum value — keep ordering in sync.
	constexpr DodgeDirectionTuning kDodgeTuning[] = {
		/* Forward */         { 6, 600.0f },
		/* Backward */        { 5, 600.0f },
		/* Left */            { 6,  350.0f },
		/* Right */           { 6,  350.0f },
		/* ForwardLeft */     { 6,  500.0f },
		/* ForwardRight */    { 6,  500.0f },
		/* BackwardLeft */    { 5,  500.0f },
		/* BackwardRight */   { 5,  500.0f },
	};

	// Direction graph variable mapping (standard Skyrim convention): 8-way
	// blend in 0.125 increments, clockwise starting at Forward=0.000 with
	// cardinal directions on quarter-circle boundaries (Right=0.250,
	// Backward=0.500, Left=0.750). Default is Backward (0.500) — that's
	// what the idle/no-input dodge falls back to.
	constexpr float DodgeDirectionToGraphValue(DodgeDirection d)
	{
		switch (d)
		{
		case DodgeDirection::Forward:       return 0.000f;
		case DodgeDirection::ForwardRight:  return 0.125f;
		case DodgeDirection::Right:         return 0.250f;
		case DodgeDirection::BackwardRight: return 0.375f;
		case DodgeDirection::Backward:      return 0.500f;
		case DodgeDirection::BackwardLeft:  return 0.625f;
		case DodgeDirection::Left:          return 0.750f;
		case DodgeDirection::ForwardLeft:   return 0.875f;
		}
		return 0.500f;
	}

}

void DodgeHandler::Initialize()
{
}

void DodgeHandler::Cleanup()
{
	{
		std::unique_lock lock(DodgeMtx);
		ActiveDodges.clear();
	}
	// Revert any AV delta we left on the player. We check m_appliedSlowDelta
	// rather than m_slowActive because save-mid-slow can desync the two: the
	// AV slot persists across save/reload but in-memory flags reset. Reverting
	// unconditionally on any non-zero delta covers that case.
	{
		std::lock_guard lock(m_slowMtx);
		if (m_appliedSlowDelta != 0.0f)
		{
			if (auto* player = RE::PlayerCharacter::GetSingleton())
			{
				if (auto* avo = player->AsActorValueOwner())
				{
					avo->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, -m_appliedSlowDelta);
				}
			}
			m_appliedSlowDelta = 0.0f;
		}
		m_slowActive = false;
		m_slowRemaining = 0.0f;
		m_playerCooldown = 0.0f;
	}
}

// Player only, at dodge end: starts the dodge cooldown and the post-dodge slow.
void DodgeHandler::StartPostDodgeSlow()
{
	{
		std::lock_guard lock(m_slowMtx);
		m_playerCooldown = kPlayerDodgeCooldown;
	}
	if (kPostDodgeSlowMult >= 1.0f || kPostDodgeSlowDuration <= 0.0f)
	{
		return;  // feature disabled
	}

	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
	{
		return;
	}
	auto* avo = player->AsActorValueOwner();
	if (!avo)
	{
		return;
	}

	std::lock_guard lock(m_slowMtx);

	// Already slowing? Just refresh the timer; don't double-apply.
	if (m_slowActive)
	{
		m_slowRemaining = kPostDodgeSlowDuration;
		return;
	}

	// SpeedMult baseline is 100. Convert mult (e.g., 0.6) to a temporary-slot
	// delta: bonus = (mult * 100) - 100. So 0.6 → -40.
	const float delta = (kPostDodgeSlowMult * 100.0f) - 100.0f;
	avo->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, delta);

	m_appliedSlowDelta = delta;
	m_slowActive = true;
	m_slowRemaining = kPostDodgeSlowDuration;
}

void DodgeHandler::TickPostDodgeSlow(float dt)
{
	std::lock_guard lock(m_slowMtx);
	if (dt > 0.0f && m_playerCooldown > 0.0f)
	{
		m_playerCooldown = std::max(0.0f, m_playerCooldown - dt);
	}
	if (!m_slowActive || dt <= 0.0f)
	{
		return;
	}

	m_slowRemaining -= dt;
	if (m_slowRemaining > 0.0f)
	{
		return;
	}

	// Expired — reverse the exact delta we wrote.
	if (auto* player = RE::PlayerCharacter::GetSingleton())
	{
		if (auto* avo = player->AsActorValueOwner())
		{
			avo->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, -m_appliedSlowDelta);
		}
	}
	m_slowActive = false;
	m_slowRemaining = 0.0f;
	m_appliedSlowDelta = 0.0f;
}

bool DodgeHandler::CanDodge(RE::Actor* actor)
{
	if (!actor || !actor->AsActorState()) {
		return false;
	}

	if (IsDodging(actor)) {
		// A dodge lasts well under a second; one still running after this never ended.
		constexpr float StuckDodgeSeconds = 3.f;
		const float Since = SecondsSinceDodge(actor);
		if (actor->IsPlayerRef() && Since > StuckDodgeSeconds) {
			logger::warn("[dodge] {} refused: a dodge started {:.1f}s ago still counts as running", Who(actor), Since);
		}
		return false;
	}

	if (actor->AsActorState()->actorState2.staggered || actor->AsActorState()->actorState1.swimming) {
		return false;
	}

	if (actor->IsAttacking()) {
		return false;
	}

	// Reject if the actor is in any full-body locked animation. IsAnimationDriven
	// alone misses several common cases: killmoves, sitting/sleeping, ragdoll/
	// getup, bleedout, paralysis, paired synced animations. 
	if (IsLockedInAnimation(actor)) {
		return false;
	}

	float currentStamina = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kStamina);
	float cost = GetDodgeCost(actor);
	if (currentStamina < cost) {
		return false;
	}

	// Cooldown: block dodge while post-dodge recovery is still in effect.
	// m_slowActive is a single global state tied to the player's slow only —
	// gate this check on IsPlayerRef so AI dodges aren't rejected because the
	// player happens to be recovering. AI cooldown is tracked separately in
	// AIHandler's DifficultyMap.DodgeCooldown.
	if (actor->IsPlayerRef()) {
		std::lock_guard slowLock(m_slowMtx);
		if (m_slowActive || m_playerCooldown > 0.0f) {
			return false;
		}
	}

	// Direction-transition gate: if the engine's Direction graph value
	// disagrees with the player's current input direction, the player just
	// changed direction and the graph hasn't caught up. Block the dodge until
	// input and graph agree. Skipped when the player has no input — in that
	// case the dodge falls back to a default direction picked by the caller.
	if (actor->IsPlayerRef()) {
		if (auto* pc = RE::PlayerControls::GetSingleton()) {
			const auto& mv = pc->data.moveInputVec;
			const float mag = std::sqrt(mv.x * mv.x + mv.y * mv.y);

			if (mag > 0.1f) {
				// Player is moving — apply transition gate
				float currentDirection = 0.0f;
				actor->GetGraphVariableFloat("Direction", currentDirection);

				float angleRad = std::atan2(mv.x, mv.y);  // CW from +Y (forward)
				float angleDeg = angleRad * 57.2957795f;
				if (angleDeg < 0.0f) angleDeg += 360.0f;
				float expectedDir = std::fmod(angleDeg - 45.0f + 360.0f, 360.0f) / 360.0f;

				float diff = std::abs(currentDirection - expectedDir);
				if (diff > 0.5f) diff = 1.0f - diff;

				constexpr float kTransitionThreshold = 0.35f;
				if (diff > kTransitionThreshold) {
					return false;
				}
			}
			// else: zero input — let dodge fire with default Backward direction
		}
	}

	return true;
}

bool DodgeHandler::IsDodging(RE::Actor* actor)
{
	std::shared_lock lock(DodgeMtx);
	return ActiveDodges.contains(actor->GetHandle()) && ActiveDodges.at(actor->GetHandle()).active;
}

float DodgeHandler::SecondsSinceDodge(RE::Actor* actor)
{
	std::shared_lock lock(DodgeMtx);
	auto Iter = DodgeStarts.find(actor->GetHandle());
	if (Iter == DodgeStarts.end())
	{
		return -1.f;
	}
	return std::chrono::duration<float>(std::chrono::steady_clock::now() - Iter->second).count();
}

float DodgeHandler::GetDodgeCost(RE::Actor* actor)
{
	float maxStamina = actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kStamina);
	// Use the setting from DifficultySettings (defaulting to a ratio of max stamina)
	return maxStamina * DifficultySettings::DodgeCost;
}

void DodgeHandler::TriggerDodge(RE::Actor* actor, DodgeDirection direction)
{
	if (!actor) return;

	switch (Settings::ActiveDodgeSystem)
	{
	case DodgeSystem::Custom:
		ApplyImpulse(actor, direction);
		break;
	case DodgeSystem::DMCO:
	{
		// Map our 8-direction enum to DMCO's 3-direction (back-side) scheme.
		// DMCO uses Dodge_Direction graph variable with values 4/5/6 and the
		// corresponding Dodge_RB/Dodge_B/Dodge_LB events. Any non-back-side
		// direction falls through to the plain Back dodge.
		int dmcoDir = 5;
		const char* event = "Dodge_B";
		if (direction == DodgeDirection::BackwardRight) { dmcoDir = 4; event = "Dodge_RB"; }
		else if (direction == DodgeDirection::BackwardLeft) { dmcoDir = 6; event = "Dodge_LB"; }
		actor->SetGraphVariableInt("Dodge_Direction", dmcoDir);
		actor->NotifyAnimationGraph(event);
		actor->NotifyAnimationGraph("Dodge");
		break;
	}
	case DodgeSystem::None:
	default:
		// no dodge support — no-op
		break;
	}
}

void DodgeHandler::ApplyImpulse(RE::Actor* actor, DodgeDirection direction)
{
	std::unique_lock lock(DodgeMtx);
	if (ActiveDodges.contains(actor->GetHandle()) && ActiveDodges[actor->GetHandle()].active)
	{
		return;
	}
	if (IsLockedInAnimation(actor))
	{
		return;
	}
	const float staminaCost = GetDodgeCost(actor);
	actor->AsActorValueOwner()->DamageActorValue(RE::ActorValue::kStamina, staminaCost);

	const auto& tuning = kDodgeTuning[static_cast<size_t>(direction)];
	// A slowed actor dodges shorter, floored; buffs never lengthen it.
	const float SpeedScale = std::clamp(
		actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kSpeedMult) / 100.0f, kDodgeSlowFloor, 1.0f);

	DodgeState state;
	state.direction = direction;
	state.speed = tuning.speed * SpeedScale;
	state.animSpeedPeak = tuning.animSpeedPeak * SpeedScale;
	state.elapsed = 0.0f;
	state.active = true;
	state.startPos = actor->GetPosition();
	state.footstepFired = false;
	state.startSpeedSampled = 0.0f;
	actor->GetGraphVariableFloat("SpeedSampled", state.startSpeedSampled);
	// Pick timing based on whether the actor was already moving. From idle the
	// locomotion graph has to cross-fade idle clip → run clip, which needs
	// wall-clock time; we stretch the ramp and the total duration so the
	// transition has room to breathe instead of snapping.
	const bool startedFromIdle = state.startSpeedSampled < kIdleStartThreshold;
	state.timeTotal = startedFromIdle ? kDodgeDurationIdle : kDodgeDuration;
	state.rampEnd   = startedFromIdle ? kRampEndIdle       : kRampEnd;
	ActiveDodges[actor->GetHandle()] = state;
	DodgeStarts[actor->GetHandle()] = std::chrono::steady_clock::now();
	// Write graph variables for the dodge state machine. Direction is the
	// vanilla locomotion variable (may get rewritten by the engine each frame
	// from movement intent); DirDodge is our custom variable for the alt-blend
	// to read at state entry.
	const float graphDir = DodgeDirectionToGraphValue(direction);
	actor->SetGraphVariableFloat("DirDodge", graphDir);

	// Graph-side dodge entry signal. Fires BEFORE every other notify so the
	// dodge state machine can latch on its own clean transition without
	// racing the locomotion/alt-blend state changes that follow.
	actor->NotifyAnimationGraph("moveStart");
	// Signal behavior graph that we're entering an alternate-blend state for the dodge.
	// Graph-side wiring uses this to swap whatever blend the dodge needs.
	//actor->NotifyAnimationGraph("DirmodForceAltBlend");
	if (!actor->IsPlayer())
	{
		actor->SetGraphVariableInt("iUseDirDodge", 1);
	}
}

void DodgeHandler::ApplyOnGround(RE::bhkCharacterController* controller)
{
	if (!controller)
	{
		return;
	}

	const float dt = controller->stepInfo.deltaTime;

	// Tick post-dodge slow ONLY when this hook fires for the player's
	// controller. The slow uses a single global state and writes to the
	// player's SpeedMult AV, and the hook fires once per physics tick per
	// actor — so without this gate, a scene with N actors would tick the
	// slow timer N× per frame, expiring it almost immediately.
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player && player->GetCharController() == controller)
		{
			TickPostDodgeSlow(dt);
		}
	}

	std::unique_lock lock(DodgeMtx);

	// The hook fires for every on-ground actor's controller. Find which entry
	// in ActiveDodges (if any) belongs to this controller. Iteration is O(N) on
	// active dodges, which in practice is small (≤ a handful at once).
	auto it = ActiveDodges.end();
	RE::Actor* dodgingActor = nullptr;
	for (auto iter = ActiveDodges.begin(); iter != ActiveDodges.end(); ++iter)
	{
		if (!iter->first) continue;
		auto ptr = iter->first.get();
		RE::Actor* a = ptr ? ptr.get() : nullptr;
		if (a && a->GetCharController() == controller)
		{
			it = iter;
			dodgingActor = a;
			break;
		}
	}

	if (it == ActiveDodges.end() || !it->second.active || !dodgingActor)
	{
		return;
	}

	if (dt <= 0.0f)
	{
		return;
	}

	auto* vel = reinterpret_cast<float*>(&(controller->velocityMod));

	it->second.elapsed += dt;
	if (it->second.elapsed >= it->second.timeTotal)
	{
		vel[0] = 0.0f;
		vel[1] = 0.0f;
		// Hand locomotion back to the engine at the post-slow-adjusted value.
		{
			const bool slowWillApply = (kPostDodgeSlowMult < 1.0f && kPostDodgeSlowDuration > 0.0f);
			const float endValue = slowWillApply
				? (it->second.startSpeedSampled * kPostDodgeSlowMult)
				: it->second.startSpeedSampled;
			dodgingActor->SetGraphVariableFloat("SpeedSampled", endValue);
		}

		
		if (Settings::VerboseLogging)
		{
			const RE::NiPoint3 endPos = dodgingActor->GetPosition();
			const RE::NiPoint3 delta = endPos - it->second.startPos;
			const float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
			logger::info("Dodge end ({}): speed={} elapsed={:.3f}/{:.3f} -> horizontal={:.1f} units (3D={:.1f})",
				Who(dodgingActor), it->second.speed, it->second.elapsed, it->second.timeTotal, horizontal, delta.Length());
		}
		
		// Signal behavior graph back to normal blend state.
		//dodgingActor->NotifyAnimationGraph("DirmodForceNormalBlend");
		if (!dodgingActor->IsPlayer())
		{
			dodgingActor->SetGraphVariableInt("iUseDirDodge", 0);
		}
		const bool wasPlayer = dodgingActor->IsPlayerRef();
		ActiveDodges.erase(it);
		// Drop the dodge lock before grabbing the slow lock to avoid lock ordering risk.
		lock.unlock();
		if (wasPlayer)
		{
			StartPostDodgeSlow();
		}
		return;
	}

	const float t = it->second.elapsed / it->second.timeTotal;

	// Fire a vanilla footstep event once at peak velocity (~end of ramp).
	// We dispatch directly to BGSFootstepManager because NotifyAnimationGraph
	// doesn't trigger the engine's footstep audio path — only graph-internal
	// events do, and the manager listener catches those. Direct dispatch
	// produces armor + surface-appropriate audio.
	if (!it->second.footstepFired && t >= it->second.rampEnd)
	{
		if (auto* mgr = RE::BGSFootstepManager::GetSingleton())
		{
			RE::BGSFootstepEvent ev{};
			ev.actor = dodgingActor->GetHandle();
			ev.tag = "FootLeft";
			mgr->SendEvent(&ev);
			RE::BGSFootstepEvent ev2{};
			ev2.actor = dodgingActor->GetHandle();
			ev2.tag = "FootRight";
			mgr->SendEvent(&ev2);
		}
		it->second.footstepFired = true;
	}

	// Quadratic ease-out: peak at t=0, decays to 0 at t=1.
	// Roughly constant velocity for the first half keeps the locomotion
	// animation in sync with body displacement (less foot-slide).
	const float falloff = 1.0f - (t * t);
	const float currentSpeed = it->second.speed * falloff;

	// vel[0] = strafe (R+/L-), vel[1] = forward (F+/B-) in actor-local space
	float strafe = 0.0f;
	float forward = 0.0f;
	switch (it->second.direction)
	{
	case DodgeDirection::Forward:       forward = 1.0f;  break;
	case DodgeDirection::Backward:      forward = -1.0f; break;
	case DodgeDirection::Left:          strafe = -1.0f;  break;
	case DodgeDirection::Right:         strafe = 1.0f;   break;
	case DodgeDirection::ForwardLeft:   forward = 0.707f; strafe = -0.707f; break;
	case DodgeDirection::ForwardRight:  forward = 0.707f; strafe = 0.707f;  break;
	case DodgeDirection::BackwardLeft:  forward = -0.707f; strafe = -0.707f; break;
	case DodgeDirection::BackwardRight: forward = -0.707f; strafe = 0.707f;  break;
	}

	vel[0] = strafe * currentSpeed;
	vel[1] = forward * currentSpeed;

	// Drive the locomotion graph so the run animation plays during the dodge.
	// Trapezoid for SpeedSampled (constants at top of file): ramp from
	// startSpeedSampled up to peak (snappy ease-out), hold at peak, then
	// ease back down to the post-dodge slow target (decay end). Both
	// transitions smoothed so the graph blend doesn't flicker.
	// Direction is an 8-way blend in 0.125 increments, clockwise starting at
	// forwardright=0.000, ending at forward=0.875.
	const float startSampled = it->second.startSpeedSampled;
	const float animPeak = it->second.animSpeedPeak;
	// After the dodge, SpeedMult will be reduced to kPostDodgeSlowMult, so the
	// engine will write a proportionally lower SpeedSampled. Decay to that
	// reduced value so the graph doesn't snap from full-run cadence to
	// slow-run cadence on the next physics tick.
	const bool slowWillApply = (kPostDodgeSlowMult < 1.0f && kPostDodgeSlowDuration > 0.0f);
	const float decayTarget = slowWillApply ? (startSampled * kPostDodgeSlowMult) : startSampled;
	const float rampEnd = it->second.rampEnd;
	float speedSampled;
	if (t < rampEnd)
	{
		// quadratic ease-out: snappy ramp from startSampled to peak
		const float rampT = t / rampEnd;
		const float invT = 1.0f - rampT;
		const float easedT = 1.0f - invT * invT;
		speedSampled = startSampled + (animPeak - startSampled) * easedT;
	}
	else if (t < kDecayStart)
	{
		speedSampled = animPeak;
	}
	else
	{
		const float decayT = (t - kDecayStart) / (1.0f - kDecayStart);
		const float easedT = decayT * decayT;
		speedSampled = animPeak + (decayTarget - animPeak) * easedT;
	}

	// Direction is set once in ApplyImpulse and held steady for the rest
	// of the dodge by the suppression hook.
	dodgingActor->SetGraphVariableFloat("SpeedSampled", speedSampled);
}

void DodgeHandler::CancelDodgeOnAir(RE::bhkCharacterController* controller)
{
	if (!controller)
	{
		return;
	}

	std::unique_lock lock(DodgeMtx);

	// Same controller-to-actor lookup as ApplyOnGround.
	auto it = ActiveDodges.end();
	RE::Actor* dodgingActor = nullptr;
	for (auto iter = ActiveDodges.begin(); iter != ActiveDodges.end(); ++iter)
	{
		if (!iter->first) continue;
		auto ptr = iter->first.get();
		RE::Actor* a = ptr ? ptr.get() : nullptr;
		if (a && a->GetCharController() == controller)
		{
			it = iter;
			dodgingActor = a;
			break;
		}
	}

	if (it == ActiveDodges.end() || !dodgingActor)
	{
		return;
	}

	auto* vel = reinterpret_cast<float*>(&(controller->velocityMod));
	vel[0] = 0.0f;
	vel[1] = 0.0f;
	{
		const bool slowWillApply = (kPostDodgeSlowMult < 1.0f && kPostDodgeSlowDuration > 0.0f);
		const float endValue = slowWillApply
			? (it->second.startSpeedSampled * kPostDodgeSlowMult)
			: it->second.startSpeedSampled;
		dodgingActor->SetGraphVariableFloat("SpeedSampled", endValue);
	}
	if (Settings::VerboseLogging)
	{
		const RE::NiPoint3 endPos = dodgingActor->GetPosition();
		const RE::NiPoint3 delta = endPos - it->second.startPos;
		const float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
		logger::info("Dodge canceled airborne ({}): elapsed={:.3f}/{:.3f} -> horizontal={:.1f} units",
			Who(dodgingActor), it->second.elapsed, it->second.timeTotal, horizontal);
	}
	// Signal behavior graph back to normal blend state.
	dodgingActor->NotifyAnimationGraph("DirmodForceNormalBlend");
	dodgingActor->SetGraphVariableInt("iUseDirDodge", 0);
	const bool wasPlayer = dodgingActor->IsPlayerRef();
	ActiveDodges.erase(it);
	lock.unlock();
	if (wasPlayer)
	{
		StartPostDodgeSlow();
	}
}
