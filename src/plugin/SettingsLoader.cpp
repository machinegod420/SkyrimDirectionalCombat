#include "SettingsLoader.h"

#define SETTING_MACRO(sectionName, settingsClass, settingName, newval) \
    do { \
        settingsClass::settingName = newval; \
        logger::info("Loaded section {} setting {} with new value {}", \
            sectionName, #settingName, settingsClass::settingName); \
    } while (0)

float DifficultySettings::ComboResetTimer = 4.f;
float DifficultySettings::MeleeDamageMult = 2.f;
float DifficultySettings::UnblockableDamageMult = 3.f;
float DifficultySettings::ProjectileDamageMult = 0.25f;
float DifficultySettings::StaggerResetTimer = 1.5f;
float DifficultySettings::ChamberWindowTime = 0.2f;
float DifficultySettings::FeintWindowTime = 0.4f;
float DifficultySettings::HyperarmorTimer = 1.0f;
float DifficultySettings::StaminaRegenMult = 39.f;
float DifficultySettings::AttackTimeoutTime = 1.0f;
bool DifficultySettings::AttacksCostStamina = true;
float DifficultySettings::NonNPCStaggerMult = 2.f;
float DifficultySettings::StaminaCost = 0.1f;
float DifficultySettings::WeaponWeightStaminaMult = 0.33f;
float DifficultySettings::KnockbackMult = 2.f;
float DifficultySettings::StaminaDamageCap = 0.4f;
float DifficultySettings::DMCODodgeCost = 0.4f;

float Settings::ActiveDistance = 4000.f;
bool Settings::HasPrecision = false;
bool Settings::HasTDM = false;
bool Settings::EnableForH2H = true;
bool Settings::MNBMode = false;
bool Settings::ForHonorMode = false;
bool Settings::ExperimentalMode = false;
bool Settings::DMCOSupport = false;
bool Settings::BufferInput = true;
bool Settings::SwitchingCostsStamina = true;
bool Settings::RemovePowerAttacks = true;
bool Settings::VerboseLogging = false;

InputSettings::InputTypes InputSettings::InputType = InputSettings::InputTypes::MouseOnly;
int InputSettings::MouseSens = 5;
unsigned InputSettings::KeyModifierCode = 56;
unsigned InputSettings::KeyCodeTR = 2;
unsigned InputSettings::KeyCodeTL = 3;
unsigned InputSettings::KeyCodeBL = 4;
unsigned InputSettings::KeyCodeBR = 5;
unsigned InputSettings::KeyCodeFeint = 16;
unsigned InputSettings::KeyCodeBash = 18;
bool InputSettings::InvertY = false;

bool WeaponSettings::RebalanceWeapons = true;
float WeaponSettings::WarhammerSpeed = 0.72f;
float WeaponSettings::BattleaxeSpeed = 0.74f;
float WeaponSettings::GreatSwordSpeed = 0.77f;
float WeaponSettings::SwordSpeed = 0.8f;
float WeaponSettings::AxeSpeed = 0.8f;
float WeaponSettings::WeaponSpeedMult = 0.9f;
float WeaponSettings::BowSpeedMult = 0.6f;

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

float AISettings::LegendaryUpdateTimer = 0.15f;
float AISettings::VeryHardUpdateTimer = 0.18f;
float AISettings::HardUpdateTimer = 0.2f;
float AISettings::NormalUpdateTimer = 0.2f;
float AISettings::EasyUpdateTimer = 0.24f;
float AISettings::VeryEasyUpdateTimer = 0.3f;

float AISettings::LegendaryActionTimer = 0.13f;
float AISettings::VeryHardActionTimer = 0.17f;
float AISettings::HardActionTimer = 0.2f;
float AISettings::NormalActionTimer = 0.2f;
float AISettings::EasyActionTimer = 0.24f;
float AISettings::VeryEasyActionTimer = 0.28f;

float UISettings::Size = 300.f;
float UISettings::Length = 13.f;
float UISettings::Thickness = 7.f;
float UISettings::DisplayDistance = 2000.0;
bool UISettings::ShowUI = true;
bool UISettings::FlashUI = false;
bool UISettings::HarderUI = true;
bool UISettings::OnlyShowTargetted = true;

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
					InputSettings::KeyCodeBL = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, InputSettings::KeyCodeBL);
				}
				else if (fieldName == "KeyCodeBR")
				{
					int newval = field.as<unsigned>();
					InputSettings::KeyCodeBR = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, InputSettings::KeyCodeBR);
				}
				else if (fieldName == "KeyCodeFeint")
				{
					int newval = field.as<unsigned>();
					InputSettings::KeyCodeFeint = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, InputSettings::KeyCodeFeint);
				}
				else if (fieldName == "KeyCodeBash")
				{
					int newval = field.as<unsigned>();
					InputSettings::KeyCodeBash = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, InputSettings::KeyCodeBash);
				}
				else if (fieldName == "InvertY")
				{
					bool newval = field.as<bool>();
					InputSettings::InvertY = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, InputSettings::InvertY);
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
				if (fieldName == "UnblockableDamageMult")
				{
					float newval = field.as<float>();
					DifficultySettings::UnblockableDamageMult = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::UnblockableDamageMult);
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
				else if (fieldName == "StaminaCost")
				{
					float newval = field.as<float>();
					DifficultySettings::StaminaCost = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, DifficultySettings::StaminaCost);
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
				else if (fieldName == "ForHonorMode")
				{
					bool newval = field.as<bool>();
					Settings::ForHonorMode = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::ForHonorMode);
				}
				else if (fieldName == "ExperimentalMode")
				{
					bool newval = field.as<bool>();
					Settings::ExperimentalMode = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::ExperimentalMode);
				}
				else if (fieldName == "DMCOSupport")
				{
					bool newval = field.as<bool>();
					Settings::DMCOSupport = newval;
					logger::info("Loaded section {} setting {} with new value {}",
						sectionName, fieldName, Settings::DMCOSupport);
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
		else if (weap->IsBow() || weap->IsCrossbow())
		{
			weap->weaponData.speed *= WeaponSettings::BowSpeedMult;
		}
	}
}

