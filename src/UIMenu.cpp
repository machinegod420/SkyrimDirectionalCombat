#include "UIMenu.h"
#include "SettingsLoader.h"
#include <shared_mutex>
#include <cmath>
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

struct TextAlert_Internal
{
	RE::NiPoint2 position;
	std::string text;
};

// ui is drawn on a seperate thread but accesses these static variables so
// we need to wrap them in mutexes
static std::vector<DrawCommand> DrawCommands;
// Debug panel state. Own mutex rather than sharing the draw-command one: it's
// written once per AI decision and read once per frame, so it should never
// contend with the per-marker traffic.
static DebugSnapshot DebugState;
static std::shared_mutex DebugMtx;
static std::mutex mtx;
static std::vector<TextAlert_Internal> TextAlerts_Internal;
static std::mutex mtx2;

// Per-actor display smoothing for the conditioning arcs. The AI updates its
// values in chunky integer steps at ~200ms ticks; the render loop eases a
// displayed float toward them every frame so the arc animates instead of
// stepping. Render-thread only (touched exclusively inside draw()), so no
// mutex. Entries for actors not drawn this frame are pruned each pass.
struct SmoothedConditioning
{
	std::array<float, 4> values{};
	float confidence = 0.f;
	double lastSeen = 0.0;
};
static std::unordered_map<uint32_t, SmoothedConditioning> SmoothedCond;
// Seconds of command-bearing frames (frozen while menus/loads present empty
// frames). Double: accumulates for the whole session, and float precision
// dies within hours at per-frame increments.
static double CondClock = 0.0;

namespace UI
{
	void AddDrawCommand(RE::NiPoint3 position, Directions dir, bool mirror, UIDirectionState state, UIHostileState hostileState, bool firstperson, bool lockout, bool isplayer, const std::array<int, 4>& conditioning, uint32_t actorId, float confidence)
	{
		// surprised this hasnt crashed from all the race conditions
		// this is populated on the game thread and emptied on the UI thread
		mtx.lock();
		//logger::info("Attempting to add new draw command");
		DrawCommands.push_back({ position, dir, mirror, state, hostileState, firstperson, lockout, isplayer, conditioning, actorId, confidence });
		mtx.unlock();
	}

	void SetDebugSnapshot(const DebugSnapshot& snapshot)
	{
		DebugMtx.lock();
		DebugState = snapshot;
		DebugMtx.unlock();
	}

