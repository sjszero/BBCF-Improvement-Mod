#pragma once
#include "impl_format.h"

#include <string>

// The game's own colours, read straight out of data/Char/char_XX_pal.pac.
//
// The mod only embeds one template per character (implTemplates[], which is Color 01), so
// anything that wants "Color 06 Kokonoe" as a starting point has to ask the game files.
// Each archive holds 26 colours x 8 files as "<tag><colour>_<file>.hpl", every entry a
// 32-byte HPAL header followed by the same 1024 BGRA bytes a .cfpl stores - so a colour
// maps onto IMPL_data_t file for file, with no conversion.
namespace NativePalettes
{
	// Colours the character select screen offers; the pac carries two more that it never
	// shows, and they are left out on purpose.
	const int kColorCount = 24;

	// Fills `out` with colour `colorIndex` (0-based, so 5 is "Color 06") of the character.
	// palInfo is cleared. Returns false and a user-facing message on failure.
	bool Load(int charIndex, int colorIndex, IMPL_data_t& out, std::string& error);

	// The character's asset tag, the XX in data/Char/char_XX_*.pac; nullptr if unknown.
	const char* CharTag(int charIndex);
}
