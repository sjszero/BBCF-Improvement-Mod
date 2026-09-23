#pragma once

#include <Windows.h>

// An eyedropper for the whole screen: the next click anywhere - on the game, on another
// program, on another monitor - reports the colour under it.
//
// A click outside the game would normally go to whatever window is there, activating it
// and taking focus away from the game. So while picking, a window of our own covers every
// monitor: WS_EX_LAYERED at alpha 1, which makes it invisible but still the thing that
// gets the click, with a crosshair cursor. The colour is read from the screen with
// GetPixel, which (like BitBlt without CAPTUREBLT) leaves layered windows out, so the
// cover never tints what it samples.
//
// A loupe follows the cursor - the pixels around it magnified, the centre one outlined,
// and the colour with its hex code - so a pick can be precise. It is layered too, so it
// never shows up in its own screen reads, and click-through.
//
// It runs on its own thread with its own message loop, never the render thread. Alt-
// tabbing away does not end it - finding a colour in another program is the point - but
// right-click or Escape (read from the keyboard directly, so it works whatever has focus)
// does, and so does a ten-minute backstop.
namespace ScreenColorPicker
{
	enum State
	{
		State_Idle,
		State_Picking,   // the colour under the cursor is being reported live
		State_Picked,    // a click chose it; reported once, then back to idle
		State_Cancelled, // right-click, Escape or timed out; reported once
	};

	// Starts picking. `returnFocusTo` gets the foreground back when it ends. False when a
	// pick is already running or the cover window could not be made.
	bool Start(HWND returnFocusTo);

	// Where it is, with the colour (0x00RRGGBB) under the cursor while picking, or the one
	// clicked once picked. Picked and Cancelled are each reported to exactly one call.
	State Poll(unsigned int* rgb);

	// Ends a running pick as cancelled.
	void Cancel();

	bool IsActive();
}
