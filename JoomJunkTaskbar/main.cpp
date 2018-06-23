#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <tchar.h>
#include <map>
#include <psapi.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <algorithm>

// for making the menu show up better
#include <ShellScalingAPI.h>

//used for the tray things
#include <shellapi.h>
#include "resource.h"

//we use a GUID for uniqueness
const static LPCWSTR singleProcName = L"344635E9-9AE4-4E60-B128-D53E25AB70A7";

//needed for tray exit
bool run = true;

// config file path (defaults to ./config.cfg)
std::wstring configfile;
// holds whether the user passed a --config parameter on the command line
bool explicitconfig;

// holds the alpha channel value between 0 or 255,
// defaults to -1 (not set).
int forcedtransparency;

HWND taskbar;
HWND secondtaskbar;
HMENU popup;

#pragma region composition

struct ACCENTPOLICY
{
	int nAccentState;
	int nFlags;
	int nColor;
	int nAnimationId;
};
struct WINCOMPATTRDATA
{
	int nAttribute;
	PVOID pData;
	ULONG ulDataSize;
};

struct OPTIONS
{
	int taskbar_appearance;
	int color;
} opt;

enum TASKBARSTATE { Normal, WindowMaximised, StartMenuOpen }; // Create a state to store all 
															  // states of the Taskbar
			// Normal           | Proceed as normal. If no dynamic options are set, act as it says in opt.taskbar_appearance
			// WindowMaximised  | There is a window which is maximised on the monitor this HWND is in. Display as blurred.
			// StartMenuOpen    | The Start Menu is open on the monitor this HWND is in. Display as it would be without JoomJunkTaskbar active.

enum SAVECONFIGSTATES { DoNotSave, SaveTransparency, SaveAll } shouldsaveconfig;  // Create an enum to store all config states
			// DoNotSave        | Fairly self-explanatory
			// SaveTransparency | Save opt.taskbar_appearance
			// SaveAll          | Save all options


struct READFROMCONFIG
{
	bool tint;
} configfileoptions; // Keep a struct, as we will need to save them later

struct TASKBARPROPERTIES
{
	HMONITOR hmon;
	TASKBARSTATE state;
};

std::vector<std::wstring> IgnoredClassNames;
std::vector<std::wstring> IgnoredExeNames;
std::vector<std::wstring> IgnoredWindowTitles;

int counter = 0;
const int ACCENT_DISABLED = 4; // Disables TTB for that taskbar
const int ACCENT_ENABLE_TRANSPARENTGRADIENT = 2; // Makes the taskbar a tinted transparent overlay. nColor is the tint color, sending nothing results in it interpreted as 0x00000000 (totally transparent, blends in with desktop)
const int ACCENT_ENABLE_TINTED = 5; // This is not a real state. We will handle it later.
unsigned int WM_TASKBARCREATED;
unsigned int NEW_TTB_INSTANCE;
int DYNAMIC_WS_STATE = ACCENT_ENABLE_TRANSPARENTGRADIENT; // State to activate when d-ws is enabled
std::map<HWND, TASKBARPROPERTIES> taskbars; // Create a map for all taskbars

std::wstring ExcludeFile = L"dynamic-ws-exclude.csv";

IVirtualDesktopManager *desktop_manager;

typedef BOOL(WINAPI*pSetWindowCompositionAttribute)(HWND, WINCOMPATTRDATA*);
static pSetWindowCompositionAttribute SetWindowCompositionAttribute = (pSetWindowCompositionAttribute)GetProcAddress(GetModuleHandle(TEXT("user32.dll")), "SetWindowCompositionAttribute");

void SetWindowBlur(HWND hWnd, int appearance = 0) // `appearance` can be 0, which means 'follow opt.taskbar_appearance'
{
	if (SetWindowCompositionAttribute)
	{
		ACCENTPOLICY policy;

		if (appearance)
		{
			policy = { appearance, 2, opt.color, 0 };
		}
		else
		{
			policy = { opt.taskbar_appearance, 2, opt.color, 0 };
		}

		WINCOMPATTRDATA data = { 19, &policy, sizeof(ACCENTPOLICY) }; // WCA_ACCENT_POLICY=19
		SetWindowCompositionAttribute(hWnd, &data);
	}

}

#pragma endregion 

#pragma region IO help

bool file_exists(std::wstring path)
{
	std::ifstream infile(path);
	return infile.good();
}

#pragma endregion

