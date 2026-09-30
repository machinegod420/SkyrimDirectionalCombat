#pragma once

class FXHandler
{
public:
	static FXHandler* GetSingleton()
	{
		static FXHandler obj;
		return std::addressof(obj);
	}
	void Initialize();
	void PlayMasterstrike(RE::Actor* actor);
	void PlayBlock(RE::Actor* actor);
	void PlayTimedBlock(RE::Actor* actor);
	// Flash on an attacker whose landed power attack advanced their combo two steps.
	void PlayComboPowerStep(RE::Actor* actor);
	// Invisibility warp on the body. Not used yet; the masterstrike is the likely home.
	void PlayWarp(RE::Actor* actor);
	// A player swing refused: lockout, feint window or stamina.
	void PlayAttackRefused();
private:
	void PlaySound(RE::Actor* actor, RE::BGSSoundDescriptorForm* sound);
	void PlaySoundAt(RE::BGSSoundDescriptorForm* sound, const RE::NiPoint3& position, RE::NiAVObject* follow);
	RE::BGSArtObject* ComboPowerStepArt = nullptr;
	RE::BGSSoundDescriptorForm* ComboPowerStepSound = nullptr;
	RE::BGSArtObject* WarpArt = nullptr;
	RE::BGSSoundDescriptorForm* MasterstrikeSound;
	RE::BGSSoundDescriptorForm* MasterstrikeSound2;
	RE::BGSSoundDescriptorForm* BlockSound;
	RE::BGSSoundDescriptorForm* TimedBlockSound;
	RE::BGSSoundDescriptorForm* TimedBlockSound2;
	RE::BGSSoundDescriptorForm* RefusedSound = nullptr;
};
