#include "UIMenu.h"
#include "SettingsLoader.h"
#include <shared_mutex>

// ui is drawn on a seperate thread but accesses these static variables so
// we need to wrap them in mutexes
static std::vector<DrawCommand> DrawCommands;
static std::mutex mtx;
static std::mutex mtx2;
bool UIMenu::visible = false;

RE::BSEventNotifyControl MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_eventSource)
{
	UNUSED(a_eventSource);
	auto mName = a_event->menuName;

	if (mName == UIMenu::MENU_NAME)
	{
		//UIMenu::Update();
	}
	else if (mName == RE::LoadingMenu::MENU_NAME)
	{
		if (!a_event->opening)
		{
			UIMenu::ShowMenu();
		}
	}
	return RE::BSEventNotifyControl::kContinue;
}

void UIMenu::AddDrawCommand(RE::NiPoint3 position, Directions dir, bool mirror, UIDirectionState state, UIHostileState hostileState, bool firstperson, bool lockout)
{
	// surprised this hasnt crashed from all the race conditions
	// this is populated on the game thread and emptied on the UI thread
	mtx.lock();
	DrawCommands.push_back({ position, dir, mirror, state, hostileState, firstperson, lockout });
	mtx.unlock();
}

void UIMenu::ShowMenu()
{
	mtx2.lock();
	if (!visible)
	{
		RE::UIMessageQueue::GetSingleton()->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
		visible = true;
	}
	mtx2.unlock();
}

