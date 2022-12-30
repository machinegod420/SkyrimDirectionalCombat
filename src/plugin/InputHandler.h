#pragma once

#include "SettingsLoader.h"

class InputEventHandler : public RE::BSTEventSink<RE::InputEvent*>
{
public:
	virtual RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>* a_eventSource) override;

	static void Register()
	{
		auto deviceManager = RE::BSInputDeviceManager::GetSingleton();
		deviceManager->AddEventSink(InputEventHandler::GetSingleton());
	}

	bool GetKeyModifierDown() const { return KeyModifierDown; }

	static InputEventHandler* GetSingleton()
	{
		static InputEventHandler singleton;
		return std::addressof(singleton);
	}

private:
	bool KeyModifierDown = false;

	enum : uint32_t
	{
		kInvalid = static_cast<uint32_t>(-1),
		kKeyboardOffset = 0,
		kMouseOffset = 256,
		kGamepadOffset = 266
	};
};
