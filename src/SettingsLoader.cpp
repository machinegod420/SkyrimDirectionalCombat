#include "SettingsLoader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <new>

#define SETTING_MACRO(sectionName, settingsClass, settingName, newval) \
    do { \
        settingsClass::settingName = newval; \
        logger::info("Loaded section {} setting {} with new value {}", \
            sectionName, #settingName, settingsClass::settingName); \
    } while (0)

float DifficultySettings::ComboResetTimer = 3.f;
float DifficultySettings::MeleeDamageMult = 2.f;
float DifficultySettings::SameSideSpeedPenalty = 0.15f;
float DifficultySettings::ChainPokeSpeedPenalty = 0.3f;
float DifficultySettings::UnblockableDamageMult = 2.5f;
float DifficultySettings::UnblockableHealthFloor = 0.1f;
float DifficultySettings::PlayerUnblockableHealthFloor = 0.1f;
float DifficultySettings::ProjectileDamageMult = 0.25f;
float DifficultySettings::StaggerResetTimer = 1.5f;
float DifficultySettings::ChamberWindowTime = 0.2f;
float DifficultySettings::FeintWindowTime = 0.4f;
float DifficultySettings::FeintStaminaCost = 0.05f;
float DifficultySettings::StaminaRegenMult = 39.f;
float DifficultySettings::CreatureStaminaRegenMult = 5.f;
float DifficultySettings::MaxRegenBonus = 0.5f;
float DifficultySettings::MaxRegenPenalty = 0.5f;
float DifficultySettings::AttackTimeoutTime = 1.0f;
bool DifficultySettings::AttacksCostStamina = true;
float DifficultySettings::NonNPCStaggerMult = 2.f;
float DifficultySettings::StaminaCost = 0.1f;
float DifficultySettings::WeaponWeightStaminaMult = 0.25f;
float DifficultySettings::KnockbackMult = 2.f;
float DifficultySettings::StaminaDamageCap = 0.4f;
float DifficultySettings::BlockCostRatio = 0.1f;
float DifficultySettings::BlockSkillMaxReduction = 0.5f;
float DifficultySettings::PowerAttackBlockCostMult = 1.75f;
float DifficultySettings::PowerAttackStaminaMult = 1.5f;
float DifficultySettings::DodgeCost = 0.15f;
float DifficultySettings::TimedBlockStartup = 0.333f;
float DifficultySettings::TimedBlockActiveTime = 0.2f;
float DifficultySettings::TimedBlockCooldown = 0.2f;
bool DifficultySettings::KCDStyleCombos = false;
bool DifficultySettings::EnableGuardCharge = true;
float DifficultySettings::GuardChargeStartup = 0.5f;
float DifficultySettings::GuardChargeFull = 2.0f;
// for scale: the existing small attack-speed buff is 0.12, the full one 0.25
float DifficultySettings::GuardChargeMaxSpeedBonus = 0.20f;

float Settings::ActiveDistance = 4000.f;
bool Settings::HasPrecision = false;
bool Settings::HasTDM = false;
bool Settings::EnableForH2H = true;
bool Settings::MNBMode = false;
DirectionMode Settings::ActiveDirectionMode = DirectionMode::Normal;
bool Settings::ExperimentalMode = false;
DodgeSystem Settings::ActiveDodgeSystem = DodgeSystem::None;
bool Settings::SwitchingCostsStamina = true;
bool Settings::RemovePowerAttacks = true;
bool Settings::VerboseLogging = false;
bool Settings::TDMOnlyLockedHumanoids = false;
bool Settings::CreatureDirectionalAttacks = true;
float Settings::CreatureLargeHeightRatio = 1.f;
float Settings::CreatureSizeWeight = 15.f;

InputSettings::InputTypes InputSettings::InputType = InputSettings::InputTypes::MouseOnly;
int InputSettings::MouseSens = 5;
unsigned InputSettings::KeyModifierCode = 56;
bool InputSettings::KeyModifierLocksCamera = false;
unsigned InputSettings::KeyCodeTR = 2;
unsigned InputSettings::KeyCodeTL = 3;
unsigned InputSettings::KeyCodeBL = 4;
unsigned InputSettings::KeyCodeBR = 5;
unsigned InputSettings::KeyCodeFeint = 16;
unsigned InputSettings::KeyCodeBash = 18;
unsigned InputSettings::KeyCodePowerAttack = 260;
unsigned InputSettings::KeyCodeSwitchHud = 6;
unsigned InputSettings::KeyCodeDodge = 56;
bool InputSettings::InvertY = false;

bool WeaponSettings::RebalanceWeapons = true;
float WeaponSettings::WarhammerSpeed = 1.f;
float WeaponSettings::BattleaxeSpeed = 1.f;
float WeaponSettings::GreatSwordSpeed = 1.f;
float WeaponSettings::SwordSpeed = 1.f;
float WeaponSettings::AxeSpeed = 1.f;
float WeaponSettings::WeaponSpeedMult = 1.f;
float WeaponSettings::BowSpeedMult = 1.f;
float WeaponSettings::SpearSpeed = 1.f;
bool WeaponSettings::PhysicalWeaponSpeed = true;
float WeaponSettings::PhysicalSpeedStrength = 0.15f;

float AISettings::AIWaitTimer = 1.f;
int AISettings::LegendaryLvl = 11;
int AISettings::VeryHardLvl = 7;
int AISettings::HardLvl = 5;
int AISettings::NormalLvl = 3;
int AISettings::EasyLvl = -3;
int AISettings::VeryEasyLvl = -8;
float AISettings::AIDifficultyMult = 1.0f;
float AISettings::AIGrowthFactor = 0.01f;
float AISettings::AIMistakeRatio = 2.0f;

int AISettings::PreBlockBeliefThreshold = 40;
int AISettings::BeliefAccumBase = 5;
int AISettings::MaxDirectionTracked = 5;
float AISettings::PreBlockBaseChance = 20.f;
float AISettings::PreBlockCautionScale = 55.f;
float AISettings::PreBlockMaxChance = 75.f;
float AISettings::ConditionedFixationSeconds = 0.5f;
float AISettings::CommitWindowTicks = 1.0f;
float AISettings::DefendPatienceSeconds = 2.0f;
float AISettings::FatigueOnsetSeconds = 30.f;
float AISettings::FatigueHorizonSeconds = 90.f;
float AISettings::FatigueUpdateSeconds = 0.08f;
float AISettings::FatigueActionSeconds = 0.05f;
float AISettings::ComboReadStrength = 0.5f;
float AISettings::ComboReadLowTierScale = 0.2f;
float AISettings::InitiativeRatePerTier = 0.57f;
float AISettings::InitiativeRateTierOffset = 0.f;
bool AISettings::LearnAcrossFights = true;
bool AISettings::LearnReach = true;
float AISettings::BeliefDisconfirmFraction = 0.5f;
int AISettings::BeliefDisconfirmFloor = 5;
int AISettings::BeliefCascadeDrain = 5;
int AISettings::BeliefSpreadModifierHit = 4;
int AISettings::BeliefSpreadModifierBlock = 5;

float AISettings::LegendaryUpdateTimer = 0.15f;
float AISettings::VeryHardUpdateTimer = 0.16f;
float AISettings::HardUpdateTimer = 0.16f;
float AISettings::NormalUpdateTimer = 0.16f;
float AISettings::EasyUpdateTimer = 0.18f;
float AISettings::VeryEasyUpdateTimer = 0.18f;

float AISettings::LegendaryActionTimer = 0.15f;
float AISettings::VeryHardActionTimer = 0.16f;
float AISettings::HardActionTimer = 0.16f;
float AISettings::NormalActionTimer = 0.17f;
float AISettings::EasyActionTimer = 0.18f;
float AISettings::VeryEasyActionTimer = 0.18f;

float UISettings::Size = 300.f;
float UISettings::Length = 13.f;
float UISettings::Thickness = 7.f;
float UISettings::DisplayDistance = 2000.0;
bool UISettings::ShowUI = true;
bool UISettings::FlashUI = false;
bool UISettings::HarderUI = true;
bool UISettings::OnlyShowTargetted = true;
float UISettings::NPCUIScale = 0.9f;
float UISettings::PlayerUIScale = 1.f;
bool UISettings::Force1PHud = false;
bool UISettings::ShowConditioningArcs = true;
bool UISettings::ShowDebugOverlay = false;

void SettingsLoader::InitializeDefaultValues()
{
}

void SettingsLoader::Load(const std::string& path)
{
	InitializeDefaultValues();

	RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
	IsBaxe = DataHandler->LookupForm<RE::BGSKeyword>(0x6D932, "Skyrim.esm");
	IsWarhammer = DataHandler->LookupForm<RE::BGSKeyword>(0x6D930, "Skyrim.esm");
	SettingsIni.load(path);

	for (const auto& sectionPair : SettingsIni)
	{
		const std::string& sectionName = sectionPair.first;
		logger::info("INI file loaded section {}", sectionName);


		for (const auto& fieldPair : sectionPair.second)
		{
			const std::string& fieldName = fieldPair.first;
			const ini::IniField& field = fieldPair.second;
			
			if (sectionName == "Input")
			{
				if (fieldName == "InputType")
				{
					
					InputSettings::InputTypes newval = (InputSettings::InputTypes)field.as<int>();
					InputSettings::InputType = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, (int)InputSettings::InputType);
				}
				else if (fieldName == "KeyModifierCode")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, KeyModifierCode, newval);
				}
				else if (fieldName == "KeyModifierLocksCamera")
				{
					bool newval = field.as<bool>();
					SETTING_MACRO(sectionName, InputSettings, KeyModifierLocksCamera, newval);
				}
				else if (fieldName == "MouseSens")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, MouseSens, newval);
				}
				else if (fieldName == "KeyCodeTR")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, KeyCodeTR, newval);
				}
				else if (fieldName == "KeyCodeTL")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, KeyCodeTL, newval);
				}
				else if (fieldName == "KeyCodeBL")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, KeyCodeBL, newval);
				}
				else if (fieldName == "KeyCodeBR")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, KeyCodeBR, newval);
				}
				else if (fieldName == "KeyCodeFeint")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, KeyCodeFeint, newval);
				}
				else if (fieldName == "KeyCodeBash")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, KeyCodeBash, newval);
				}
				else if (fieldName == "KeyCodePowerAttack")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, KeyCodePowerAttack, newval);
				}
				else if (fieldName == "KeyCodeSwitchHud")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, KeyCodeSwitchHud, newval);
				}
				else if (fieldName == "KeyCodeDodge")
				{
					int newval = field.as<unsigned>();
					SETTING_MACRO(sectionName, InputSettings, KeyCodeDodge, newval);
				}
				else if (fieldName == "InvertY")
				{
					bool newval = field.as<bool>();
					SETTING_MACRO(sectionName, InputSettings, InvertY, newval);
				}
			}
			else if (sectionName == "Difficulty")
			{
				if (fieldName == "MeleeDamageMult")
				{
					float newval = field.as<float>();
					DifficultySettings::MeleeDamageMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::MeleeDamageMult);
				}
				else if (fieldName == "SameSideSpeedPenalty")
				{
					float newval = field.as<float>();
					DifficultySettings::SameSideSpeedPenalty = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::SameSideSpeedPenalty);
				}
				else if (fieldName == "ChainPokeSpeedPenalty")
				{
					float newval = field.as<float>();
					DifficultySettings::ChainPokeSpeedPenalty = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::ChainPokeSpeedPenalty);
				}
				else if (fieldName == "UnblockableDamageMult")
				{
					float newval = field.as<float>();
					DifficultySettings::UnblockableDamageMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::UnblockableDamageMult);
				}
				else if (fieldName == "UnblockableHealthFloor")
				{
					float newval = field.as<float>();
					DifficultySettings::UnblockableHealthFloor = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::UnblockableHealthFloor);
				}
				else if (fieldName == "PlayerUnblockableHealthFloor")
				{
					float newval = field.as<float>();
					DifficultySettings::PlayerUnblockableHealthFloor = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::PlayerUnblockableHealthFloor);
				}
				else if (fieldName == "ProjectileDamageMult")
				{
					float newval = field.as<float>();
					DifficultySettings::ProjectileDamageMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::ProjectileDamageMult);
				}
				else if (fieldName == "ComboResetTimer")
				{
					float newval = field.as<float>();
					DifficultySettings::ComboResetTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::ComboResetTimer);
				}
				else if (fieldName == "ChamberWindowTime")
				{
					float newval = field.as<float>();
					DifficultySettings::ChamberWindowTime = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::ChamberWindowTime);
				}
				else if (fieldName == "FeintWindowTime")
				{
					float newval = field.as<float>();
					DifficultySettings::FeintWindowTime = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::FeintWindowTime);
				}
				else if (fieldName == "FeintStaminaCost")
				{
					float newval = field.as<float>();
					DifficultySettings::FeintStaminaCost = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::FeintStaminaCost);
				}

				else if (fieldName == "AttacksCostStamina")
				{
					bool newval = field.as<bool>();
					DifficultySettings::AttacksCostStamina = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::AttacksCostStamina);
				}
				else if (fieldName == "AttackTimeoutTime")
				{
					float newval = field.as<float>();
					DifficultySettings::AttackTimeoutTime = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::AttackTimeoutTime);
				}
				else if (fieldName == "NonNPCStaggerMult")
				{
					float newval = field.as<float>();
					DifficultySettings::NonNPCStaggerMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::NonNPCStaggerMult);
				}
				else if (fieldName == "StaminaRegenMult")
				{
					float newval = field.as<float>();
					DifficultySettings::StaminaRegenMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::StaminaRegenMult);
				}
				else if (fieldName == "CreatureStaminaRegenMult")
				{
					float newval = field.as<float>();
					DifficultySettings::CreatureStaminaRegenMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::CreatureStaminaRegenMult);
				}
				else if (fieldName == "MaxRegenBonus")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, MaxRegenBonus, newval);
				}
				else if (fieldName == "MaxRegenPenalty")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, MaxRegenPenalty, newval);
				}
				else if (fieldName == "StaminaCost")
				{
					float newval = field.as<float>();
					DifficultySettings::StaminaCost = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::StaminaCost);
				}
				else if (fieldName == "DodgeCost")
				{
					float newval = field.as<float>();
					DifficultySettings::DodgeCost = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::DodgeCost);
				}
				else if (fieldName == "KnockbackMult")
				{
					float newval = field.as<float>();
					DifficultySettings::KnockbackMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::KnockbackMult);
				}
				else if (fieldName == "StaminaDamageCap")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, StaminaDamageCap, newval);
				}
				else if (fieldName == "WeaponWeightStaminaMult")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, WeaponWeightStaminaMult, newval);
				}
				else if (fieldName == "BlockCostRatio")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, BlockCostRatio, newval);
				}
				else if (fieldName == "BlockSkillMaxReduction")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, BlockSkillMaxReduction, newval);
				}
				else if (fieldName == "PowerAttackBlockCostMult")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, PowerAttackBlockCostMult, newval);
				}
				else if (fieldName == "PowerAttackStaminaMult")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, PowerAttackStaminaMult, newval);
				}
				else if (fieldName == "TimedBlockStartup")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, TimedBlockStartup, newval);
				}
				else if (fieldName == "TimedBlockActiveTime")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, TimedBlockActiveTime, newval);
				}
				else if (fieldName == "TimedBlockCooldown")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, TimedBlockCooldown, newval);
				}
				else if (fieldName == "KCDStyleCombos")
				{
					bool newval = field.as<bool>();
					SETTING_MACRO(sectionName, DifficultySettings, KCDStyleCombos, newval);
				}
				else if (fieldName == "EnableGuardCharge")
				{
					bool newval = field.as<bool>();
					SETTING_MACRO(sectionName, DifficultySettings, EnableGuardCharge, newval);
				}
				else if (fieldName == "GuardChargeStartup")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, GuardChargeStartup, newval);
				}
				else if (fieldName == "GuardChargeFull")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, GuardChargeFull, newval);
				}
				else if (fieldName == "GuardChargeMaxSpeedBonus")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, DifficultySettings, GuardChargeMaxSpeedBonus, newval);
				}
			}
			else if (sectionName == "AI")
			{
				if (fieldName == "AIDifficultyMult")
				{
					float newval = field.as<float>();
					AISettings::AIDifficultyMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::AIDifficultyMult);
				}
				else if (fieldName == "AIGrowthFactor")
				{
					float newval = field.as<float>();
					AISettings::AIGrowthFactor = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::AIGrowthFactor);
				}
				else if (fieldName == "AIMistakeRatio")
				{
					float newval = field.as<float>();
					AISettings::AIMistakeRatio = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::AIMistakeRatio);
				}
				else if (fieldName == "PreBlockBeliefThreshold")
				{
					int newval = field.as<int>();
					SETTING_MACRO(sectionName, AISettings, PreBlockBeliefThreshold, newval);
				}
				else if (fieldName == "BeliefAccumBase")
				{
					int newval = field.as<int>();
					SETTING_MACRO(sectionName, AISettings, BeliefAccumBase, newval);
				}
				else if (fieldName == "MaxDirectionTracked")
				{
					int newval = field.as<int>();
					SETTING_MACRO(sectionName, AISettings, MaxDirectionTracked, newval);
				}
				else if (fieldName == "PreBlockBaseChance")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, PreBlockBaseChance, newval);
				}
				else if (fieldName == "PreBlockCautionScale")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, PreBlockCautionScale, newval);
				}
				else if (fieldName == "PreBlockMaxChance")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, PreBlockMaxChance, newval);
				}
				else if (fieldName == "ConditionedFixationSeconds")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, ConditionedFixationSeconds, newval);
				}
				else if (fieldName == "CommitWindowTicks")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, CommitWindowTicks, newval);
				}
				else if (fieldName == "DefendPatienceSeconds")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, DefendPatienceSeconds, newval);
				}
				else if (fieldName == "FatigueOnsetSeconds")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, FatigueOnsetSeconds, newval);
				}
				else if (fieldName == "FatigueHorizonSeconds")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, FatigueHorizonSeconds, newval);
				}
				else if (fieldName == "FatigueUpdateSeconds")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, FatigueUpdateSeconds, newval);
				}
				else if (fieldName == "FatigueActionSeconds")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, FatigueActionSeconds, newval);
				}
				else if (fieldName == "ComboReadStrength")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, ComboReadStrength, newval);
				}
				else if (fieldName == "ComboReadLowTierScale")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, ComboReadLowTierScale, newval);
				}
				else if (fieldName == "InitiativeRatePerTier")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, InitiativeRatePerTier, newval);
				}
				else if (fieldName == "InitiativeRateTierOffset")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, InitiativeRateTierOffset, newval);
				}
				else if (fieldName == "LearnAcrossFights")
				{
					bool newval = field.as<bool>();
					SETTING_MACRO(sectionName, AISettings, LearnAcrossFights, newval);
				}
				else if (fieldName == "LearnReach")
				{
					bool newval = field.as<bool>();
					SETTING_MACRO(sectionName, AISettings, LearnReach, newval);
				}
				else if (fieldName == "BeliefDisconfirmFraction")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, AISettings, BeliefDisconfirmFraction, newval);
				}
				else if (fieldName == "BeliefDisconfirmFloor")
				{
					int newval = field.as<int>();
					SETTING_MACRO(sectionName, AISettings, BeliefDisconfirmFloor, newval);
				}
				else if (fieldName == "BeliefCascadeDrain")
				{
					int newval = field.as<int>();
					SETTING_MACRO(sectionName, AISettings, BeliefCascadeDrain, newval);
				}
				else if (fieldName == "BeliefSpreadModifierHit")
				{
					int newval = field.as<int>();
					SETTING_MACRO(sectionName, AISettings, BeliefSpreadModifierHit, newval);
				}
				else if (fieldName == "BeliefSpreadModifierBlock")
				{
					int newval = field.as<int>();
					SETTING_MACRO(sectionName, AISettings, BeliefSpreadModifierBlock, newval);
				}
				else if (fieldName == "VeryEasyLvl")
				{
					int newval = field.as<int>();
					AISettings::VeryEasyLvl = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::VeryEasyLvl);
				}
				else if (fieldName == "EasyLvl")
				{
					int newval = field.as<int>();
					AISettings::EasyLvl = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::EasyLvl);
				}
				else if (fieldName == "NormalLvl")
				{
					int newval = field.as<int>();
					AISettings::NormalLvl = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::NormalLvl);
				}
				else if (fieldName == "HardLvl")
				{
					int newval = field.as<int>();
					AISettings::HardLvl = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::HardLvl);
				}
				else if (fieldName == "VeryHardLvl")
				{
					int newval = field.as<int>();
					AISettings::VeryHardLvl = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::VeryHardLvl);
				}
				else if (fieldName == "LegendaryLvl")
				{
					int newval = field.as<int>();
					AISettings::LegendaryLvl = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::LegendaryLvl);
				}
				else if (fieldName == "VeryEasyUpdateTimer")
				{
					float newval = field.as<float>();
					AISettings::VeryEasyUpdateTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::VeryEasyUpdateTimer);
				}
				else if (fieldName == "EasyUpdateTimer")
				{
					float newval = field.as<float>();
					AISettings::EasyUpdateTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::EasyUpdateTimer);
				}
				else if (fieldName == "NormalUpdateTimer")
				{
					float newval = field.as<float>();
					AISettings::NormalUpdateTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::NormalUpdateTimer);
				}
				else if (fieldName == "HardUpdateTimer")
				{
					float newval = field.as<float>();
					AISettings::HardUpdateTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::HardUpdateTimer);
				}
				else if (fieldName == "VeryHardUpdateTimer")
				{
					float newval = field.as<float>();
					AISettings::VeryHardUpdateTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::VeryHardUpdateTimer);
				}
				else if (fieldName == "LegendaryUpdateTimer")
				{
					float newval = field.as<float>();
					AISettings::LegendaryUpdateTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::LegendaryUpdateTimer);
				}
				else if (fieldName == "VeryEasyActionTimer")
				{
					float newval = field.as<float>();
					AISettings::VeryEasyActionTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::VeryEasyActionTimer);
				}
				else if (fieldName == "EasyActionTimer")
				{
					float newval = field.as<float>();
					AISettings::EasyActionTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::EasyActionTimer);
				}
				else if (fieldName == "NormalActionTimer")
				{
					float newval = field.as<float>();
					AISettings::NormalActionTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::NormalActionTimer);
				}
				else if (fieldName == "HardActionTimer")
				{
					float newval = field.as<float>();
					AISettings::HardActionTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::HardActionTimer);
				}
				else if (fieldName == "VeryHardActionTimer")
				{
					float newval = field.as<float>();
					AISettings::VeryHardActionTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::VeryHardActionTimer);
				}
				else if (fieldName == "LegendaryActionTimer")
				{
					float newval = field.as<float>();
					AISettings::LegendaryActionTimer = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, AISettings::LegendaryActionTimer);
				}
			}
			else if (sectionName == "UI")
			{
				if (fieldName == "Size")
				{
					float newval = field.as<float>();
					UISettings::Size = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, UISettings::Size);
				}
				else if (fieldName == "Length")
				{
					float newval = field.as<float>();
					UISettings::Length = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, UISettings::Length);
				}
				else if (fieldName == "Thickness")
				{
					float newval = field.as<float>();
					UISettings::Thickness = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, UISettings::Thickness);
				}
				else if (fieldName == "DisplayDistance")
				{
					float newval = field.as<float>();
					UISettings::DisplayDistance = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, UISettings::DisplayDistance);
				}
				else if (fieldName == "ShowUI")
				{
					bool newval = field.as<bool>();
					UISettings::ShowUI = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, UISettings::ShowUI);
				}
				else if (fieldName == "HarderUI")
				{
					bool newval = field.as<bool>();
					UISettings::HarderUI = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, UISettings::HarderUI);
				}
				else if (fieldName == "OnlyShowTargettedEnemies")
				{
					bool newval = field.as<bool>();
					UISettings::OnlyShowTargetted = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, UISettings::OnlyShowTargetted);
				}
				else if (fieldName == "PlayerUIScale")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, UISettings, PlayerUIScale, newval);
				}
				else if (fieldName == "NPCUIScale")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, UISettings, NPCUIScale, newval);
				}
				else if (fieldName == "Force1PHud")
				{
					bool newval = field.as<bool>();
					SETTING_MACRO(sectionName, UISettings, Force1PHud, newval);
				}
				else if (fieldName == "ShowConditioningArcs")
				{
					bool newval = field.as<bool>();
					SETTING_MACRO(sectionName, UISettings, ShowConditioningArcs, newval);
				}
				else if (fieldName == "ShowDebugOverlay")
				{
					bool newval = field.as<bool>();
					SETTING_MACRO(sectionName, UISettings, ShowDebugOverlay, newval);
				}
			}
			else if (sectionName == "Settings")
			{
				if (fieldName == "ActiveDistance")
				{
					float newval = field.as<float>();
					Settings::ActiveDistance = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::ActiveDistance);
				}
				else if (fieldName == "MNBMode")
				{
					bool newval = field.as<bool>();
					Settings::MNBMode = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::MNBMode);
				}
				else if (fieldName == "DirectionMode")
				{
					int newval = field.as<int>();
					Settings::ActiveDirectionMode = static_cast<DirectionMode>(newval);
					logger::info("Loaded section {} setting {} with new value {} ({})",
						sectionName, fieldName, newval,
						newval == 1 ? "ForHonor" : (newval == 2 ? "KCD" : "Normal"));
				}
				else if (fieldName == "ExperimentalMode")
				{
					bool newval = field.as<bool>();
					Settings::ExperimentalMode = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::ExperimentalMode);
				}
				else if (fieldName == "DodgeSystem")
				{
					int newval = field.as<int>();
					Settings::ActiveDodgeSystem = static_cast<DodgeSystem>(newval);
					logger::info("Loaded section {} setting {} with new value {} ({})",
						sectionName, fieldName, newval,
						newval == 1 ? "DMCO" : (newval == 2 ? "Custom" : "None"));
				}
				else if (fieldName == "SwitchingCostsStamina")
				{
					bool newval = field.as<bool>();
					Settings::SwitchingCostsStamina = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::SwitchingCostsStamina);
				}
				else if (fieldName == "EnableForH2H")
				{
					bool newval = field.as<bool>();
					Settings::EnableForH2H = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::EnableForH2H);
				}
				else if (fieldName == "RemovePowerAttacks")
				{
					bool newval = field.as<bool>();
					Settings::RemovePowerAttacks = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::RemovePowerAttacks);
				}
				else if (fieldName == "VerboseLogging")
				{
					bool newval = field.as<bool>();
					Settings::VerboseLogging = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::VerboseLogging);
				}
				else if (fieldName == "TDMOnlyHumanoids")
				{
					bool newval = field.as<bool>();
					Settings::TDMOnlyLockedHumanoids = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::VerboseLogging);
				}
				else if (fieldName == "CreatureDirectionalAttacks")
				{
					bool newval = field.as<bool>();
					Settings::CreatureDirectionalAttacks = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::CreatureDirectionalAttacks);
				}
				else if (fieldName == "CreatureLargeHeightRatio")
				{
					float newval = field.as<float>();
					Settings::CreatureLargeHeightRatio = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::CreatureLargeHeightRatio);
				}
				else if (fieldName == "CreatureSizeWeight")
				{
					float newval = field.as<float>();
					Settings::CreatureSizeWeight = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::CreatureSizeWeight);
				}
			}
			else if (sectionName == "Weapons")
			{
				if (fieldName == "WeaponSpeedMult")
				{
					float newval = field.as<float>();
					WeaponSettings::WeaponSpeedMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, WeaponSettings::WeaponSpeedMult);
				}
				else if(fieldName == "RebalanceWeaponSpeed")
				{
					bool newval = field.as<bool>();
					WeaponSettings::RebalanceWeapons = newval;
						logger::info("Loaded section {} setting {} with new value {}",
							sectionName, fieldName, WeaponSettings::RebalanceWeapons);
				}
				else if (fieldName == "WarhammerSpeed")
				{
					float newval = field.as<float>();
					WeaponSettings::WarhammerSpeed = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, WeaponSettings::WarhammerSpeed);
				}
				else if (fieldName == "BattleaxeSpeed")
				{
					float newval = field.as<float>();
					WeaponSettings::BattleaxeSpeed = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, WeaponSettings::BattleaxeSpeed);
				}
				else if (fieldName == "GreatSwordSpeed")
				{
					float newval = field.as<float>();
					WeaponSettings::GreatSwordSpeed = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, WeaponSettings::GreatSwordSpeed);
				}
				else if (fieldName == "SwordSpeed")
				{
					float newval = field.as<float>();
					WeaponSettings::SwordSpeed = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, WeaponSettings::SwordSpeed);
				}
				else if (fieldName == "AxeSpeed")
				{
					float newval = field.as<float>();
					WeaponSettings::AxeSpeed = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, WeaponSettings::AxeSpeed);
				}
				else if (fieldName == "BowSpeed")
				{
					float newval = field.as<float>();
					WeaponSettings::BowSpeedMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, WeaponSettings::BowSpeedMult);
				}
				else if (fieldName == "SpearSpeed")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, WeaponSettings, SpearSpeed, newval);
				}
				else if (fieldName == "PhysicalWeaponSpeed")
				{
					bool newval = field.as<bool>();
					SETTING_MACRO(sectionName, WeaponSettings, PhysicalWeaponSpeed, newval);
				}
				else if (fieldName == "PhysicalSpeedStrength")
				{
					float newval = field.as<float>();
					SETTING_MACRO(sectionName, WeaponSettings, PhysicalSpeedStrength, newval);
				}
			}
		}
	}

	if (WeaponSettings::RebalanceWeapons)
	{
		RebalanceWeapons();
	}

	logger::info("Changing Weapon Speeds by WeaponSpeedMult");
	for (auto& weap : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESObjectWEAP>())
	{
		if (weap->IsMelee())
		{
			weap->weaponData.speed = weap->weaponData.speed * WeaponSettings::WeaponSpeedMult;
		}
		// Not crossbows: a changed speed broke their reload.
		else if (weap->IsBow())
		{
			weap->weaponData.speed *= WeaponSettings::BowSpeedMult;
		}
	}
}