void add_to_startup()
{
	HMODULE hModule = GetModuleHandle(NULL);
	TCHAR path[MAX_PATH];
	GetModuleFileName(hModule, path, MAX_PATH);
	std::wstring unsafePath = path;
	std::wstring progPath = L"\"" + unsafePath + L"\"";
	HKEY hkey = NULL;
	LONG createStatus = RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", &hkey); //Creates a key       
	LONG status = RegSetValueEx(hkey, L"JoomJunkTaskbar", 0, REG_SZ, (BYTE *)progPath.c_str(), (DWORD)((progPath.size() + 1) * sizeof(wchar_t)));
}

void ParseSingleConfigOption(std::wstring arg, std::wstring value)
{
	opt.taskbar_appearance = ACCENT_ENABLE_TRANSPARENTGRADIENT;
}

void ParseConfigFile(std::wstring path)
{
	std::wifstream configstream(path);

	for (std::wstring line; std::getline(configstream, line); )
	{
		// Skip comments
		size_t comment_index = line.find(L';');
		if (comment_index == 0)
		{
			continue;
		}
		else
		{
			line = line.substr(0, comment_index);
		}

		size_t split_index = line.find(L'=');
		std::wstring key = line.substr(0, split_index);
		std::wstring val = line.substr(split_index + 1, line.length() - split_index - 1);

		ParseSingleConfigOption(key, val);
	}

	if (forcedtransparency >= 0)
	{
		opt.color = (forcedtransparency << 24) +
			(opt.color & 0x00FFFFFF);
	}
}

void SaveConfigFile()
{
	if (!configfile.empty())
	{
		using namespace std;
		wofstream configstream(configfile);

		configstream << L"; Taskbar appearance: opaque, transparent, or blur (default)." << endl;

		configstream << L"accent=transparent" << endl;
	}
}

void ParseSingleOption(std::wstring arg, std::wstring value)
{
	opt.taskbar_appearance = ACCENT_ENABLE_TRANSPARENTGRADIENT;

	if (arg == L"--startup")
	{
		add_to_startup();
	}
}

void ParseCmdOptions(bool configonly = false)
{
	// Set default values
	if (configonly)
	{
		shouldsaveconfig = DoNotSave;
		explicitconfig = false;
		configfile = L"config.cfg";
		forcedtransparency = -1;

		opt.taskbar_appearance = ACCENT_ENABLE_TRANSPARENTGRADIENT;
		opt.color = 0x00000000;
	}

	// Loop through command line arguments
	LPWSTR *szArglist;
	int nArgs;

	szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);


	// Find the --config option if provided
	for (int i = 0; i < nArgs; i++)
	{
		LPWSTR lparg = szArglist[i];
		LPWSTR lpvalue = (i + 1 < nArgs) ? szArglist[i + 1] : L"";

		std::wstring arg = std::wstring(lparg);
		std::wstring value = std::wstring(lpvalue);

		if (arg == L"--config")
		{
			// We allow multiple --config options. The later ones will override the previous ones.
			// The lates will be assigned to configfile, and that's where changes are saved.
			if (value.length() > 0 &&
				file_exists(value))
			{
				configfile = value;
				ParseConfigFile(value);
			}
			// TODO else? Missing or invalid parameter, should log
		}
	}

	// Iterate over the rest of the arguments 
	// Those options override the config files.
	if (configonly == false) // If configonly is false
	{
		for (int i = 0; i < nArgs; i++)
		{
			LPWSTR lparg = szArglist[i];
			LPWSTR lpvalue = (i + 1 < nArgs) ? szArglist[i + 1] : L"";

			std::wstring arg = std::wstring(lparg);
			std::wstring value = std::wstring(lpvalue);

			ParseSingleOption(arg, value);
		}
	}

	LocalFree(szArglist);
}

void RefreshHandles()
{
	HWND _taskbar;
	TASKBARPROPERTIES _properties;

	taskbars.clear();
	_taskbar = FindWindowW(L"Shell_TrayWnd", NULL);

	_properties.hmon = MonitorFromWindow(_taskbar, MONITOR_DEFAULTTOPRIMARY);
	_properties.state = Normal;
	taskbars.insert(std::make_pair(_taskbar, _properties));
	while (secondtaskbar = FindWindowEx(0, secondtaskbar, L"Shell_SecondaryTrayWnd", NULL))
	{
		_properties.hmon = MonitorFromWindow(secondtaskbar, MONITOR_DEFAULTTOPRIMARY);
		_properties.state = Normal;
		taskbars.insert(std::make_pair(secondtaskbar, _properties));
	}
}

