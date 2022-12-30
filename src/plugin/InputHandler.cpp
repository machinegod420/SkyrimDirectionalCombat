#include "InputHandler.h"
#include "DirectionHandler.h"

RE::BSEventNotifyControl InputEventHandler::ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_eventSource)
{
	UNUSED(a_eventSource);
	for (auto event = *a_event; event; event = event->next) 
	{
		if (event->eventType != RE::INPUT_EVENT_TYPE::kButton)
		{
			continue;
		}
		auto button = static_cast<RE::ButtonEvent*>(event);
		if (event->AsButtonEvent()->IsDown())
		{
			auto key = button->idCode;
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

			auto Player = RE::PlayerCharacter::GetSingleton();

			if (key == InputSettings::KeyModifierCode)
			{
				KeyModifierDown = true;
			}

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

		if (event->AsButtonEvent()->IsUp())
		{
			auto key = button->idCode;
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
		}

	}

	return RE::BSEventNotifyControl::kContinue;
}