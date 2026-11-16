#pragma once

// This is an explicit SDK include as well as the optional native precompiled
// header. The source files remain self-contained when precompilation is off.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <ole2.h>
#include <ocidl.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <shellapi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite_3.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <commctrl.h>
#include <wrl/client.h>
