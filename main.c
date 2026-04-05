#include "sim.h"

#include <stdlib.h>

#if defined(_WIN32)
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

#if defined(_WIN32)
typedef void *HWND;
typedef unsigned int UINT;
typedef unsigned long long WPARAM;
typedef long long LPARAM;
typedef long long LRESULT;
typedef long long LONG_PTR;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

#define GWLP_WNDPROC -4
#define WM_SIZE 0x0005
#define WM_PAINT 0x000F
#define WM_SIZING 0x0214
#define CALLBACK __stdcall

LONG_PTR SetWindowLongPtrA(HWND hWnd, int nIndex, LONG_PTR dwNewLong);
LRESULT CallWindowProcA(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg,
                        WPARAM wParam, LPARAM lParam);

static WNDPROC oldWndProc = 0;
#endif

#if defined(_WIN32)
static LRESULT CALLBACK LiveResizeWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
  LRESULT result = CallWindowProcA(oldWndProc, hwnd, msg, wParam, lParam);
  if (msg == WM_SIZING || msg == WM_SIZE || msg == WM_PAINT) {
    if (IsWindowReady()) {
      ExecuteFrame();
    }
  }
  return result;
}
#endif

int main(void) {
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_ALWAYS_RUN);
  InitWindow(START_WIDTH, START_HEIGHT, "Cellular Automata Engine");

  int monitor = GetCurrentMonitor();
  SetTargetFPS(GetMonitorRefreshRate(monitor));

#if defined(_WIN32)
  HWND hwnd = (HWND)GetWindowHandle();
  if (hwnd) {
    oldWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                                            (LONG_PTR)LiveResizeWndProc);
  }
#endif

  while (!WindowShouldClose()) {
    ExecuteFrame();
  }

  ShutdownRenderer();
  CloseWindow();

  if (currentGrid != NULL) {
    free(currentGrid);
  }
  if (nextGrid != NULL) {
    free(nextGrid);
  }

  return 0;
}
