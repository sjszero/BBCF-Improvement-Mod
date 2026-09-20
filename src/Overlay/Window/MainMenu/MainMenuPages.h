#pragma once

#include "MainMenuNav.h"

namespace MainMenu
{
	void DrawGamePage(const PageContext& ctx);
	void DrawTrainingPage(const PageContext& ctx);
	void DrawOverlaysPage(const PageContext& ctx);
	void DrawOnlinePage(const PageContext& ctx);
	void DrawReplaysPage(const PageContext& ctx);
	void DrawLookAndSoundPage(const PageContext& ctx);
	void DrawControllersPage(const PageContext& ctx);

	void DrawPage(PageId page, const PageContext& ctx);

	// The freeze / frame-step hotkeys. Polled from WindowManager::HandleButtons so they work
	// with the mod menu shut; the Overlays page only draws their buttons.
	void TickFreezeAndStepHotkeys();
}
