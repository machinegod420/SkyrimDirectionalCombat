#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#define NOMINMAX
#include <Windows.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

namespace WinAPI = SKSE::WinAPI;
namespace logger = SKSE::log;
namespace fs = std::filesystem;
using namespace std::literals;

const std::string PluginName = "DirectionMod.esp";

namespace stl
{
	using namespace SKSE::stl;
}

#define UNUSED(x) (void)(x)

