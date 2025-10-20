#pragma once

#include "3rdparty/inicpp.h"
#include "3rdparty/TrueDirectionalMovementAPI.h"
#include "3rdparty/PrecisionAPI.h"

struct DifficultySettings
{
	static float ComboResetTimer;
	static float MeleeDamageMult;
	static float UnblockableDamageMult;
	static float ProjectileDamageMult;
	static float StaggerResetTimer;
	static float ChamberWindowTime;
	static float FeintWindowTime;
	static float StaminaRegenMult;
	static float AttackTimeoutTime;
	static bool AttacksCostStamina;
	static float NonNPCStaggerMult;
	static float StaminaCost;
	static float WeaponWeightStaminaMult;
	static float KnockbackMult;
	static float StaminaDamageCap;
	static float DMCODodgeCost;
	static float TimedBlockStartup;
	static float TimedBlockActiveTime;
	static float TimedBlockCooldown;
};

struct Settings
{
	static float ActiveDistance;
	static bool HasPrecision;
	static bool HasTDM;
	static bool EnableForH2H;
	static bool MNBMode;
	static bool ForHonorMode;
	static bool ExperimentalMode;
	static bool DMCOSupport;
	static bool BufferInput;
	static bool SwitchingCostsStamina;
	static bool RemovePowerAttacks;
	static bool VerboseLogging;
	
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
	static unsigned KeyCodeFeint;
	static unsigned KeyCodeBash;
	static unsigned KeyCodeSwitchHud;
	static bool InvertY;
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
	static float BowSpeedMult;

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
	static float AIMistakeRatio;

	// you're gonna mess with these for a long time
	static float LegendaryUpdateTimer;
	static float VeryHardUpdateTimer;
	static float HardUpdateTimer;
	static float NormalUpdateTimer;
	static float EasyUpdateTimer;
	static float VeryEasyUpdateTimer;

	static float LegendaryActionTimer;
	static float VeryHardActionTimer;
	static float HardActionTimer;
	static float NormalActionTimer;
	static float EasyActionTimer;
	static float VeryEasyActionTimer;
};

struct UISettings
{
	static float Size;
	static float Length;
	static float Thickness;
	static float DisplayDistance;
	static bool ShowUI;
	static bool FlashUI;
	static bool HarderUI;
	static bool OnlyShowTargetted; 
	static float PlayerUIScale;
	static float NPCUIScale;
	static bool Force1PHud;
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