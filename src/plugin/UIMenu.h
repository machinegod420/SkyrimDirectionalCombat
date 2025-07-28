#pragma once

#include <vector>
#include <queue>
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
	UIHostileState hostileState;
	bool firstperson;
	bool lockout;
	bool isplayer;
};

struct TextAlert
{
	std::string text;
	float timeout;
};


namespace UI
{
	void AddDrawCommand(RE::NiPoint3 position, Directions dir, bool mirror, UIDirectionState state, UIHostileState hostileState, bool firstperson, bool lockout, bool isplayer);
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
	ForHonorMarkerOutline
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
	static void DrawDirection(RE::NiPoint2 StartPos, float depth, float uiscale, Directions dir, bool mirror, ColorRGBA color, ColorRGBA backgroundcolor, uint32_t transparency);
	static void LoadTexture(const std::string &path, bool png, IconTypes idx);


public:
	static bool Install();

};