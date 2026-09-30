#include "FXHandler.h"
#include "SettingsLoader.h"

constexpr float ComboPowerStepSeconds = 3.0f;
constexpr float WarpSeconds = 1.0f;

void FXHandler::Initialize()
{
	RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
	MasterstrikeSound = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0xEBC26, "Skyrim.esm");
	MasterstrikeSound2 = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0x3F37C, "Skyrim.esm");
	BlockSound = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0x4D2DE, "Skyrim.esm");
	//TimedBlockSound = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0xF69C2, "Skyrim.esm");
	//TimedBlockSound = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0x10F804, "Skyrim.esm");
	TimedBlockSound = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0xB6343, "Skyrim.esm");
	TimedBlockSound2 = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0x3EDD8, "Skyrim.esm");
	// DLC1GargoyleBruteMeleeFX
	ComboPowerStepArt = DataHandler->LookupForm<RE::BGSArtObject>(0x005132, "Dawnguard.esm");
	// MAGRestorationFirePotion
	ComboPowerStepSound = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0xCD671, "Skyrim.esm");
	// InvisFXBody01
	WarpArt = DataHandler->LookupForm<RE::BGSArtObject>(0x339C8, "Skyrim.esm");
	// MAGFailSD
	RefusedSound = DataHandler->LookupForm<RE::BGSSoundDescriptorForm>(0x3D0D3, "Skyrim.esm");
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

void FXHandler::PlayTimedBlock(RE::Actor* actor)
{
	PlaySound(actor, TimedBlockSound);
	PlaySound(actor, TimedBlockSound);
	PlaySound(actor, TimedBlockSound2);
}

void FXHandler::PlayComboPowerStep(RE::Actor* actor)
{
	if (!actor || !ComboPowerStepArt)
	{
		return;
	}
	// 0x3c73c
	actor->ApplyArtObject (ComboPowerStepArt, ComboPowerStepSeconds);
	PlaySound(actor, ComboPowerStepSound);
}

void FXHandler::PlayWarp(RE::Actor* actor)
{
	if (!actor || !WarpArt)
	{
		return;
	}
	actor->ApplyArtObject(WarpArt, WarpSeconds);
}

void FXHandler::PlayAttackRefused()
{
	// At the camera rather than on the body, so the camera angle can't bury it.
	PlaySoundAt(RefusedSound, RE::PlayerCamera::GetSingleton()->GetRuntimeData2().pos, nullptr);
}

void FXHandler::PlayMasterstrike(RE::Actor* actor)
{
	PlaySound(actor, MasterstrikeSound);
	PlaySound(actor, MasterstrikeSound2);
}

void FXHandler::PlaySound(RE::Actor* actor, RE::BGSSoundDescriptorForm* sound)
{
	PlaySoundAt(sound, actor->data.location, actor->Get3D());
}

void FXHandler::PlaySoundAt(RE::BGSSoundDescriptorForm* sound, const RE::NiPoint3& position, RE::NiAVObject* follow)
{
	if (!sound)
	{
		logger::error("FXHandler: null sound descriptor");
		return;
	}
	RE::BSSoundHandle handle;
	handle.soundID = static_cast<uint32_t>(-1);
	handle.assumeSuccess = false;
	*(uint32_t*)&handle.state = 0;


	soundHelper_a(RE::BSAudioManager::GetSingleton(), &handle, sound->GetFormID(), 16);
	if (set_sound_position(&handle, position.x, position.y, position.z)) {
		if (follow)
		{
			soundHelper_b(&handle, follow);
		}
		soundHelper_c(&handle);
	}
}