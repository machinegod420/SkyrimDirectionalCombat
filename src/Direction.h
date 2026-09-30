#pragma once

// should be unique tbh
// The co-save indexes by these values: never renumber.
enum class Directions : int
{
	TR = 0,
	TL = 1,
	BL = 2,
	BR = 3,
	Unblockable = 4
};

// The five attack animation sets. A copy of the OAR conditions, on purpose —
// the graph can't report which one it picked. The co-save indexes by these
// values: append new sets, never renumber.
enum class WeaponSet : int
{
	OneHand = 0,
	OneHandShield = 1,
	TwoHandSword = 2,
	Battleaxe = 3,
	Warhammer = 4
};
constexpr int NumWeaponSets = 5;

inline const char* WeaponSetName(WeaponSet set)
{
	switch (set)
	{
	case WeaponSet::OneHand: return "1h";
	case WeaponSet::OneHandShield: return "1h+shield";
	case WeaponSet::TwoHandSword: return "2h";
	case WeaponSet::Battleaxe: return "battleaxe";
	case WeaponSet::Warhammer: return "warhammer";
	}
	return "?";
}

