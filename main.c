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
int cellSizePixels = 2;
#define START_WIDTH 800
#define START_HEIGHT 450

// Drawing brush radius in on-screen pixels. It is converted to simulation-cell
// radius using cellSizePixels, so brush feel stays consistent if cell scale
// changes.
int drawBrushRadiusPixels = 6;

// Gravity speed in on-screen pixels per physics step. It is converted to
// simulation cells using cellSizePixels.
int gravitySpeedPixelsPerStep = 6;
int bulbStemBulbRatioPercent = 35;

// Physics Simulation Constants
#define PHYSICS_TICK_RATE 60.0f

// Dynamic variables updated each frame defining the currently visible window
// chunk
int gridCols = 0;
int gridRows = 0;

// Tracking the actual RAM capacity dynamically requested
int allocatedCols = 0;
int allocatedRows = 0;

typedef enum {
  EMPTY = 0,
  SAND,         // Demonstrates physics/spatial rules
  WATER,        // Demonstrates liquid-like spreading
  BULB_SEED,  // Falls and contributes to Bulb growth
  BULB_BLOCK, // Grown Bulb structure cells
  SHADER_BLOCK, // Demonstrates time/color rules
  WALL          // Static indestructible environment
} CellType;

// Define universal attributes that can be enabled specifically per cell type
typedef struct {
  bool isSolid;
  bool isImmutable;
  bool affectedByGravity;
  bool spreadsLikeLiquid;
  float mass;              // Relative fall mass: >1 heavier, <1 lighter
  float gravityMoveChance; // 0.0-1.0 chance to attempt gravity each tick
  float spreadMoveChance;  // 0.0-1.0 chance to attempt lateral spread
  int spreadDistanceCells; // Max lateral travel when spreading
} BlockProperties;

// Global Registry permanently attaching behaviors directly to specific blocks!
// Any omitted fields default to zero/false via C aggregate initialization.
const BlockProperties BlockRegistry[] = {
    [EMPTY] = {},
    [SAND] = {.isSolid = true,
              .affectedByGravity = true,
              .mass = 1.35f,
              .gravityMoveChance = 1.0f},
    [WATER] = {.isSolid = true,
               .affectedByGravity = true,
               .spreadsLikeLiquid = true,
               .mass = 0.72f,
               .gravityMoveChance = 0.8f,
               .spreadMoveChance = 0.9f,
               .spreadDistanceCells = 8},
    [BULB_SEED] = {.isSolid = true,
                   .affectedByGravity = true,
                   .mass = 1.0f,
                   .gravityMoveChance = 1.0f},
    [BULB_BLOCK] = {.isSolid = true},
    [SHADER_BLOCK] = {},
    [WALL] = {.isSolid = true, .isImmutable = true}};

typedef struct {
  CellType type;
  Color color;
  float timeAlive;
} PixelCell;

// Heap Memory Pointers explicitly sized at run-time
PixelCell *currentGrid = NULL;
PixelCell *nextGrid = NULL;
int uiActiveControlIndex = -1;
bool uiPointerCaptured = false;
bool bulbNeedsRebuild = false;

#define MAX_BULB_NODES 1024
typedef struct {
  bool active;
  int anchorX;
  int anchorY;
  int mass;
} BulbNode;
BulbNode bulbNodes[MAX_BULB_NODES];

// Helper Macros for math flattening
#define GET_CELL(grid, x, y) (grid[(y) * allocatedCols + (x)])
#define SET_CELL(grid, x, y, cType, cColor, cTime)                             \
  (grid[(y) * allocatedCols + (x)] = (PixelCell){cType, cColor, cTime})

bool IsSolid(CellType type);
bool IsImmutable(CellType type);
bool RollChance(float chance);
Color GetBulbShaderColor(int x, int y, float timeAlive);
CellType GetResolvedCellType(int x, int y);
bool TryAbsorbBulbSeed(int x, int y);
void ClearAllBulbs(void);
void RebuildBulbs(void);

