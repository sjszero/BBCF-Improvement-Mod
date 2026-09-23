#include "ScreenColorPicker.h"

#include <atomic>
#include <cwchar>
#include <thread>

namespace
{
	const wchar_t* const kWindowClass = L"BBCFIM_ScreenColorPicker";
	const wchar_t* const kLoupeClass = L"BBCFIM_ScreenColorPickerLoupe";
	const UINT_PTR kSampleTimer = 1;
	const UINT kSampleIntervalMs = 16;
	// Only a backstop against a pick nobody finishes; alt-tabbing away to find a colour in
	// another program is the point, so there is no cancelling on focus loss.
	const DWORD kTimeoutMs = 10 * 60 * 1000;

	// The loupe: kLoupePixels x kLoupePixels screen pixels around the cursor, each drawn
	// kLoupeZoom times over, with the colour and its hex code in a bar underneath.
	const int kLoupePixels = 11;
	const int kLoupeZoom = 11;
	const int kLoupeImage = kLoupePixels * kLoupeZoom;
	const int kLoupeBar = 26;
	const int kLoupeOffset = 24;

	HWND g_loupe = NULL;
	HDC g_loupeCapture = NULL;
	HBITMAP g_loupeCaptureBitmap = NULL;
	HGDIOBJ g_loupeCaptureOld = NULL;

	std::atomic<int> g_state(ScreenColorPicker::State_Idle);
	std::atomic<unsigned int> g_colour(0);
	std::atomic<bool> g_running(false);
	std::atomic<HWND> g_window(NULL);
	HWND g_returnFocusTo = NULL;
	DWORD g_startTick = 0;

	unsigned int SampleAt(POINT p)
	{
		HDC screen = GetDC(NULL);
		const COLORREF c = GetPixel(screen, p.x, p.y);
		ReleaseDC(NULL, screen);
		if (c == CLR_INVALID)
			return g_colour.load();
		return ((unsigned int)GetRValue(c) << 16) | ((unsigned int)GetGValue(c) << 8) | GetBValue(c);
	}

	unsigned int SampleCursor()
	{
		POINT p;
		if (!GetCursorPos(&p))
			return g_colour.load();
		return SampleAt(p);
	}

	void Finish(HWND hwnd, ScreenColorPicker::State state)
	{
		g_state = state;
		KillTimer(hwnd, kSampleTimer);
		DestroyWindow(hwnd);
	}

	// Grabs the pixels around the cursor and moves the loupe next to it, flipped to the
	// other side near a screen edge so it stays on screen.
	void UpdateLoupe()
	{
		if (!g_loupe)
			return;
		POINT p;
		if (!GetCursorPos(&p))
			return;

		// BitBlt without CAPTUREBLT leaves layered windows out: the cover and the loupe
		// itself are never in the picture.
		HDC screen = GetDC(NULL);
		BitBlt(g_loupeCapture, 0, 0, kLoupePixels, kLoupePixels, screen,
			p.x - kLoupePixels / 2, p.y - kLoupePixels / 2, SRCCOPY);
		ReleaseDC(NULL, screen);

		const int width = kLoupeImage + 2;
		const int height = kLoupeImage + kLoupeBar + 2;
		HMONITOR monitor = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
		MONITORINFO info = { sizeof(info) };
		GetMonitorInfo(monitor, &info);
		int x = p.x + kLoupeOffset;
		int y = p.y + kLoupeOffset;
		if (x + width > info.rcMonitor.right)
			x = p.x - kLoupeOffset - width;
		if (y + height > info.rcMonitor.bottom)
			y = p.y - kLoupeOffset - height;
		SetWindowPos(g_loupe, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
		InvalidateRect(g_loupe, NULL, FALSE);
	}

	void PaintLoupe(HWND hwnd)
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT client;
		GetClientRect(hwnd, &client);

		// Border, the magnified pixels, and the centre pixel outlined.
		HBRUSH border = CreateSolidBrush(RGB(20, 20, 24));
		FillRect(dc, &client, border);
		DeleteObject(border);
		SetStretchBltMode(dc, COLORONCOLOR);
		StretchBlt(dc, 1, 1, kLoupeImage, kLoupeImage, g_loupeCapture, 0, 0, kLoupePixels, kLoupePixels, SRCCOPY);
		const int centre = 1 + (kLoupePixels / 2) * kLoupeZoom;
		RECT cell = { centre - 1, centre - 1, centre + kLoupeZoom + 1, centre + kLoupeZoom + 1 };
		HBRUSH outerFrame = CreateSolidBrush(RGB(0, 0, 0));
		HBRUSH innerFrame = CreateSolidBrush(RGB(255, 255, 255));
		FrameRect(dc, &cell, outerFrame);
		InflateRect(&cell, -1, -1);
		FrameRect(dc, &cell, innerFrame);
		DeleteObject(outerFrame);
		DeleteObject(innerFrame);

		// The colour itself, and its hex code.
		const unsigned int rgb = g_colour.load();
		const COLORREF colour = RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
		RECT swatch = { 4, kLoupeImage + 5, 4 + kLoupeBar - 6, kLoupeImage + kLoupeBar - 1 };
		HBRUSH swatchBrush = CreateSolidBrush(colour);
		FillRect(dc, &swatch, swatchBrush);
		DeleteObject(swatchBrush);
		wchar_t hex[16];
		swprintf_s(hex, L"#%06X", rgb & 0xFFFFFF);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(235, 235, 235));
		RECT text = { swatch.right + 6, swatch.top, client.right - 2, swatch.bottom };
		DrawTextW(dc, hex, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

		EndPaint(hwnd, &ps);
	}