std::wstring trim(std::wstring& str)
{
	size_t first = str.find_first_not_of(' ');
	size_t last = str.find_last_not_of(' ');

	if (first == std::wstring::npos)
	{
		return std::wstring(L"");
	}
	return str.substr(first, (last - first + 1));
}

std::vector<std::wstring> ParseByDelimiter(std::wstring row, std::wstring delimiter = L",")
{
	std::vector<std::wstring> result;
	std::wstring token;
	size_t pos = 0;
	while ((pos = row.find(delimiter)) != std::string::npos)
	{
		token = trim(row.substr(0, pos));
		result.push_back(token);
		row.erase(0, pos + delimiter.length());
	}
	return result;
}

void ParseDWSExcludesFile(std::wstring filename)
{
	std::wifstream excludesfilestream(filename);

	std::wstring delimiter = L","; // Change to change the char(s) used to split,

	for (std::wstring line; std::getline(excludesfilestream, line); )
	{
		size_t comment_index = line.find(L';');
		if (comment_index == 0)
		{
			continue;
		}
		else
		{
			line = line.substr(0, comment_index);
		}

		if (line.length() > delimiter.length())
		{
			if (line.compare(line.length() - delimiter.length(), delimiter.length(), delimiter))
			{
				line.append(delimiter);
			}
		}
		std::wstring line_lowercase = line;
		std::transform(line_lowercase.begin(), line_lowercase.end(), line_lowercase.begin(), tolower);
		if (line_lowercase.substr(0, 5) == L"class")
		{
			IgnoredClassNames = ParseByDelimiter(line, delimiter);
			IgnoredClassNames.erase(IgnoredClassNames.begin());
		}
		else if (line_lowercase.substr(0, 5) == L"title" || line.substr(0, 13) == L"windowtitle")
		{
			IgnoredWindowTitles = ParseByDelimiter(line, delimiter);
			IgnoredWindowTitles.erase(IgnoredWindowTitles.begin());
		}
		else if (line_lowercase.substr(0, 7) == L"exename")
		{
			IgnoredExeNames = ParseByDelimiter(line, delimiter);
			IgnoredExeNames.erase(IgnoredExeNames.begin());
		}
	}
}

#pragma endregion

#pragma region tray

#define WM_NOTIFY_TB 3141

HMENU menu;
NOTIFYICONDATA Tray;
HWND tray_hwnd;

void RefreshMenu()
{
	if (opt.taskbar_appearance == ACCENT_ENABLE_TRANSPARENTGRADIENT)
	{
		CheckMenuRadioItem(popup, IDM_BLUR, IDM_DYNAMICWS, IDM_CLEAR, MF_BYCOMMAND);
	}

	CheckMenuItem(popup, IDM_DYNAMICSTART, MF_BYCOMMAND | MF_UNCHECKED);

	if (RegGetValue(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", L"JoomJunkTaskbar", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS)
	{
		CheckMenuItem(popup, IDM_AUTOSTART, MF_BYCOMMAND | MF_CHECKED);
	}
}

void initTray(HWND parent)
{
	Tray.cbSize = sizeof(Tray);
	Tray.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1));
	Tray.hWnd = parent;
	wcscpy_s(Tray.szTip, L"TranslucentTB");
	Tray.uCallbackMessage = WM_NOTIFY_TB;
	Tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	Tray.uID = 101;
	Shell_NotifyIcon(NIM_ADD, &Tray);
	Shell_NotifyIcon(NIM_SETVERSION, &Tray);
	RefreshMenu();
}

LRESULT CALLBACK TBPROCWND(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_CLOSE:
		PostQuitMessage(0);
		break;
	case WM_NOTIFY_TB:
		if (lParam == WM_LBUTTONUP || lParam == WM_RBUTTONUP)
		{
			POINT pt;
			GetCursorPos(&pt);
			SetForegroundWindow(hWnd);
			UINT tray = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, NULL);
			switch (tray)
			{
			case IDM_CLEAR:
				opt.taskbar_appearance = ACCENT_ENABLE_TRANSPARENTGRADIENT;
				if (shouldsaveconfig == DoNotSave && shouldsaveconfig != SaveAll)
				{
					shouldsaveconfig = SaveTransparency;
				}
				RefreshMenu();
				break;
			case IDM_AUTOSTART:
				if (RegGetValue(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", L"JoomJunkTaskbar", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS)
				{
					HKEY hkey = NULL;
					RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", &hkey);
					RegDeleteValue(hkey, L"JoomJunkTaskbar");
				}
				else
				{
					add_to_startup();
				}
				RefreshMenu();
				break;
			case IDM_EXIT:
				run = false;
				break;
			}
		}
	}
	if (message == WM_TASKBARCREATED) // Unfortunately, WM_TASKBARCREATED is not a constant, so I can't include it in the switch.
	{
		RefreshHandles();
		initTray(tray_hwnd);
	}
	else if (message == NEW_TTB_INSTANCE)
	{
		shouldsaveconfig = DoNotSave;
		run = false;
	}
	return DefWindowProc(hWnd, message, wParam, lParam);
}