int GetGravitySpeedInCells(void);
int GetGravitySpeedInCellsForMass(float mass);
int ClampInt(int value, int minValue, int maxValue);
int GetUIControlSettingsCount(void);
Rectangle GetUIControlsPanelRect(int settingCount);
void UpdateUIControls(void);
void RenderUIControls(void);

// Generate base colors for specific types
Color GetCellColor(CellType type) {
  switch (type) {
  case SAND:
    return (Color){242, 209, 107, 255};
  case WATER:
    return (Color){73, 166, 219, 255};
  case BULB_SEED:
    return (Color){220, 138, 186, 255};
  case BULB_BLOCK:
    return (Color){199, 83, 163, 255};
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
  ClearAllBulbs();

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

typedef enum { MOVE_EMPTY = 0, MOVE_NON_SOLID, MOVE_SOLID } MoveCellState;

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

// For rows that have already been processed this tick (below the current
// particle), nextGrid is the authoritative occupancy state.
MoveCellState GetProcessedRowMoveCellState(int x, int y) {
  PixelCell futureCell = InspectFutureCell(x, y);
  if (futureCell.type == EMPTY) {
    return MOVE_EMPTY;
  }
  return IsSolid(futureCell.type) ? MOVE_SOLID : MOVE_NON_SOLID;
}

CellType GetResolvedCellType(int x, int y) {
  PixelCell futureCell = InspectFutureCell(x, y);
  if (futureCell.type != EMPTY) {
    return futureCell.type;
  }
  return InspectCell(x, y).type;
}

void ClearAllBulbs(void) {
  memset(bulbNodes, 0, sizeof(bulbNodes));
  bulbNeedsRebuild = false;
}

int FindNearestBulbNodeIndex(int x, int y, int maxDistSq) {
  int bestIndex = -1;
  int bestDistSq = maxDistSq;
  for (int i = 0; i < MAX_BULB_NODES; i++) {
    if (!bulbNodes[i].active) {
      continue;
    }
    int dx = bulbNodes[i].anchorX - x;
    int dy = bulbNodes[i].anchorY - y;
    int distSq = dx * dx + dy * dy;
    if (distSq <= bestDistSq) {
      bestDistSq = distSq;
      bestIndex = i;
    }
  }
  return bestIndex;
}

void StampBulbCell(PixelCell *grid, int x, int y) {
  if (x < 0 || x >= gridCols || y < 0 || y >= gridRows) {
    return;
  }
  PixelCell existing = GET_CELL(grid, x, y);
  if (IsImmutable(existing.type)) {
    return;
  }
  SET_CELL(grid, x, y, BULB_BLOCK, GetCellColor(BULB_BLOCK), 0);
}

void StampBulbShape(PixelCell *grid, const BulbNode *node) {
  int mass = node->mass < 1 ? 1 : node->mass;
  float stemRatio =
      (float)ClampInt(bulbStemBulbRatioPercent, 10, 90) / 100.0f;
  float stemBudget = (float)mass * stemRatio;
  float bulbBudget = (float)mass * (1.0f - stemRatio);
  int stemHeight = 1 + (int)floorf(sqrtf(stemBudget) * 0.4f);
  int stemHalfWidth = (int)floorf(sqrtf(stemBudget) * 0.6f);
  int bodyRadiusX = 2 + (int)floorf(sqrtf(fmaxf(1.0f, bulbBudget)) * 1.1f);
  int bodyRadiusY = 3 + (int)floorf((float)bodyRadiusX * 1.45f);
  int tipRadius = 1 + (int)floorf((float)bodyRadiusX * 0.35f);
  if (tipRadius > bodyRadiusX - 1) {
    tipRadius = bodyRadiusX - 1;
  }
  if (tipRadius < 1) {
    tipRadius = 1;
  }
  if (stemHeight > bodyRadiusY) {
    stemHeight = bodyRadiusY;
  }

  int stemTopY = node->anchorY + stemHeight;
  int stemBottomY = node->anchorY + 1;
  for (int y = stemBottomY; y <= stemTopY; y++) {
    for (int x = node->anchorX - stemHalfWidth; x <= node->anchorX + stemHalfWidth;
         x++) {
      StampBulbCell(grid, x, y);
    }
  }

  int centerX = node->anchorX;
  int centerY = stemTopY + bodyRadiusY - 1;
  for (int dy = -bodyRadiusY; dy <= bodyRadiusY; dy++) {
    for (int dx = -bodyRadiusX; dx <= bodyRadiusX; dx++) {
      float nx = bodyRadiusX > 0 ? (float)dx / (float)bodyRadiusX : 0.0f;
      float ny = bodyRadiusY > 0 ? (float)dy / (float)bodyRadiusY : 0.0f;
      if (nx * nx + ny * ny <= 1.0f) {
        StampBulbCell(grid, centerX + dx, centerY + dy);
      }
    }
  }

  int tipCenterY = centerY + bodyRadiusY - tipRadius;
  for (int dy = -tipRadius; dy <= tipRadius; dy++) {
    for (int dx = -tipRadius; dx <= tipRadius; dx++) {
      if (dx * dx + dy * dy <= tipRadius * tipRadius) {
        StampBulbCell(grid, centerX + dx, tipCenterY + dy);
      }
    }
  }
}

void RebuildBulbs(void) {
  for (int y = 0; y < gridRows; y++) {
    for (int x = 0; x < gridCols; x++) {
      if (GET_CELL(currentGrid, x, y).type == BULB_BLOCK) {
        SET_CELL(currentGrid, x, y, EMPTY, BLANK, 0);
      }
    }
  }

  for (int i = 0; i < MAX_BULB_NODES; i++) {
    if (!bulbNodes[i].active) {
      continue;
    }
    if (bulbNodes[i].anchorX < 0 || bulbNodes[i].anchorX >= gridCols ||
        bulbNodes[i].anchorY < 0 || bulbNodes[i].anchorY >= gridRows) {
      bulbNodes[i].active = false;
      continue;
    }
    StampBulbShape(currentGrid, &bulbNodes[i]);
  }
  bulbNeedsRebuild = false;
}

bool AddBulbMassAt(int anchorX, int anchorY) {
  int nearbyIndex = FindNearestBulbNodeIndex(anchorX, anchorY, 9);
  if (nearbyIndex >= 0) {
    bulbNodes[nearbyIndex].mass++;
    if (bulbNodes[nearbyIndex].mass > 4096) {
      bulbNodes[nearbyIndex].mass = 4096;
    }
    bulbNeedsRebuild = true;
    return true;
  }

  for (int i = 0; i < MAX_BULB_NODES; i++) {
    if (!bulbNodes[i].active) {
      bulbNodes[i] =
          (BulbNode){.active = true, .anchorX = anchorX, .anchorY = anchorY, .mass = 1};
      bulbNeedsRebuild = true;
      return true;
    }
  }
  return false;
}

bool TryAbsorbBulbSeed(int x, int y) {
  bool touchingBulb = false;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      if (GetResolvedCellType(x + dx, y + dy) == BULB_BLOCK) {
        touchingBulb = true;
      }
    }
  }

  if (touchingBulb) {
    int nearest = FindNearestBulbNodeIndex(x, y, 2500);
    if (nearest >= 0) {
      bulbNodes[nearest].mass++;
      if (bulbNodes[nearest].mass > 4096) {
        bulbNodes[nearest].mass = 4096;
      }
      bulbNeedsRebuild = true;
      return true;
    }
  }

  CellType belowType = GetResolvedCellType(x, y - 1);
  if (belowType == BULB_BLOCK) {
    int nearest = FindNearestBulbNodeIndex(x, y, 2500);
    if (nearest >= 0) {
      bulbNodes[nearest].mass++;
      if (bulbNodes[nearest].mass > 4096) {
        bulbNodes[nearest].mass = 4096;
      }
      bulbNeedsRebuild = true;
      return true;
    }
  }

  if (IsSolid(belowType) && belowType != BULB_SEED && belowType != BULB_BLOCK) {
    return AddBulbMassAt(x, y - 1);
  }

  return false;
}