// The behavior graph runs two-handed swings this much faster than the weapon's
// speed value says, so the class speeds are divided by it before being written.
constexpr float TwoHandSpeedFactor = 1.5f;

static void CombineBound(RE::NiBound& a_into, const RE::NiBound& a_add)
{
	const RE::NiPoint3 Delta = a_add.center - a_into.center;
	const float Dist = Delta.Length();
	if (Dist + a_add.radius <= a_into.radius)
	{
		return;
	}
	if (Dist + a_into.radius <= a_add.radius)
	{
		a_into = a_add;
		return;
	}
	const float Radius = (Dist + a_into.radius + a_add.radius) * 0.5f;
	a_into.center += Delta * ((Radius - a_into.radius) / Dist);
	a_into.radius = Radius;
}

// Precision's GetModelBounds without its render-flag checks. Particles are always skipped,
// glow shells unless nothing else is left (bound weapons are all glow).
static void AccumulateBounds(RE::NiAVObject* a_obj, const RE::NiTransform& a_parent, bool a_skipGlow,
	RE::NiBound& a_bound, bool& a_any)
{
	// Scb is the sheath, which the engine moves off a drawn weapon; Precision never sees it.
	const char* Name = a_obj ? a_obj->name.c_str() : nullptr;
	if (!a_obj || a_obj->GetFlags().any(RE::NiAVObject::Flag::kHidden) || (Name && _stricmp(Name, "Scb") == 0))
	{
		return;
	}
	if (auto* Geom = a_obj->AsGeometry())
	{
		const auto Type = Geom->GetType();
		if (Type == RE::BSGeometry::Type::kParticles || Type == RE::BSGeometry::Type::kStripParticles ||
			(a_skipGlow && netimmerse_cast<RE::BSEffectShaderProperty*>(Geom->GetGeometryRuntimeData().shaderProperty.get())))
		{
			return;
		}
		RE::NiBound Bound = Geom->GetModelData().modelBound;
		Bound.center = a_parent * (Geom->local * Bound.center);
		Bound.radius *= Geom->local.scale * a_parent.scale;
		if (!a_any)
		{
			a_bound = Bound;
			a_any = true;
		}
		else
		{
			CombineBound(a_bound, Bound);
		}
		return;
	}
	if (auto* Node = a_obj->AsNode())
	{
		const RE::NiTransform Transform = a_parent * Node->local;
		for (auto& Child : Node->GetChildren())
		{
			AccumulateBounds(Child.get(), Transform, a_skipGlow, a_bound, a_any);
		}
	}
}