void SetTaskbarBlur()
{
	if (counter >= 10)
	{
		for (auto &taskbar : taskbars)
		{
			taskbar.second.state = Normal;
		}
	}

	for (auto const &taskbar : taskbars)
	{
		if (taskbar.second.state == WindowMaximised)
		{
			SetWindowBlur(taskbar.first, DYNAMIC_WS_STATE);
		}
		else if (taskbar.second.state == Normal)
		{
			SetWindowBlur(taskbar.first);
		}

	}
	counter++;
}

#pragma endregion

HANDLE ev;

bool singleProc()
{
	ev = CreateEvent(NULL, TRUE, FALSE, singleProcName);
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		return false;
	}
	return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPreInst, LPSTR pCmdLine, int nCmdShow)
{
	HRESULT dpi_success = SetProcessDpiAwareness(PROCESS_SYSTEM_DPI_AWARE);
	if (!dpi_success) 
	{
		OutputDebugStringW(L"Per-monitor DPI scaling failed");
	}

	ParseCmdOptions(true); // Command line argument settings, config file only
	ParseConfigFile(L"config.cfg"); // Config file settings
	ParseCmdOptions(false); // Command line argument settings, all lines
	ParseDWSExcludesFile(ExcludeFile);

	NEW_TTB_INSTANCE = RegisterWindowMessage(L"NewTTBInstance");
	if (!singleProc()) 
	{
		HWND oldInstance = FindWindow(L"JoomJunkTaskbar", L"TrayWindow");
		SendMessage(oldInstance, NEW_TTB_INSTANCE, NULL, NULL);
	}

	MSG msg; // for message translation and dispatch
	popup = LoadMenu(hInstance, MAKEINTRESOURCE(IDR_POPUP_MENU));
	menu = GetSubMenu(popup, 0);
	WNDCLASSEX wnd = { 0 };

	wnd.hInstance = hInstance;
	wnd.lpszClassName = L"JoomJunkTaskbar";
	wnd.lpfnWndProc = TBPROCWND;
	wnd.style = CS_HREDRAW | CS_VREDRAW;
	wnd.cbSize = sizeof(WNDCLASSEX);

	wnd.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wnd.hCursor = LoadCursor(NULL, IDC_ARROW);
	wnd.hbrBackground = (HBRUSH)BLACK_BRUSH;
	RegisterClassEx(&wnd);

	tray_hwnd = CreateWindowEx(WS_EX_TOOLWINDOW, L"JoomJunkTaskbar", L"TrayWindow", WS_OVERLAPPEDWINDOW, 0, 0,
		400, 400, NULL, NULL, hInstance, NULL);

	initTray(tray_hwnd);

	ShowWindow(tray_hwnd, WM_SHOWWINDOW);

	//Virtual Desktop stuff
	::CoInitialize(NULL);
	HRESULT desktop_success = ::CoCreateInstance(__uuidof(VirtualDesktopManager), NULL, CLSCTX_INPROC_SERVER, IID_IVirtualDesktopManager, (void **)&desktop_manager);
	if (!desktop_success)
	{
		OutputDebugStringW(L"Initialization of VirtualDesktopManager failed");
	}

	RefreshHandles();

	WM_TASKBARCREATED = RegisterWindowMessage(L"TaskbarCreated");

	while (run)
	{
		if (PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		SetTaskbarBlur();
		Sleep(10);
	}
	Shell_NotifyIcon(NIM_DELETE, &Tray);

	if (shouldsaveconfig != DoNotSave)
	{
		SaveConfigFile();
	}

	opt.taskbar_appearance = ACCENT_ENABLE_TRANSPARENTGRADIENT;
	SetTaskbarBlur();
	CloseHandle(ev);
	return 0;
}
