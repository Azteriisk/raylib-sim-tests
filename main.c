#include "raylib.h"
#include <math.h>
#include <stdlib.h> // For malloc, free
#include <string.h> // For memset

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
#define START_WIDTH 800
#define START_HEIGHT 450

// Physics Simulation Constants
#define GRAVITY_SPEED 1
#define PHYSICS_TICK_RATE 60.0f
#define INPUT_POLL_RATE                                                        \
  0.03f // Configurable limit to ink "Flow Rate" (in seconds)

// Dynamic variables updated each frame defining the currently visible window
// chunk
int gridCols = (START_WIDTH / TILE_SIZE);
int gridRows = (START_HEIGHT / TILE_SIZE);

// Tracking the actual RAM capacity dynamically requested
int allocatedCols = 0;
int allocatedRows = 0;

typedef enum {
  EMPTY = 0,
  SAND,         // Demonstrates physics/spatial rules
  WATER,        // Demonstrates liquid-like spreading
  SHADER_BLOCK, // Demonstrates time/color rules
  WALL          // Static indestructible environment
} CellType;

// Define universal attributes that can be enabled specifically per cell type
typedef struct {
  bool isSolid;
  bool affectedByGravity;
  bool spreadsLikeLiquid;
} BlockProperties;

// Global Registry permanently attaching behaviors directly to specific blocks!
const BlockProperties BlockRegistry[] = {
    [EMPTY]        = { .isSolid = false, .affectedByGravity = false, .spreadsLikeLiquid = false },
    [SAND]         = { .isSolid = true,  .affectedByGravity = true,  .spreadsLikeLiquid = false },
    [WATER]        = { .isSolid = true,  .affectedByGravity = true,  .spreadsLikeLiquid = true  },
    [SHADER_BLOCK] = { .isSolid = false, .affectedByGravity = false, .spreadsLikeLiquid = false },
    [WALL]         = { .isSolid = true,  .affectedByGravity = false, .spreadsLikeLiquid = false }
};

typedef struct {
  CellType type;
  Color color;
  float timeAlive;
} PixelCell;

// Heap Memory Pointers explicitly sized at run-time
PixelCell *currentGrid = NULL;
PixelCell *nextGrid = NULL;

// Helper Macros for math flattening
#define GET_CELL(grid, x, y) (grid[(y) * allocatedCols + (x)])
#define SET_CELL(grid, x, y, cType, cColor, cTime)                             \
  (grid[(y) * allocatedCols + (x)] = (PixelCell){cType, cColor, cTime})

bool IsSolid(CellType type);

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

// Memory Allocation logic
void ResizeGrid(int targetCols, int targetRows) {
  if (targetCols <= allocatedCols && targetRows <= allocatedRows)
    return;

  // We only grow the buffer. If window shrinks, we just process less of it to
  // save CPU cycles mapping copies
  int newCols = allocatedCols > targetCols ? allocatedCols : targetCols;
  int newRows = allocatedRows > targetRows ? allocatedRows : targetRows;

  PixelCell *newCurrent =
      (PixelCell *)calloc(newCols * newRows, sizeof(PixelCell));
  PixelCell *newNext =
      (PixelCell *)calloc(newCols * newRows, sizeof(PixelCell));

  // Provide a completely clean slate (calloc natively zeroes everything to
  // EMPTY!)
  for (int x = 0; x < newCols; x++) {
    newCurrent[x] = (PixelCell){WALL, GetCellColor(WALL), 0};
    newNext[x] = (PixelCell){WALL, GetCellColor(WALL), 0};
  }

  // Remap any existing falling physics data into the new grid boundary sizes
  if (currentGrid != NULL) {
    for (int y = 0; y < allocatedRows; y++) {
      for (int x = 0; x < allocatedCols; x++) {
        newCurrent[y * newCols + x] = currentGrid[y * allocatedCols + x];
        newNext[y * newCols + x] = nextGrid[y * allocatedCols + x];
      }
    }
    free(currentGrid);
    free(nextGrid);
  }

  currentGrid = newCurrent;
  nextGrid = newNext;
  allocatedCols = newCols;
  allocatedRows = newRows;
}

