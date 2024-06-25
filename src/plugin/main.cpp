#include "Hooks.h"
#include "UIMenu.h"
#include "Utils.h"
#include "SettingsLoader.h"
#include "InputHandler.h"
#include "3rdparty/PrecisionAPI.h"
#include "3rdparty/TrueDirectionalMovementAPI.h"

void InitLogger()
{
	auto path = logger::log_directory();
	if (!path)
		return;
	
	auto plugin = SKSE::PluginDeclaration::GetSingleton();
	*path /= fmt::format(FMT_STRING("{}.log"), plugin->GetName());

	std::shared_ptr<spdlog::sinks::sink> sink;
	if (WinAPI::IsDebuggerPresent()) {
		sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
	} else {
		sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
	}

	auto log = std::make_shared<spdlog::logger>("global log"s, sink);
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%s(%#): [%^%l%$] %v"s);
}


void OnDataLoad()
{
	SettingsLoader::GetSingleton()->Load("Data\\SKSE\\Plugins\\Settings.ini");
	PRECISION_API::IVPrecision3* precision = reinterpret_cast<PRECISION_API::IVPrecision3*>(PRECISION_API::RequestPluginAPI(PRECISION_API::InterfaceVersion::V3));
	if (precision)
	{
		logger::info("Precision dll found");
		precision->AddPreHitCallback(SKSE::GetPluginHandle(), Hooks::PrecisionCallback::PrecisionPrehit);
		Settings::HasPrecision = true;
	}
	else
	{
		logger::info("Precision dll not found");
		Settings::HasPrecision = false;
	}

	TDM_API::IVTDM2* tdm = reinterpret_cast<TDM_API::IVTDM2*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V2));
	if (tdm)
	{
		logger::info("TDM dll found");
		Settings::HasTDM = true;
	}
	else
	{
		logger::info("TDM dll not found");
		Settings::HasTDM = false;
	}
	
	Hooks::Hooks::Install();
	DirectionHandler::GetSingleton()->Initialize(tdm);
	FXHandler::GetSingleton()->Initialize();
	BlockHandler::GetSingleton()->Initialize();
	AttackHandler::GetSingleton()->Initialize();
	InputEventHandler::Register();
	SettingsLoader::GetSingleton()->RemovePowerAttacks();
	//SettingsLoader::GetSingleton()->RemovePowerAttacks();
	AIHandler::GetSingleton()->InitializeValues(precision);
}

void OnPostLoad()
{
	AIHandler::GetSingleton()->Cleanup();
	DirectionHandler::GetSingleton()->Cleanup();
	BlockHandler::GetSingleton()->Cleanup();
	AttackHandler::GetSingleton()->Cleanup();
}


void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
	switch (a_msg->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		OnDataLoad();
		break;
	case SKSE::MessagingInterface::kPreLoadGame:
	case SKSE::MessagingInterface::kPostLoadGame:
	case SKSE::MessagingInterface::kNewGame:
	case SKSE::MessagingInterface::kPostLoad:
		OnPostLoad();
		break;
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLogger();

	auto plugin = SKSE::PluginDeclaration::GetSingleton();
	logger::info("{} v{}"sv, plugin->GetName(), plugin->GetVersion());

	SKSE::Init(a_skse);

	logger::info("{} loaded"sv, plugin->GetName());

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging->RegisterListener("SKSE", MessageHandler)) {
		return false;
	}
	// as early as possible
	RenderManager::Install();
	return true;
}