// -------------------------------------------------------------
// BEHAVIOR MODULES
// -------------------------------------------------------------

// Queries the Global Registry for a block type's solidity.
bool IsSolid(CellType type) { return BlockRegistry[type].isSolid; }
bool IsImmutable(CellType type) { return BlockRegistry[type].isImmutable; }

// Checks left and right randomly at a specified Y level to see if a cell can
// slide there. If targetY is below sourceY, that row has already been
// processed this tick and should be read from nextGrid.
bool TryScatter(int x, int sourceY, int targetY, int maxDistance,
                int *nextX, int *nextY) {
  bool goLeftFirst = (GetRandomValue(0, 1) == 0);
  int firstDir = goLeftFirst ? -1 : 1;
  int secondDir = goLeftFirst ? 1 : -1;
  bool targetRowProcessed = (targetY < sourceY);

  int dirs[2] = {firstDir, secondDir};
  for (int i = 0; i < 2; i++) {
    int dir = dirs[i];
    for (int step = 1; step <= maxDistance; step++) {
      int scanX = x + dir * step;
      if (scanX < 0 || scanX >= gridCols) {
        break;
      }
      MoveCellState state =
          targetRowProcessed
              ? GetProcessedRowMoveCellState(scanX, targetY)
              : GetMoveCellState(scanX, targetY);

      if (state == MOVE_SOLID) {
        break;
      }
      if (state == MOVE_EMPTY) {
        *nextX = scanX;
        *nextY = targetY;
        return true;
      }
      // MOVE_NON_SOLID: keep scanning through transparent cells.
    }
  }

  return false;
}