// Precision's weapon length: bounding radius plus the centre's offset along the blade.
// -1 when the model has no usable mesh.
static float ModelLength(const char* a_model)
{
	RE::NiPointer<RE::NiNode> Root;
	const RE::BSModelDB::DBTraits::ArgsType Args{};
	if (RE::BSModelDB::Demand(a_model, Root, Args) != RE::BSResource::ErrorCode::kNone || !Root)
	{
		return -1.f;
	}
	// From the root's children: in hand the game ignores the root's own transform (a replacer's
	// root offset of -20 along the shaft didn't move the weapon in game).
	RE::NiBound Bound{};
	bool Any = false;
	for (const bool SkipGlow : { true, false })
	{
		for (auto& Child : Root->GetChildren())
		{
			AccumulateBounds(Child.get(), RE::NiTransform{}, SkipGlow, Bound, Any);
		}
		if (Any)
		{
			break;
		}
	}
	return Any ? Bound.radius + std::fabs(Bound.center.y) : -1.f;
}

// A swung weapon as a shaft with mass along its length plus a head at the far end. Mass per
// model unit and head mass in kg, estimates: a steel blade, or a wooden haft about a third of it.
struct SwingBody
{
	float perUnit;
	float head;
	const char* kind;
};
constexpr float SteelPerUnit = 0.015f;
constexpr float WoodPerUnit = 0.005f;
constexpr SwingBody Blade{ SteelPerUnit, 0.f, "blade" };
constexpr SwingBody WarAxe{ WoodPerUnit, 0.7f, "war axe" };
constexpr SwingBody Mace{ WoodPerUnit, 0.9f, "mace" };
constexpr SwingBody Battleaxe{ WoodPerUnit, 1.5f, "battleaxe" };
constexpr SwingBody Warhammer{ WoodPerUnit, 1.8f, "warhammer" };
constexpr SwingBody Halberd{ WoodPerUnit, 2.f, "halberd" };
// Spears, pikes and quarterstaffs share one: a long wooden shaft with a light head.
constexpr SwingBody Spear{ WoodPerUnit, 0.3f, "spear" };
// Physical speed is held to this band around the class speed.
constexpr float PhysicalSpeedMin = 0.8f;
constexpr float PhysicalSpeedMax = 1.2f;

