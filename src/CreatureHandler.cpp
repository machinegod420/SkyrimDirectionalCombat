#include "CreatureHandler.h"
#include "DirectionHandler.h"
#include "SettingsLoader.h"
#include "UIMenu.h"

// Case-insensitive substring test on the raw string: no copy, no state.
void CreatureHandler::UpdateActor(RE::Actor* actor)
{
	if (!IsCreatureOpponent(actor))
	{
		return;
	}
	if (!actor->IsInCombat())
	{
		std::unique_lock Lock(AttackLinesMtx);
		AttackLines.erase(actor->GetHandle());
		return;
	}
	const auto State = actor->AsActorState()->actorState1.meleeAttackState;
	const bool Attacking = State != RE::ATTACK_STATE_ENUM::kNone && State != RE::ATTACK_STATE_ENUM::kBash;
	// The line outlives the swing: between swings it is the guard the AI
	// reads, so a new line is a switch and a repeat is a hold. Only the swing
	// tag is cleared, so the next swing is assigned afresh.
	Directions Line = Directions::TR;
	bool HasLine = false;
	if (!Attacking)
	{
		std::unique_lock Lock(AttackLinesMtx);
		auto Iter = AttackLines.find(actor->GetHandle());
		if (Iter != AttackLines.end())
		{
			Iter->second.attack = nullptr;
		}
		else
		{
			// Seed a real line before the first swing, so the AI never reads
			// the accessor's default as a guard the creature can't attack on.
			Directions Seed[2];
			PairFor(actor, Seed);
			AttackLines[actor->GetHandle()] = { Seed[0], nullptr };
		}
	}
	else
	{
		// attackData can trail the state change; retry next frame until it is set.
		// It also changes mid-chain, and that is what marks the next swing.
		auto Process = actor->GetActorRuntimeData().currentProcess;
		RE::BGSAttackData* AttackData = (Process && Process->high) ? Process->high->attackData.get() : nullptr;
		if (AttackData)
		{
			{
				std::shared_lock Lock(AttackLinesMtx);
				auto Iter = AttackLines.find(actor->GetHandle());
				if (Iter != AttackLines.end() && Iter->second.attack == AttackData)
				{
					Line = Iter->second.line;
					HasLine = true;
				}
			}
			if (!HasLine)
			{
				Line = LineForAttack(actor, AttackData);
				{
					std::unique_lock Lock(AttackLinesMtx);
					AttackLines[actor->GetHandle()] = { Line, AttackData };
				}
				HasLine = true;
				LogRaceOnce(actor);
				if (Settings::VerboseLogging) logger::info("[creature] {} swing {} -> line {} (height {} player {})", Who(actor), AttackData->event.data() ? AttackData->event.data() : "", static_cast<int>(Line), BodyHeight(actor), BodyHeight(RE::PlayerCharacter::GetSingleton()));
			}
		}
	}
	// The pair is the tell, shown whenever the creature is fighting; the swing
	// in progress lights its line.
	if (UISettings::ShowUI && DirectionHandler::GetSingleton()->ShouldShowMarker(actor))
	{
		Directions Pair[2];
		PairFor(actor, Pair);
		const uint8_t Shown = static_cast<uint8_t>((1 << static_cast<int>(Pair[0])) | (1 << static_cast<int>(Pair[1])));
		// Between swings the lit slot is parked on a hidden line, so the pair
		// draws as plain background.
		Directions Hidden = Directions::TR;
		for (int d = 0; d < 4; ++d)
		{
			if (static_cast<Directions>(d) != Pair[0] && static_cast<Directions>(d) != Pair[1])
			{
				Hidden = static_cast<Directions>(d);
				break;
			}
		}
		const RE::NiPoint3 Position = MarkerPosition(actor);
		const UIHostileState Hostile = actor->IsHostileToActor(RE::PlayerCharacter::GetSingleton()) ? UIHostileState::Hostile : UIHostileState::Neutral;
		UI::AddDrawCommand(Position, HasLine ? Line : Hidden, true, HasLine ? UIDirectionState::Attacking : UIDirectionState::Default,
			0.f, 0.f, Hostile, false, false, false, {}, actor->GetHandle().native_handle(), 0.f, Shown);
	}
}

void CreatureHandler::Update(float)
{
	std::unique_lock Lock(AttackLinesMtx);
	auto Iter = AttackLines.begin();
	while (Iter != AttackLines.end())
	{
		if (!Iter->first || !Iter->first.get())
		{
			Iter = AttackLines.erase(Iter);
			continue;
		}
		Iter++;
	}
}

