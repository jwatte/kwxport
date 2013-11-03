
#include "kwxport.h"
#include "resource.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern HINSTANCE hInstance;

namespace {
  HWND gErrorWnd;
  HWND gErrorTxt;
  bool gSuppressPrompts;
  ATOM gErrorClass;
  HFONT gPrettyFont;

  LARGE_INTEGER startTime_;
  double conversion_;
  FILE *curLogFile;
}


char const *Timestamp()
{
  static char buf[100];
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  sprintf(buf, "@%0.06f: ", (double)(now.QuadPart - startTime_.QuadPart) * conversion_);
  return buf;
}

void MakeFont()
{
  if (gPrettyFont == NULL)
    gPrettyFont = ::CreateFont(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_SWISS | VARIABLE_PITCH, "");
}

LRESULT CALLBACK ErrorWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg) {
    case WM_CREATE: {
        RECT r;
        ::GetClientRect(hWnd, &r);
        r.left += 10;
        r.top += 10;
        r.right -= 10;
        r.bottom -= 10;
        gErrorTxt = ::CreateWindow(
            WC_EDIT,
            "",
            WS_VISIBLE | WS_CHILD | ES_AUTOVSCROLL | ES_READONLY | ES_MULTILINE,
            r.left, r.top, r.right-r.left, r.bottom-r.top,
            hWnd,
            0,
            0,
            0);
        MakeFont();
        ::SendMessage(gErrorTxt, WM_SETFONT, (WPARAM)gPrettyFont, FALSE);
      }
      break;
    case WM_SIZE: {
        RECT r;
        ::GetClientRect(hWnd, &r);
        r.left += 10;
        r.top += 10;
        r.right -= 10;
        r.bottom -= 10;
        ::MoveWindow(gErrorTxt, r.left, r.top, r.right-r.left, r.bottom-r.top, true);
      }
      break;
    case WM_CLOSE:
      ::DestroyWindow(hWnd);
      break;
    case WM_DESTROY:
      gErrorTxt = NULL;
      gErrorWnd = NULL;
      break;
  }
  return ::DefWindowProc(hWnd, msg, wParam, lParam);
}

void RegisterErrorClass()
{
  if (gErrorClass != NULL)
    return;
  WNDCLASS wc;
  memset(&wc, 0, sizeof(wc));
  wc.style = 0;
  wc.lpszClassName = "kW X-port Error Window Class";
  wc.lpfnWndProc = &ErrorWndProc;
  wc.hInstance = hInstance;
  wc.hCursor = ::LoadCursor(0, MAKEINTRESOURCE(IDC_ARROW));
  wc.hbrBackground = ::GetSysColorBrush(COLOR_3DFACE);
  wc.hIcon = ::LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
  gErrorClass = ::RegisterClass(&wc);
}

void CreateErrorWnd()
{
  if (gErrorWnd != NULL)
    return;
  RegisterErrorClass();
  RECT drect;
  ::GetWindowRect(::GetDesktopWindow(), &drect);
  gErrorWnd = ::CreateWindowEx(
      WS_EX_OVERLAPPEDWINDOW | WS_EX_APPWINDOW,
      (LPCSTR)gErrorClass,
      "kW X-port Errors",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
      int((drect.right-drect.left-800)*0.9), int((drect.bottom-drect.top-300)*0.1), 800, 300,
      0,
      0,
      hInstance,
      NULL);
}

void AddErrorText(char const *str)
{
  if (gSuppressPrompts)
    return;
  CreateErrorWnd();
  LRESULT len = ::SendMessage(gErrorTxt, WM_GETTEXTLENGTH, 0, 0);
  ::SendMessage(gErrorTxt, EM_SETSEL, len, len);
  std::string foo(str);
  std::string::size_type off = 0;
  while ((off = foo.find("\n", off)) != std::string::npos) {
    foo.replace(off, 1, "\r\n");
    off += 2;
  }
  ::SendMessage(gErrorTxt, EM_REPLACESEL, 0, (LPARAM)foo.c_str());
}


void LogBegin(char const *name, bool suppressPrompts)
{
  char path[1024];
  ::LogEnd();

  gSuppressPrompts = suppressPrompts;

  strncpy(path, name, 1024);
  path[1015] = 0;
  strcat(path, ".log");
  curLogFile = fopen(path, "wb");

  LARGE_INTEGER ticks;
  QueryPerformanceFrequency(&ticks);
  conversion_ = 1.0 / (double)ticks.QuadPart;
  QueryPerformanceCounter(&startTime_);

  LogPrint("Output file name: %s\n", name);
}

void LogEnd()
{
  if (curLogFile != NULL)
  {
    fclose(curLogFile);
    curLogFile = NULL;
  }
}

void LogPrint(char const *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  if (curLogFile != NULL)
  {
    fprintf(curLogFile, "%s", Timestamp());
    vfprintf(curLogFile, fmt, args);
  }
  else
  {
    char buf[2148] = "No log file: ";
    vsnprintf(buf + strlen(buf), 2048, fmt, args);
    buf[2047] = 0;
    ::OutputDebugStringA(buf);
  }
}

void LogError(char const *fmt, ...)
{
  char buf[2148];
  va_list args;
  va_start(args, fmt);
  if (curLogFile != NULL) {
    fprintf(curLogFile, "%s", Timestamp());
    vfprintf(curLogFile, fmt, args);
  }
  else {
    strcpy(buf, "No log file: ");
    vsnprintf(buf + strlen(buf), 2048, fmt, args);
    buf[2147] = 0;
    ::OutputDebugStringA(buf);
  }

  //  add the message to the window
  vsnprintf(buf, 2048, fmt, args);
  buf[2147] = 0;
  AddErrorText(buf);
  va_end(args);
}
