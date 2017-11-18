#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#define STRICT
#define OPTPARSE_IMPLEMENTATION
#define OPTPARSE_API static
#include <windows.h>
#include <Windowsx.h>
#include <Commctrl.h>
#include <shellapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>
#include <Strsafe.h>
#include "optparse.h"
#include "resource.h"

#if defined (DEBUG) | defined (_DEBUG)
#define DEBUG_PRINTF(fmt, ...) \
		{ \
			WCHAR buffer[1024]; \
			StringCchPrintf(buffer, ARRAYSIZE(buffer), fmt, __VA_ARGS__); \
			OutputDebugString(buffer); \
		}
#else
#define DEBUG_PRINTF(fmt, ...)
#endif

#define WM_NOTIFYICON	(WM_USER + 1)
#define IDM_EXIT		(WM_USER + 2)


typedef enum
{
	MOUSE_BUTTON_UNKNOWN = -1,
	MOUSE_BUTTON_LEFT,
	MOUSE_BUTTON_RIGHT,
	MOUSE_BUTTON_MIDDLE,
	MOUSE_BUTTON_X1,
	MOUSE_BUTTON_X2,
	MOUSE_BUTTON_COUNT
} MouseButton;

typedef struct
{
	bool isMonitored;
	bool isBlocked;
	uint32_t blocks;
	uint64_t previousTime;
	uint64_t threshold;
	uint32_t thresholdMs;
} MOUSEBUTTONDATA;

static LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
static inline bool RegisterInvisibleClass(const HINSTANCE hInstance);
static bool AddNotifyIcon(const HINSTANCE hInstance, const HWND hWnd, const bool restore);
static void RemoveNotifyIcon();
static bool InstallMSLLHook();
static void UninstallMSLLHook();
static void ProcessCommandLineArgs();
static void PrepareMouseButtonData();
static void SetDoubleClickThreshold(const int threshold, const MouseButton button);
static void ShowContextMenu(const HWND hWnd, const int x, const int y);
static void CDECL ShowErrorMessageBox(LPCWSTR message, ...);
static MouseButton GetButtonByWParam(const WPARAM wParam, const PMSLLHOOKSTRUCT pdata);
static LPCWSTR GetButtonName(const MouseButton button);

static LPCWSTR APPNAME = L"Mouse Debouncer";
static LPCWSTR CLASSNAME = L"MouseDebouncerWndClass";

static HHOOK msll_hook = NULL;
static NOTIFYICONDATA notify_icon_data;
static MOUSEBUTTONDATA mouse_button_data[MOUSE_BUTTON_COUNT];
static int32_t double_click_threshold_ms_max = 500;
static int32_t double_click_threshold_ms_min = 1;
static uint32_t double_click_threshold_ms = 60;
static uint64_t double_click_threshold;
static uint64_t counts_per_second;
static bool use_qpc;

int CALLBACK WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
	// Limit the application to one instance.
	const HANDLE mutex = CreateMutex(NULL, TRUE, L"{05B95384-625D-491A-A326-94758957C021}");
	if (!mutex)
	{
		ShowErrorMessageBox(L"The mutex could not be created!");
		return EXIT_FAILURE;
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_ACCESS_DENIED)
	{
		ShowErrorMessageBox(L"Only one instance at a time!");
		return EXIT_SUCCESS;
	}

	ProcessCommandLineArgs();
	PrepareMouseButtonData();

	const ATOM atom = RegisterInvisibleClass(hInstance);
	if (atom)
	{
		// hWndParent is not HWND_MESSAGE because message-only windows don't receive broadcast messages like TaskbarCreated.
		if (!CreateWindow(CLASSNAME, APPNAME, 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL))
		{
			ShowErrorMessageBox(L"The window couldn't be created.");
			PostQuitMessage(1);
		}
	}
	else
	{
		ShowErrorMessageBox(L"The window class couldn't be registered.");
		PostQuitMessage(1);
	}

	// Start the message loop.
	// Set hWnd to NULL because WM_QUIT will be sent to the message loop thread, not to any particular window.
	// If the function retrieves WM_QUIT, the return value is zero.
	MSG msg;
	BOOL ret;
	while ((ret = GetMessage(&msg, NULL, 0, 0)) != 0)
	{
		if (ret != -1)
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			// It's extremely unlikely that GetMessage() will return -1 but just in case try to release the important stuff.
			ShowErrorMessageBox(L"An unknown error occured: 0x%lx", GetLastError());
			UninstallMSLLHook();
			ReleaseMutex(mutex);
			CloseHandle(mutex);
			return GetLastError();
		}
	}

	if (atom) UnregisterClass(CLASSNAME, hInstance);
	ReleaseMutex(mutex);
	CloseHandle(mutex);

	// Return the exit code given in PostQuitMessage() function.
	return (int)msg.wParam;
}

static LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// This message is needed for restoring the notification icon after a explorer crash.
	static UINT wm_taskbarcreated;

	switch (uMsg)
	{
		case WM_CREATE:
			// Add NotifyIcon to the taskbar ("system tray") and install the hook.
			if (!AddNotifyIcon(((LPCREATESTRUCT)lParam)->hInstance, hWnd, false) || !InstallMSLLHook())
				return -1; // destroy the window
			wm_taskbarcreated = RegisterWindowMessage(L"TaskbarCreated");
			return 0;
		case WM_NOTIFYICON:
			// With NOTIFYICON_VERSION_4 LOWORD(lParam) contains notification events (WM_CONTEXTMENU, ..).
			switch (LOWORD(lParam))
			{
				case WM_CONTEXTMENU:
					ShowContextMenu(hWnd, GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
					return 0;
				default: ;
			}
			break;
		case WM_COMMAND:
			// If the message source is a menu, the lower word of wParam contains the menu identifier (IDM_*).
			switch (LOWORD(wParam))
			{
				case IDM_EXIT:
					PostMessage(hWnd, WM_CLOSE, 0, 0);
					return 0;
				default: ;
			}
			break;
		case WM_CLOSE:
			// Destroy the window and send WM_DESTROY to deactivate it.
			DestroyWindow(hWnd);
			break;
		case WM_DESTROY:
			// Free the icon and post a WM_QUIT message to the thread's message queue.
			UninstallMSLLHook();
			RemoveNotifyIcon();
			PostQuitMessage(0);
			return 0;
		default:
			if (uMsg == wm_taskbarcreated)
				AddNotifyIcon((HINSTANCE)GetModuleHandle(NULL), hWnd, true);
			break;
	}

	// Pass all unhandled messages to DefWindowProc.
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && wParam != WM_MOUSEMOVE)
	{
		// MSLLHOOKSTRUCT is needed to differentiate between XBUTTON1 & XBUTTON2 and to get the message's time stamp.
		const PMSLLHOOKSTRUCT pdata = (PMSLLHOOKSTRUCT)lParam;
		const MouseButton pressedButton = GetButtonByWParam(wParam, pdata);

		if (pressedButton >= 0 && mouse_button_data[pressedButton].isMonitored)
		{
			uint64_t currentTime;
			if (use_qpc)
				// Casting should be fine because LARGE_INTEGER is a union containing LONGLONG next to the parts struct,
				// so the memory layout will not be a problem (?) - otherwise QuadPart of a temporal LI has to be used.
				QueryPerformanceCounter((LARGE_INTEGER*)&currentTime);
			else
				currentTime = pdata->time;

			switch (wParam)
			{
				case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
				{
					const uint64_t elapsedTime = currentTime - mouse_button_data[pressedButton].previousTime;
					if (!mouse_button_data[pressedButton].isBlocked && elapsedTime <= double_click_threshold)
					{
						mouse_button_data[pressedButton].isBlocked = true;
						mouse_button_data[pressedButton].blocks++;
						DEBUG_PRINTF(L"blocked 0x%04X (%I64u)\n", wParam, elapsedTime);
						// Return nonzero value to prevent the system from passing the message to the rest of the hook chain.
						return 1;
					}
					break;
				}
				case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
				{
					// The corresponding button up event should be blocked too..
					if (mouse_button_data[pressedButton].isBlocked)
					{
						mouse_button_data[pressedButton].isBlocked = false;
						return 1;
					}

					mouse_button_data[pressedButton].previousTime = currentTime;
					break;
				}
				default: ;
			}
		}
	}
	// First parameter is optional and apparently ignored anyway.
	return CallNextHookEx(msll_hook, nCode, wParam, lParam);
}