bool CreatureHandler::GetAttackLine(RE::Actor* actor, Directions& outDir) const
{
	std::shared_lock Lock(AttackLinesMtx);
	auto Iter = AttackLines.find(actor->GetHandle());
	if (Iter == AttackLines.end())
	{
		return false;
	}
	outDir = Iter->second.line;
	return true;
}

bool CreatureHandler::IsDirectionalAttacker(RE::Actor* actor) const
{
	if (!IsCreatureOpponent(actor))
	{
		return false;
	}
	std::shared_lock Lock(AttackLinesMtx);
	auto Iter = AttackLines.find(actor->GetHandle());
	return Iter != AttackLines.end() && Iter->second.attack != nullptr;
}

void CreatureHandler::PairFor(RE::Actor* actor, Directions outPair[2])
{
	// Taller than the player swings high, shorter swings low.
	const float PlayerHeight = BodyHeight(RE::PlayerCharacter::GetSingleton());
	const bool Large = BodyHeight(actor) >= PlayerHeight * Settings::CreatureLargeHeightRatio;
	outPair[0] = Large ? Directions::TL : Directions::BL;
	outPair[1] = Large ? Directions::TR : Directions::BR;
	// A folded mode collapses one of the pairs onto a single line, so the pair
	// borrows the far line from the other height rather than attacking one line.
	if (DirectionHandler::FoldDirection(outPair[0]) == DirectionHandler::FoldDirection(outPair[1]))
	{
		outPair[0] = Large ? Directions::BL : Directions::TL;
	}
}

Directions CreatureHandler::LineForAttack(RE::Actor* actor, RE::BGSAttackData* attackData)
{
	// Size picks the pair, the attack picks the side.
	Directions Pair[2];
	PairFor(actor, Pair);
	// The name is the tell when it has one (AttackStart_LeftSide etc.).
	const std::string_view Name = attackData->event.data() ? attackData->event.data() : "";
	size_t Side = 0;
	if (ContainsNoCase(Name, "left"))
	{
		Side = 0;
	}
	else if (ContainsNoCase(Name, "right"))
	{
		Side = 1;
	}
	else
	{
		Side = AttackIndex(actor->GetRace(), attackData) % 2;
	}
	return Pair[Side];
}

float CreatureHandler::BodyHeight(RE::Actor* actor)
{
	return actor->GetHeight();
}

float CreatureHandler::SizeRatio(RE::Actor* actor)
{
	const float PlayerHeight = BodyHeight(RE::PlayerCharacter::GetSingleton());
	return PlayerHeight > 0.f ? BodyHeight(actor) / PlayerHeight : 1.f;
}

float CreatureHandler::SizeWeight(RE::Actor* actor)
{
	return SizeRatio(actor) * Settings::CreatureSizeWeight;
}

size_t CreatureHandler::AttackIndex(RE::TESRace* race, RE::BGSAttackData* attackData)
{
	const char* Name = attackData->event.data();
	const std::string Event = Name ? Name : "";
	if (!race || !race->attackDataMap)
	{
		return std::hash<std::string>{}(Event);
	}
	// A dozen names at most, once per swing: cheaper than a shared cache.
	std::vector<std::string> Names;
	for (auto& iter : race->attackDataMap->attackDataMap)
	{
		Names.emplace_back(iter.first.data() ? iter.first.data() : "");
	}
	std::sort(Names.begin(), Names.end());
	const auto Found = std::find(Names.begin(), Names.end(), Event);
	if (Found == Names.end())
	{
		return std::hash<std::string>{}(Event);
	}
	return static_cast<size_t>(Found - Names.begin());
}

bool CreatureHandler::IsCreatureOpponent(RE::Actor* actor) const
{
	// TDMOnlyLockedHumanoids effectively disables this whole file
	if (!Settings::CreatureDirectionalAttacks || Settings::TDMOnlyLockedHumanoids)
	{
		return false;
	}
	return actor && !actor->IsPlayerRef() && IsCreatureGraph(actor->GetRace());
}

