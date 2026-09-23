#include "NativePalettes.h"

#include "Audio/PacFile.h"
#include "Core/utils.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	// Asset tag of each character's archives, in CharIndex order. Established by
	// byte-matching implTemplates[] against every shipped char_XX_pal.pac, so the odd ones
	// (Nu=ny, Lambda=rm, Nine=ph, Izanami=mi, Mai=ma) are data rather than guesses.
	const char* const kCharTags[] = {
		"rg", "jn", "no", "rc", "tk", "tg", "lc", "ar", "bn", "ca", "ha", "ny",
		"tb", "hz", "mu", "mk", "vh", "pt", "rl", "iz", "am", "bl", "az", "kg",
		"kk", "tm", "ce", "rm", "hb", "ph", "nt", "mi", "su", "es", "ma", "jb",
	};
	const int kCharTagCount = sizeof(kCharTags) / sizeof(kCharTags[0]);

	const size_t kHpalHeaderLen = 32;

	unsigned int ReadU32(const std::vector<unsigned char>& buf, size_t off)
	{
		if (off + 4 > buf.size())
			return 0;
		return (unsigned int)buf[off] | ((unsigned int)buf[off + 1] << 8) |
			((unsigned int)buf[off + 2] << 16) | ((unsigned int)buf[off + 3] << 24);
	}

	// The last character's archive, inflated. Switching the base colour back and forth
	// in the editor is the common case, and that should not re-read the file each time.
	int g_cachedChar = -1;
	std::vector<unsigned char> g_cachedPac;

	bool EndsWith(const char* name, size_t len, const char* suffix)
	{
		const size_t suffixLen = strlen(suffix);
		return len >= suffixLen && _strnicmp(name + len - suffixLen, suffix, suffixLen) == 0;
	}
}

namespace NativePalettes
{
	const char* CharTag(int charIndex)
	{
		return (charIndex >= 0 && charIndex < kCharTagCount) ? kCharTags[charIndex] : nullptr;
	}

	bool Load(int charIndex, int colorIndex, IMPL_data_t& out, std::string& error)
	{
		if (charIndex < 0 || charIndex >= kCharTagCount)
		{
			error = "No game palettes are known for this character.";
			return false;
		}
		if (colorIndex < 0 || colorIndex >= kColorCount)
		{
			error = "That colour does not exist.";
			return false;
		}

		if (g_cachedChar != charIndex)
		{
			g_cachedChar = -1;
			g_cachedPac.clear();

			const std::string path = GamePath(std::string("data\\Char\\char_") +
				kCharTags[charIndex] + "_pal.pac");
			std::string readError;
			if (!PacFile::Read(path, g_cachedPac, &readError))
			{
				g_cachedPac.clear();
				error = "Could not read " + path + ": " + readError;
				return false;
			}
			g_cachedChar = charIndex;
		}

		const std::vector<unsigned char>& pac = g_cachedPac;
		if (pac.size() < 0x20 || memcmp(pac.data(), "FPAC", 4) != 0)
		{
			error = "The game's palette archive is not in a format this build understands.";
			return false;
		}

		const unsigned int dataStart = ReadU32(pac, 0x04);
		const unsigned int fileCount = ReadU32(pac, 0x0C);
		const unsigned int nameField = ReadU32(pac, 0x14);
		if (fileCount == 0 || nameField < 4 || nameField > 256 ||
			dataStart < 0x20 || dataStart > pac.size())
		{
			error = "The game's palette archive is damaged.";
			return false;
		}
		const unsigned int stride = (dataStart - 0x20) / fileCount;
		if (stride < nameField + 12)
		{
			error = "The game's palette archive is damaged.";
			return false;
		}

		char* files[IMPL_PALETTE_FILES_COUNT] = {
			out.file0, out.file1, out.file2, out.file3,
			out.file4, out.file5, out.file6, out.file7
		};
		bool found[IMPL_PALETTE_FILES_COUNT] = {};

		// Matched on the "NN_FF.hpl" tail rather than the full name: Lambda's archive, for
		// one, is not named after the tag its entries would suggest.
		char suffixes[IMPL_PALETTE_FILES_COUNT][16];
		for (int f = 0; f < IMPL_PALETTE_FILES_COUNT; f++)
			sprintf_s(suffixes[f], "%02d_%02d.hpl", colorIndex, f);

		for (unsigned int i = 0; i < fileCount; i++)
		{
			const size_t entry = 0x20 + (size_t)i * stride;
			if (entry + nameField + 12 > pac.size())
				break;

			const char* name = (const char*)&pac[entry];
			size_t nameLen = 0;
			while (nameLen < nameField && name[nameLen] != '\0')
				nameLen++;

			for (int f = 0; f < IMPL_PALETTE_FILES_COUNT; f++)
			{
				if (found[f] || !EndsWith(name, nameLen, suffixes[f]))
					continue;

				const size_t offset = (size_t)dataStart + ReadU32(pac, entry + nameField + 4);
				const size_t size = ReadU32(pac, entry + nameField + 8);
				if (size < kHpalHeaderLen + IMPL_PALETTE_DATALEN || offset + size > pac.size() ||
					memcmp(&pac[offset], "HPAL", 4) != 0)
				{
					continue;
				}

				memcpy(files[f], &pac[offset + kHpalHeaderLen], IMPL_PALETTE_DATALEN);
				found[f] = true;
			}
		}

		for (int f = 0; f < IMPL_PALETTE_FILES_COUNT; f++)
		{
			if (!found[f])
			{
				error = "The game's palette archive is missing part of this colour.";
				return false;
			}
		}

		out.palInfo = IMPL_info_t();
		return true;
	}
}