// Wipes everything
void InitGrid(void) {
  memset(currentGrid, 0, allocatedCols * allocatedRows * sizeof(PixelCell));
  memset(nextGrid, 0, allocatedCols * allocatedRows * sizeof(PixelCell));

  for (int x = 0; x < allocatedCols; x++) {
    SET_CELL(currentGrid, x, 0, WALL, GetCellColor(WALL), 0);
    SET_CELL(nextGrid, x, 0, WALL, GetCellColor(WALL), 0);
  }
}

// Reset nextGrid active area to EMPTY using hardware-accelerated memory wiping
void ClearNextGrid(void) {
  for (int y = 0; y < gridRows; y++) {
    memset(&nextGrid[y * allocatedCols], 0, gridCols * sizeof(PixelCell));
  }
}

// Swaps the active Arrays instantly without using CPU cycles to copy data
void SwapGrids(void) {
  PixelCell *temp = currentGrid;
  currentGrid = nextGrid;
  nextGrid = temp;
}

// -------------------------------------------------------------
// GRID INSPECTION & SPATIAL HELPERS
// -------------------------------------------------------------

// Safely look at any cell, returning an impenetrable WALL if you try to look
// off-screen
PixelCell InspectCell(int x, int y) {
  if (x < 0 || x >= gridCols || y < 0 || y >= gridRows) {
    return (PixelCell){WALL, GetCellColor(WALL), 0};
  }
  return GET_CELL(currentGrid, x, y);
}

// Safely check the future grid to prevent two pixels from jumping into the same
// spot
PixelCell InspectFutureCell(int x, int y) {
  if (x < 0 || x >= gridCols || y < 0 || y >= gridRows) {
    return (PixelCell){WALL, GetCellColor(WALL), 0};
  }
  return GET_CELL(nextGrid, x, y);
}

typedef enum {
  MOVE_EMPTY = 0,
  MOVE_NON_SOLID,
  MOVE_SOLID
} MoveCellState;

// Resolve movement occupancy using the future grid first to avoid two moving
// cells targeting the same empty space.
MoveCellState GetMoveCellState(int x, int y) {
  PixelCell futureCell = InspectFutureCell(x, y);
  if (futureCell.type != EMPTY) {
    return IsSolid(futureCell.type) ? MOVE_SOLID : MOVE_NON_SOLID;
  }

  PixelCell currentCell = InspectCell(x, y);
  if (currentCell.type == EMPTY) {
    return MOVE_EMPTY;
  }

  return IsSolid(currentCell.type) ? MOVE_SOLID : MOVE_NON_SOLID;
}

// -------------------------------------------------------------
// BEHAVIOR MODULES
// -------------------------------------------------------------

// Queries the Global Registry for a block type's solidity.
bool IsSolid(CellType type) {
  return BlockRegistry[type].isSolid;
}

// Checks left and right randomly at a specified Y level to see if a cell can slide there
bool TryScatter(int x, int targetY, int *nextX, int *nextY) {
  bool goLeftFirst = (GetRandomValue(0, 1) == 0);
  int firstDir = goLeftFirst ? -1 : 1;
  int secondDir = goLeftFirst ? 1 : -1;

  if (GetMoveCellState(x + firstDir, targetY) == MOVE_EMPTY) {
    *nextX = x + firstDir;
    *nextY = targetY;
    return true;
  }
  if (GetMoveCellState(x + secondDir, targetY) == MOVE_EMPTY) {
    *nextX = x + secondDir;
    *nextY = targetY;
    return true;
  }
  return false;
}

// Gravity: Falls down, or cascades diagonally if blocked
bool ApplyGravity(int x, int y, int *nextX, int *nextY) {
  int openCellsSeen = 0;

  // Skip through non-colliding occupied cells and only stop on truly empty
  // landing spaces.
  for (int scanY = y - 1; scanY >= 0; scanY--) {
    MoveCellState state = GetMoveCellState(x, scanY);
    if (state == MOVE_SOLID) {
      break;
    }

    if (state == MOVE_EMPTY) {
      openCellsSeen++;
      *nextY = scanY;

      if (openCellsSeen >= GRAVITY_SPEED) {
        return true;
      }
    }
  }

  if (openCellsSeen > 0) {
    return true;
  }

  // If we can't fall straight down, attempt to slide diagonally down!
  return TryScatter(x, y - 1, nextX, nextY);
}