void UIMenu::HideMenu()
{
	mtx2.lock();
	if (visible)
	{
		RE::UIMessageQueue::GetSingleton()->AddMessage(MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		visible = false;
	}
	mtx2.unlock();
}

RE::UI_MESSAGE_RESULTS UIMenu::ProcessMessage(RE::UIMessage& a_message)
{
	switch (*a_message.type) {
	case RE::UI_MESSAGE_TYPE::kShow:
		mtx2.lock();
		visible = true;
		mtx2.unlock();
		return IMenu::ProcessMessage(a_message);
	case RE::UI_MESSAGE_TYPE::kHide:
		mtx2.lock();
		visible = false;
		mtx2.unlock();
		return IMenu::ProcessMessage(a_message);
	default:
		return IMenu::ProcessMessage(a_message);
	}
}

void UIMenu::AdvanceMovie(float interval, std::uint32_t a_currentTime)
{
	UNUSED(interval);
	UNUSED(a_currentTime);
	//logger::info("advancemovie");
	uiMovie->Invoke("clear", nullptr, nullptr, 0);
	mtx.lock();
	for (uint32_t i = 0; i < DrawCommands.size(); ++i)
	{

		RE::NiPoint3 Position = DrawCommands[i].position;
		Directions Dir = DrawCommands[i].dir;
		uint32_t white = 0xB0B0B3;
		uint32_t active = 0xFFFF40;
		uint32_t background = 0x000000;
		// unorm color value
		uint32_t transparency = 256u;

		switch (DrawCommands[i].hostileState)
		{
		case UIHostileState::Friendly:
			white = 0xB0DAB3;
			break;
		case UIHostileState::Hostile:
			white = 0xD4B0B3;
			break;
		case UIHostileState::Player:
			white = 0xC9C9CC;
			break;
		case UIHostileState::Neutral:
			transparency = 100;
			break;
		}

		switch (DrawCommands[i].state)
		{
		case UIDirectionState::Attacking:
			active = 0xFF4040;
			break;
		case UIDirectionState::Blocking:
			active = 0x3030FF;
			break;
		case UIDirectionState::ImperfectBlock:
			active = 0x40E0D0;
			break;
		case UIDirectionState::Unblockable:
			white = 0xFF6600;
			active = white;
			break;
		}

		if (DrawCommands[i].lockout)
		{
			background = 0xFF4040;
		}

		float depth;
		RE::NiPoint2 StartPos;
		if (DrawCommands[i].firstperson) 
		{
			depth = 0.f;
			RE::GRectF rect = uiMovie->GetVisibleFrameRect();
			float x = (rect.right - rect.left) / 2.f;
			float y = (rect.bottom - rect.top) / 2.f;
			StartPos = RE::NiPoint2(x, y);
		} 
		else
		{
			StartPos = WorldToScreen(Position, depth);
		}
		

		if (depth >= 0 && IsOnScreen(StartPos))
		{
			
			// only flip horizontal axes
			if (!DrawCommands[i].mirror)
			{
				DrawDirection(StartPos, depth, Directions::TR, DrawCommands[i].mirror, Dir == Directions::TR ? active : white, background, transparency);
				DrawDirection(StartPos, depth, Directions::TL, DrawCommands[i].mirror, Dir == Directions::TL ? active : white, background, transparency);
				DrawDirection(StartPos, depth, Directions::BR, DrawCommands[i].mirror, Dir == Directions::BR ? active : white, background, transparency);
				DrawDirection(StartPos, depth, Directions::BL, DrawCommands[i].mirror, Dir == Directions::BL ? active : white, background, transparency);
			}
			else
			{
				DrawDirection(StartPos, depth, Directions::TR, DrawCommands[i].mirror, Dir == Directions::TL ? active : white, background, transparency);
				DrawDirection(StartPos, depth, Directions::TL, DrawCommands[i].mirror, Dir == Directions::TR ? active : white, background, transparency);
				DrawDirection(StartPos, depth, Directions::BR, DrawCommands[i].mirror, Dir == Directions::BL ? active : white, background, transparency);
				DrawDirection(StartPos, depth, Directions::BL, DrawCommands[i].mirror, Dir == Directions::BR ? active : white, background, transparency);
			}
		}


	}
	DrawCommands.clear();
	mtx.unlock();
}


bool UIMenu::IsOnScreen(RE::NiPoint2 position) const
{
	RE::GRectF rect = uiMovie->GetVisibleFrameRect();
	float x = fabs(rect.left - rect.right);
	float y = fabs(rect.top - rect.bottom);
	return (position.x <= x && position.x >= 0.0 && position.y <= y && position.y >= 0.0);
}


void UIMenu::DrawDirection(RE::NiPoint2 StartPos, float depth, Directions dir, bool mirror, uint32_t color, uint32_t backgroundcolor, uint32_t transparency)
{
	UNUSED(mirror);

	// just draw lines for now
	// clamp depth
	
	depth = std::clamp(depth, 300.f, 1000.f);
	RE::NiPoint2 EndPos = StartPos;
	float scale = UISettings::Size / depth;
	float dist = UISettings::Length * scale;
	float size = UISettings::Length * scale;
	float thickness = UISettings::Thickness * scale;
	switch (dir)
	{
	case Directions::TR:
		StartPos.x += dist;
		StartPos.y -= dist;
		EndPos = StartPos;
		EndPos.x += size;
		EndPos.y -= size;
		break;
	case Directions::TL:
		StartPos.x -= dist;
		StartPos.y -= dist;
		EndPos = StartPos;
		EndPos.x -= size;
		EndPos.y -= size;
		break;
	case Directions::BL:
		StartPos.x -= dist;
		StartPos.y += dist;
		EndPos = StartPos;
		EndPos.x -= size;
		EndPos.y += size;
		break;
	case Directions::BR:
		StartPos.x += dist;
		StartPos.y += dist;
		EndPos = StartPos;
		EndPos.x += size;
		EndPos.y += size;
		break;
	}

	RE::GFxValue argsFill[1]{ 0x00FF00 };
	uiMovie->Invoke("beginFill", nullptr, argsFill, 1);

	RE::GFxValue argsLineStyle2[3]{ thickness * 1.5, backgroundcolor, transparency };
	uiMovie->Invoke("lineStyle", nullptr, argsLineStyle2, 3);

	RE::GFxValue argsStartPos2[2]{ StartPos.x, StartPos.y };
	uiMovie->Invoke("moveTo", nullptr, argsStartPos2, 2);

	RE::GFxValue argsEndPos2[2]{ EndPos.x, EndPos.y };
	uiMovie->Invoke("lineTo", nullptr, argsEndPos2, 2);

	RE::GFxValue argsLineStyle[3]{ thickness, color, 100.f };
	uiMovie->Invoke("lineStyle", nullptr, argsLineStyle, 3);

	RE::GFxValue argsStartPos[2]{ StartPos.x, StartPos.y };
	uiMovie->Invoke("moveTo", nullptr, argsStartPos, 2);

	RE::GFxValue argsEndPos[2]{ EndPos.x, EndPos.y };
	uiMovie->Invoke("lineTo", nullptr, argsEndPos, 2);

	uiMovie->Invoke("endFill", nullptr, nullptr, 0);
}

RE::NiPoint2 UIMenu::WorldToScreen(const RE::NiPoint3& a_worldPos, float& outDepth) const
{
	RE::NiPoint2 screenPos;
	// https://github.com/ersh1/TrueHUD/blob/master/src/Offsets.h#L9
	static uintptr_t g_worldToCamMatrix = RELOCATION_ID(519579, 406126).address();                       // 2F4C910, 2FE75F0
	static RE::NiRect<float>* g_viewPort = (RE::NiRect<float>*)RELOCATION_ID(519618, 406160).address();  // 2F4DED0, 2FE8B98
	static float* g_fNear = (float*)(RELOCATION_ID(517032, 403540).address() + 0x40);                    // 2F26FC0, 2FC1A90
	static float* g_fFar = (float*)(RELOCATION_ID(517032, 403540).address() + 0x44);                     // 2F26FC4, 2FC1A94

	RE::NiCamera::WorldPtToScreenPt3((float(*)[4])g_worldToCamMatrix, *g_viewPort, a_worldPos, screenPos.x, screenPos.y, outDepth, 1e-5f);
	if (outDepth > 0)
	{
		float fNear = *g_fNear;
		float fFar = *g_fFar;
		// linearize depth from fnear to ffar
		outDepth = fNear * fFar / (fFar + outDepth * (fNear - fFar));
	}
	else 
	{
		// just throw it out
		outDepth = -1;
	}


	RE::GRectF rect = uiMovie->GetVisibleFrameRect();

	screenPos.x = rect.left + (rect.right - rect.left) * screenPos.x;
	screenPos.y = 1.f - screenPos.y;
	screenPos.y = rect.top + (rect.bottom - rect.top) * screenPos.y;
	return screenPos;
}

 