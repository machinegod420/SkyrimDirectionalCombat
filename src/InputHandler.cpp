#include "InputHandler.h"
#include "DirectionHandler.h"
#include "AttackHandler.h"
#include "DodgeHandler.h"
#include "Hooks.h"

// Inverse of DodgeDirectionToGraphValue — rounds the engine's `Direction`
// graph value to the nearest 0.125 increment and returns the matching enum.
static DodgeDirection GraphDirectionToDodgeDir(float dir)
{
	float d = std::fmod(dir, 1.0f);
	if (d < 0.0f) d += 1.0f;
	const int slot = static_cast<int>(std::round(d * 8.0f)) % 8;
	switch (slot)
	{
	case 0: return DodgeDirection::Forward;
	case 1: return DodgeDirection::ForwardRight;
	case 2: return DodgeDirection::Right;
	case 3: return DodgeDirection::BackwardRight;
	case 4: return DodgeDirection::Backward;
	case 5: return DodgeDirection::BackwardLeft;
	case 6: return DodgeDirection::Left;
	case 7: return DodgeDirection::ForwardLeft;
	}
	return DodgeDirection::Backward;
}

// Frames the buffered press waits after the switch commits, so the animation
// thread has read the new spell before the swing is chosen.
constexpr int BufferedAttackFramesAfterCommit = 2;
// A switch that hasn't committed by then was parked; drop the press.
constexpr float BufferedAttackMaxSeconds = 0.5f;
// A press during your own swing is held this long for it to land; earlier is on you.
constexpr float BufferedMidSwingMaxSeconds = 0.2f;

void InputEventHandler::BufferAttack(bool power)
{
	PendingAttack = true;
	PendingPower = PendingPower || power;
}

bool InputEventHandler::ShouldHoldPress() const
{
	auto* Player = RE::PlayerCharacter::GetSingleton();
	Directions Queued;
	bool GraphBash = false;
	return PendingAttack || DirectionHandler::GetSingleton()->HasQueuedDirection(Player, Queued) ||
		(Player->GetGraphVariableBool("IsBashing", GraphBash) && GraphBash) || IsInWindup(Player);
}

void InputEventHandler::OnBlockPressed()
{
	PendingAttack = false;
	PendingPower = false;
	PendingAge = 0.f;
	PendingFrames = 0;
}

void InputEventHandler::Update(float delta)
{
	auto* Player = RE::PlayerCharacter::GetSingleton();
	AttackHandler::GetSingleton()->ClearStuckAttack(Player, GraphIdleAttacking, delta);
	if (!PendingAttack)
	{
		return;
	}
	Directions Queued;
	// Hold through a bash too: the engine drops kBash before the graph finishes the bash, and
	// an attack fired into that tail is accepted as kDraw but never played, orphaning it.
	bool GraphBash = false;
	const bool BashPlaying = Player->GetGraphVariableBool("IsBashing", GraphBash) && GraphBash;
	const bool MidSwing = IsInWindup(Player);
	if (DirectionHandler::GetSingleton()->HasQueuedDirection(Player, Queued) || BashPlaying || MidSwing)
	{
		PendingAge += delta;
		if (PendingAge <= (MidSwing ? BufferedMidSwingMaxSeconds : BufferedAttackMaxSeconds))
		{
			return;
		}
	}
	// Past your hit, wait for the chain window: fired before it, the attack is refused.
	else if (Player->IsAttacking() && !DirectionHandler::GetSingleton()->InAttackWindow(Player))
	{
		return;
	}
	else if (++PendingFrames < BufferedAttackFramesAfterCommit)
	{
		return;
	}
	// Backstop for a guard raised some other way than the block press: blocking wins.
	else if (IsGuardUp(Player))
	{
	}
	else if (Hooks::HookAttackHandler::CanPlayerAttack())
	{
		// same rule as the power attack key: an unblockable never powers
		if (PendingPower && !DirectionHandler::GetSingleton()->IsUnblockable(Player))
		{
			AttackHandler::GetSingleton()->DoPowerAttack(Player);
		}
		else
		{
			AttackHandler::GetSingleton()->DoAttack(Player);
		}
	}
	else
	{
		Hooks::HookAttackHandler::OnPlayerAttackRefused();
	}
	PendingAttack = false;
	PendingPower = false;
	PendingAge = 0.f;
	PendingFrames = 0;
}