// Visual Oscillations: Pulses colors dynamically over time
Color GetShaderOscillatedColor(int x, int y, float timeAlive) {
  float r = sinf(timeAlive * 3.0f + x * 0.1f) * 127.0f + 128.0f;
  float g = sinf(timeAlive * 2.0f + y * 0.1f) * 127.0f + 128.0f;
  float b = sinf(timeAlive * 4.0f) * 127.0f + 128.0f;
  return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
}

// -------------------------------------------------------------
// CORE PHYSICS SYSTEM
// -------------------------------------------------------------

// Core rule mechanics
void ApplyPixelRules(int x, int y, float deltaTime, PixelCell cell) {
  cell.timeAlive += deltaTime;
  int nextX = x;
  int nextY = y;

  const BlockProperties* props = &BlockRegistry[cell.type];

  // 1. Data-Driven Spatial Physics 
  if (props->affectedByGravity) {
    if (!ApplyGravity(x, y, &nextX, &nextY)) {
      if (props->spreadsLikeLiquid) {
        TryScatter(x, y, &nextX, &nextY);
      }
    }
  }

  // 2. Push final evaluated state to the next active buffer
  GET_CELL(nextGrid, nextX, nextY) = cell;
}

// Triggers double-buffering and runs physics sweep across the active grid
// bounds
void ProcessPhysics(float deltaTime) {
  ClearNextGrid();
  for (int y = 0; y < gridRows; y++) {
    for (int x = 0; x < gridCols; x++) {
      PixelCell cell = GET_CELL(currentGrid, x, y);
      if (cell.type != EMPTY) {
        ApplyPixelRules(x, y, deltaTime, cell);
      }
    }
  }
  SwapGrids();
}

