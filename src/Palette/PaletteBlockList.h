#pragma once

#include "impl_format.h"

#include <cstdint>
#include <string>
#include <vector>

// Palettes, and players, whose custom palettes you never want to see online.
//
// A blocked palette is remembered by a fingerprint of its colours - a hash of all eight
// files - plus what it takes to recognise it later: its name, creator and description, the
// character, who it came from, and a thumbnail as plain pixels. Never the palette itself:
// keeping it would be a way to take a copy of a palette its owner did not allow to be
// downloaded. A blocked player is their SteamID and the name they had when blocked.
//
// Kept in BBCF_IM\BlockedPalettes.txt, thumbnails in BBCF_IM\BlockedPalettes\. Enforced by
// OnlinePaletteManager, which checks every palette it receives before showing it.
namespace PaletteBlockList
{
	struct BlockedPalette
	{
		uint64_t hash = 0;
		int charIndex = 0;
		std::string name;
		std::string creator;
		std::string desc;
		std::string fromName;
		uint64_t fromSteamId = 0;
		long long blockedAt = 0;          // unix time
		std::vector<unsigned int> thumb;  // 0xAARRGGBB, empty if the build had no sprite
		int thumbWidth = 0;
		int thumbHeight = 0;
	};

	struct BlockedUser
	{
		uint64_t steamId = 0;
		std::string name;
		long long blockedAt = 0;
	};

	// The fingerprint a palette is blocked by: all eight files, so a recolour of any of
	// them is a different palette.
	uint64_t HashPalette(const IMPL_data_t& palette);

	bool IsPaletteBlocked(uint64_t hash);
	bool IsUserBlocked(uint64_t steamId);

	void BlockPalette(const IMPL_data_t& palette, int charIndex, uint64_t fromSteamId, const std::string& fromName);
	void BlockUser(uint64_t steamId, const std::string& name);
	void UnblockPalette(uint64_t hash);
	void UnblockUser(uint64_t steamId);

	// Keeps a blocked player's name current: people rename, and the name is how the list
	// is read. Called when they turn up in a match.
	void UpdateUserName(uint64_t steamId, const std::string& name);
	// Asks Steam for the names of blocked players stored without one (blocked by SteamID).
	// Cheap enough to call every frame the list is on screen.
	void ResolveMissingNames();

	const std::vector<BlockedPalette>& Palettes();
	const std::vector<BlockedUser>& Users();

	// Changes whenever either list does, so whoever enforces them can tell when to look again.
	int Revision();

	// "2026-09-23", for showing when something was blocked.
	std::string FormatDate(long long unixTime);
}
