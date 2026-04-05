#include "raylib.h"
#include <math.h>

// -------------------------------------------------------------
// Manual Win32 API Definitions (Bypasses <windows.h> conflicts)
// -------------------------------------------------------------
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

WNDPROC oldWndProc = 0;
#endif

// Define the standard 'pixel' / cell size here.
#define TILE_SIZE 6

// Grid dimensions max limits (supports up to a 4k monitor natively)
#define MAX_COLS (3840 / TILE_SIZE)
#define MAX_ROWS (2160 / TILE_SIZE)

// Dynamic variables updated each frame
int gridCols = (800 / TILE_SIZE);
int gridRows = (450 / TILE_SIZE);

typedef enum {
  EMPTY = 0,
  SAND,         // Demonstrates physics/spatial rules
  WATER,        // Demonstrates liquid-like spreading
  SHADER_BLOCK, // Demonstrates time/color rules
  WALL          // Static indestructible environment
} CellType;

typedef struct {
  CellType type;
  Color color;
  float timeAlive;
} PixelCell;

PixelCell currentGrid[MAX_COLS][MAX_ROWS];
PixelCell nextGrid[MAX_COLS][MAX_ROWS];

// Helper function to draw a cell (Anchored to the Bottom Left)
void DrawGridPixel(int gridX, int gridY, Color color) {
  // We flip the Y-axis conceptually:
  // gridY = 0 is the physical bottom of your game screen window!
  int screenY = GetScreenHeight() - (gridY + 1) * tileSize;
  DrawRectangle(gridX * tileSize, screenY, tileSize, tileSize, color);
}

// Generate base colors for specific types
Color GetCellColor(CellType type) {
  switch (type) {
  case SAND:
    return (Color){242, 209, 107, 255};
  case WATER:
    return (Color){73, 166, 219, 255};
  case SHADER_BLOCK:
    return PURPLE;
  case WALL:
    return DARKGRAY;
  default:
    return BLANK;
  }
}

// Initialize everything to empty, and draw the starting ground floor
void InitGrid(void) {
  for (int x = 0; x < MAX_COLS; x++) {
    for (int y = 0; y < MAX_ROWS; y++) {
      if (y == 0) {
        // The floor is perpetually fixed at Array index 0!
        // This is always drawn at the bottom pixel row of the window!
        currentGrid[x][y] = (PixelCell){WALL, GetCellColor(WALL), 0};
        nextGrid[x][y] = (PixelCell){WALL, GetCellColor(WALL), 0};
      } else {
        currentGrid[x][y] = (PixelCell){EMPTY, BLANK, 0};
        nextGrid[x][y] = (PixelCell){EMPTY, BLANK, 0};
      }
    }
  }
}

// Reset nextGrid active area to EMPTY (used at the start of a logical tick)
void ClearNextGrid(void) {
  for (int x = 0; x < gridCols; x++) {
    for (int y = 0; y < gridRows; y++) {
      nextGrid[x][y] = (PixelCell){EMPTY, BLANK, 0};
    }
  }
}

// Copies nextGrid back into currentGrid
void SwapGrids(void) {
  for (int x = 0; x < gridCols; x++) {
    for (int y = 0; y < gridRows; y++) {
      currentGrid[x][y] = nextGrid[x][y];
    }
  }
}

