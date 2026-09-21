#pragma once

/*
	BBCF's online input delay, as a setting.

	The game ships a hardcoded 2 frames of GGPO input delay regardless of ping. That number
	reaches the netcode from exactly one place, and it is one immediate byte:

	    0x4E5971  6a 02     push 2
	    0x4E5973  6a 02     push 2      <- this one; its operand lives at 0x4E5974
	    0x4E5975  56        push esi
	    0x4E5976  6a 02     push 2
	    0x4E5978  51        push ecx
	              ...       sub esp,1Ch + rep movs (28-byte struct by value)
	    0x4E598C  e8 ...    call 0x783750        ; session setup

	Inside 0x783750 that argument sits at [ebp+30h] and is pushed at 0x7838D5 into
	ggpo_set_frame_delay (0x77C760). Verified against the shipped BBCF.exe: 0x783750 has
	exactly one caller and 0x77C760 has exactly one caller, so that byte is the only source
	of frame delay for every session the game creates.

	Why a byte and not a hook: there is nothing to intercept. The value is a constant the
	game pushes, it is read once at session creation, and changing it needs no trampoline,
	no prologue patch and no branch-target analysis. A signature scan was also deliberately
	not used - placeHooks_bbcf already costs over a second of brute-force scanning at boot
	(see docs/AI_REPO_MAP.md), and this does not need to add to it. The fixed offset is
	guarded by its surrounding bytes instead: if a game update moves this, the guard fails,
	the feature turns itself off and says so in the log rather than corrupting code.

	WHY THE RANGE IS 2-8 AND NOT 0-8
	--------------------------------
	GGPO stamps local input as frame (F + delay) and sends it to the peer with that stamp.
	A smaller delay therefore does not conjure responsiveness out of nothing: it means your
	inputs arrive later relative to the opponent's simulation, so THEY predict deeper and
	roll back more. Lowering your own delay bills your opponent for it.

	It also has a second victim. Deeper prediction is what walks into the prediction barrier
	(0x4E60D1, max 8 frames from 0x77CA6F) and into the starved-SyncInput path at 0x4E60D5 -
	which is the known spectator desync bug, not a hypothetical. A low-delay player can push
	spectators into it.

	Raising the delay is the opposite and is entirely safe: your inputs reach the opponent
	earlier, they predict less, and you pay for it yourself in input lag. That is the case
	this feature exists for - somebody on a bad connection who would rather have lag than
	rollback.

	So the shipped range is 2 to 8: stock or kinder, never ruder. Values below 2 are reachable
	only with EnableInDevelopmentFeatures on, for testing, and are clamped back up to 2
	otherwise. None of this is enforceable in general - anyone can chain-load
	super-continent/bbcf-online-delay and set 0 today - it only decides what this mod will do.
*/
namespace OnlineInputDelay
{
	// What the game ships with, and the floor for everyone who has not opted into
	// in-development features.
	constexpr int kStockDelay = 2;
	// The prediction cap is 8 frames, so there is no point offering more than that.
	constexpr int kMaxDelay = 8;

	// The setting, clamped: to [0, kMaxDelay] always, and up to kStockDelay unless
	// in-development features are on. This is what the UI should display as the value in
	// effect, so that a below-stock number in settings.ini does not silently read as active.
	int EffectiveDelay();

	// True while the user may choose a value below stock, i.e. while in-development
	// features are on. The UI uses it to set its own lower bound and to explain itself.
	bool BelowStockAllowed();

	// Writes the byte if the effective value has changed since the last write. Cheap enough
	// to call every rendered frame - the common case is one integer compare - and doing it
	// that way means the setting can be changed from the Settings window, the mod menu or
	// settings.ini by hand and still take effect without a game restart.
	//
	// It applies to the NEXT session, not the one in progress: the game reads the byte when
	// it creates the GGPO session, so a change made mid-match lands on the following match.
	void EnsureApplied();

	// False once the guard has failed, i.e. the patch site is not what this build expects.
	// The UI greys itself out and says so rather than pretending the number does anything.
	bool IsAvailable();
}