// Gravity: Falls down, or cascades diagonally if blocked
bool ApplyGravity(int x, int y, const BlockProperties *props, int *nextX,
                  int *nextY) {
  if (!RollChance(props->gravityMoveChance)) {
    return false;
  }

  int gravityStepCells = GetGravitySpeedInCellsForMass(props->mass);
  int openCellsSeen = 0;

  // Skip through non-colliding occupied cells and only stop on truly empty
  // landing spaces.
  for (int scanY = y - 1; scanY >= 0; scanY--) {
    MoveCellState state = GetProcessedRowMoveCellState(x, scanY);
    if (state == MOVE_SOLID) {
      break;
    }

    if (state == MOVE_EMPTY) {
      openCellsSeen++;
      *nextY = scanY;

      if (openCellsSeen >= gravityStepCells) {
        return true;
      }
    }
  }

  if (openCellsSeen > 0) {
    return true;
  }

  // If we can't fall straight down, attempt to slide diagonally down!
  return TryScatter(x, y, y - 1, 1, nextX, nextY);
}

// Visual Oscillations: Pulses colors dynamically over time
Color GetShaderOscillatedColor(int x, int y, float timeAlive) {
  float r = sinf(timeAlive * 3.0f + x * 0.1f) * 127.0f + 128.0f;
  float g = sinf(timeAlive * 2.0f + y * 0.1f) * 127.0f + 128.0f;
  float b = sinf(timeAlive * 4.0f) * 127.0f + 128.0f;
  return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
}

Color GetBulbShaderColor(int x, int y, float timeAlive) {
  float pulse = sinf(timeAlive * 2.5f + x * 0.18f - y * 0.11f) * 0.5f + 0.5f;
  unsigned char r = (unsigned char)(36 + pulse * 24.0f);
  unsigned char g = (unsigned char)(130 + pulse * 60.0f);
  unsigned char b = (unsigned char)(72 + pulse * 30.0f);
  return (Color){r, g, b, 255};
}

bool RollChance(float chance) {
  if (chance <= 0.0f) {
    return false;
  }
  if (chance >= 1.0f) {
    return true;
  }
  int threshold = (int)(chance * 1000.0f);
  return GetRandomValue(0, 999) < threshold;
}

// -------------------------------------------------------------
// CORE PHYSICS SYSTEM
// -------------------------------------------------------------

