#pragma once

#include <vector>
#include <queue>

#include "Direction.h"


enum class UIDirectionState
{
	Default,
	Attacking,
	Blocking,
	Unblockable,
	ImperfectBlock
};

enum class UIHostileState
{
	Neutral,
	Friendly,
	Hostile,
	Player
};

struct DrawCommand
{
	RE::NiPoint3 position;
	Directions dir;
	bool mirror;
	UIDirectionState state;
	UIHostileState hostileState;
	bool firstperson;
	bool lockout;
};

class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
	static MenuOpenCloseEventHandler* GetSingleton()
	{
		static MenuOpenCloseEventHandler obj;
		return std::addressof(obj);
	}
	static void Register()
	{
		auto ui = RE::UI::GetSingleton();
		if (!ui)
		{
			logger::error("failed to register MenuOpenCloseEventHandler");
			return;
		}

		ui->AddEventSink(GetSingleton());
	}

	virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_eventSource) override;
};


class UIMenu : RE::IMenu
{
public:

	static constexpr const char* MENU_NAME = "directions";
	static constexpr const char* MENU_PATH = "DirectionMenu";
	static bool visible;

	static RE::stl::owner<RE::IMenu*> Creator() { return new UIMenu(); }
	static void RegisterMenu() 
	{
		RE::UI::GetSingleton()->Register(UIMenu::MENU_NAME, UIMenu::Creator);
		logger::info("created UI menu");
	}

	UIMenu() {
		auto scaleformManager = RE::BSScaleformManager::GetSingleton();
		logger::info("ctor");
		scaleformManager->LoadMovieEx(this, MENU_PATH, [this](RE::GFxMovieDef* a_def) -> void
			{
				a_def->SetState(RE::GFxState::StateType::kLog,
					RE::make_gptr<Logger>().get());

				logger::info("testmod loaded UI");
			});
		inputContext = Context::kNone;
		depthPriority = 0;
		menuFlags.set(
			Flag::kAlwaysOpen,
			Flag::kAllowSaving,
			Flag::kRequiresUpdate);


	}
	virtual ~UIMenu() {}
	static void HideMenu();
	static void ShowMenu();


	RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override;

	RE::NiPoint2 WorldToScreen(const RE::NiPoint3& a_worldPos, float& outDepth) const;

	static void AddDrawCommand(RE::NiPoint3 position, Directions dir, bool mirror, UIDirectionState state, UIHostileState hostileState, bool firstperson, bool lockout);
	void AdvanceMovie(float interval, std::uint32_t a_currentTime) override;
private:
	void DrawDirection(RE::NiPoint2 position, float depth, Directions dir, bool mirror, uint32_t color, uint32_t backgroundcolor, uint32_t transparency);

	bool IsOnScreen(RE::NiPoint2 position) const;

	RE::NiPoint2 ScreenSize;
	class Logger : public RE::GFxLog
	{
	public:
		void LogMessageVarg(LogMessageType, const char* a_fmt, std::va_list a_argList) override
		{
			std::string fmt(a_fmt ? a_fmt : "");
			while (!fmt.empty() && fmt.back() == '\n') {
				fmt.pop_back();
			}

			std::va_list args;
			va_copy(args, a_argList);
			std::vector<char> buf(static_cast<std::size_t>(std::vsnprintf(0, 0, fmt.c_str(), a_argList) + 1));
			std::vsnprintf(buf.data(), buf.size(), fmt.c_str(), args);
			va_end(args);

			logger::info("directionmod: {}"sv, buf.data());
		}
	};
};