// Submits all graphics drawing calls to the GPU natively
void RenderWorld(void) {
  BeginDrawing();
  ClearBackground(RAYWHITE);

  for (int y = 0; y < gridRows; y++) {
    for (int x = 0; x < gridCols; x++) {
      PixelCell cell = GET_CELL(currentGrid, x, y);
      if (cell.type != EMPTY) {
        Color drawColor = cell.color;
        if (cell.type == SHADER_BLOCK) {
          drawColor = GetShaderOscillatedColor(x, y, cell.timeAlive);
        }
        int screenY = GetScreenHeight() - (y + 1) * TILE_SIZE;
        DrawRectangle(x * TILE_SIZE, screenY, TILE_SIZE, TILE_SIZE, drawColor);
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
// INPUT & INTERACTION HELPERS
// -------------------------------------------------------------

typedef enum {
  INPUT_BIND_MOUSE_BUTTON = 0,
  INPUT_BIND_KEY
} InputBindingType;

typedef struct {
  InputBindingType triggerType;
  int triggerCode;
  CellType resultCellType;
} InputBinding;

// Input priority is defined by array order.
const InputBinding kInputBindings[] = {
    {INPUT_BIND_MOUSE_BUTTON, MOUSE_BUTTON_LEFT, SAND},
    {INPUT_BIND_MOUSE_BUTTON, MOUSE_BUTTON_RIGHT, SHADER_BLOCK},
    {INPUT_BIND_KEY, KEY_SPACE, WATER},
};

bool IsBindingActive(const InputBinding *binding) {
  switch (binding->triggerType) {
  case INPUT_BIND_MOUSE_BUTTON:
    return IsMouseButtonDown(binding->triggerCode);
  case INPUT_BIND_KEY:
    return IsKeyDown(binding->triggerCode);
  default:
    return false;
  }
}

const InputBinding *GetActiveInputBinding(void) {
  int count = sizeof(kInputBindings) / sizeof(kInputBindings[0]);
  for (int i = 0; i < count; i++) {
    if (IsBindingActive(&kInputBindings[i])) {
      return &kInputBindings[i];
    }
  }
  return NULL;
}

// Place gravity-affected materials without overwriting non-solid occupied cells
// (for example shader blocks): insert them at the first reachable empty space
// below.
void PlaceDrawnCell(int x, int y, CellType type) {
  PixelCell existing = GET_CELL(currentGrid, x, y);

  // Default painter behavior: draw directly into empty cells, solid cells, or
  // non-gravity types.
  if (existing.type == EMPTY || IsSolid(existing.type) ||
      !BlockRegistry[type].affectedByGravity) {
    SET_CELL(currentGrid, x, y, type, GetCellColor(type), 0);
    return;
  }

  // Gravity materials passing through non-solid occupied cells should settle
  // in the first empty slot below without erasing the non-solid cell.
  for (int scanY = y - 1; scanY >= 0; scanY--) {
    PixelCell scanned = GET_CELL(currentGrid, x, scanY);
    if (IsSolid(scanned.type)) {
      break;
    }
    if (scanned.type == EMPTY) {
      SET_CELL(currentGrid, x, scanY, type, GetCellColor(type), 0);
      return;
    }
  }
}

// Integer Bresenham algorithm directly mapped to integer Matrix logic
// Guarantees a perfectly uniform 1-cell thick stroke with zero floating point
// gaps!
void SpawnLineBetweenGrids(int x0, int y0, int x1, int y1, CellType type) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;

  for (;;) {
    // Enforce array boundary safety
    if (x0 >= 0 && x0 < gridCols && y0 >= 0 && y0 < gridRows) {
      PlaceDrawnCell(x0, y0, type);
    }

    if (x0 == x1 && y0 == y1)
      break;
    e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

// Polls active human drawing stroke intent against the Matrix
void HandleUserInputs(void) {
  int mx = GetMouseX();
  int my = GetMouseY();

  // Snap raw floating hardware coordinates into our pure integer Array structure
  int gridX = mx / TILE_SIZE;
  int gridY = (GetScreenHeight() - my) / TILE_SIZE;

  static int lastGridX = -1;
  static int lastGridY = -1;

  const InputBinding *activeBinding = GetActiveInputBinding();

  // Track the hardware natively while idling so that a fresh click always
  // originates identically at the cursor.
  if (activeBinding == NULL) {
    lastGridX = gridX;
    lastGridY = gridY;
  }

  // Natively trace an ultra smooth, gapless line strictly through our Array
  // space.
  if (activeBinding != NULL) {
    SpawnLineBetweenGrids(lastGridX, lastGridY, gridX, gridY,
                          activeBinding->resultCellType);
  }

  lastGridX = gridX;
  lastGridY = gridY;
}

// Safely ticks hardware inputs and cellular logic exactly at the Physics Tick
// Rate
void UpdateSimulation(void) {
  float deltaTime = GetFrameTime();
  if (deltaTime > 0.1f)
    deltaTime = 0.016f; // Cap physics freeze spikes

  static float physicsAccumulator = 0.0f;
  static float inputAccumulator = 0.0f;

  float tickRate = 1.0f / PHYSICS_TICK_RATE;

  physicsAccumulator += deltaTime;
  inputAccumulator += deltaTime;

  while (physicsAccumulator >= tickRate) {
    // Only query inputs and drop ink if enough time has passed based on the
    // customizable rate!
    if (inputAccumulator >= INPUT_POLL_RATE) {
      HandleUserInputs();
      inputAccumulator = 0.0f; // Flush to 0 to prevent blocky chunking!
    }

    ProcessPhysics(tickRate);
    physicsAccumulator -= tickRate;
  }
}

// -------------------------------------------------------------
// CORE GAME LOOP ABSTRACTION
// -------------------------------------------------------------
void ExecuteFrame(void) {
  // 1. Unpolled hardware actions
  if (IsKeyPressed(KEY_C)) {
    InitGrid();
  }

  // 2. Maintain memory bounds
  gridCols = GetScreenWidth() / TILE_SIZE;
  gridRows = GetScreenHeight() / TILE_SIZE;
  ResizeGrid(gridCols, gridRows);

  // 3. Time-Independent Engine Core
  UpdateSimulation();

  // 4. Graphics Context Hook
  RenderWorld();
}

// -------------------------------------------------------------
// WINDOWS SPECIFIC RESIZE HOOK
// -------------------------------------------------------------
#if defined(_WIN32)
LRESULT CALLBACK LiveResizeWndProc(HWND hwnd, UINT msg, WPARAM wParam,
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

// -------------------------------------------------------------
// PROGRAM ENTRY POINT
// -------------------------------------------------------------
int main(void) {
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_ALWAYS_RUN);
  InitWindow(START_WIDTH, START_HEIGHT, "Cellular Automata Engine");

  // Sync the game engine directly to the host's actual hardware refresh rate!
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

  CloseWindow();

  // Clean up operating system memory requests just to be professional
  if (currentGrid != NULL)
    free(currentGrid);
  if (nextGrid != NULL)
    free(nextGrid);

  return 0;
}