// Resistance to being swung about a grip at one end.
static float SwingInertia(float a_length, const SwingBody& a_body)
{
	return a_body.perUnit * a_length * a_length * a_length / 3.f + a_body.head * a_length * a_length;
}

// Spear mods file spears under the sword types, so only the keywords tell.
static bool IsSpear(RE::TESObjectWEAP* a_weap)
{
	return a_weap->HasKeywordString("WeapTypeSpear") || a_weap->HasKeywordString("WeapTypePike") ||
		a_weap->HasKeywordString("WeapTypeQtrStaff");
}

static const SwingBody& BodyOf(RE::TESObjectWEAP* a_weap, RE::BGSKeyword* a_warhammer)
{
	if (IsSpear(a_weap))
	{
		return Spear;
	}
	if (a_weap->HasKeywordString("WeapTypeHalberd"))
	{
		return Halberd;
	}
	switch (a_weap->GetWeaponType())
	{
	case RE::WEAPON_TYPE::kOneHandAxe:
		return WarAxe;
	case RE::WEAPON_TYPE::kOneHandMace:
		return Mace;
	case RE::WEAPON_TYPE::kTwoHandAxe:
		return a_weap->HasKeyword(a_warhammer) ? Warhammer : Battleaxe;
	default:
		return Blade;
	}
}

