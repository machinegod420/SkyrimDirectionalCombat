#include "UIMenu.h"
#include "SettingsLoader.h"
#include <shared_mutex>
#include "imgui.h"
#include <d3d11.h>

#include <dxgi.h>
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "3rdparty/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "3rdparty/nanosvgrast.h"
#define STB_IMAGE_IMPLEMENTATION
#include "3rdparty/stb_image.h"

#include "3rdparty/imgui_impl_dx11.h"
#include "3rdparty/imgui_impl_dx12.h"
#include "3rdparty/imgui_impl_win32.h"

#include "imgui_internal.h"

// ui is drawn on a seperate thread but accesses these static variables so
// we need to wrap them in mutexes
static std::vector<DrawCommand> DrawCommands;
static std::mutex mtx;
static std::mutex mtx2;
// need to cache for imgui as mutex locks lag behind swapchain since its a different thread and swpachain should never stall
static std::vector<DrawCommand> DrawCommandsCached;
bool UIMenu::visible = false;

namespace UI
{
	void AddDrawCommand(RE::NiPoint3 position, Directions dir, bool mirror, UIDirectionState state, UIHostileState hostileState, bool firstperson, bool lockout)
	{
		// surprised this hasnt crashed from all the race conditions
		// this is populated on the game thread and emptied on the UI thread
		mtx.lock();
		//logger::info("Attempting to add new draw command");
		DrawCommands.push_back({ position, dir, mirror, state, hostileState, firstperson, lockout });
		mtx.unlock();
	}
};

