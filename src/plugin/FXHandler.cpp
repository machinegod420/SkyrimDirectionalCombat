#include "FXHandler.h"

void FXHandler::Initialize()
{
	RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
	MasterstrikeSound = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0xEBC26, "Skyrim.esm");
	MasterstrikeSound2 = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0x3F37C, "Skyrim.esm");
	BlockSound = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0x97783, "Skyrim.esm");
	logger::info("FXHandler Initialized");
}

// https://github.com/D7ry/EldenParry/blob/main/src/Utils.hpp#L10
static inline int soundHelper_a(void* manager, RE::BSSoundHandle* a2, int a3, int a4)  //sub_140BEEE70
{
	using func_t = decltype(&soundHelper_a);
	REL::Relocation<func_t> func{ RELOCATION_ID(66401, 67663) };
	return func(manager, a2, a3, a4);
}

static inline void soundHelper_b(RE::BSSoundHandle* a1, RE::NiAVObject* source_node)  //sub_140BEDB10
{
	using func_t = decltype(&soundHelper_b);
	REL::Relocation<func_t> func{ RELOCATION_ID(66375, 67636) };
	return func(a1, source_node);
}

static inline char __fastcall soundHelper_c(RE::BSSoundHandle* a1)  //sub_140BED530
{
	using func_t = decltype(&soundHelper_c);
	REL::Relocation<func_t> func{ RELOCATION_ID(66355, 67616) };
	return func(a1);
}

static inline char set_sound_position(RE::BSSoundHandle* a1, float x, float y, float z)
{
	using func_t = decltype(&set_sound_position);
	REL::Relocation<func_t> func{ RELOCATION_ID(66370, 67631) };
	return func(a1, x, y, z);
}

void FXHandler::PlayBlock(RE::Actor* actor)
{
	PlaySound(actor, BlockSound);
}

void FXHandler::PlayMasterstrike(RE::Actor* actor)
{
	PlaySound(actor, MasterstrikeSound);
	PlaySound(actor, MasterstrikeSound2);
}

void FXHandler::PlaySound(RE::Actor* actor, RE::BGSSoundDescriptorForm* sound)
{
	RE::BSSoundHandle handle;
	handle.soundID = static_cast<uint32_t>(-1);
	handle.assumeSuccess = false;
	*(uint32_t*)&handle.state = 0;


	soundHelper_a(RE::BSAudioManager::GetSingleton(), &handle, sound->GetFormID(), 16);
	if (set_sound_position(&handle, actor->data.location.x, actor->data.location.y, actor->data.location.z)) {
		soundHelper_b(&handle, actor->Get3D());
		soundHelper_c(&handle);
	}
}