static bool RegisterInvisibleClass(const HINSTANCE hInstance)
{
	// Set the few window class information required for creating an invisible window.
	WNDCLASSEX wc = { 0 };
	wc.cbSize = sizeof(WNDCLASSEX);
	wc.lpfnWndProc = (WNDPROC)WindowProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = CLASSNAME;

	// Register the window class for subsequent use of CreateWindow.
	return RegisterClassEx(&wc);
}

static bool AddNotifyIcon(const HINSTANCE hInstance, const HWND hWnd, const bool restore)
{
	if (!restore)
	{
		notify_icon_data.cbSize = sizeof(NOTIFYICONDATA);
		notify_icon_data.hWnd = hWnd;
		// Using uID instead of guidItem because GUIDs are such a hassle (dependent on path, ..).
		notify_icon_data.uID = IDI_MAINICON;
		notify_icon_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
		notify_icon_data.uCallbackMessage = WM_NOTIFYICON;
		// Using NOTIFYICON_VERSION_4 because it's recommended for Windows Vista and later.
		notify_icon_data.uVersion = NOTIFYICON_VERSION_4;

		StringCchCopy(notify_icon_data.szTip, wcslen(APPNAME) + 1, APPNAME);

		// Using LoadIconMetric to ensure that the correct icon is loaded and scaled appropriately.
		if (FAILED(LoadIconMetric(hInstance, MAKEINTRESOURCE(IDI_NOTIFYICON), LIM_SMALL, &notify_icon_data.hIcon)))
		{
			ShowErrorMessageBox(L"The icon couldn't be loaded.");
			return false;
		}
	}
	
	// Try to add the icon to the notification area...
	if (!Shell_NotifyIcon(NIM_ADD, &notify_icon_data))
	{
		ShowErrorMessageBox(L"The notify icon couldn't be added.");
		return false;
	}

	// ...and to set it's version.
	if (!Shell_NotifyIcon(NIM_SETVERSION, &notify_icon_data))
	{
		ShowErrorMessageBox(L"The requested NOTIFYICON_VERSION_4 isn't supported.");
		return false;
	}

	return true;
}

static void RemoveNotifyIcon()
{
	// Remove the icon from the notification area.
	Shell_NotifyIcon(NIM_DELETE, &notify_icon_data);
	// The icon has to be destroyed because it was retrieved by LoadIconMetric.
	DestroyIcon(notify_icon_data.hIcon);
}

static bool InstallMSLLHook()
{
	msll_hook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, NULL, 0);
	if (!msll_hook)
	{
		ShowErrorMessageBox(L"The mouse hook couldn't be installed.");
		return false;
	}
	return true;
}

static void UninstallMSLLHook()
{
	if (msll_hook) UnhookWindowsHookEx(msll_hook);
}