float SettingsLoader::CalcDamage(float diff)
{
	float ret = 1.0;
	ret += diff;
	ret = std::min(ret, 1.5f);
	ret = std::max(ret, 0.5f);
	return ret;
}

void SettingsLoader::RebalanceWeapons()
{
	logger::info("Rebalancing weapons!");

	for (RE::TESCombatStyle* combatStyle : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESCombatStyle>())
	{
		//logger::info("parsed {}", combatStyle->GetFormEditorID());
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
		combatStyle->meleeData.specialAttackMult = 0.2f;
		combatStyle->meleeData.bashPowerAttackMult = 0.f;
		combatStyle->meleeData.bashAttackMult = 0.f;
		combatStyle->meleeData.bashRecoilMult = 0.f;
		combatStyle->flags.reset(RE::TESCombatStyle::FLAG::kAllowDualWielding);
		float circleMult = combatStyle->closeRangeData.circleMult * 1.f;
		circleMult = std::min(0.99f, circleMult);
		combatStyle->closeRangeData.circleMult = circleMult;

		//float fallbackMult = actor->combatStyle->closeRangeData.fallbackMult * 1.5f;
		//fallbackMult = std::min(0.99f, fallbackMult);
		//actor->combatStyle->closeRangeData.fallbackMult = fallbackMult; 

	}
	for (auto& weap : RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESObjectWEAP>())
	{
		if (weap->IsMelee())
		{
			float speed = weap->weaponData.speed;
			float damage = (float)weap->attackDamage;
			uint16_t newdamage = weap->attackDamage;
			float newspeed = weap->weaponData.speed;
			switch (weap->GetWeaponType())
			{
			case RE::WEAPON_TYPE::kOneHandDagger:
			case RE::WEAPON_TYPE::kOneHandSword:
			{
				newdamage = uint16_t(damage * CalcDamage(speed - WeaponSettings::SwordSpeed));
				newspeed = WeaponSettings::SwordSpeed;
				break;
			}
			case RE::WEAPON_TYPE::kOneHandMace:
			case RE::WEAPON_TYPE::kOneHandAxe:
			{
				newdamage = uint16_t(damage * CalcDamage(speed - WeaponSettings::AxeSpeed));
				newspeed = WeaponSettings::AxeSpeed;
				break;
			}

			case RE::WEAPON_TYPE::kTwoHandSword:
			{
				newdamage = uint16_t(damage * CalcDamage(speed - WeaponSettings::GreatSwordSpeed));
				newspeed = WeaponSettings::GreatSwordSpeed;
				break;
			}

			case RE::WEAPON_TYPE::kTwoHandAxe:
			{
				//special case here due to having to use keywords
				if (weap->HasKeyword(IsWarhammer))
				{
					newdamage = uint16_t(damage * CalcDamage(speed - WeaponSettings::WarhammerSpeed));
					newspeed = WeaponSettings::WarhammerSpeed;
				}
				else
				{
					newdamage = uint16_t(damage * CalcDamage(speed - WeaponSettings::BattleaxeSpeed));
					newspeed = WeaponSettings::BattleaxeSpeed;
				}
				break;
			}
			}
			//logger::info("{} got its damaged changed from {} to {} and speed from {} to {}", weap->GetName(), weap->attackDamage, newdamage, speed, weap->weaponData.speed);
			weap->attackDamage = newdamage;
			weap->weaponData.speed = newspeed;
		}

		
	}
}

void SettingsLoader::RemovePowerAttacks()
{
	RE::TESDataHandler* DataHandler = RE::TESDataHandler::GetSingleton();
	RE::BGSKeyword* AnimalKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x13798, "Skyrim.esm");
	RE::BGSKeyword* DwemerKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x1397A, "Skyrim.esm");
	RE::BGSKeyword* IncludePowerAttackKeyword = DataHandler->LookupForm<RE::BGSKeyword>(0x833E, "DirectionMod.esp");
	if (!Settings::RemovePowerAttacks)
	{
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
					//logger::info("got {}", iter.first.c_str());
					if (iter.first.contains("attack") && iter.first.contains("Power") && !iter.first.contains("InPlace"))
					{
						race->attackDataMap->attackDataMap.erase(iter.first);
						//logger::info("erasing {} with result {}", iter.first.c_str(), result);
					}
					if (iter.first.contains("attack") && iter.first.contains("Sprint"))
					{
						race->attackDataMap->attackDataMap.erase(iter.first);
						//logger::info("erasing {} with result {}", iter.first.c_str(), result);
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

							//logger::info("{}", (*newevents)[j].eventName);
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