// Core rule mechanics
void ApplyPixelRules(int x, int y, float deltaTime, PixelCell cell) {
  cell.timeAlive += deltaTime;
  int nextX = x;
  int nextY = y;

  const BlockProperties *props = &BlockRegistry[cell.type];

  if (cell.type == BULB_SEED) {
    if (ApplyGravity(x, y, props, &nextX, &nextY)) {
      GET_CELL(nextGrid, nextX, nextY) = cell;
      return;
    }

    if (TryAbsorbBulbSeed(x, y)) {
      return; // Seed is consumed into Bulb growth.
    }

    GET_CELL(nextGrid, x, y) = cell;
    return;
  }

  // 1. Data-Driven Spatial Physics
  if (props->affectedByGravity) {
    if (!ApplyGravity(x, y, props, &nextX, &nextY)) {
      if (props->spreadsLikeLiquid && RollChance(props->spreadMoveChance)) {
        int spreadDistance =
            props->spreadDistanceCells < 1 ? 1 : props->spreadDistanceCells;
        TryScatter(x, y, y, spreadDistance, &nextX, &nextY);
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
  if (bulbNeedsRebuild) {
    RebuildBulbs();
  }
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
        } else if (cell.type == BULB_BLOCK) {
          drawColor = GetBulbShaderColor(x, y, cell.timeAlive);
        }
        int screenY = GetScreenHeight() - (y + 1) * cellSizePixels;
        DrawRectangle(x * cellSizePixels, screenY, cellSizePixels,
                      cellSizePixels, drawColor);

        if (cell.type == BULB_BLOCK) {
          Color outlineColor = (Color){255, 30, 180, 255};
          int px = x * cellSizePixels;
          int py = screenY;
          int thickness = cellSizePixels >= 4 ? 2 : 1;
          if (InspectCell(x - 1, y).type != BULB_BLOCK) {
            DrawRectangle(px, py, thickness, cellSizePixels, outlineColor);
          }
          if (InspectCell(x + 1, y).type != BULB_BLOCK) {
            DrawRectangle(px + cellSizePixels - thickness, py, thickness,
                          cellSizePixels, outlineColor);
          }
          if (InspectCell(x, y + 1).type != BULB_BLOCK) {
            DrawRectangle(px, py, cellSizePixels, thickness, outlineColor);
          }
          if (InspectCell(x, y - 1).type != BULB_BLOCK) {
            DrawRectangle(px, py + cellSizePixels - thickness, cellSizePixels,
                          thickness, outlineColor);
          }
        }
      }
    }
  }

  int settingCount = GetUIControlSettingsCount();
  Rectangle panelRect = GetUIControlsPanelRect(settingCount);
  RenderUIControls();
  DrawText("Left Click: Sand  |  Right Click: Shader Block  |  Space: Water  | "
           " E: Bulb Seed  |  C: Clear",
           10, (int)(panelRect.y + panelRect.height + 8.0f), 16, DARKGRAY);
  DrawFPS(GetScreenWidth() - 100, 30);
  EndDrawing();
}

// -------------------------------------------------------------
// INPUT & INTERACTION HELPERS
// -------------------------------------------------------------

typedef enum { INPUT_BIND_MOUSE_BUTTON = 0, INPUT_BIND_KEY } InputBindingType;

typedef struct {
  InputBindingType triggerType;
  int triggerCode;
  CellType resultCellType;
} InputBinding;

typedef struct {
  const char *label;
  int *value;
  int minValue;
  int maxValue;
} UIControlSetting;

// Input priority is defined by array order.
const InputBinding kInputBindings[] = {
    {INPUT_BIND_MOUSE_BUTTON, MOUSE_BUTTON_LEFT, SAND},
    {INPUT_BIND_MOUSE_BUTTON, MOUSE_BUTTON_RIGHT, SHADER_BLOCK},
    {INPUT_BIND_KEY, KEY_SPACE, WATER},
    {INPUT_BIND_KEY, KEY_E, BULB_SEED},
};

