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

	// An attack pressed while a guard switch is pending is held here and fired
	// by Update once the switch has committed and the graph has had a frame to
	// read the new spell.
	void BufferAttack(bool power);
	bool HasBufferedAttack() const { return PendingAttack; }
	// A press can't fire yet: one is already held, the guard is still moving, a bash is
	// finishing, or your own swing hasn't landed.
	bool ShouldHoldPress() const;
	// The block button: a press cancels any buffered attack.
	void OnBlockPressed();
	void Update(float delta);

	static InputEventHandler* GetSingleton()
	{
		static InputEventHandler singleton;
		return std::addressof(singleton);
	}

private:
	bool KeyModifierDown = false;
	bool PendingAttack = false;
	bool PendingPower = false;
	float PendingAge = 0.f;
	int PendingFrames = 0;
	float GraphIdleAttacking = 0.f;

	enum : uint32_t
	{
		kInvalid = static_cast<uint32_t>(-1),
		kKeyboardOffset = 0,
		kMouseOffset = 256,
		kGamepadOffset = 266
	};
};