void CreatureHandler::Initialize()
{
	for (auto* race : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESRace>())
	{
		if (GraphIsCreature(race))
		{
			CreatureRaces.insert(race);
		}
	}
	logger::info("CreatureHandler: {} creature races", CreatureRaces.size());

	// The first bone of each entry in TDM's TrueDirectionalMovement_base.toml, its default lock point.
	struct Override
	{
		RE::FormID id;
		const char* plugin;
		const char* bone;
	};
	static constexpr Override Overrides[] = {
		{ 0x13492, "Skyrim.esm", "NPC Head" },             // dragon
		{ 0x1BC4A, "Dragonborn.esm", "NPC Head" },         // Dragonborn dragon
		{ 0x517AB, "Skyrim.esm", "NPC Spine1 [Spn1]" },    // giant
		{ 0x5B2E9, "Skyrim.esm", "NPC Spine2 [Spn2]" },    // frost atronach
		{ 0x800EC, "Skyrim.esm", "NPC Spine2 [Spn2]" },    // dwarven centurion
		{ 0x1E2A3, "Dragonborn.esm", "Boar_Reikling_Chest" },  // mounted riekling
		{ 0x28537, "Dragonborn.esm", "NPC Spine1 [Spn1]" },    // benthic lurker
		{ 0x1DCBC, "Dragonborn.esm", "MagicEffectsNode" },     // seeker
	};
	for (const auto& Entry : Overrides)
	{
		if (auto* Parts = RE::TESDataHandler::GetSingleton()->LookupForm<RE::BGSBodyPartData>(Entry.id, Entry.plugin))
		{
			MarkerBones.emplace(Parts, Entry.bone);
		}
	}
}

RE::NiPoint3 CreatureHandler::MarkerPosition(RE::Actor* actor) const
{
	RE::BGSBodyPartData* Parts = actor->GetRace() ? actor->GetRace()->bodyPartData : nullptr;
	if (Parts)
	{
		const auto Override = MarkerBones.find(Parts);
		if (Override != MarkerBones.end())
		{
			if (RE::NiAVObject* Node = actor->GetNodeByName(Override->second))
			{
				return Node->world.translate;
			}
		}
		RE::BGSBodyPart* Torso = Parts->parts[RE::BGSBodyPartDefs::LIMB_ENUM::kTorso];
		if (RE::NiAVObject* Node = Torso ? actor->GetNodeByName(Torso->targetName) : nullptr)
		{
			return Node->world.translate;
		}
	}
	return actor->GetPosition() + RE::NiPoint3(0.f, 0.f, actor->GetHeight() * 0.6f);
}

bool CreatureHandler::IsCreatureGraph(RE::TESRace* race) const
{
	return race && CreatureRaces.contains(race);
}

bool CreatureHandler::GraphIsCreature(RE::TESRace* race)
{
	if (!race)
	{
		return false;
	}
	// Humanoids run the default character project; everything else has its own.
	for (const auto& Graph : race->behaviorGraphs)
	{
		const std::string_view Path = Graph.model.data() ? Graph.model.data() : "";
		if (ContainsNoCase(Path, "defaultmale") || ContainsNoCase(Path, "defaultfemale"))
		{
			return false;
		}
	}
	return true;
}

void CreatureHandler::LogRaceOnce(RE::Actor* actor)
{
	if (!Settings::VerboseLogging)
	{
		return;
	}
	RE::TESRace* Race = actor->GetRace();
	if (!Race)
	{
		return;
	}
	{
		std::lock_guard Lock(LoggedRacesMtx);
		if (!LoggedRaces.insert(Race).second)
		{
			return;
		}
	}
	const RE::NiPoint3 Capsule = actor->GetCharController() ? actor->GetCharController()->collisionBound.extents : RE::NiPoint3();
	const float RenderRadius = actor->Get3D() ? actor->Get3D()->worldBound.radius : 0.f;
	// The player's, so the line reads as a ratio: GetHeight is 128 for most records.
	auto* Player = RE::PlayerCharacter::GetSingleton();
	const RE::NiPoint3 PlayerCapsule = Player->GetCharController() ? Player->GetCharController()->collisionBound.extents : RE::NiPoint3();
	const float PlayerRender = Player->Get3D() ? Player->Get3D()->worldBound.radius : 0.f;
	std::string Attacks;
	if (Race->attackDataMap)
	{
		for (auto& iter : Race->attackDataMap->attackDataMap)
		{
			Attacks += iter.first.data() ? iter.first.data() : "";
			Attacks += ' ';
		}
	}
	logger::info("[creature] race {} graph {} bounds {} scale {} capsule {} {} {} render {} | player bounds {} capsule {} {} {} render {} | attacks [{}]",
		Race->GetFormEditorID(), Race->behaviorGraphs[0].model.data() ? Race->behaviorGraphs[0].model.data() : "",
		actor->GetHeight(), actor->GetScale(),
		Capsule.x, Capsule.y, Capsule.z, RenderRadius, BodyHeight(Player),
		PlayerCapsule.x, PlayerCapsule.y, PlayerCapsule.z, PlayerRender, Attacks);
}