	void AddTextAlert(RE::NiPoint2 position, const std::string& text)
	{
		mtx2.lock();
		TextAlerts_Internal.push_back({ position,text });
		mtx2.unlock();
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

void TextAlertHandler::Update(float deltaTime)
{
	TextAlertMtx.lock();
	auto Iter = TextAlerts.begin();
	while (Iter != TextAlerts.end())
	{
		if (!Iter->first)
		{
			Iter = TextAlerts.erase(Iter);
			continue;
		}
		RE::Actor* actor = Iter->first.get().get();
		if (!actor)
		{
			Iter = TextAlerts.erase(Iter);
			continue;
		}
		if (Iter->second.timeout < 0)
		{
			Iter = TextAlerts.erase(Iter);
			continue;
		}
		else
		{
			float depth = 0.f;
			RE::BGSBodyPart* bodyPart = actor->GetRace()->bodyPartData->parts[RE::BGSBodyPartDefs::LIMB_ENUM::kTorso];
			RE::NiPoint3 Position = actor->GetPosition();
			if (bodyPart)
			{
				Position = actor->GetNodeByName(bodyPart->targetName)->world.translate;
			}
			RE::NiPoint2 ScreenPosition = WorldToScreen(Position, depth, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);

			UI::AddTextAlert(ScreenPosition, Iter->second.text);
			Iter->second.timeout -= deltaTime;
		}
		Iter++;
	}
	TextAlertMtx.unlock();
}

LRESULT RenderManager::WndProcHook::thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	auto& io = ImGui::GetIO();
	if (uMsg == WM_KILLFOCUS) {
		//io.ClearInputCharacters();
		io.ClearInputKeys();
	}

	return func(hWnd, uMsg, wParam, lParam);
}

void RenderManager::D3DInitHook::thunk()
{
	func();

	logger::info("RenderManager: Initializing...");
	auto render_manager = RE::BSGraphics::Renderer::GetSingleton();
	if (!render_manager) {
		logger::error("Cannot find render manager. Initialization failed!");
		return;
	}

	auto render_data = render_manager->GetRuntimeData();

	logger::info("Getting swapchain...");
	auto swapchain = render_data.renderWindows[0].swapChain;
	if (!swapchain) {
		logger::error("Cannot find swapchain. Initialization failed!");
		return;
	}

	logger::info("Getting swapchain desc...");
	DXGI_SWAP_CHAIN_DESC sd{};
	if (swapchain->GetDesc(reinterpret_cast<REX::W32::DXGI_SWAP_CHAIN_DESC*>(std::addressof(sd))) < 0) {
		logger::error("IDXGISwapChain::GetDesc failed.");
		return;
	}

	device = reinterpret_cast<ID3D11Device*>(render_data.forwarder);
	context = reinterpret_cast<ID3D11DeviceContext*>(render_data.context);

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
	// Present keeps firing while game logic is paused (menus, load screens)
	// but the game thread produces no draw commands then. Freeze the
	// smoothing clock on commandless frames so pausing doesn't age out (and
	// wipe) every actor's arc state.
	const bool HasDrawCommands = !DrawCommands.empty();
	if (HasDrawCommands)
	{
		CondClock += ImGui::GetIO().DeltaTime;
	}
	// True exponential ease (1 - e^-kt): identical convergence per second at
	// any framerate, unlike the linear-alpha lerp (dt*k) which converges
	// faster at low fps and snaps outright below 1/k seconds per frame.
	// k=10 → ~90% settled in ~230ms, one AI tick's worth of change animating
	// over roughly one tick interval.
	const float condEase = 1.f - std::exp(-ImGui::GetIO().DeltaTime * 10.f);
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
		case UIDirectionState::FullBlock:
			active = { 0x40, 0x40, 0xFF, 0x00 };
			white = active;
			transparency2 = transparency;
			break;
		case UIDirectionState::FullBlockAndTimedBlock:
			active = { 0x80, 0xDA, 0xEA, 0x00 };
			white = active;
			transparency2 = transparency;
			break;
		case UIDirectionState::ImperfectBlock:
			active = { 0x30, 0x20, 0xD0, 0x00 };
			break;
		case UIDirectionState::TimedBlock:
			active = { 0x80, 0xDA, 0xEA, 0x00 };
			break;
		case UIDirectionState::Unblockable:
			white = { 0xFF, 0x66, 0x00, 0x00 };
			active = white;
			transparency2 = transparency;
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

		// Ease displayed arc values toward the AI's chunky tick values.
		// Runs regardless of screen visibility so arcs don't re-animate from
		// zero when an actor pops back on screen.
		std::array<float, 4> ArcVals{};
		float ArcConfidence = DrawCommands[i].confidence;
		if (DrawCommands[i].actorId != 0)
		{
			auto& smooth = SmoothedCond[DrawCommands[i].actorId];
			smooth.lastSeen = CondClock;
			for (int c = 0; c < 4; ++c)
			{
				smooth.values[c] += (static_cast<float>(DrawCommands[i].conditioning[c]) - smooth.values[c]) * condEase;
			}
			smooth.confidence += (DrawCommands[i].confidence - smooth.confidence) * condEase;
			ArcVals = smooth.values;
			ArcConfidence = smooth.confidence;
		}
		else
		{
			for (int c = 0; c < 4; ++c)
			{
				ArcVals[c] = static_cast<float>(DrawCommands[i].conditioning[c]);
			}
		}

		if (depth >= 0 && IsOnScreen(StartPos, ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y))
		{
			float scale = 1.f;
			if (DrawCommands[i].isplayer)
			{
				scale = UISettings::PlayerUIScale;
			}
			else
			{
				scale = UISettings::NPCUIScale;
			}
			// only flip horizontal axes
			if (!DrawCommands[i].mirror)
			{
				DrawDirection(StartPos, depth, scale, Directions::TR, DrawCommands[i].mirror, Dir == Directions::TR ? active : white, background, Dir == Directions::TR ? transparency : transparency2);
				DrawDirection(StartPos, depth, scale, Directions::TL, DrawCommands[i].mirror, Dir == Directions::TL ? active : white, background, Dir == Directions::TL ? transparency : transparency2);
				DrawDirection(StartPos, depth, scale, Directions::BR, DrawCommands[i].mirror, Dir == Directions::BR ? active : white, background, Dir == Directions::BR ? transparency : transparency2);
				DrawDirection(StartPos, depth, scale, Directions::BL, DrawCommands[i].mirror, Dir == Directions::BL ? active : white, background, Dir == Directions::BL ? transparency : transparency2);
				// conditioning meter arcs — dedicated element outside the
				// markers; sweep length carries the value so it stays
				// readable without touching marker color/alpha semantics
				DrawConditioningArc(StartPos, depth, scale, Directions::TR, ArcVals[(int)Directions::TR], ArcConfidence);
				DrawConditioningArc(StartPos, depth, scale, Directions::TL, ArcVals[(int)Directions::TL], ArcConfidence);
				DrawConditioningArc(StartPos, depth, scale, Directions::BR, ArcVals[(int)Directions::BR], ArcConfidence);
				DrawConditioningArc(StartPos, depth, scale, Directions::BL, ArcVals[(int)Directions::BL], ArcConfidence);
			}
			else
			{
				DrawDirection(StartPos, depth, scale, Directions::TR, DrawCommands[i].mirror, Dir == Directions::TL ? active : white, background, Dir == Directions::TL ? transparency : transparency2);
				DrawDirection(StartPos, depth, scale, Directions::TL, DrawCommands[i].mirror, Dir == Directions::TR ? active : white, background, Dir == Directions::TR ? transparency : transparency2);
				DrawDirection(StartPos, depth, scale, Directions::BR, DrawCommands[i].mirror, Dir == Directions::BL ? active : white, background, Dir == Directions::BL ? transparency : transparency2);
				DrawDirection(StartPos, depth, scale, Directions::BL, DrawCommands[i].mirror, Dir == Directions::BR ? active : white, background, Dir == Directions::BR ? transparency : transparency2);
				// mirror mapping matches the marker draw above
				DrawConditioningArc(StartPos, depth, scale, Directions::TR, ArcVals[(int)Directions::TL], ArcConfidence);
				DrawConditioningArc(StartPos, depth, scale, Directions::TL, ArcVals[(int)Directions::TR], ArcConfidence);
				DrawConditioningArc(StartPos, depth, scale, Directions::BR, ArcVals[(int)Directions::BL], ArcConfidence);
				DrawConditioningArc(StartPos, depth, scale, Directions::BL, ArcVals[(int)Directions::BR], ArcConfidence);
			}
		}
		//logger::info("Direction pos {} {}", StartPos.x, StartPos.y);

	}
	// prune smoothing entries by age (seconds of command-bearing time, so
	// framerate-independent and frozen through pauses) — per-actor display
	// gates (OnlyShowTargetted, DisplayDistance) can drop an actor's command
	// for a frame or two, and exact-match pruning would replay their arc
	// from zero on return.
	if (HasDrawCommands)
	{
		for (auto it = SmoothedCond.begin(); it != SmoothedCond.end();)
		{
			if (CondClock - it->second.lastSeen > 1.5)
			{
				it = SmoothedCond.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
	DrawCommands.clear();
	mtx.unlock();

	mtx2.lock();
	for (uint32_t i = 0; i < TextAlerts_Internal.size(); ++i)
	{
		float currFontSize = ImGui::GetFontSize();
		ImVec2 ScreenPosition = ImVec2(TextAlerts_Internal[i].position.x, TextAlerts_Internal[i].position.y);
		ImVec2 text_size = ImGui::CalcTextSize(TextAlerts_Internal[i].text.c_str());
		auto text_x = 0.f;
		auto text_y = 0.f;

		text_x = -text_size.x * 0.5f;
		text_y = -text_size.y;


		ImGui::GetWindowDrawList()->AddText(ScreenPosition, 0xFFFFFFFF, TextAlerts_Internal[i].text.c_str());
	}
	TextAlerts_Internal.clear();
	mtx2.unlock();

	ImGui::End();

	if (UISettings::ShowDebugOverlay)
	{
		DebugSnapshot Snapshot;
		{
			std::shared_lock lock(DebugMtx);
			Snapshot = DebugState;
		}
		if (Snapshot.valid)
		{
			// Its own window, decorated and positioned — the marker window
			// above is fullscreen, borderless and input-transparent, which is
			// wrong for something you read rather than glance at.
			ImGui::SetNextWindowPos(ImVec2(20.f, 20.f), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowBgAlpha(0.65f);
			ImGui::Begin("Dirmod AI", nullptr,
				ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);

			ImGui::Text("%s  (%s, tier %d)", Snapshot.name.c_str(), Snapshot.archetype, Snapshot.difficulty);
			ImGui::Separator();
			ImGui::Text("decision   %s", Snapshot.decisionKind);
			ImGui::Text("state      %s", Snapshot.defending ? "defending" : "offense");
			ImGui::Text("guard      %d   target %d", (int)Snapshot.guard, (int)Snapshot.targetGuard);
			ImGui::Separator();
			ImGui::Text("belief     TR %3d  TL %3d  BL %3d  BR %3d",
				Snapshot.beliefs[(int)Directions::TR], Snapshot.beliefs[(int)Directions::TL],
				Snapshot.beliefs[(int)Directions::BL], Snapshot.beliefs[(int)Directions::BR]);
			ImGui::Text("streaks    cond %d  switch %d  same %d",
				Snapshot.conditioningStreak, Snapshot.switchStreak, Snapshot.sameStreak);
			ImGui::Text("spacing    %4.2f", Snapshot.spacingMult);
			// Unsquared alongside the squared pair the AI actually compares, so
			// the two reach sources can be read against a real distance.
			ImGui::Text("range      dist %6.1f   attackReach %6.1f (+lunge)",
				std::sqrt(Snapshot.targetDistSQ),
				std::sqrt(Snapshot.weaponLengthSQ));
			ImGui::Text("           Actor::GetReach  self %6.1f   target %6.1f",
				Snapshot.actorReach, Snapshot.targetActorReach);
			if (Snapshot.powerWindup > 0.f)
			{
				ImGui::Text("pwr windup %4.0fms  (timing blocks)", Snapshot.powerWindup * 1000.f);
			}
			else
			{
				ImGui::Text("pwr windup   --     (blocking on reflex)");
			}

			ImGui::End();
		}
	}
}

void RenderManager::DrawDirection(RE::NiPoint2 StartPos, float depth, float uiscale, Directions dir, bool mirror, ColorRGBA color, ColorRGBA backgroundcolor, uint32_t transparency)
{
	/*
	logger::info("Drawing quad at pos {} {} with colors {} {} transparency {}", StartPos.x, StartPos.x,
		IM_COL32(color.r, color.b, color.g, 255),IM_COL32(backgroundcolor.r, backgroundcolor.b, backgroundcolor.g, 255), transparency);
	*/

	constexpr static ImVec2 uvs[4] = { ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.0f), ImVec2(1.0f, 1.0f), ImVec2(0.0f, 1.0f) };
	RE::NiPoint2 CenterPos = StartPos;
	depth = std::clamp(depth, 300.f, 1000.f);
	RE::NiPoint2 EndPos = StartPos;
	float scale = UISettings::Size / depth;
	scale *= uiscale;
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

	//ImGui::GetWindowDrawList()->AddText(ImVec2(CenterPos.x, CenterPos.y), 0xFFFFFFFF, "test");
}

void RenderManager::DrawConditioningArc(RE::NiPoint2 StartPos, float depth, float uiscale, Directions dir, float value, float confidence)
{
	// Sweep carries the value: the arc grows symmetrically outward from the
	// direction's center angle as belief builds, reaching ~48 degrees at the
	// display max. Constant color and opacity — length is readable at a
	// glance in a way brightness deltas on faint markers are not.
	//
	// Display normalization: under the belief-budget model a single line can
	// theoretically hold up to 100, but committed play mostly lives below
	// ~50 — full sweep at 50 keeps the arc expressive in the range that
	// actually occurs; deeper values just hold max sweep.
	if (value <= 2.f)
	{
		return;
	}
	if (value > 50.f)
	{
		value = 50.f;
	}
	depth = std::clamp(depth, 300.f, 1000.f);
	float scale = UISettings::Size / depth;
	scale *= uiscale;
	const float dist = UISettings::Length * scale;
	const float size = UISettings::Length * scale;
	const float pad = size * 0.35f;

	constexpr float pi = 3.14159265359f;
	// screen space, y down: 0 = right, -pi/2 = up
	float centerAngle = 0.f;
	float radius = 0.f;
	if (Settings::MNBMode)
	{
		// cardinal layout: markers offset by dist along the axes
		switch (dir)
		{
		case Directions::TR: centerAngle = -0.5f * pi; break;
		case Directions::TL: centerAngle = pi; break;
		case Directions::BL: centerAngle = 0.5f * pi; break;
		case Directions::BR: centerAngle = 0.f; break;
		}
		radius = dist + size + pad;
	}
	else if (Settings::ForHonorMode)
	{
		// top marker sits at 2*dist, side markers at 1.5*dist
		switch (dir)
		{
		case Directions::TR:
		case Directions::TL:
			centerAngle = -0.5f * pi;
			radius = 2.f * dist + size + pad;
			break;
		case Directions::BL:
			centerAngle = pi;
			radius = 1.5f * dist + size + pad;
			break;
		case Directions::BR:
			centerAngle = 0.f;
			radius = 1.5f * dist + size + pad;
			break;
		}
	}
	else
	{
		// default diagonal layout: markers at (+-dist, +-dist)
		switch (dir)
		{
		case Directions::TR: centerAngle = -0.25f * pi; break;
		case Directions::TL: centerAngle = -0.75f * pi; break;
		case Directions::BL: centerAngle = 0.75f * pi; break;
		case Directions::BR: centerAngle = 0.25f * pi; break;
		}
		radius = 1.414f * dist + size + pad;
	}

	const float t = value / 50.f;
	const float halfSweep = (24.f * pi / 180.f) * t;
	const float thickness = std::max(1.2f, size * 0.12f);

	// confidence tint: the whole arc set warms from yellow toward red as
	// this enemy's recent exchange record improves — same "how is this
	// fight going" family as the sweep, different axis (them vs you).
	if (confidence < 0.f) { confidence = 0.f; }
	if (confidence > 1.f) { confidence = 1.f; }
	const int g = 0xE0 + (int)(confidence * (float)(0x58 - 0xE0));
	const int b = 0x40 + (int)(confidence * (float)(0x2E - 0x40));

	auto* drawList = ImGui::GetWindowDrawList();
	drawList->PathArcTo(ImVec2(StartPos.x, StartPos.y), radius, centerAngle - halfSweep, centerAngle + halfSweep, 20);
	drawList->PathStroke(IM_COL32(0xFF, g, b, 210), ImDrawFlags_None, thickness);
}

void RenderManager::LoadTexture(const std::string& path, bool png, IconTypes index)
{
	Icon NewIcon;
	assert(device != nullptr);
	auto* RenderManager = RE::BSGraphics::Renderer::GetSingleton();
	
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
	//
	// Mipmap generation requires three things in concert:
	//   1. MipLevels = 0           -> runtime auto-computes the full chain
	//                                 (floor(log2(max(W,H))) + 1 levels).
	//   2. BIND_RENDER_TARGET      -> GenerateMips writes to lower mips
	//                                 internally by rendering into them.
	//   3. MISC_GENERATE_MIPS      -> explicitly opts the texture into the
	//                                 generation path.
	// Without all three, the GenerateMips() call below silently no-ops on a
	// single-level texture and the sampler only ever reads mip 0 — which is
	// where the "rotated icons look jaggy when minified" problem comes from.
	D3D11_TEXTURE2D_DESC Desc;
	ZeroMemory(&Desc, sizeof(Desc));
	Desc.Width = ImageWidth;
	Desc.Height = ImageHeight;
	Desc.MipLevels = 0;
	Desc.ArraySize = 1;
	Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	Desc.SampleDesc.Count = 1;
	Desc.Usage = D3D11_USAGE_DEFAULT;
	Desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	Desc.CPUAccessFlags = 0;
	Desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

	// MISC_GENERATE_MIPS is incompatible with passing pInitialData to
	// CreateTexture2D — the texture has to be created empty and then
	// populated via UpdateSubresource below.
	ID3D11Texture2D* PTexture = nullptr;
	HRESULT Hr = device->CreateTexture2D(&Desc, nullptr, &PTexture);
	if (FAILED(Hr))
	{
		logger::error("Failed texture creation");
		return;
	}

	// Upload pixel data into mip 0. GenerateMips() further down will
	// derive mips 1..N from this.
	RuntimeData.context->UpdateSubresource(
		reinterpret_cast<REX::W32::ID3D11Resource*>(PTexture),
		0,        // mip 0
		nullptr,  // full subresource
		ImageData,
		ImageWidth * 4,
		0);

	// Create Texture View
	D3D11_SHADER_RESOURCE_VIEW_DESC SrvDesc;
	ZeroMemory(&SrvDesc, sizeof(SrvDesc));
	SrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	SrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	// -1 = "use all mips starting from MostDetailedMip." Required so the
	// sampler can actually select lower mips at minification — paired with
	// the auto-generated chain above.
	SrvDesc.Texture2D.MipLevels = static_cast<UINT>(-1);
	SrvDesc.Texture2D.MostDetailedMip = 0;

	ID3D11ShaderResourceView* PTextureView = nullptr;
	Hr = RuntimeData.forwarder->CreateShaderResourceView(
		reinterpret_cast<REX::W32::ID3D11Resource*>(PTexture),
		reinterpret_cast<const REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC*>(&SrvDesc),
		reinterpret_cast<REX::W32::ID3D11ShaderResourceView**>(&NewIcon.Texture));
	if (FAILED(Hr))
	{
		// Handle shader resource view creation error
		logger::error("Failed SRV creation");
		PTexture->Release();
		return;
	}

	// Generate Mipmaps
	RuntimeData.context->GenerateMips(reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(NewIcon.Texture));

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