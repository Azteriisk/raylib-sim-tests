#include "sim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int cellSizePixels = 2;
int drawBrushRadiusPixels = 6;
int gravitySpeedPixelsPerStep = 6;
int bulbStemBulbRatioPercent = 35;
int bulbMergeOverlapPercent = 35;

int gridCols = 0;
int gridRows = 0;
int allocatedCols = 0;
int allocatedRows = 0;

PixelCell *currentGrid = NULL;
PixelCell *nextGrid = NULL;
int uiActiveControlIndex = -1;
bool uiPointerCaptured = false;
bool bulbNeedsRebuild = false;
float bulbRebuildAccumulator = 0.0f;
BulbNode bulbNodes[MAX_BULB_NODES];

const BlockProperties BlockRegistry[] = {
    [EMPTY] = {.baseColor = {0, 0, 0, 0}},
    [SAND] = {.isSolid = true,
              .affectedByGravity = true,
              .baseColor = {242, 209, 107, 255},
              .mass = 1.35f,
              .gravityMoveChance = 1.0f},
    [WATER] = {.isSolid = true,
               .affectedByGravity = true,
               .spreadsLikeLiquid = true,
               .baseColor = {73, 166, 219, 255},
               .mass = 0.72f,
               .gravityMoveChance = 0.8f,
               .spreadMoveChance = 0.9f,
               .spreadDistanceCells = 8},
    [BULB_SEED] = {.isSolid = true,
                   .affectedByGravity = true,
                   .baseColor = {220, 138, 186, 255},
                   .mass = 1.0f,
                   .gravityMoveChance = 1.0f},
    [BULB_BLOCK] = {.isSolid = true,
                    .baseColor = {46, 150, 86, 255},
                    .colorShader = GetBulbShaderColor,
                    .outlineMode = OUTLINE_EXTERIOR,
                    .outlineColor = {255, 30, 180, 255},
                    .outlineThicknessCells = 1,
                    .stackShape = STACK_SHAPE_MUSHROOM_CLOUD,
                    .mergeBehavior = MERGE_OVERLAP,
                    .mergeOverlapRatio = 0.35f,
                    .mergeAttachReachBias = -2},
    [SHADER_BLOCK] = {.baseColor = {200, 122, 255, 255},
                      .colorShader = GetShaderOscillatedColor},
    [WALL] = {
        .isSolid = true, .isImmutable = true, .baseColor = {80, 80, 80, 255}}};

Color GetCellColor(CellType type) { return BlockRegistry[type].baseColor; }

void ResizeGrid(int targetCols, int targetRows) {
  if (targetCols <= allocatedCols && targetRows <= allocatedRows) {
    return;
  }

  int newCols = allocatedCols > targetCols ? allocatedCols : targetCols;
  int newRows = allocatedRows > targetRows ? allocatedRows : targetRows;

  PixelCell *newCurrent =
      (PixelCell *)calloc(newCols * newRows, sizeof(PixelCell));
  PixelCell *newNext = (PixelCell *)calloc(newCols * newRows, sizeof(PixelCell));

  for (int x = 0; x < newCols; x++) {
    newCurrent[x] = (PixelCell){WALL, GetCellColor(WALL), 0};
    newNext[x] = (PixelCell){WALL, GetCellColor(WALL), 0};
  }

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

void InitGrid(void) {
  memset(currentGrid, 0, allocatedCols * allocatedRows * sizeof(PixelCell));
  memset(nextGrid, 0, allocatedCols * allocatedRows * sizeof(PixelCell));
  ClearAllBulbs();

  for (int x = 0; x < allocatedCols; x++) {
    SET_CELL(currentGrid, x, 0, WALL, GetCellColor(WALL), 0);
    SET_CELL(nextGrid, x, 0, WALL, GetCellColor(WALL), 0);
  }
}

void ClearNextGrid(void) {
  for (int y = 0; y < gridRows; y++) {
    memset(&nextGrid[y * allocatedCols], 0, gridCols * sizeof(PixelCell));
  }
}

void SwapGrids(void) {
  PixelCell *temp = currentGrid;
  currentGrid = nextGrid;
  nextGrid = temp;
}

PixelCell InspectCell(int x, int y) {
  if (x < 0 || x >= gridCols || y < 0 || y >= gridRows) {
    return (PixelCell){WALL, GetCellColor(WALL), 0};
  }
  return GET_CELL(currentGrid, x, y);
}

PixelCell InspectFutureCell(int x, int y) {
  if (x < 0 || x >= gridCols || y < 0 || y >= gridRows) {
    return (PixelCell){WALL, GetCellColor(WALL), 0};
  }
  return GET_CELL(nextGrid, x, y);
}

CellType GetResolvedCellType(int x, int y) {
  PixelCell futureCell = InspectFutureCell(x, y);
  if (futureCell.type != EMPTY) {
    return futureCell.type;
  }
  return InspectCell(x, y).type;
}

bool IsSolid(CellType type) { return BlockRegistry[type].isSolid; }
bool IsImmutable(CellType type) { return BlockRegistry[type].isImmutable; }

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

int ClampInt(int value, int minValue, int maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
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
