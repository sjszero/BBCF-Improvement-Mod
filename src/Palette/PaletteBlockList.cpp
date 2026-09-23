#include "PaletteBlockList.h"

#include "PaletteThumbnails.h"

#include "Core/interfaces.h"
#include "Core/logger.h"
#include "Core/utils.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

namespace
{
	const char* const kListFile = "BBCF_IM\\BlockedPalettes.txt";
	const char* const kThumbFolder = "BBCF_IM\\BlockedPalettes";
	const char kThumbMagic[4] = { 'B', 'B', 'T', 'H' };

	bool g_loaded = false;
	int g_revision = 1;
	std::vector<PaletteBlockList::BlockedPalette> g_palettes;
	std::vector<PaletteBlockList::BlockedUser> g_users;

	// One line per entry, tab-separated; text fields lose tabs and line breaks.
	std::string Clean(const std::string& text)
	{
		std::string out;
		for (char c : text)
			out.push_back((c == '\t' || c == '\n' || c == '\r') ? ' ' : c);
		return out;
	}

	std::string CleanField(const char* text, size_t maxLength)
	{
		return Clean(std::string(text, strnlen(text, maxLength)));
	}

	std::vector<std::string> Split(const std::string& line)
	{
		std::vector<std::string> fields;
		size_t start = 0;
		for (;;)
		{
			const size_t tab = line.find('\t', start);
			fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
			if (tab == std::string::npos)
				break;
			start = tab + 1;
		}
		return fields;
	}

	std::string HashText(uint64_t hash)
	{
		char text[24];
		sprintf_s(text, "%016llx", (unsigned long long)hash);
		return text;
	}

	std::string ThumbPath(uint64_t hash)
	{
		return GamePath(std::string(kThumbFolder) + "\\" + HashText(hash) + ".thumb");
	}

	// "BBTH", u32 width, u32 height, u32 deflated size, then the deflated 0xAARRGGBB pixels.
	void WriteThumb(const PaletteBlockList::BlockedPalette& entry)
	{
		if (entry.thumb.empty())
			return;
		CreateDirectoryA(GamePath("BBCF_IM").c_str(), NULL);
		CreateDirectoryA(GamePath(kThumbFolder).c_str(), NULL);

		const mz_ulong rawSize = (mz_ulong)(entry.thumb.size() * 4);
		mz_ulong packedSize = mz_compressBound(rawSize);
		std::vector<unsigned char> packed(packedSize);
		if (mz_compress2(packed.data(), &packedSize, (const unsigned char*)entry.thumb.data(), rawSize, 9) != MZ_OK)
			return;

		std::ofstream file(utf8_to_utf16(ThumbPath(entry.hash)).c_str(), std::ios::binary);
		const unsigned int header[3] = { (unsigned int)entry.thumbWidth, (unsigned int)entry.thumbHeight, (unsigned int)packedSize };
		file.write(kThumbMagic, 4);
		file.write((const char*)header, sizeof(header));
		file.write((const char*)packed.data(), packedSize);
	}

	void ReadThumb(PaletteBlockList::BlockedPalette& entry)
	{
		std::ifstream file(utf8_to_utf16(ThumbPath(entry.hash)).c_str(), std::ios::binary);
		char magic[4];
		unsigned int header[3];
		if (!file.read(magic, 4) || memcmp(magic, kThumbMagic, 4) != 0 || !file.read((char*)header, sizeof(header)))
			return;
		const unsigned int width = header[0], height = header[1], packedSize = header[2];
		if (width == 0 || height == 0 || width > 1024 || height > 1024 || packedSize > 16u * 1024 * 1024)
			return;
		std::vector<unsigned char> packed(packedSize);
		if (!file.read((char*)packed.data(), packedSize))
			return;
		std::vector<unsigned int> pixels((size_t)width * height);
		mz_ulong rawSize = (mz_ulong)(pixels.size() * 4);
		if (mz_uncompress((unsigned char*)pixels.data(), &rawSize, packed.data(), packedSize) != MZ_OK ||
			rawSize != pixels.size() * 4)
			return;
		entry.thumb.swap(pixels);
		entry.thumbWidth = (int)width;
		entry.thumbHeight = (int)height;
	}

	void Save()
	{
		CreateDirectoryA(GamePath("BBCF_IM").c_str(), NULL);
		std::ofstream file(utf8_to_utf16(GamePath(kListFile)).c_str(), std::ios::binary);
		file << "# Palettes and players whose custom palettes are never shown. Edit from the Palettes window.\n";
		file << "# P <hash> <char> <blocked at> <from steamid> <name> <creator> <description> <from name>\n";
		file << "# U <steamid> <blocked at> <name>\n";
		for (const auto& p : g_palettes)
		{
			file << "P\t" << HashText(p.hash) << '\t' << p.charIndex << '\t' << p.blockedAt << '\t' << p.fromSteamId
				<< '\t' << Clean(p.name) << '\t' << Clean(p.creator) << '\t' << Clean(p.desc) << '\t' << Clean(p.fromName) << '\n';
		}
		for (const auto& u : g_users)
			file << "U\t" << u.steamId << '\t' << u.blockedAt << '\t' << Clean(u.name) << '\n';
		g_revision++;
	}

