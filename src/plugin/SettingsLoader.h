#pragma once

#include "3rdparty/inicpp.h"

struct DifficultySettings
{
	static float ComboResetTimer;
	static float MeleeDamageMult;
	static float UnblockableDamageMult;
	static float ProjectileDamageMult;
	static float StaggerResetTimer;
	static float ChamberWindowTime;
	static float HyperarmorTimer;
	static float StaminaRegenMult;
	static float AttackTimeoutTime;
	static bool AttacksCostStamina;
};

struct Settings
{
	static float ActiveDistance;
	static bool HasPrecision;
	static bool InvertY;
};

struct InputSettings
{
	enum class InputTypes : int
	{
		MouseOnly = 1,
		MouseKeyModifier = 2,
		Keyboard = 3
	};

	static InputTypes InputType;
	static int MouseSens;
	static unsigned KeyModifierCode;
	static unsigned KeyCodeTR;
	static unsigned KeyCodeTL;
	static unsigned KeyCodeBL;
	static unsigned KeyCodeBR;
};

struct WeaponSettings
{
	static bool RebalanceWeapons;
	static float WarhammerSpeed;
	static float BattleaxeSpeed;
	static float GreatSwordSpeed;
	static float SwordSpeed;
	static float AxeSpeed;
	static float WeaponSpeedMult;

	// unneeded as battleaxes and warhammers are treated as polearms in this mod
	static float HalberdSpeed;
	static float QtrStaffSpeed;
};

struct AISettings
{
	static float AIWaitTimer;
	static int LegendaryLvl;
	static int VeryHardLvl;
	static int HardLvl;
	static int NormalLvl;
	static int EasyLvl;
	static int VeryEasyLvl;
	static float AIDifficultyMult;
	static float AIGrowthFactor;
};

struct UISettings
{
	static float Size;
	static float Length;
	static float Thickness;
	static float DisplayDistance;
};

class SettingsLoader
{
public:
	enum DirectionalInput
	{
		Mouse,
		MouseModifier,
		Hotkeys
	};
	static SettingsLoader* GetSingleton()
	{
		static SettingsLoader obj;
		return std::addressof(obj);
	}
	void InitializeDefaultValues();
	void Load(const std::string& path);

	void RebalanceWeapons();
	void RemovePowerAttacks();

private:
	float CalcDamage(float diff);
	RE::BGSKeyword* IsWarhammer;
	RE::BGSKeyword* IsBaxe;

	ini::IniFile SettingsIni;
};