// ToDo: Add optparse wchar support to switch to wWinMain and replace atoi?
static void ProcessCommandLineArgs()
{
	struct optparse_long longopts[] =
	{
		{ "qpc", 'q', OPTPARSE_NONE },
		{ "threshold", 't', OPTPARSE_REQUIRED },
		{ "left", 'l', OPTPARSE_OPTIONAL },
		{ "right", 'r', OPTPARSE_OPTIONAL },
		{ "middle", 'm', OPTPARSE_OPTIONAL },
		{ "four", 'b', OPTPARSE_OPTIONAL },
		{ "five", 'f', OPTPARSE_OPTIONAL },
		{ 0 }
	};

	int option;
	struct optparse options;

	optparse_init(&options, __argv);

	while ((option = optparse_long(&options, longopts, NULL)) != -1)
	{
		switch (option)
		{
			case 'q':
				use_qpc = true;
				break;
			case 't':
				SetDoubleClickThreshold(atoi(options.optarg), MOUSE_BUTTON_COUNT);
				break;
			case 'l':
				mouse_button_data[MOUSE_BUTTON_LEFT].isMonitored = true;
				if (options.optarg) SetDoubleClickThreshold(atoi(options.optarg), MOUSE_BUTTON_LEFT);
				break;
			case 'r':
				mouse_button_data[MOUSE_BUTTON_RIGHT].isMonitored = true;
				if (options.optarg) SetDoubleClickThreshold(atoi(options.optarg), MOUSE_BUTTON_RIGHT);
				break;
			case 'm':
				mouse_button_data[MOUSE_BUTTON_MIDDLE].isMonitored = true;
				if (options.optarg) SetDoubleClickThreshold(atoi(options.optarg), MOUSE_BUTTON_MIDDLE);
				break;
			case 'b':
				mouse_button_data[MOUSE_BUTTON_X1].isMonitored = true;
				if (options.optarg) SetDoubleClickThreshold(atoi(options.optarg), MOUSE_BUTTON_X1);
				break;
			case 'f':
				mouse_button_data[MOUSE_BUTTON_X2].isMonitored = true;
				if (options.optarg) SetDoubleClickThreshold(atoi(options.optarg), MOUSE_BUTTON_X2);
				break;
			default:;
		}
	}
}

static void PrepareMouseButtonData()
{
	if (use_qpc && !QueryPerformanceFrequency((LARGE_INTEGER*)&counts_per_second))
	{
		// That shouldn't happen for systems that run on Windows XP or later.
		ShowErrorMessageBox(
			L"Apparently your system doesn't support the high-resolution performance counter."
			L"Please remove -q; --qpc from the command line parameters."
			L"Switching to the ordinary timing method now.");
		use_qpc = false;
	}

	if (use_qpc)
		// double_click_threshold : counts per x ms
		// Setting it like that will safe time in the hook callback because we do not have to convert the counts to ms everytime.
		double_click_threshold = double_click_threshold_ms * counts_per_second / 1000;
	else
		double_click_threshold = double_click_threshold_ms;

	// Set the individual button thresholds to the global threshold if they weren't specified in the launch options.
	bool isButtonSpecified = false;
	for (int i = MOUSE_BUTTON_COUNT; i--;)
	{
		if (!mouse_button_data[i].thresholdMs)
		{
			mouse_button_data[i].threshold = double_click_threshold_ms;
			mouse_button_data[i].thresholdMs = double_click_threshold_ms;
		}

		if (use_qpc)
			mouse_button_data[i].threshold *= counts_per_second / 1000;

		if (mouse_button_data[i].isMonitored)
			isButtonSpecified = true;
	}

	// Default the monitored button to the left one if none were specified in the launch options.
	if (!isButtonSpecified)
		mouse_button_data[MOUSE_BUTTON_LEFT].isMonitored = true;
}

static void SetDoubleClickThreshold(const int threshold, const MouseButton button)
{
	// Set double click threshold for passed button if in value range.
	if (threshold >= double_click_threshold_ms_min && threshold <= double_click_threshold_ms_max)
	{
		// Exception: Set the general threshold if MOUSE_BUTTON_COUNT was passed.
		if (button == MOUSE_BUTTON_COUNT)
			double_click_threshold_ms = threshold;
		else
			mouse_button_data[button].thresholdMs = threshold;
	}
	else
		ShowErrorMessageBox(L"Invalid threshold for '%s Mouse Button': %d (min: %I32ums max: %I32ums)",
			GetButtonName(button), threshold, double_click_threshold_ms_min, double_click_threshold_ms_max);
}