	void Load()
	{
		if (g_loaded)
			return;
		g_loaded = true;

		std::ifstream file(utf8_to_utf16(GamePath(kListFile)).c_str(), std::ios::binary);
		std::string line;
		while (std::getline(file, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty() || line[0] == '#')
				continue;
			const std::vector<std::string> f = Split(line);
			if (f[0] == "P" && f.size() >= 9)
			{
				PaletteBlockList::BlockedPalette p;
				p.hash = strtoull(f[1].c_str(), nullptr, 16);
				p.charIndex = atoi(f[2].c_str());
				p.blockedAt = _atoi64(f[3].c_str());
				p.fromSteamId = strtoull(f[4].c_str(), nullptr, 10);
				p.name = f[5];
				p.creator = f[6];
				p.desc = f[7];
				p.fromName = f[8];
				ReadThumb(p);
				g_palettes.push_back(p);
			}
			else if (f[0] == "U" && f.size() >= 4)
			{
				PaletteBlockList::BlockedUser u;
				u.steamId = strtoull(f[1].c_str(), nullptr, 10);
				u.blockedAt = _atoi64(f[2].c_str());
				u.name = f[3];
				g_users.push_back(u);
			}
		}
		LOG(2, "PaletteBlockList: %d palettes, %d players blocked\n", (int)g_palettes.size(), (int)g_users.size());
	}
}

namespace PaletteBlockList
{
	uint64_t HashPalette(const IMPL_data_t& palette)
	{
		// FNV-1a over the colour files only: renaming a palette does not unblock it.
		uint64_t hash = 14695981039346656037ull;
		const unsigned char* bytes = (const unsigned char*)palette.file0;
		for (size_t i = 0; i < (size_t)IMPL_PALETTE_DATALEN * IMPL_PALETTE_FILES_COUNT; i++)
			hash = (hash ^ bytes[i]) * 1099511628211ull;
		return hash;
	}

	bool IsPaletteBlocked(uint64_t hash)
	{
		Load();
		for (const auto& p : g_palettes)
			if (p.hash == hash)
				return true;
		return false;
	}

	bool IsUserBlocked(uint64_t steamId)
	{
		Load();
		if (!steamId)
			return false;
		for (const auto& u : g_users)
			if (u.steamId == steamId)
				return true;
		return false;
	}

	void BlockPalette(const IMPL_data_t& palette, int charIndex, uint64_t fromSteamId, const std::string& fromName)
	{
		Load();
		const uint64_t hash = HashPalette(palette);
		if (IsPaletteBlocked(hash))
			return;
		BlockedPalette p;
		p.hash = hash;
		p.charIndex = charIndex;
		p.name = CleanField(palette.palInfo.palName, IMPL_PALNAME_LENGTH);
		p.creator = CleanField(palette.palInfo.creator, IMPL_CREATOR_LENGTH);
		p.desc = CleanField(palette.palInfo.desc, IMPL_DESC_LENGTH);
		p.fromName = Clean(fromName);
		p.fromSteamId = fromSteamId;
		p.blockedAt = (long long)_time64(nullptr);
		PaletteThumbnails::RenderPixels(charIndex, palette.file0, p.thumb, &p.thumbWidth, &p.thumbHeight);
		WriteThumb(p);
		g_palettes.insert(g_palettes.begin(), p);
		Save();
	}

	void BlockUser(uint64_t steamId, const std::string& name)
	{
		Load();
		if (!steamId || IsUserBlocked(steamId))
			return;
		BlockedUser u;
		u.steamId = steamId;
		u.name = Clean(name);
		u.blockedAt = (long long)_time64(nullptr);
		g_users.insert(g_users.begin(), u);
		Save();
	}

	void UnblockPalette(uint64_t hash)
	{
		Load();
		const size_t before = g_palettes.size();
		g_palettes.erase(std::remove_if(g_palettes.begin(), g_palettes.end(),
			[hash](const BlockedPalette& p) { return p.hash == hash; }), g_palettes.end());
		if (g_palettes.size() != before)
		{
			DeleteFileA(ThumbPath(hash).c_str());
			Save();
		}
	}

	void UnblockUser(uint64_t steamId)
	{
		Load();
		const size_t before = g_users.size();
		g_users.erase(std::remove_if(g_users.begin(), g_users.end(),
			[steamId](const BlockedUser& u) { return u.steamId == steamId; }), g_users.end());
		if (g_users.size() != before)
			Save();
	}

	void UpdateUserName(uint64_t steamId, const std::string& name)
	{
		Load();
		const std::string clean = Clean(name);
		if (clean.empty() || clean == "[unknown]")
			return;
		for (auto& u : g_users)
		{
			if (u.steamId == steamId && u.name != clean)
			{
				u.name = clean;
				Save();
				return;
			}
		}
	}

	void ResolveMissingNames()
	{
		Load();
		ISteamFriends* friends = g_interfaces.pSteamFriendsWrapper;
		if (!friends)
			return;
		for (auto& u : g_users)
		{
			if (!u.name.empty() && u.name != "[unknown]")
				continue;
			// Steam answers from its cache, or "[unknown]" until the request below comes back;
			// asking again while it is in flight is harmless.
			friends->RequestUserInformation(CSteamID((uint64)u.steamId), true);
			const char* name = friends->GetFriendPersonaName(CSteamID((uint64)u.steamId));
			if (name && name[0] && strcmp(name, "[unknown]") != 0)
			{
				u.name = Clean(name);
				Save();
			}
		}
	}

	const std::vector<BlockedPalette>& Palettes()
	{
		Load();
		return g_palettes;
	}

	const std::vector<BlockedUser>& Users()
	{
		Load();
		return g_users;
	}

	int Revision()
	{
		Load();
		return g_revision;
	}

	std::string FormatDate(long long unixTime)
	{
		if (unixTime <= 0)
			return "?";
		const __time64_t t = (__time64_t)unixTime;
		tm local;
		if (_localtime64_s(&local, &t) != 0)
			return "?";
		char text[32];
		strftime(text, sizeof(text), "%Y-%m-%d", &local);
		return text;
	}
}