float SettingsLoader::CalcDamage(float oldEffective, float newEffective)
{
	if (oldEffective <= 0.f || newEffective <= 0.f)
	{
		return 1.f;
	}
	return std::clamp(oldEffective / newEffective, 0.5f, 1.5f);
}

void SettingsLoader::RebalanceWeapons()
{
	logger::info("Rebalancing weapons!");

	for (RE::TESCombatStyle* combatStyle : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESCombatStyle>())
	{
		// these have caps
		float meleeScoreMult = combatStyle->generalData.meleeScoreMult * 1.2f;
		//meleeScoreMult = std::min(0.99f, meleeScoreMult);
		combatStyle->generalData.meleeScoreMult = meleeScoreMult;

		float rangedScoreMult = combatStyle->generalData.rangedScoreMult * 0.8f;
		//rangedScoreMult = std::min(0.99f, rangedScoreMult);
		combatStyle->generalData.rangedScoreMult = rangedScoreMult;

		float defensiveMult = 0.f;
		defensiveMult = std::min(0.99f, defensiveMult);
		combatStyle->generalData.defensiveMult = defensiveMult;


		float offensiveMult = combatStyle->generalData.offensiveMult * 2.f;
		offensiveMult = std::min(1.f, offensiveMult);
		combatStyle->generalData.offensiveMult = offensiveMult;


		//actor->combatStyle->meleeData.powerAttackBlockingMult = 0.33f;
		//actor->combatStyle->meleeData.powerAttackIncapacitatedMult = 0.5f;
		//combatStyle->meleeData.specialAttackMult = 0.2f;
		combatStyle->meleeData.bashPowerAttackMult = 0.f;
		combatStyle->meleeData.bashAttackMult = 0.f;
		combatStyle->meleeData.bashRecoilMult = 0.f;
		combatStyle->flags.reset(RE::TESCombatStyle::FLAG::kAllowDualWielding);
		float circleMult = combatStyle->closeRangeData.circleMult * 1.f;
		circleMult = std::min(0.99f, circleMult);
		combatStyle->closeRangeData.circleMult = circleMult;


	}
	std::unordered_map<RE::TESObjectWEAP*, float> PhysicalMults;
	if (WeaponSettings::PhysicalWeaponSpeed && Settings::HasPrecision)
	{
		PhysicalMults = PhysicalSpeedMults();
	}
	else if (WeaponSettings::PhysicalWeaponSpeed)
	{
		logger::info("Physical weapon speed needs Precision; weapons keep their class speeds");
	}
	std::unordered_set<std::string> Logged;
	for (auto& weap : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESObjectWEAP>())
	{
		if (weap->IsMelee())
		{
			float speed = weap->weaponData.speed;
			float damage = (float)weap->attackDamage;
			// The rate we want the weapon to swing at, before the graph's
			// two-handed multiplier is taken back out.
			float classSpeed = 0.f;
			bool twoHand = false;
			switch (weap->GetWeaponType())
			{
			case RE::WEAPON_TYPE::kOneHandDagger:
			case RE::WEAPON_TYPE::kOneHandSword:
			{
				classSpeed = WeaponSettings::SwordSpeed;
				break;
			}
			case RE::WEAPON_TYPE::kOneHandMace:
			case RE::WEAPON_TYPE::kOneHandAxe:
			{
				classSpeed = WeaponSettings::AxeSpeed;
				break;
			}

			case RE::WEAPON_TYPE::kTwoHandSword:
			{
				// Two-handed spears swing the polearm set, which the battleaxe speed describes.
				classSpeed = IsSpear(weap) ? WeaponSettings::BattleaxeSpeed : WeaponSettings::GreatSwordSpeed;
				twoHand = true;
				break;
			}

			case RE::WEAPON_TYPE::kTwoHandAxe:
			{
				//special case here due to having to use keywords
				classSpeed = weap->HasKeyword(IsWarhammer) ?
					WeaponSettings::WarhammerSpeed : WeaponSettings::BattleaxeSpeed;
				twoHand = true;
				break;
			}
			}
			if (classSpeed <= 0.f)
			{
				continue;
			}
			const auto Physical = PhysicalMults.find(weap);
			if (Physical != PhysicalMults.end())
			{
				classSpeed *= Physical->second;
			}
			if (IsSpear(weap))
			{
				classSpeed *= WeaponSettings::SpearSpeed;
			}
			const float factor = twoHand ? TwoHandSpeedFactor : 1.f;
			const uint16_t newdamage = uint16_t(damage * CalcDamage(speed * factor, classSpeed));
			const float newspeed = classSpeed / factor;
			// One line per model; enchanted copies share the numbers.
			const char* Model = weap->GetModel();
			if (Settings::VerboseLogging && Model && Logged.insert(Model).second)
			{
				logger::info("[rebalance] {} {:08X}: speed {:.3f} -> {:.3f}, damage {} -> {}",
					weap->GetName(), weap->GetFormID(), speed, newspeed, weap->attackDamage, newdamage);
			}
			weap->attackDamage = newdamage;
			weap->weaponData.speed = newspeed;
		}


	}
}

