#include "Hooks.h"
#include "UIMenu.h"
#include "Utils.h"
#include "SettingsLoader.h"
#include "InputHandler.h"
#include "CreatureHandler.h"
#include "3rdparty/PrecisionAPI.h"
#include "3rdparty/TrueDirectionalMovementAPI.h"

void InitLogger()
{
	auto path = logger::log_directory();
	if (!path)
		return;
	
	auto plugin = SKSE::PluginDeclaration::GetSingleton();
	*path /= std::format("{}.log", plugin->GetName());

	std::shared_ptr<spdlog::sinks::sink> sink;
	if (IsDebuggerPresent()) {
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
	DodgeHandler::GetSingleton()->Initialize();
	InputEventHandler::Register();
	SettingsLoader::GetSingleton()->HumanoidUndeadRaces();
	CreatureHandler::GetSingleton()->Initialize();
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
	DodgeHandler::GetSingleton()->Cleanup();
}


void OnGameSaved(SKSE::SerializationInterface* a_intfc)
{
	AIHandler::GetSingleton()->SavePlayerHabit(a_intfc);
	AIHandler::GetSingleton()->SavePlayerStats(a_intfc);
	AIHandler::GetSingleton()->LogPlayerStats("at save");
}

// New data goes in its own record type. Types this build doesn't know are skipped,
// and a record missing from an older save keeps the revert defaults.
void OnGameLoaded(SKSE::SerializationInterface* a_intfc)
{
	std::uint32_t Type = 0;
	std::uint32_t Version = 0;
	std::uint32_t Length = 0;
	while (a_intfc->GetNextRecordInfo(Type, Version, Length))
	{
		if (Type == AIHandler::PlayerHabitRecord)
		{
			AIHandler::GetSingleton()->LoadPlayerHabit(a_intfc, Version, Length);
		}
		else if (Type == AIHandler::PlayerStatsRecord)
		{
			AIHandler::GetSingleton()->LoadPlayerStats(a_intfc, Version, Length);
		}
		else if (Type == AIHandler::PlayerStaminaRecord)
		{
			AIHandler::GetSingleton()->LoadPlayerStaminaStats(a_intfc, Version, Length);
		}
		else if (Type == AIHandler::PlayerFeintRecord)
		{
			AIHandler::GetSingleton()->LoadPlayerFeintStats(a_intfc, Version, Length);
		}
		else if (Type == AIHandler::PlayerOutcomeRecord)
		{
			AIHandler::GetSingleton()->LoadPlayerOutcomeStats(a_intfc, Version, Length);
		}
	}
	AIHandler::GetSingleton()->LogPlayerStats("at load");
}

// New game, or a load: runs before OnGameLoaded. Resets everything the co-save holds.
void OnGameReverted(SKSE::SerializationInterface*)
{
	AIHandler::GetSingleton()->ResetPlayerHabit();
	AIHandler::GetSingleton()->ResetPlayerStats();
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
	logger::info("{} v{}"sv, plugin->GetName(), plugin->GetVersion().string());

	// One trampoline for every hook: CommonLib only honours the first request.
	SKSE::Init(a_skse, { .trampoline = true, .trampolineSize = 256 });

	logger::info("{} loaded"sv, plugin->GetName());

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging->RegisterListener("SKSE", MessageHandler)) {
		return false;
	}
	auto* serialization = SKSE::GetSerializationInterface();
	serialization->SetUniqueID('DIRP');
	serialization->SetSaveCallback(OnGameSaved);
	serialization->SetLoadCallback(OnGameLoaded);
	serialization->SetRevertCallback(OnGameReverted);
	// as early as possible
	RenderManager::Install();
	return true;
}


