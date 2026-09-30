#include "Utils.h"
#include "DirectionHandler.h"
#include "AIHandler.h"
#include "AttackHandler.h"
#include "BlockHandler.h"
#include "SettingsLoader.h"


void DumpActorState(RE::Actor* a_actor, const char* a_tag)
{
	if (!a_actor || !Settings::VerboseLogging)
	{
		return;
	}

	auto* state = a_actor->AsActorState();
	const std::uint32_t s1 = *reinterpret_cast<const std::uint32_t*>(&state->actorState1);
	const std::uint32_t s2 = *reinterpret_cast<const std::uint32_t*>(&state->actorState2);
	auto& runtime = a_actor->GetActorRuntimeData();

	std::uint32_t cached = 0;
	bool doNoDamage = false;
	bool alwaysHit = false;
	if (auto* process = runtime.currentProcess)
	{
		if (process->cachedValues)
		{
			cached = process->cachedValues->flags.underlying();
		}
		if (process->middleHigh)
		{
			doNoDamage = process->middleHigh->doNoDamage;
			alwaysHit = process->middleHigh->alwaysHit;
		}
	}

	bool precisionActive = false;
	bool precisionHittable = false;
	auto* precision = AIHandler::GetSingleton()->Precision;
	if (Settings::HasPrecision && precision)
	{
		const auto handle = a_actor->GetHandle();
		precisionActive = precision->IsActorActive(handle);
		precisionHittable = precision->IsActorCharacterControllerHittable(handle);
	}

	logger::info("[snap {}] {} s1={:08X} s2={:08X} bits={:08X} flags={:08X} cached={:08X} "
				 "ghost={} noDmg={} alwaysHit={} cc={} pAct={} pHit={} perks={}",
		a_tag, a_actor->GetName(), s1, s2,
		runtime.boolBits.underlying(), runtime.boolFlags.underlying(), cached,
		a_actor->IsGhost(), doNoDamage, alwaysHit,
		a_actor->GetCharController() != nullptr,
		precisionActive, precisionHittable,
		DirectionHandler::GetSingleton()->HasDirectionalPerks(a_actor));
}