int ClampInt(int value, int minValue, int maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

int GetUIControlSettings(UIControlSetting settings[4]) {
  settings[0] = (UIControlSetting){
      .label = "Cell Px", .value = &cellSizePixels, .minValue = 1, .maxValue = 8};
  settings[1] = (UIControlSetting){.label = "Brush Px",
                                   .value = &drawBrushRadiusPixels,
                                   .minValue = 0,
                                   .maxValue = 64};
  settings[2] = (UIControlSetting){.label = "Gravity Px",
                                   .value = &gravitySpeedPixelsPerStep,
                                   .minValue = 1,
                                   .maxValue = 48};
  settings[3] = (UIControlSetting){.label = "Stem:Bulb %",
                                   .value = &bulbStemBulbRatioPercent,
                                   .minValue = 10,
                                   .maxValue = 90};
  return 4;
}

int GetUIControlSettingsCount(void) { return 4; }

Rectangle GetUIControlsPanelRect(int settingCount) {
  float rowHeight = 24.0f;
  float panelHeight = 12.0f + rowHeight * settingCount + 8.0f;
  return (Rectangle){8.0f, 8.0f, 340.0f, panelHeight};
}

Rectangle GetUIControlTrackRect(int index) {
  float rowHeight = 24.0f;
  float y = 26.0f + index * rowHeight;
  return (Rectangle){116.0f, y, 170.0f, 6.0f};
}

Rectangle GetUIControlHitRect(int index) {
  Rectangle track = GetUIControlTrackRect(index);
  return (Rectangle){track.x - 6.0f, track.y - 8.0f, track.width + 12.0f,
                     track.height + 16.0f};
}

float GetUIControlT(const UIControlSetting *setting) {
  int range = setting->maxValue - setting->minValue;
  if (range <= 0) {
    return 0.0f;
  }
  return (float)(*setting->value - setting->minValue) / (float)range;
}

int GetSliderValueFromMouse(const UIControlSetting *setting, Rectangle track,
                            float mouseX) {
  float t = (mouseX - track.x) / track.width;
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;
  int range = setting->maxValue - setting->minValue;
  return setting->minValue + (int)roundf(t * (float)range);
}

void UpdateUIControls(void) {
  UIControlSetting settings[4];
  int settingCount = GetUIControlSettings(settings);
  Rectangle panelRect = GetUIControlsPanelRect(settingCount);
  Vector2 mouse = {(float)GetMouseX(), (float)GetMouseY()};

  bool leftDown = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
  bool rightDown = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
  if (!leftDown) {
    uiActiveControlIndex = -1;
  }

  if (leftDown) {
    if (uiActiveControlIndex < 0) {
      for (int i = 0; i < settingCount; i++) {
        if (CheckCollisionPointRec(mouse, GetUIControlHitRect(i))) {
          uiActiveControlIndex = i;
          break;
        }
      }
    }

    if (uiActiveControlIndex >= 0 && uiActiveControlIndex < settingCount) {
      Rectangle track = GetUIControlTrackRect(uiActiveControlIndex);
      UIControlSetting *active = &settings[uiActiveControlIndex];
      *active->value = GetSliderValueFromMouse(active, track, mouse.x);
    }
  }

  for (int i = 0; i < settingCount; i++) {
    *settings[i].value =
        ClampInt(*settings[i].value, settings[i].minValue, settings[i].maxValue);
  }

  uiPointerCaptured = (uiActiveControlIndex >= 0) ||
                      (CheckCollisionPointRec(mouse, panelRect) &&
                       (leftDown || rightDown));
}

void RenderUIControls(void) {
  UIControlSetting settings[4];
  int settingCount = GetUIControlSettings(settings);
  Rectangle panelRect = GetUIControlsPanelRect(settingCount);
  DrawRectangleRec(panelRect, (Color){235, 235, 235, 230});
  DrawRectangleLinesEx(panelRect, 1.0f, LIGHTGRAY);

  for (int i = 0; i < settingCount; i++) {
    Rectangle track = GetUIControlTrackRect(i);
    bool active = (uiActiveControlIndex == i);
    float t = GetUIControlT(&settings[i]);
    float knobX = track.x + t * track.width;
    DrawText(settings[i].label, 18, (int)track.y - 9, 16, DARKGRAY);
    DrawRectangleRec(track, LIGHTGRAY);
    DrawRectangle((int)track.x, (int)track.y, (int)(knobX - track.x),
                  (int)track.height, SKYBLUE);
    DrawCircle((int)knobX, (int)(track.y + track.height * 0.5f), 7.0f,
               active ? BLUE : DARKBLUE);
    DrawText(TextFormat("%d", *settings[i].value), 296, (int)track.y - 9, 16,
             DARKGRAY);
  }
}

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
  if (IsImmutable(existing.type)) {
    return;
  }

  if (type == BULB_SEED && existing.type == BULB_BLOCK) {
    int nearest = FindNearestBulbNodeIndex(x, y, 2500);
    if (nearest >= 0) {
      bulbNodes[nearest].mass++;
      if (bulbNodes[nearest].mass > 4096) {
        bulbNodes[nearest].mass = 4096;
      }
      bulbNeedsRebuild = true;
    }
    return;
  }

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
    if (IsImmutable(scanned.type)) {
      break;
    }
    if (IsSolid(scanned.type)) {
      break;
    }
    if (scanned.type == EMPTY) {
      SET_CELL(currentGrid, x, scanY, type, GetCellColor(type), 0);
      return;
    }
  }
}