std::unordered_map<RE::TESObjectWEAP*, float> SettingsLoader::PhysicalSpeedMults()
{
	std::unordered_map<RE::TESObjectWEAP*, float> Mults;
	if (WeaponSettings::PhysicalSpeedStrength <= 0.f)
	{
		return Mults;
	}
	const auto Start = std::chrono::steady_clock::now();
	// Per model: enchanted copies share one, so each loads once.
	std::unordered_map<std::string, float> Lengths;
	int Unmeasured = 0;
	auto LengthOf = [&](RE::TESObjectWEAP* a_weap) {
		const char* Model = a_weap->GetModel();
		if (!Model || !*Model)
		{
			return -1.f;
		}
		auto Found = Lengths.find(Model);
		if (Found == Lengths.end())
		{
			Found = Lengths.emplace(Model, ModelLength(Model)).first;
			if (Found->second < 0.f)
			{
				++Unmeasured;
			}
		}
		return Found->second;
	};
	// Each class speed describes the class's steel weapon, whatever its model is in this load order.
	auto SteelBaseline = [&](RE::FormID a_id) {
		auto* Steel = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESObjectWEAP>(a_id, "Skyrim.esm");
		const float Length = Steel ? LengthOf(Steel) : -1.f;
		if (Length <= 0.f)
		{
			logger::warn("Physical weapon speed: steel weapon {:06X} has no usable model; its class keeps the class speed", a_id);
			return 0.f;
		}
		return SwingInertia(Length, BodyOf(Steel, IsWarhammer));
	};
	const float SwordBaseline = SteelBaseline(0x13989);
	const float AxeBaseline = SteelBaseline(0x13983);
	const float GreatswordBaseline = SteelBaseline(0x13987);
	const float BattleaxeBaseline = SteelBaseline(0x13984);
	const float WarhammerBaseline = SteelBaseline(0x1398A);

	std::unordered_set<std::string> Logged;
	for (auto* weap : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESObjectWEAP>())
	{
		if (!weap->IsMelee())
		{
			continue;
		}
		float Baseline = 0.f;
		switch (weap->GetWeaponType())
		{
		case RE::WEAPON_TYPE::kOneHandDagger:
		case RE::WEAPON_TYPE::kOneHandSword:
			Baseline = SwordBaseline;
			break;
		case RE::WEAPON_TYPE::kOneHandMace:
		case RE::WEAPON_TYPE::kOneHandAxe:
			Baseline = AxeBaseline;
			break;
		case RE::WEAPON_TYPE::kTwoHandSword:
			Baseline = IsSpear(weap) ? BattleaxeBaseline : GreatswordBaseline;
			break;
		case RE::WEAPON_TYPE::kTwoHandAxe:
			Baseline = weap->HasKeyword(IsWarhammer) ? WarhammerBaseline : BattleaxeBaseline;
			break;
		default:
			break;
		}
		const float Length = Baseline > 0.f ? LengthOf(weap) : -1.f;
		if (Length <= 0.f)
		{
			continue;
		}
		const SwingBody& Body = BodyOf(weap, IsWarhammer);
		// Swing time goes with the square root of inertia; the strength compresses that.
		const float Mult = std::clamp(std::pow(Baseline / SwingInertia(Length, Body), 0.5f * WeaponSettings::PhysicalSpeedStrength),
			PhysicalSpeedMin, PhysicalSpeedMax);
		Mults.emplace(weap, Mult);
		if (Settings::VerboseLogging && Logged.insert(weap->GetModel()).second)
		{
			logger::info("[weapon] {} {:08X} {} length {:.1f}: speed x{:.2f} ({})",
				weap->GetName(), weap->GetFormID(), Body.kind, Length, Mult, weap->GetModel());
		}
	}
	const auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Start).count();
	logger::info("Physical weapon speed: {} models measured in {} ms, {} without a usable mesh keep their class speed",
		Lengths.size(), Ms, Unmeasured);
	return Mults;
}