static void ShowContextMenu(const HWND hWnd, const int x, const int y)
{
	// The current window must be the foreground window before calling TrackPopupMenu.
	// Otherwise, the menu will not disappear when the user clicks outside of the menu.
	SetForegroundWindow(hWnd);

	const HMENU hMenu = CreatePopupMenu();
	if (hMenu)
	{
		WCHAR buffer[64];
		uint32_t totalblocks = 0;

		// Calculate total blocked clicks.
		for (int i = MOUSE_BUTTON_COUNT; i--;)
			totalblocks += mouse_button_data[i].blocks;

		// Add general information.
		StringCchPrintf(buffer, 64, L"General\t%6I32u blocks       %03I32u ms", totalblocks, double_click_threshold_ms);
		InsertMenu(hMenu, -1, MF_BYPOSITION | MF_STRING | MF_GRAYED, 0, buffer);
		InsertMenu(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);

		// Add individual button information.
		for (int i = 0; i < MOUSE_BUTTON_COUNT; i++)
		{
			if (mouse_button_data[i].isMonitored)
			{
				StringCchPrintf(buffer, 64, L"%s Mouse Button\t%6I32u blocks       %03I32u ms",
				                GetButtonName(i), mouse_button_data[i].blocks, mouse_button_data[i].thresholdMs);
				InsertMenu(hMenu, -1, MF_BYPOSITION | MF_STRING | MF_GRAYED, 0, buffer);
			}
		}

		// Add exit item.
		InsertMenu(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
		InsertMenu(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_EXIT, L"Exit");

		// The window does not receive a WM_COMMAND message from the menu until the function returns.
		TrackPopupMenu(hMenu, TPM_BOTTOMALIGN, x, y, 0, hWnd, NULL);
		DestroyMenu(hMenu);
	}
	else
	{
		ShowErrorMessageBox(L"The popup menu couldn't be created. Exiting now..");
		PostMessage(hWnd, WM_CLOSE, 0, 0);
	}
}

static void CDECL ShowErrorMessageBox(LPCWSTR message, ...)
{
	WCHAR buffer[1024];
	va_list args;
	va_start(args, message);
	StringCchVPrintf(buffer, ARRAYSIZE(buffer), message, args);
	va_end(args);

	// Set hWnd to NULL, so the message box will not have a owner window,
	// which is invisible anyway. With NULL it'll also show up in the taskbar.
	MessageBox(NULL, buffer, APPNAME, MB_OK | MB_ICONEXCLAMATION);
}

static inline MouseButton GetButtonByWParam(const WPARAM wParam, const PMSLLHOOKSTRUCT pdata)
{
	switch (wParam)
	{
		case WM_LBUTTONDOWN: case WM_LBUTTONUP:
			return MOUSE_BUTTON_LEFT;
		case WM_RBUTTONDOWN: case WM_RBUTTONUP:
			return MOUSE_BUTTON_RIGHT;
		case WM_MBUTTONDOWN: case WM_MBUTTONUP:
			return MOUSE_BUTTON_MIDDLE;
		case WM_XBUTTONDOWN: case WM_XBUTTONUP:
			return HIWORD(pdata->mouseData) == XBUTTON1	? MOUSE_BUTTON_X1 : MOUSE_BUTTON_X2;
		default: 
			return MOUSE_BUTTON_UNKNOWN; // That shouldn't happen..
	}
}

static LPCWSTR GetButtonName(const MouseButton button)
{
	switch (button)
	{
		case MOUSE_BUTTON_LEFT:	return L"Left";
		case MOUSE_BUTTON_RIGHT: return L"Right";
		case MOUSE_BUTTON_MIDDLE: return L"Middle";
		case MOUSE_BUTTON_X1: return L"4th";
		case MOUSE_BUTTON_X2: return L"5th";
		case MOUSE_BUTTON_COUNT: return L"Every";
		default: return NULL;
	}
}
