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
#include <ShellScalingAPI.h>
#include <shellapi.h>
#include "resource.h"

struct OPTIONS
{
	int taskbar_appearance;
	int color;
} opt;