int GetBrushRadiusInCells(void) {
  int clampedPixels = drawBrushRadiusPixels < 0 ? 0 : drawBrushRadiusPixels;
  int size = cellSizePixels < 1 ? 1 : cellSizePixels;
  if (clampedPixels == 0) {
    return 0;
  }

  // Round up to keep at least the requested visual radius.
  return (clampedPixels + size - 1) / size;
}

int GetGravitySpeedInCells(void) {
  int size = cellSizePixels < 1 ? 1 : cellSizePixels;
  int clampedPixels =
      gravitySpeedPixelsPerStep < size ? size : gravitySpeedPixelsPerStep;
  return (clampedPixels + size - 1) / size;
}

int GetGravitySpeedInCellsForMass(float mass) {
  int baseCells = GetGravitySpeedInCells();
  float effectiveMass = mass <= 0.0f ? 1.0f : mass;
  int scaledCells = (int)floorf((float)baseCells * effectiveMass + 0.5f);
  return scaledCells < 1 ? 1 : scaledCells;
}

// Stamp a filled circular brush centered at (centerX, centerY).
void SpawnFilledCircleAtGrid(int centerX, int centerY, int radius,
                             CellType type) {
  int clampedRadius = radius < 0 ? 0 : radius;
  int radiusSquared = clampedRadius * clampedRadius;

  for (int dy = -clampedRadius; dy <= clampedRadius; dy++) {
    int y = centerY + dy;
    if (y < 0 || y >= gridRows) {
      continue;
    }

    for (int dx = -clampedRadius; dx <= clampedRadius; dx++) {
      if (dx * dx + dy * dy > radiusSquared) {
        continue;
      }

      int x = centerX + dx;
      if (x < 0 || x >= gridCols) {
        continue;
      }
      PlaceDrawnCell(x, y, type);
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
  int brushRadiusInCells = GetBrushRadiusInCells();

  for (;;) {
    SpawnFilledCircleAtGrid(x0, y0, brushRadiusInCells, type);

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

  // Snap raw floating hardware coordinates into our pure integer Array
  // structure
  int gridX = mx / cellSizePixels;
  int gridY = (GetScreenHeight() - my) / cellSizePixels;

  static int lastGridX = -1;
  static int lastGridY = -1;

  if (uiPointerCaptured) {
    lastGridX = gridX;
    lastGridY = gridY;
    return;
  }

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

  float tickRate = 1.0f / PHYSICS_TICK_RATE;

  physicsAccumulator += deltaTime;

  while (physicsAccumulator >= tickRate) {
    // Sample inputs every physics tick so fast cursor motion stays smooth.
    HandleUserInputs();
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

  // 2. UI controls
  UpdateUIControls();

  // 3. Maintain memory bounds
  gridCols = GetScreenWidth() / cellSizePixels;
  gridRows = GetScreenHeight() / cellSizePixels;
  if (gridCols < 1)
    gridCols = 1;
  if (gridRows < 1)
    gridRows = 1;
  ResizeGrid(gridCols, gridRows);

  // 4. Time-Independent Engine Core
  UpdateSimulation();

  // 5. Graphics Context Hook
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