RE::NiPoint2 WorldToScreen(const RE::NiPoint3& a_worldPos, float& outDepth, int ScreenWidth, int ScreenHeight)
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



	screenPos.x = ScreenWidth * screenPos.x;
	screenPos.y = 1.f - screenPos.y;
	screenPos.y = ScreenHeight * screenPos.y;
	return screenPos;
}

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
	return;
	UNUSED(interval);
	UNUSED(a_currentTime);
	//logger::info("advancemovie");
	uiMovie->Invoke("clear", nullptr, nullptr, 0);
	mtx.lock();
	for (uint32_t i = 0; i < DrawCommands.size(); ++i)
	{
		// mirror effectively means not player
		if (DrawCommands[i].mirror && UISettings::HarderUI)
		{
			if (DrawCommands[i].state != UIDirectionState::Attacking)
			{
				continue;
			}
		}
		RE::NiPoint3 Position = DrawCommands[i].position;
		Directions Dir = DrawCommands[i].dir;
		uint32_t white = 0xB0B0B3;
		uint32_t active = 0xFFFF40;
		uint32_t background = 0x000000;
		// unorm color value
		uint32_t transparency = 130u;
		uint32_t transparency2 = 10u;

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
				DrawDirection(StartPos, depth, Directions::TR, DrawCommands[i].mirror, Dir == Directions::TR ? active : white, background, Dir == Directions::TR ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::TL, DrawCommands[i].mirror, Dir == Directions::TL ? active : white, background, Dir == Directions::TL ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::BR, DrawCommands[i].mirror, Dir == Directions::BR ? active : white, background, Dir == Directions::BR ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::BL, DrawCommands[i].mirror, Dir == Directions::BL ? active : white, background, Dir == Directions::BL ? transparency : transparency2);
			}
			else
			{
				DrawDirection(StartPos, depth, Directions::TR, DrawCommands[i].mirror, Dir == Directions::TL ? active : white, background, Dir == Directions::TL ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::TL, DrawCommands[i].mirror, Dir == Directions::TR ? active : white, background, Dir == Directions::TR ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::BR, DrawCommands[i].mirror, Dir == Directions::BL ? active : white, background, Dir == Directions::BL ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::BL, DrawCommands[i].mirror, Dir == Directions::BR ? active : white, background, Dir == Directions::BR ? transparency : transparency2);
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
	if (Settings::MNBMode)
	{
		switch (dir)
		{
		case Directions::TR:
			StartPos.y -= dist;
			EndPos = StartPos;
			EndPos.y -= size;
			break;
		case Directions::TL:
			StartPos.x -= dist;
			EndPos = StartPos;
			EndPos.x -= size;
			break;
		case Directions::BL:
			StartPos.y += dist;
			EndPos = StartPos;
			EndPos.y += size;
			break;
		case Directions::BR:
			StartPos.x += dist;
			EndPos = StartPos;
			EndPos.x += size;
			break;
		}
	}
	else if (Settings::ForHonorMode)
	{
		switch (dir)
		{
		case Directions::TR:
		case Directions::TL:
			StartPos.y -= dist;
			StartPos.y -= size;
			StartPos.x -= dist;
			EndPos = StartPos;
			EndPos.x += dist;
			EndPos.x += dist;
			break;
		case Directions::BL:
			EndPos = StartPos;
			StartPos.x -= dist;
			StartPos.x -= size;
			StartPos.y -= dist;
			EndPos.y += dist;
			EndPos.x -= size;
			break;
		case Directions::BR:
			EndPos = StartPos;
			StartPos.x += dist;
			StartPos.x += size;
			StartPos.y -= dist;
			EndPos.y += dist;
			EndPos.x += size;
			break;
		}
	}
	else
	{
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
	}


	RE::GFxValue argsFill[1]{ 0x00FF00 };
	uiMovie->Invoke("beginFill", nullptr, argsFill, 1);

	RE::GFxValue argsLineStyle2[3]{ thickness * 1.5, backgroundcolor, transparency };
	uiMovie->Invoke("lineStyle", nullptr, argsLineStyle2, 3);

	RE::GFxValue argsStartPos2[2]{ StartPos.x, StartPos.y };
	uiMovie->Invoke("moveTo", nullptr, argsStartPos2, 2);

	RE::GFxValue argsEndPos2[2]{ EndPos.x, EndPos.y };
	uiMovie->Invoke("lineTo", nullptr, argsEndPos2, 2);

	RE::GFxValue argsLineStyle[3]{ thickness, color, transparency };
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


namespace stl
{
	using namespace SKSE::stl;

	template <class T>
	void write_thunk_call()
	{
		auto& trampoline = SKSE::GetTrampoline();
		const REL::Relocation<std::uintptr_t> hook{ T::id, T::offset };
		T::func = trampoline.write_call<5>(hook.address(), T::thunk);
	}
}


LRESULT RenderManager::WndProcHook::thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	auto& io = ImGui::GetIO();
	if (uMsg == WM_KILLFOCUS) {
		io.ClearInputCharacters();
		io.ClearInputKeys();
	}

	return func(hWnd, uMsg, wParam, lParam);
}

void RenderManager::D3DInitHook::thunk()
{
	func();

	logger::info("RenderManager: Initializing...");
	auto render_manager = RE::BSRenderManager::GetSingleton();
	if (!render_manager) {
		logger::error("Cannot find render manager. Initialization failed!");
		return;
	}

	auto render_data = render_manager->GetRuntimeData();

	logger::info("Getting swapchain...");
	auto swapchain = render_data.swapChain;
	if (!swapchain) {
		logger::error("Cannot find swapchain. Initialization failed!");
		return;
	}

	logger::info("Getting swapchain desc...");
	DXGI_SWAP_CHAIN_DESC sd{};
	if (swapchain->GetDesc(std::addressof(sd)) < 0) {
		logger::error("IDXGISwapChain::GetDesc failed.");
		return;
	}

	device = render_data.forwarder;
	context = render_data.context;

	logger::info("Initializing ImGui...");
	ImGui::CreateContext();
	if (!ImGui_ImplWin32_Init(sd.OutputWindow)) {
		logger::error("ImGui initialization failed (Win32)");
		return;
	}
	if (!ImGui_ImplDX11_Init(device, context)) {
		logger::error("ImGui initialization failed (DX11)");
		return;
	}

	logger::info("...ImGui Initialized");

	initialized.store(true);

	WndProcHook::func = reinterpret_cast<WNDPROC>(
		SetWindowLongPtrA(
			sd.OutputWindow,
			GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(WndProcHook::thunk)));
	if (!WndProcHook::func)
		logger::error("SetWindowLongPtrA failed!");

	LoadTexture("./Data/SKSE/Plugins/resources/marker.png", true, IconTypes::Marker);
	LoadTexture("./Data/SKSE/Plugins/resources/markeroutline.png", true, IconTypes::MarkerOutline);
	LoadTexture("./Data/SKSE/Plugins/resources/forhonormarker.png", true, IconTypes::ForHonorMarker);
	LoadTexture("./Data/SKSE/Plugins/resources/forhonormarkeroutline.png", true, IconTypes::ForHonorMarkerOutline);
}