	LRESULT CALLBACK LoupeProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (msg == WM_PAINT)
		{
			PaintLoupe(hwnd);
			return 0;
		}
		if (msg == WM_NCHITTEST)
			return HTTRANSPARENT;
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_TIMER:
			if (wParam == kSampleTimer)
			{
				g_colour = SampleCursor();
				UpdateLoupe();
				// Escape is read from the keyboard itself: after an alt-tab the cover is not
				// the focused window and would never see the key.
				if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || GetTickCount() - g_startTick > kTimeoutMs)
					Finish(hwnd, ScreenColorPicker::State_Cancelled);
			}
			return 0;
		case WM_LBUTTONDOWN:
			g_colour = SampleCursor();
			Finish(hwnd, ScreenColorPicker::State_Picked);
			return 0;
		case WM_RBUTTONDOWN:
			Finish(hwnd, ScreenColorPicker::State_Cancelled);
			return 0;
		case WM_KEYDOWN:
			if (wParam == VK_ESCAPE)
				Finish(hwnd, ScreenColorPicker::State_Cancelled);
			return 0;
		case WM_SETCURSOR:
			SetCursor(LoadCursor(NULL, IDC_CROSS));
			return TRUE;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	// Physical pixels for everything this thread does. The game is not DPI aware, and on
	// a scaled monitor the cursor position and the screen read would otherwise be in
	// different units and sample the wrong pixel. Looked up at runtime: Windows 10 1607+.
	void UsePhysicalPixels()
	{
		typedef HANDLE(WINAPI* SetThreadDpiAwarenessContextFn)(HANDLE);
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		SetThreadDpiAwarenessContextFn setContext = user32 ?
			(SetThreadDpiAwarenessContextFn)GetProcAddress(user32, "SetThreadDpiAwarenessContext") : NULL;
		if (setContext)
			setContext((HANDLE)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
	}

	void Run()
	{
		UsePhysicalPixels();
		HINSTANCE instance = GetModuleHandleW(NULL);
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = WindowProc;
		wc.hInstance = instance;
		wc.hCursor = LoadCursor(NULL, IDC_CROSS);
		wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
		wc.lpszClassName = kWindowClass;
		RegisterClassExW(&wc); // fails harmlessly when already registered by an earlier pick

		WNDCLASSEXW loupeClass = {};
		loupeClass.cbSize = sizeof(loupeClass);
		loupeClass.lpfnWndProc = LoupeProc;
		loupeClass.hInstance = instance;
		loupeClass.lpszClassName = kLoupeClass;
		RegisterClassExW(&loupeClass);

		// Every monitor at once.
		const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
		const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
		const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
		const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
		HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW, kWindowClass, L"",
			WS_POPUP, x, y, w, h, NULL, NULL, instance, NULL);
		if (!hwnd)
		{
			g_state = ScreenColorPicker::State_Cancelled;
			g_running = false;
			return;
		}
		// Alpha 1, not 0: a fully transparent layered window lets clicks through.
		SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);

		// The loupe: layered (so screen reads skip it) at full opacity, and click-through,
		// so it never gets in the way of the click it is previewing.
		g_loupe = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
			kLoupeClass, L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, instance, NULL);
		if (g_loupe)
		{
			SetLayeredWindowAttributes(g_loupe, 0, 255, LWA_ALPHA);
			HDC screen = GetDC(NULL);
			g_loupeCapture = CreateCompatibleDC(screen);
			g_loupeCaptureBitmap = CreateCompatibleBitmap(screen, kLoupePixels, kLoupePixels);
			g_loupeCaptureOld = SelectObject(g_loupeCapture, g_loupeCaptureBitmap);
			ReleaseDC(NULL, screen);
		}
		g_window = hwnd;
		g_startTick = GetTickCount();
		g_colour = SampleCursor();
		ShowWindow(hwnd, SW_SHOW);
		SetForegroundWindow(hwnd);
		SetFocus(hwnd);
		SetTimer(hwnd, kSampleTimer, kSampleIntervalMs, NULL);

		MSG msg;
		while (GetMessageW(&msg, NULL, 0, 0) > 0)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		g_window = NULL;
		if (g_loupe)
		{
			DestroyWindow(g_loupe);
			g_loupe = NULL;
			SelectObject(g_loupeCapture, g_loupeCaptureOld);
			DeleteObject(g_loupeCaptureBitmap);
			DeleteDC(g_loupeCapture);
			g_loupeCapture = NULL;
			g_loupeCaptureBitmap = NULL;
		}
		if (g_returnFocusTo && IsWindow(g_returnFocusTo))
			SetForegroundWindow(g_returnFocusTo);
		g_running = false;
	}
}

namespace ScreenColorPicker
{
	bool Start(HWND returnFocusTo)
	{
		bool expected = false;
		if (!g_running.compare_exchange_strong(expected, true))
			return false;
		g_returnFocusTo = returnFocusTo;
		g_state = State_Picking;
		std::thread(Run).detach();
		return true;
	}

	State Poll(unsigned int* rgb)
	{
		const int state = g_state.load();
		if (rgb)
			*rgb = g_colour.load();
		if (state == State_Picked || state == State_Cancelled)
		{
			// One report per ending.
			int expected = state;
			g_state.compare_exchange_strong(expected, State_Idle);
		}
		return (State)state;
	}

	void Cancel()
	{
		HWND hwnd = g_window.load();
		if (hwnd)
			PostMessageW(hwnd, WM_RBUTTONDOWN, 0, 0);
	}

	bool IsActive()
	{
		return g_running.load();
	}
}
