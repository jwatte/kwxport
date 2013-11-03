
#ifndef kwexport_h
#define kwexport_h

#ifndef NDEBUG
#define _ASSERTE(x) \
  if (x); else assert_failure(__FILE__, __LINE__, #x)
void assert_failure(char const *file, int line, char const *expr);
#else
#define _ASSERTE(x) (void)0
#endif

#include <string>
#include <exception>

#include "coreexp.h"
#include "Max.h"

#include "resource.h"
#include "istdplug.h"
#include "iparamb2.h"
#include "iparamm2.h"

#include "decomp.h"
#include "utilapi.h"

#include "IGame.h"
#include "IGameObject.h"
#include "IGameProperty.h"
#include "IGameControl.h"
#include "IGameModifier.h"
#include "IConversionManager.h"
#include "IGameError.h"
#include "IGameFX.h"

#include <windows.h>
#include <commctrl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <dxerr.h>
#include <atlbase.h>
#include <stdio.h>

#include <vector>
#include <map>
#include <string>
#include <algorithm>

#define NOMINMAX
#define NO_MINMAX
#define NO_MIN_MAX

#pragma comment(lib, "igame.lib")
#pragma comment(lib, "core.lib")
#pragma comment(lib, "paramblk2.lib")
#pragma comment(lib, "maxutil.lib")
#pragma comment(lib, "geom.lib")


#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "d3d9.lib")
#if !defined(NDEBUG)
 #pragma comment(lib, "d3dx9d.lib")
#else
 #pragma comment(lib, "d3dx9.lib")
#endif
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxerr.lib")

extern HINSTANCE hInstance;

#include "kwlog.h"

#undef min
#undef max

#endif
