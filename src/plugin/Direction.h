#pragma once

// should be unique tbh
enum class Directions : uint8_t
{
	TR = 0x1,
	TL = 0x2,
	BL = 0x4,
	BR = 0x8,
	Unblockable = 0x16
};