// The core rules function (Physics + Time shaders)
void ApplyPixelRules(int x, int y, float deltaTime) {
  PixelCell cell = currentGrid[x][y];

  if (cell.type == EMPTY)
    return; // Don't process empty space

  cell.timeAlive += deltaTime;
  int nextX = x;
  int nextY = y;

  if (cell.type == SAND) {
    // Because 0 is the floor, falls travel into the negative direction (y - 1)
    if (y - 1 >= 0 && currentGrid[x][y - 1].type == EMPTY &&
        nextGrid[x][y - 1].type == EMPTY) {
      nextY = y - 1; // Fall straight down
    } else if (y - 1 >= 0 && x - 1 >= 0 &&
               currentGrid[x - 1][y - 1].type == EMPTY &&
               nextGrid[x - 1][y - 1].type == EMPTY) {
      nextX = x - 1; // Slide down-left
      nextY = y - 1;
    } else if (y - 1 >= 0 && x + 1 < gridCols &&
               currentGrid[x + 1][y - 1].type == EMPTY &&
               nextGrid[x + 1][y - 1].type == EMPTY) {
      nextX = x + 1; // Slide down-right
      nextY = y - 1;
    }
    nextGrid[nextX][nextY] = cell;

  } else if (cell.type == WATER) {
    if (y - 1 >= 0 && currentGrid[x][y - 1].type == EMPTY &&
        nextGrid[x][y - 1].type == EMPTY) {
      nextY = y - 1; // Fall straight down
    } else if (y - 1 >= 0 && x - 1 >= 0 &&
               currentGrid[x - 1][y - 1].type == EMPTY &&
               nextGrid[x - 1][y - 1].type == EMPTY) {
      nextX = x - 1; // Slide down-left
      nextY = y - 1;
    } else if (y - 1 >= 0 && x + 1 < gridCols &&
               currentGrid[x + 1][y - 1].type == EMPTY &&
               nextGrid[x + 1][y - 1].type == EMPTY) {
      nextX = x + 1; // Slide down-right
      nextY = y - 1;
    } else if (x - 1 >= 0 && currentGrid[x - 1][y].type == EMPTY &&
               nextGrid[x - 1][y].type == EMPTY) {
      nextX = x - 1; // Spread left
    } else if (x + 1 < gridCols && currentGrid[x + 1][y].type == EMPTY &&
               nextGrid[x + 1][y].type == EMPTY) {
      nextX = x + 1; // Spread right
    }
    nextGrid[nextX][nextY] = cell;

  } else if (cell.type == SHADER_BLOCK) {
    float r = sinf(cell.timeAlive * 3.0f + x * 0.1f) * 127.0f + 128.0f;
    float g = sinf(cell.timeAlive * 2.0f + y * 0.1f) * 127.0f + 128.0f;
    float b = sinf(cell.timeAlive * 4.0f) * 127.0f + 128.0f;
    cell.color =
        (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
    nextGrid[nextX][nextY] = cell;
  } else if (cell.type == WALL) {
    // Walls don't move
    nextGrid[nextX][nextY] = cell;
  }
}

// -------------------------------------------------------------
// CORE GAME LOOP ABSTRACTION
// -------------------------------------------------------------
void ExecuteFrame(void) {
  // Dynamically update bounds
  gridCols = GetScreenWidth() / tileSize;
  gridRows = GetScreenHeight() / tileSize;

  // Bounds checking array allocations
  if (gridCols > MAX_COLS)
    gridCols = MAX_COLS;
  if (gridRows > MAX_ROWS)
    gridRows = MAX_ROWS;

  // Use a fixed deltaTime if resizing causes massive frame skips,
  // otherwise the shader blocks wildly strobe during scale operations
  float deltaTime = GetFrameTime();
  if (deltaTime > 0.1f)
    deltaTime = 0.016f; // Cap physics freeze spikes

  // 1. Handle Input (Mouse Drawing)
  if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
    int gridX = GetMouseX() / tileSize;
    int gridY = (GetScreenHeight() - GetMouseY()) /
                tileSize; // Flip vertical mouse position
    if (gridX >= 0 && gridX < gridCols && gridY >= 0 && gridY < gridRows) {
      currentGrid[gridX][gridY] = (PixelCell){SAND, GetCellColor(SAND), 0};
    }
  } else if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
    int gridX = GetMouseX() / tileSize;
    int gridY = (GetScreenHeight() - GetMouseY()) / tileSize; // Flip vertical
    if (gridX >= 0 && gridX < gridCols && gridY >= 0 && gridY < gridRows) {
      currentGrid[gridX][gridY] =
          (PixelCell){SHADER_BLOCK, GetCellColor(SHADER_BLOCK), 0};
    }
  }

  if (IsKeyDown(KEY_SPACE)) {
    int gridX = GetMouseX() / tileSize;
    int gridY = (GetScreenHeight() - GetMouseY()) / tileSize; // Flip vertical
    if (gridX >= 0 && gridX < gridCols && gridY >= 0 && gridY < gridRows) {
      currentGrid[gridX][gridY] = (PixelCell){WATER, GetCellColor(WATER), 0};
    }
  }

  if (IsKeyPressed(KEY_C)) {
    InitGrid();
  }

  // 2. Rules Processing / Physics Simulation
  ClearNextGrid();

  // Iterate upwards since y = 0 is now the physical floor!
  for (int y = 0; y < gridRows; y++) {
    for (int x = 0; x < gridCols; x++) {
      ApplyPixelRules(x, y, deltaTime);
    }
  }

  SwapGrids();

  // 3. Render Pass
  BeginDrawing();
  ClearBackground(RAYWHITE);

  for (int x = 0; x < gridCols; x++) {
    for (int y = 0; y < gridRows; y++) {
      if (currentGrid[x][y].type != EMPTY) {
        DrawGridPixel(x, y, currentGrid[x][y].color);
      }
    }
  }

  DrawText("Left Click: Sand  |  Right Click: Shader Block  |  Space: Water  | "
           " C: Clear",
           10, 10, 20, DARKGRAY);
  DrawFPS(GetScreenWidth() - 100, 30);
  EndDrawing();
}

// -------------------------------------------------------------
// WINDOWS SPECIFIC RESIZE HOOK
// -------------------------------------------------------------
#if defined(_WIN32)

LRESULT CALLBACK LiveResizeWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam) {
  // Hand it down to GLFW first, so Raylib internals update their viewport /
  // bounds correctly
  LRESULT result = CallWindowProcA(oldWndProc, hwnd, msg, wParam, lParam);

  // Immediately after GLFW finishes sizing logic, but BEFORE Windows ends the
  // blocking message, we hijack the thread and render exactly one frame of our
  // game!
  if (msg == WM_SIZING || msg == WM_SIZE || msg == WM_PAINT) {
    if (IsWindowReady()) { // Ensure OpenGL context is healthy
      ExecuteFrame();
    }
  }

  return result;
}
#endif

// -------------------------------------------------------------
// PROGRAM ENTRY POINT
// -------------------------------------------------------------
int main(void) {
  const int screenWidth = 800;
  const int screenHeight = 450;

  // Enable resizable window and make sure things run while
  // backgrounded/resizing
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_ALWAYS_RUN);

  InitWindow(screenWidth, screenHeight, "Cellular Automata Engine");
  SetTargetFPS(60);

  InitGrid();

#if defined(_WIN32)
  // Intercept native Windows message pump
  HWND hwnd = (HWND)GetWindowHandle();
  if (hwnd) {
    // SetWindowLongPtr subclassing
    oldWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
                                            (LONG_PTR)LiveResizeWndProc);
  }
#endif

  while (!WindowShouldClose()) {
    ExecuteFrame();
  }

  CloseWindow();
  return 0;
}
