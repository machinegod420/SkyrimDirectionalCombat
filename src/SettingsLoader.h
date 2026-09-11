#pragma once

#include "3rdparty/inicpp.h"
#include "3rdparty/TrueDirectionalMovementAPI.h"
#include "3rdparty/PrecisionAPI.h"

struct DifficultySettings
{
	static float ComboResetTimer;
	static float MeleeDamageMult;
	// WeaponSpeedMult subtracted when a combo is continued from the same
	// horizontal side it was already on (TR/BR are right, TL/BL are left).
	// Combo-relative, like the repeat-cost curve: only a landed hit sets the
	// line it compares against, so a whiff in between doesn't count. A cost
	// rather than a prohibition, so the weak continuation stays available at a
	// lower frequency instead of being removed. For scale,
	// GuardChargeMaxSpeedBonus is 0.20 in the other direction.
	static float SameSideSpeedPenalty;
	static float UnblockableDamageMult;
	static float ProjectileDamageMult;
	static float StaggerResetTimer;
	static float ChamberWindowTime;
	static float FeintWindowTime;
	static float StaminaRegenMult;
	// Ceiling on how much stacked Fortify Stamina Regeneration can raise the
	// regen rate, as a fraction (0.5 = at most +50%). Applied as a saturating
	// curve so buffs always do something and nothing reads as broken. Debuffs
	// (cold, disease) are not compressed and apply in full.
	static float MaxRegenBonus;
	static float AttackTimeoutTime;
	static bool AttacksCostStamina;
	static float NonNPCStaggerMult;
	static float StaminaCost;
	static float WeaponWeightStaminaMult;
	static float KnockbackMult;
	static float StaminaDamageCap;
	// Share of the defender's bar per blocked hit, before weapon mass and Block.
	static float BlockCostRatio;
	// Share of that cost Block skill removes at 100.
	static float BlockSkillMaxReduction;
	// Multiplier on a blocked power attack's stamina cost.
	static float PowerAttackBlockCostMult;
	// Multiplier on the attacker's own stamina cost for a power attack.
	static float PowerAttackStaminaMult;
	static float DodgeCost;
	static float TimedBlockStartup;
	static float TimedBlockActiveTime;
	static float TimedBlockCooldown;
	// Guard charge ("being set"): holding one guard line without attacking
	// builds a proportional attack-speed bonus, spent on the next attack and
	// lost on any guard switch or landed hit. Startup is a dead zone — nothing
	// accrues before it — then the bonus ramps linearly to its max at Full.
	// Set Enable to false to disable the mechanic entirely.
	static bool EnableGuardCharge;
	static float GuardChargeStartup;
	static float GuardChargeFull;
	static float GuardChargeMaxSpeedBonus;

	// Parsed but unread. Reserved for authored combo patterns.
	static bool KCDStyleCombos;
};

enum class DodgeSystem
{
	None = 0,    // no dodge support
	DMCO = 1,    // external DMCO mod
	Custom = 2,  // our built-in dodge (DodgeHandler)
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
	static DodgeSystem ActiveDodgeSystem;
	static bool BufferInput;
	static bool SwitchingCostsStamina;
	static bool RemovePowerAttacks;
	static bool VerboseLogging;
	static bool TDMOnlyLockedHumanoids;

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
	static bool KeyModifierLocksCamera;
	static unsigned KeyCodeTR;
	static unsigned KeyCodeTL;
	static unsigned KeyCodeBL;
	static unsigned KeyCodeBR;
	static unsigned KeyCodeFeint;
	static unsigned KeyCodeBash;
	static unsigned KeyCodePowerAttack;
	static unsigned KeyCodeSwitchHud;
	static unsigned KeyCodeDodge;
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

	// --- Conditioning: how fast the AI learns your guard habits, and how much
	// belief it needs before acting on one.
	static int PreBlockBeliefThreshold;
	// Base accumulation per observed switch, multiplied by AIMistakeRatio and
	// the streak. Raise if conditioning feels weak.
	static int BeliefAccumBase;
	// Streak cap. Also the point where the AI bails out of defending.
	static int MaxDirectionTracked;
	// Odds a strong read converts into a pre-block, before personality.
	static float PreBlockBaseChance;
	static float PreBlockCautionScale;
	static float PreBlockMaxChance;
	// Belief on the covered line extends the commit window by up to this many
	// seconds — a conditioned AI abandons its expectation late.
	static float ConditionedFixationSeconds;
	// How long a guard switch is protected, in MULTIPLES of the difficulty's
	// update timer
	static float CommitWindowTicks;
	// Base seconds the AI defends before the offense exit opens, scaled by the
	// actor's patience (Aggressor ~0.3x, Turtle ~2x). Raise to make NPCs turtle
	// longer.
	static float DefendPatienceSeconds;
	// Pull toward the line the target's combo makes likely. 0 = pure learned
	// belief, 1 = always the structurally predicted line.
	static float ComboReadStrength;
	// What fraction of the combo read a VeryEasy actor gets. Legendary gets all
	// of it; the tiers between interpolate.
	static float ComboReadLowTierScale;
	// Belief decay. Disconfirm fires when a guard is caught on the wrong line,
	// CascadeDrain on every conditioned pick, and the spread modifiers divide
	// an investment to decide how much it pulls off the other three lines.
	static float BeliefDisconfirmFraction;
	static int BeliefDisconfirmFloor;
	static int BeliefCascadeDrain;
	static int BeliefSpreadModifierHit;
	static int BeliefSpreadModifierBlock;

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
	static bool ShowConditioningArcs;
	// Developer overlay: per-actor AI and direction state drawn on screen.
	static bool ShowDebugOverlay;
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