void SettingsLoader::HumanoidUndeadRaces()
{
	const auto* Nord = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESRace>(0x13746, "Skyrim.esm");
	if (!Nord)
	{
		logger::error("HumanoidUndeadRaces: NordRace not found");
		return;
	}
	for (auto* race : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESRace>())
	{
		if (!race || race == Nord)
		{
			continue;
		}
		// Every vanilla draugr and skeleton race runs a project under Actors\Draugr.
		std::string Path = race->behaviorGraphs[RE::SEXES::kMale].model.c_str();
		std::transform(Path.begin(), Path.end(), Path.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (Path.find("actors\\draugr\\") == std::string::npos)
		{
			continue;
		}
		for (std::uint32_t i = 0; i < RE::SEXES::kTotal; ++i)
		{
			race->skeletonModels[i].SetModel(Nord->skeletonModels[i].model.c_str());
			race->behaviorGraphs[i].SetModel(Nord->behaviorGraphs[i].model.c_str());
			race->rootBehaviorGraphNames[i] = Nord->rootBehaviorGraphNames[i];
			race->behaviorGraphProjectNames[i] = Nord->behaviorGraphProjectNames[i];
			// Vanilla only picks attacks whose event is in this table, and it was
			// built from the draugr project at load.
			if (Nord->attackAnimationArrayMap[i] && race->attackAnimationArrayMap[i] != Nord->attackAnimationArrayMap[i])
			{
				Nord->attackAnimationArrayMap[i]->IncRefCount();
				race->attackAnimationArrayMap[i] = Nord->attackAnimationArrayMap[i];
			}
		}

		if (Nord->attackDataMap && race->attackDataMap != Nord->attackDataMap)
		{
			RetiredAttackMaps.push_back(race->attackDataMap);
			race->attackDataMap = Nord->attackDataMap;
		}
		// Body part data names a skeleton and its nodes, so it follows the
		// skeleton. Precision also keys its attack collisions on it.
		race->bodyPartData = Nord->bodyPartData;
		HumanoidUndeadPatched.insert(race);
	}
	// NPC records build their own attack data map from the race's entries while
	// records load, before this runs, so theirs still hold the draugr entries.
	// Point them at the race's map, as they would have been had the race been
	// edited before load.
	int Repointed = 0;
	for (auto* npc : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESNPC>())
	{
		RE::TESRace* Race = npc ? npc->GetRace() : nullptr;
		if (!Race || !HumanoidUndeadPatched.contains(Race) || !Race->attackDataMap)
		{
			continue;
		}
		if (npc->attackDataMap != Race->attackDataMap)
		{
			if (npc->attackDataMap)
			{
				RetiredAttackMaps.push_back(npc->attackDataMap);
			}
			npc->attackDataMap = Race->attackDataMap;
			++Repointed;
		}
	}
	logger::info("HumanoidUndeadRaces: {} races on the humanoid skeleton and graph, {} NPC records pointed at their race's attack data",
		HumanoidUndeadPatched.size(), Repointed);
}

// "NPC L Foot [LLft ]" -> "NPC L Foot"
static std::string_view BaseName(std::string_view Name)
{
	const auto Bracket = Name.find(" [");
	return Bracket == std::string_view::npos ? Name : Name.substr(0, Bracket);
}

// SMP renames a bone it inserts as "hdtSSEPhysics_AutoRename_<tag> <original name>".
static std::string_view StripSmpPrefix(std::string_view Name)
{
	if (Name.starts_with("hdtSSEPhysics_AutoRename"))
	{
		const auto Space = Name.find(' ');
		if (Space != std::string_view::npos)
		{
			return Name.substr(Space + 1);
		}
	}
	return Name;
}

// this is a hack to force all draugr and skeletons to participate in the directional combat system correctly
void SettingsLoader::RetargetStaticBones(RE::Actor* actor)
{
	if (!HumanoidUndeadPatched.contains(actor->GetRace()))
	{
		return;
	}
	// Once per loaded 3D: a reload builds new meshes with fresh bindings.
	RE::NiAVObject* Root = actor->Get3D();
	if (!Root)
	{
		return;
	}
	{
		std::lock_guard Lock(RetargetMtx);
		RE::NiAVObject*& Scanned = RetargetScannedRoot[actor->GetFormID()];
		if (Scanned == Root)
		{
			return;
		}
		Scanned = Root;
	}
	// Recorded above before this lookup: a root without COM is scanned once,
	// not walked every frame.
	RE::NiAVObject* Com = Root->GetObjectByName("NPC COM [COM ]");
	if (!Com)
	{
		return;
	}
	// Every animated bone hangs under COM. Index them by the text before the
	// bracket; null where two share it.
	std::unordered_map<std::string, RE::NiAVObject*> ByBase;
	RE::BSVisit::TraverseScenegraphObjects(Com, [&](RE::NiAVObject* Node) -> RE::BSVisit::BSVisitControl {
		const std::string_view Name = Node->name.c_str();
		if (!Name.empty())
		{
			auto [It, Inserted] = ByBase.try_emplace(std::string(BaseName(Name)), Node);
			if (!Inserted)
			{
				It->second = nullptr;
			}
		}
		return RE::BSVisit::BSVisitControl::kContinue;
	});
	const char* RaceName = actor->GetRace()->GetFormEditorID();
	// now actually assign orphaned verts to the closest bone
	RE::BSVisit::TraverseScenegraphGeometries(Root, [&](RE::BSGeometry* Geometry) -> RE::BSVisit::BSVisitControl {
		auto* Skin = Geometry->GetGeometryRuntimeData().skinInstance.get();
		if (!Skin || !Skin->skinData || !Skin->bones || !Skin->boneWorldTransforms)
		{
			return RE::BSVisit::BSVisitControl::kContinue;
		}
		for (std::uint32_t i = 0; i < Skin->skinData->GetBoneCount(); ++i)
		{
			RE::NiAVObject* Bone = Skin->bones[i];
			if (!Bone)
			{
				continue;
			}
			// A bone outside COM never animates: SMP parks bones the skeleton
			// lacks under the root.
			bool UnderCom = false;
			for (RE::NiAVObject* Node = Bone; Node && !UnderCom; Node = Node->parent)
			{
				UnderCom = Node == Com;
			}
			if (UnderCom)
			{
				continue;
			}
			const std::string_view Name = StripSmpPrefix(Bone->name.c_str());
			auto It = ByBase.find(std::string(BaseName(Name)));
			if (It == ByBase.end() || !It->second)
			{
				continue;
			}
			// Pointer-sized writes, and the old node stays alive, so a render in
			// between reads the old bone or the new one, never garbage.
			Skin->boneWorldTransforms[i] = &It->second->world;
			Skin->bones[i] = It->second;
			if (Settings::VerboseLogging)
			{
				logger::info("[skin] {} mesh {} bone {} -> {}", RaceName, Geometry->name.c_str(), Name, It->second->name.c_str());
			}
		}
		return RE::BSVisit::BSVisitControl::kContinue;
	});
}

void SettingsLoader::RemovePowerAttacks()
{
	RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
	RE::BGSKeyword* AnimalKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x13798, "Skyrim.esm");
	RE::BGSKeyword* DwemerKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x1397A, "Skyrim.esm");
	RE::BGSKeyword* IncludePowerAttackKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x833E, "DirectionMod.esp");
	if (!Settings::RemovePowerAttacks)
	{
		// allowing users to not remove power attacks was too dangerous
		//return;
	}
	logger::info("erasing power attacks");

	// double weirdness - if the AI is forced to do power attacks thru scripts or anything of the sort, it will cause weird animation freezes
	// and the character will appear stuck while in an attack state
	// also, even though we nuke these, somehow the AI magically still does power attacks during certain events
	for (auto& race : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESRace>())
	{
		if (race)
		{
			if (!race->HasKeyword(AnimalKeyword) && !race->HasKeyword(DwemerKeyword) && !race->HasKeyword(IncludePowerAttackKeyword))
			{
				for (auto& iter : race->attackDataMap->attackDataMap)
				{
					if (iter.first.contains("attack") && iter.first.contains("Power") && !iter.first.contains("InPlace"))
					{
						race->attackDataMap->attackDataMap.erase(iter.first);
					}
					if (iter.first.contains("attack") && iter.first.contains("Sprint"))
					{
						race->attackDataMap->attackDataMap.erase(iter.first);
					}
					if (iter.first.contains("attack") && iter.first.contains("DualWield"))
					{
						race->attackDataMap->attackDataMap.erase(iter.first);
					}
				}
				for (unsigned i = 0; i < RE::SEXES::kTotal; ++i)
				{
					RE::AttackAnimationArrayMap* map = race->attackAnimationArrayMap[i];
					for (auto& iter : *map)
					{
						// gross
						RE::BSTArray<RE::SetEventData>* newevents = const_cast<RE::BSTArray<RE::SetEventData>*>(iter.second);
						unsigned j = 0;
						while (j < newevents->size())
						{

							if ((*newevents)[j].eventName.contains("attack") && (*newevents)[j].eventName.contains("Power") && !(*newevents)[j].eventName.contains("InPlace"))
							{
								//newevents->erase(&(*newevents)[i]);
								newevents->erase(newevents->begin() + j);
							}
							else if ((*newevents)[j].eventName.contains("attack") && (*newevents)[j].eventName.contains("Sprint"))
							{
								//newevents->erase(&(*newevents)[i]);
								newevents->erase(newevents->begin() + j);
							}
							else if ((*newevents)[j].eventName.contains("attack") && (*newevents)[j].eventName.contains("DualWield"))
							{
								//newevents->erase(&(*newevents)[i]);
								newevents->erase(newevents->begin() + j);
							}
							else
							{
								++j;
							}

						}
						/*
						// new seems bad
						for (auto animiter : *iter.second)
						{
							if (!animiter.eventName.contains("Power") && !animiter.eventName.contains("power"))
							{
								newevents->push_back(animiter);
							}
						}
						//iter.second = newevents;
						*/

					}
				}
			}



		}
	}

}