void RenderManager::DXGIPresentHook::thunk(std::uint32_t a_p1)
{
	func(a_p1);

	if (!D3DInitHook::initialized.load())
		return;
	// prologue
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	//logger::info("DXGIPresentHook::thunk()");
	// do stuff
	RenderManager::draw();

	// epilogue
	ImGui::EndFrame();
	ImGui::Render();
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

std::map<IconTypes, Icon> RenderManager::IconMap;


void RenderManager::MessageCallback(SKSE::MessagingInterface::Message* msg)  //CallBack & LoadTextureFromFile should called after resource loaded.
{
	if (msg->type == SKSE::MessagingInterface::kDataLoaded && D3DInitHook::initialized) {
		auto& io = ImGui::GetIO();
		io.MouseDrawCursor = true;
		io.WantSetMousePos = true;
	}
}

// Code completely and blatantly stolen from lamas tiny hud and wheeler
bool RenderManager::Install()
{
	auto g_message = SKSE::GetMessagingInterface();
	if (!g_message) {
		logger::error("Messaging Interface Not Found!");
		return false;
	}

	g_message->RegisterListener(MessageCallback);

	SKSE::AllocTrampoline(14 * 2);

	stl::write_thunk_call<D3DInitHook>();
	stl::write_thunk_call<DXGIPresentHook>();
	logger::info("Rendermanager install");

	return true;
}


bool IsOnScreen(RE::NiPoint2 position, int Width, int Height)
{
	return (position.x <= Width && position.x >= 0.0 && position.y <= Height && position.y >= 0.0);
}


void RenderManager::draw()
{
	static constexpr ImGuiWindowFlags window_flag =
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs;

	const float screen_size_x = ImGui::GetIO().DisplaySize.x, screen_size_y = ImGui::GetIO().DisplaySize.y;

	ImGui::SetNextWindowSize(ImVec2(screen_size_x, screen_size_y));
	ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
	ImGui::Begin("test", nullptr, window_flag);
	// Add UI elements here
	//float deltaTime = ImGui::GetIO().DeltaTime;

	mtx.lock();
	//logger::info("DrawCommands size {}", DrawCommands.size());
	for (uint32_t i = 0; i < DrawCommands.size(); ++i)
	{
		RE::NiPoint3 Position = DrawCommands[i].position;
		Directions Dir = DrawCommands[i].dir;

		float depth;
		RE::NiPoint2 StartPos;
		// AA BB GG RR
		// use IM_COL32(255, 255, 255, alpha);
		ColorRGBA white({ 0xB0, 0xB0, 0xB3, 0x00 });
		ColorRGBA active({ 0xF0, 0xF0, 0xE0, 0x00 });
		ColorRGBA background({ 0x00,0x00,0x00,0x00 });
		// unorm color value
		uint32_t transparency = 210u;
		uint32_t transparency2 = 50u;
		switch (DrawCommands[i].hostileState)
		{
		case UIHostileState::Friendly:
			white = { 0xB0, 0xDA, 0xB3, 0x00 };
			break;
		case UIHostileState::Hostile:
			white = { 0xD4, 0xB0, 0xB3, 0x00 };
			break;
		case UIHostileState::Player:
			white = { 0xC9, 0xC9, 0xCC };
			break;
		case UIHostileState::Neutral:
			transparency = 100;
			break;
		}

		switch (DrawCommands[i].state)
		{
		case UIDirectionState::Attacking:
			active = {0xFF, 0x40, 0x40, 0x00};
			break;
		case UIDirectionState::Blocking:
			active = { 0x40, 0x40, 0xFF, 0x00 };
			break;
		case UIDirectionState::ImperfectBlock:
			active = { 0x40, 0xE0, 0xD0, 0x00 };
			break;
		case UIDirectionState::Unblockable:
			white = { 0xFF, 0x66, 0x00, 0x00 };
			active = white;
			break;
		}

		if (DrawCommands[i].lockout)
		{
			background = {0xFF, 0x40, 0x40, 0x00};
		}

		if (DrawCommands[i].firstperson)
		{
			depth = 0.f;
			float x = ImGui::GetIO().DisplaySize.x / 2.f;
			float y = ImGui::GetIO().DisplaySize.y / 2.f;
			StartPos = RE::NiPoint2(x, y);
		}
		else
		{
			StartPos = WorldToScreen(Position, depth, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
		}


		if (depth >= 0 && IsOnScreen(StartPos, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y))
		{

			// only flip horizontal axes
			if (!DrawCommands[i].mirror)
			{
				DrawDirection(StartPos, depth, Directions::TR, DrawCommands[i].mirror, Dir == Directions::TR ? active : white, background, Dir == Directions::TR ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::TL, DrawCommands[i].mirror, Dir == Directions::TL ? active : white, background, Dir == Directions::TL ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::BR, DrawCommands[i].mirror, Dir == Directions::BR ? active : white, background, Dir == Directions::BR ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::BL, DrawCommands[i].mirror, Dir == Directions::BL ? active : white, background, Dir == Directions::BL ? transparency : transparency2);
			}
			else
			{
				DrawDirection(StartPos, depth, Directions::TR, DrawCommands[i].mirror, Dir == Directions::TL ? active : white, background, Dir == Directions::TL ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::TL, DrawCommands[i].mirror, Dir == Directions::TR ? active : white, background, Dir == Directions::TR ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::BR, DrawCommands[i].mirror, Dir == Directions::BL ? active : white, background, Dir == Directions::BL ? transparency : transparency2);
				DrawDirection(StartPos, depth, Directions::BL, DrawCommands[i].mirror, Dir == Directions::BR ? active : white, background, Dir == Directions::BR ? transparency : transparency2);
			}
		}
		//logger::info("Direction pos {} {}", StartPos.x, StartPos.y);

	}
	DrawCommands.clear();
	mtx.unlock();


	ImGui::End();
}

void RenderManager::DrawDirection(RE::NiPoint2 StartPos, float depth, Directions dir, bool mirror, ColorRGBA color, ColorRGBA backgroundcolor, uint32_t transparency)
{
	/*
	logger::info("Drawing quad at pos {} {} with colors {} {} transparency {}", StartPos.x, StartPos.x,
		IM_COL32(color.r, color.b, color.g, 255),IM_COL32(backgroundcolor.r, backgroundcolor.b, backgroundcolor.g, 255), transparency);
	*/

	constexpr static ImVec2 uvs[4] = { ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f), ImVec2(1.0f, 1.0f), ImVec2(0.0f, 1.0f) };

	depth = std::clamp(depth, 300.f, 1000.f);
	RE::NiPoint2 EndPos = StartPos;
	float scale = UISettings::Size / depth;
	float dist = UISettings::Length * scale;
	float size = UISettings::Length * scale;
	float thickness = UISettings::Thickness * scale;

	float rotationAngle = 0;

	IconTypes ForegroundTexIdx = IconTypes::Marker;
	IconTypes BackgroundTexIdx = IconTypes::MarkerOutline;
	if (Settings::MNBMode)
	{
		switch (dir)
		{
		case Directions::TR:
			StartPos.y -= dist;
			rotationAngle = 0;
			break;
		case Directions::TL:
			StartPos.x -= dist;
			rotationAngle = 270.0f * 3.14159265359f / 180.0f;
			break;
		case Directions::BL:
			StartPos.y += dist;
			rotationAngle = 180.0f * 3.14159265359f / 180.0f;
			break;
		case Directions::BR:
			StartPos.x += dist;
			rotationAngle = 90.0f * 3.14159265359f / 180.0f;
			break;
		}
	}
	else if (Settings::ForHonorMode)
	{
		ForegroundTexIdx = IconTypes::ForHonorMarker;
		BackgroundTexIdx = IconTypes::ForHonorMarkerOutline;
		
		switch (dir)
		{
		case Directions::TR:
		case Directions::TL:
			StartPos.y -= dist;
			StartPos.y -= dist;
			rotationAngle = 0.0f;
			break;
		case Directions::BL:
			dist *= 1.5;
			StartPos.x -= dist;
			rotationAngle = 245.0f * 3.14159265359f / 180.0f;
			break;
		case Directions::BR:
			dist *= 1.5;
			StartPos.x += dist;
			rotationAngle = 115.0f * 3.14159265359f / 180.0f;
			break;
		}
	}
	else
	{
		switch (dir)
		{
		case Directions::TR:
			StartPos.x += dist;
			StartPos.y -= dist;
			rotationAngle = 45.0f * 3.14159265359f / 180.0f;
			break;
		case Directions::TL:
			StartPos.x -= dist;
			StartPos.y -= dist;
			rotationAngle = 315.0f * 3.14159265359f / 180.0f;
			break;
		case Directions::BL:
			StartPos.x -= dist;
			StartPos.y += dist;
			rotationAngle = 225.0f * 3.14159265359f / 180.0f;
			break;
		case Directions::BR:
			StartPos.x += dist;
			StartPos.y += dist;
			rotationAngle = 135.0f * 3.14159265359f / 180.0f;
			break;
		}
	}

	float cosAngle = cos(rotationAngle);
	float sinAngle = sin(rotationAngle);

	

	ImVec2 Pos[4] =
	{ 
		ImVec2(-size, -size),
		ImVec2(size, -size),
		ImVec2(size, size),
		ImVec2(-size, size) 
	};
	ImVec2 RotatedPos[4];
	for (int i = 0; i < 4; ++i) 
	{
		// Translate the positions to the origin
		float translatedX = Pos[i].x;
		float translatedY = Pos[i].y;
		// Apply the rotation transformation
		RotatedPos[i].x = translatedX * cosAngle - translatedY * sinAngle + StartPos.x;
		RotatedPos[i].y = translatedX * sinAngle + translatedY * cosAngle + StartPos.y;
		//logger::info("Direction translated pos {} {}", RotatedPos[i].x, RotatedPos[i].y);
	}
	if (!IconMap.contains(ForegroundTexIdx))
	{
		logger::info("Icon map contains null texture! 1");
	}
	if (!IconMap.contains(BackgroundTexIdx))
	{
		logger::info("Icon map contains null texture! 2");
	}
	ImGui::GetWindowDrawList()->AddImageQuad(IconMap[BackgroundTexIdx].Texture,
		RotatedPos[0], RotatedPos[1], RotatedPos[2], RotatedPos[3], uvs[0], uvs[1], uvs[2], uvs[3], 
		IM_COL32(backgroundcolor.r, backgroundcolor.g, backgroundcolor.b, transparency));

	ImGui::GetWindowDrawList()->AddImageQuad(IconMap[ForegroundTexIdx].Texture,
		RotatedPos[0], RotatedPos[1], RotatedPos[2], RotatedPos[3], uvs[0], uvs[1], uvs[2], uvs[3], 
		IM_COL32(color.r, color.g, color.b, transparency));
}

void RenderManager::LoadTexture(const std::string& path, bool png, IconTypes index)
{
	Icon NewIcon;
	assert(device != nullptr);
	auto* RenderManager = RE::BSRenderManager::GetSingleton();
	
	if (!RenderManager) 
	{
		logger::error("Cannot find render manager. Initialization failed.");
	}

	auto RuntimeData = RenderManager->GetRuntimeData();

	unsigned char* ImageData = nullptr;
	// Load from disk into a raw RGBA buffer
	int ImageWidth;
	int ImageHeight;
	if (png)
	{
		ImageData = stbi_load(path.c_str(), &ImageWidth, &ImageHeight, nullptr, 4);
		if (!ImageData)
		{
			logger::error("Could not open file");
		}
	}
	else
	{
		auto* svg = nsvgParseFromFile(path.c_str(), "px", 96.0f);
		if (!svg)
		{
			logger::error("Could not open file");
		}
		auto* rast = nsvgCreateRasterizer();

		ImageWidth = static_cast<int>(svg->width);
		ImageHeight = static_cast<int>(svg->height);

		ImageData = (unsigned char*)malloc(ImageWidth * ImageHeight * 4);
		nsvgRasterize(rast, svg, 0, 0, 1, ImageData, ImageWidth, ImageHeight, ImageWidth * 4);
		nsvgDelete(svg);
		nsvgDeleteRasterizer(rast);
	}
	// Create Texture
	D3D11_TEXTURE2D_DESC Desc;
	ZeroMemory(&Desc, sizeof(Desc));
	Desc.Width = ImageWidth;
	Desc.Height = ImageHeight;
	Desc.MipLevels = 1;
	Desc.ArraySize = 1;
	Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	Desc.SampleDesc.Count = 1;
	Desc.Usage = D3D11_USAGE_DEFAULT;
	Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	Desc.CPUAccessFlags = 0;
	Desc.MiscFlags = 0;

	ID3D11Texture2D* PTexture = nullptr;
	D3D11_SUBRESOURCE_DATA SubResource;
	ZeroMemory(&SubResource, sizeof(SubResource));
	SubResource.pSysMem = ImageData;
	SubResource.SysMemPitch = Desc.Width * 4;
	SubResource.SysMemSlicePitch = 0;

	HRESULT Hr = device->CreateTexture2D(&Desc, &SubResource, &PTexture);
	if (FAILED(Hr))
	{
		logger::error("Failed texture creation");
		// Handle texture creation error
		return;
	}

	// Create Texture View
	D3D11_SHADER_RESOURCE_VIEW_DESC SrvDesc;
	ZeroMemory(&SrvDesc, sizeof(SrvDesc));
	SrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	SrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	SrvDesc.Texture2D.MipLevels = Desc.MipLevels;
	SrvDesc.Texture2D.MostDetailedMip = 0;

	ID3D11ShaderResourceView* PTextureView = nullptr;
	Hr = RuntimeData.forwarder->CreateShaderResourceView(PTexture, &SrvDesc, &NewIcon.Texture);
	if (FAILED(Hr))
	{
		// Handle shader resource view creation error
		logger::error("Failed SRV creation");
		PTexture->Release();
		return;
	}

	// Generate Mipmaps
	RuntimeData.context->GenerateMips(NewIcon.Texture);

	// Free memory
	PTexture->Release();
	if (png)
	{
		stbi_image_free(ImageData);
	}
	else
	{
		free(ImageData);
	}

	// Update icon information
	NewIcon.Width = ImageWidth;
	NewIcon.Height = ImageHeight;
	logger::info("Loaded file {}", path.c_str());
	IconMap[index] = NewIcon;
}