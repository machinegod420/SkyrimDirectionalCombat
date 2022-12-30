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
private:
	void PlaySound(RE::Actor* actor, RE::BGSSoundDescriptorForm* sound);
	RE::BGSSoundDescriptorForm* MasterstrikeSound;
	RE::BGSSoundDescriptorForm* MasterstrikeSound2;
	RE::BGSSoundDescriptorForm* BlockSound;
};
