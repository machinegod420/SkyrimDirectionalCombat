#pragma once

#include <vector>
#include <d3d11.h>
#include <queue>
#include <array>
#include "Utils.h"
#include "Direction.h"


enum class UIDirectionState
{
	// bitmask?
	Default,
	Attacking,
	Blocking,
	Unblockable,
	ImperfectBlock,
	FullBlock,
	TimedBlock,
	FullBlockAndTimedBlock
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
	// 0-1, pops when this actor's combo advances. Blended toward white.
	float comboFlash = 0.f;
	// 0-1 combo fill, 1 when one more landed hit finishes it. Drives the halo.
	float comboProgress = 0.f;
	// Bitmask over Directions of the slots to draw. Creatures show only their pair.
	uint8_t shownDirs = 0xF;
	UIHostileState hostileState;
	bool firstperson;
	bool lockout;
	bool isplayer;
	// AI conditioning meter per guard direction, indexed by Directions.
	// Values follow the belief-budget model (sum across lines <= 100; a
	// single line can exceed 25). Rendered as the arc element; smoothed
	// per-frame in the draw loop.
	std::array<int, 4> conditioning{};
	// Native actor handle, used to key the per-actor arc smoothing state on
	// the render thread. 0 = no smoothing identity (arcs render unsmoothed).
	uint32_t actorId = 0;
	// Normalized 0-1 positive mistakeRatio: how well this AI has been doing
	// in recent exchanges. Warms the arc color from yellow toward red.
	float confidence = 0.f;
};

struct TextAlert
{
	std::string text;
	float timeout;
};

struct DebugSnapshot
{
	std::string name;
	const char* archetype = "?";
	const char* decisionKind = "none";
	std::array<int, 4> beliefs{};
	int conditioningStreak = 0;
	int switchStreak = 0;
	int sameStreak = 0;
	bool defending = false;
	float spacingMult = 1.f;
	float targetDistSQ = 0.f;
	float weaponLengthSQ = 0.f;
	float actorReach = 0.f;
	float targetActorReach = 0.f;
	// Learned power-attack windup for the current target, seconds. 0 = not yet
	// measured, so the AI is still blocking on reflex rather than timing.
	float powerWindup = 0.f;
	int difficulty = 0;
	Directions guard = Directions::TR;
	Directions targetGuard = Directions::TR;
	bool valid = false;
};


namespace UI
{
	void AddDrawCommand(RE::NiPoint3 position, Directions dir, bool mirror, UIDirectionState state, float comboFlash, float comboProgress, UIHostileState hostileState, bool firstperson, bool lockout, bool isplayer, const std::array<int, 4>& conditioning = {}, uint32_t actorId = 0, float confidence = 0.f, uint8_t shownDirs = 0xF);
	// Replaces the debug panel's contents. Last writer wins, so with several
	// NPCs on the player it shows whichever decided most recently — the name
	// field says which.
	void SetDebugSnapshot(const DebugSnapshot& snapshot);
}


class Icon
{
public:
	ID3D11ShaderResourceView* Texture = nullptr;
	int Width = 0;
	int Height = 0;
};

enum class IconTypes
{
	Marker,
	MarkerOutline,
	ForHonorMarker,
	ForHonorMarkerOutline,
	// Outer ring drawn on its own for the combo charge pulse.
	MarkerOutline2,
	ForHonorMarkerOutline2,
	KCDMarker,
	KCDMarkerOutline,
	KCDMarkerOutline2
};
struct ColorRGBA
{
	// Bitmask for red, green, blue, and alpha channels
	uint32_t r : 8;
	uint32_t g : 8;
	uint32_t b : 8;
	uint32_t a : 8;
};

// run on gamethread
class TextAlertHandler
{
public:
	static TextAlertHandler* GetSingleton()
	{
		static TextAlertHandler obj;
		return std::addressof(obj);
	}

	void Update(float deltaTime);

	inline void PushTextAlert(RE::ActorHandle handle, const std::string& text, float timeout)
	{
		TextAlertMtx.lock();
		TextAlerts[handle] = TextAlert({ text, timeout });
		TextAlertMtx.unlock();
	}

private:

	std::unordered_map<RE::ActorHandle, TextAlert> TextAlerts;
	std::mutex TextAlertMtx;
};

// run on rhithread
class RenderManager
{
	struct WndProcHook
	{
		static LRESULT thunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
		static inline WNDPROC func;
	};

	struct D3DInitHook
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
		// https://github.com/D7ry/wheeler/blob/5124a10a24a841886fa3a16510764325f37c69f8/src/bin/Rendering/RenderManager.h#L20
		static constexpr auto id = REL::RelocationID(75595, 77226);
		static constexpr auto offset = REL::VariantOffset(0x9, 0x275, 0x00);  // VR unknown

		static inline std::atomic<bool> initialized = false;
	};

	struct DXGIPresentHook
	{
		static void thunk(std::uint32_t a_p1);
		static inline REL::Relocation<decltype(thunk)> func;

		static constexpr auto id = REL::RelocationID(75461, 77246);
		static constexpr auto offset = REL::Offset(0x9);
	};


private:

	static std::map<IconTypes, Icon> IconMap;


	RenderManager() = delete;
	
	static void draw();
	static void MessageCallback(SKSE::MessagingInterface::Message* msg);

	static inline bool ShowMeters = false;
	static inline ID3D11Device* device = nullptr;
	static inline ID3D11DeviceContext* context = nullptr;
	// glowPass draws only the outer ring sprite, for the combo charge pulse.
	static void DrawDirection(RE::NiPoint2 StartPos, float depth, float uiscale, Directions dir, bool mirror, ColorRGBA color, ColorRGBA backgroundcolor, uint32_t transparency, bool glowPass = false);
	static void DrawConditioningArc(RE::NiPoint2 StartPos, float depth, float uiscale, Directions dir, float value, float confidence);
	static void LoadTexture(const std::string &path, bool png, IconTypes idx);


public:
	static bool Install();

};