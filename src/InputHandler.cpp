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
			//logger::info("speculative player block fix");
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
					AttackHandler::GetSingleton()->HandleFeint(Player);
				}

				else if (key == InputSettings::KeyCodeSwitchHud)
				{
					UISettings::Force1PHud = !UISettings::Force1PHud;
				}

			}
			if (key == InputSettings::KeyCodeBash)
			{
				if (AttackHandler::GetSingleton()->CanAttack(Player))
				{
					Player->NotifyAnimationGraph("bashStart");
					Player->AsActorState()->actorState1.meleeAttackState = RE::ATTACK_STATE_ENUM::kBash;
				}

			}
			else if (key == InputSettings::KeyCodePowerAttack)
			{
				if (Hooks::HookAttackHandler::CanPlayerAttack())
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
						if (Settings::VerboseLogging)
						{
							logger::info("Dodge: Direction={:.3f} -> dir={}", currentDir, static_cast<int>(dir));
						}
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