RE::BSEventNotifyControl InputEventHandler::ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_eventSource)
{
	UNUSED(a_eventSource);
	auto userEvents = RE::UserEvents::GetSingleton();

	for (auto event = *a_event; event; event = event->next) 
	{
		if (event->eventType != RE::INPUT_EVENT_TYPE::kButton)
		{
			continue;
		}
		auto button = static_cast<RE::ButtonEvent*>(event);
		auto Player = RE::PlayerCharacter::GetSingleton();

		// speculative fix for specific edge case
		if (Settings::ExperimentalMode && Player->AsActorState()->actorState2.wantBlocking && !Player->IsBlocking())
		{
			Player->NotifyAnimationGraph("blockStart");
		}
		if (event->AsButtonEvent()->IsDown())
		{
			auto key = button->GetIDCode();
			switch (button->device.get()) {
			case RE::INPUT_DEVICE::kMouse:
				key += kMouseOffset;
				break;
			case RE::INPUT_DEVICE::kKeyboard:
				key += kKeyboardOffset;
				break;
			case RE::INPUT_DEVICE::kGamepad:
				break;
			default:
				continue;
			}

			auto ui = RE::UI::GetSingleton();
			if (ui->GameIsPaused()) {
				continue;
			}

			if (DirectionHandler::GetSingleton()->HasDirectionalPerks(Player))
			{
				if (key == InputSettings::KeyModifierCode)
				{
					KeyModifierDown = true;
				}
				if (InputSettings::InputType == InputSettings::InputTypes::Keyboard)
				{
					if (key == InputSettings::KeyCodeTR)
					{
						DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TR);
					}
					else if (key == InputSettings::KeyCodeTL)
					{
						DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::TL);
					}
					else if (key == InputSettings::KeyCodeBL)
					{
						DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BL);
					}
					else if (key == InputSettings::KeyCodeBR)
					{
						DirectionHandler::GetSingleton()->WantToSwitchTo(Player, Directions::BR);
					}
				}

				else if (key == InputSettings::KeyCodeFeint)
				{
					// Read before the feint cancels the swing.
					const bool FeintedPower = IsPowerAttacking(Player);
					const int FeintedStep = DirectionHandler::GetSingleton()->GetComboStep(Player);
					// Counted like swings: only with someone to swing at.
					if (AttackHandler::GetSingleton()->HandleFeint(Player) &&
						DirectionHandler::GetSingleton()->GetSwingTarget(Player))
					{
						AIHandler::GetSingleton()->RecordPlayerFeint(
							DirectionHandler::GetSingleton()->AnimationSet(Player), FeintedStep, FeintedPower);
						AIHandler::GetSingleton()->ResolvePlayerSwing(AIHandler::SwingOutcome::Feinted);
					}
				}

				else if (key == InputSettings::KeyCodeSwitchHud)
				{
					UISettings::Force1PHud = !UISettings::Force1PHud;
				}

			}
			if (key == InputSettings::KeyCodeBash)
			{
				// drawn weapon only
				if (Player->AsActorState()->GetWeaponState() == RE::WEAPON_STATE::kDrawn &&
					AttackHandler::GetSingleton()->CanAttack(Player))
				{
					AttackHandler::GetSingleton()->DoBash(Player);
				}

			}
			else if (key == InputSettings::KeyCodePowerAttack)
			{
				if (ShouldHoldPress())
				{
					BufferAttack(true);
				}
				else if (Hooks::HookAttackHandler::CanPlayerAttack())
				{
					// prevent unblockable attacks being power attacks because it causes a lot of balance issues
					if (!DirectionHandler::GetSingleton()->IsUnblockable(Player))
					{
						AttackHandler::GetSingleton()->DoPowerAttack(Player);
					}
					else
					{
						AttackHandler::GetSingleton()->DoAttack(Player);
					}
				}
				else
				{
					Hooks::HookAttackHandler::OnPlayerAttackRefused();
				}

			}
			else if (key == InputSettings::KeyCodeDodge && Settings::ActiveDodgeSystem == DodgeSystem::Custom)
			{
				if (DodgeHandler::GetSingleton()->CanDodge(Player))
				{
					// Use the engine's `Direction` graph variable as the source
					// of truth for "which way is the actor currently trying to
					// move in actor frame." This is the same value the locomotion
					// blend reads to pick clips, so dodge direction matches
					// whatever animation is already playing
					const auto& s = Player->AsActorState()->actorState1;
					const bool isMoving = s.movingForward || s.movingBack || s.movingLeft || s.movingRight;
					DodgeDirection dir = DodgeDirection::Backward;
					if (isMoving)
					{
						float currentDir = 0.0f;
						Player->GetGraphVariableFloat("Direction", currentDir);
						dir = GraphDirectionToDodgeDir(currentDir);
						DodgeHandler::GetSingleton()->ApplyImpulse(Player, dir);
					}
				}
			}

		}

		if (event->AsButtonEvent()->IsUp())
		{

			auto key = button->GetIDCode();
			switch (button->device.get()) {
			case RE::INPUT_DEVICE::kMouse:
				key += kMouseOffset;
				break;
			case RE::INPUT_DEVICE::kKeyboard:
				key += kKeyboardOffset;
				break;
			case RE::INPUT_DEVICE::kGamepad:
				break;
			default:
				continue;
			}

			auto ui = RE::UI::GetSingleton();
			if (ui->GameIsPaused()) {
				continue;
			}

			if (key == InputSettings::KeyModifierCode)
			{
				KeyModifierDown = false;
			}
			if (key == InputSettings::KeyCodeBash)
			{
				Player->NotifyAnimationGraph("bashRelease");
			}
		}

	}

	return RE::BSEventNotifyControl::kContinue;
}