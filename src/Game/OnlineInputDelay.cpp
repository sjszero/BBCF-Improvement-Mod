#include "OnlineInputDelay.h"

#include "Core/Settings.h"
#include "Core/logger.h"
#include "Core/utils.h"

#include <Windows.h>

namespace
{
	// Offsets from the module base, i.e. virtual address minus 0x400000, the same convention
	// GhidraDefs.h uses.
	constexpr size_t kPatchSiteOffset = 0x000E5971; // the run of pushes, for the guard
	constexpr size_t kDelayByteIndex = 3;           // 6a [02] 6a XX ... -> the second push's operand

	// What the site must look like before we touch it. Byte 3 is the delay itself, so it is
	// wildcarded: it reads 2 on a fresh process and whatever we last wrote after that.
	//
	//   6a 02  6a ??  56  6a 02  51  83 ec 1c
	//   push 2 push ? push esi  push 2 push ecx  sub esp,1Ch
	//
	// The full eleven bytes occur exactly once in the shipped BBCF.exe, so matching them is
	// as strong as a signature scan would have been, at no scan cost.
	constexpr unsigned char kGuard[] = {
		0x6A, 0x02, 0x6A, 0x00, 0x56, 0x6A, 0x02, 0x51, 0x83, 0xEC, 0x1C,
	};
	constexpr size_t kGuardLength = sizeof(kGuard);

	bool g_checked = false;
	bool g_available = false;
	// -1 until the first write, so the first EnsureApplied always writes even when the
	// setting happens to equal the stock value: a previous run of the mod in the same
	// process cannot be assumed.
	int g_appliedDelay = -1;

	unsigned char* PatchSite()
	{
		char* base = GetBbcfBaseAdress();
		return base ? reinterpret_cast<unsigned char*>(base + kPatchSiteOffset) : nullptr;
	}

	// One read of the site, once, with the delay byte ignored. A failure here is permanent:
	// if the game's code is not what this build was written against, the honest thing is to
	// do nothing at all rather than write a byte into the middle of some other instruction.
	bool CheckSite()
	{
		if (g_checked)
		{
			return g_available;
		}
		g_checked = true;

		unsigned char* const site = PatchSite();
		if (site == nullptr)
		{
			LOG(1, "[OnlineDelay] no module base yet; input delay setting disabled\n");
			return false;
		}

		if (IsBadReadPtr(site, kGuardLength))
		{
			LOG(1, "[OnlineDelay] patch site unreadable; input delay setting disabled\n");
			return false;
		}

		for (size_t i = 0; i < kGuardLength; ++i)
		{
			if (i == kDelayByteIndex)
			{
				continue;
			}
			if (site[i] != kGuard[i])
			{
				LOG(1, "[OnlineDelay] patch site does not match this build at +%u "
				       "(expected %02X, found %02X); input delay setting disabled\n",
					static_cast<unsigned>(i), kGuard[i], site[i]);
				return false;
			}
		}

		LOG(1, "[OnlineDelay] patch site verified; game ships %d frames of delay\n",
			static_cast<int>(site[kDelayByteIndex]));
		g_available = true;
		return true;
	}
}

bool OnlineInputDelay::BelowStockAllowed()
{
	return Settings::settingsIni.enableInDevelopmentFeatures;
}

int OnlineInputDelay::EffectiveDelay()
{
	int delay = Settings::settingsIni.onlineInputDelay;

	if (delay < 0) { delay = 0; }
	if (delay > kMaxDelay) { delay = kMaxDelay; }

	// The clamp that is the whole policy: below stock is a testing value, not a shipped one.
	if (delay < kStockDelay && !BelowStockAllowed())
	{
		delay = kStockDelay;
	}

	return delay;
}

bool OnlineInputDelay::IsAvailable()
{
	return CheckSite();
}

void OnlineInputDelay::EnsureApplied()
{
	const int wanted = EffectiveDelay();
	if (wanted == g_appliedDelay)
	{
		return;
	}

	if (!CheckSite())
	{
		// Latch anyway. The site is not going to start matching later, and retrying every
		// frame would mean an IsBadReadPtr per frame for the life of the process.
		g_appliedDelay = wanted;
		return;
	}

	unsigned char* const target = PatchSite() + kDelayByteIndex;
	const unsigned char previous = *target;
	const unsigned char value = static_cast<unsigned char>(wanted);

	DWORD oldProtect = 0;
	if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		LOG(1, "[OnlineDelay] VirtualProtect failed (%lu); leaving the delay at %u\n",
			GetLastError(), previous);
		g_appliedDelay = wanted;
		return;
	}

	*target = value;
	VirtualProtect(target, 1, oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), target, 1);

	g_appliedDelay = wanted;
	LOG(1, "[OnlineDelay] online input delay %u -> %u frames (setting=%d, below-stock %s); "
	       "applies to the next online session\n",
		previous, value, Settings::settingsIni.onlineInputDelay,
		BelowStockAllowed() ? "unlocked" : "locked");
}
