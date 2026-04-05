#ifndef SIM_H
#define SIM_H

#include "lib/raylib.h"
#include <stdbool.h>

#define START_WIDTH 800
#define START_HEIGHT 450

#define PHYSICS_TICK_RATE 60.0f
#define BULB_REBUILD_HZ 18.0f
#define BULB_MERGE_OVERLAP_RATIO 0.35f
#define MAX_BULB_NODES 1024

typedef enum {
  EMPTY = 0,
  SAND,
  WATER,
  BULB_SEED,
  BULB_BLOCK,
  SHADER_BLOCK,
  WALL
} CellType;

typedef enum { OUTLINE_NONE = 0, OUTLINE_EXTERIOR } OutlineMode;
typedef enum { STACK_SHAPE_NONE = 0, STACK_SHAPE_MUSHROOM_CLOUD } StackShape;
typedef enum { MERGE_NONE = 0, MERGE_OVERLAP } MergeBehavior;
typedef Color (*ColorShaderFn)(int x, int y, float timeAlive, Color baseColor);

typedef struct {
  bool isSolid;
  bool isImmutable;
  bool affectedByGravity;
  bool spreadsLikeLiquid;
  float mass;
  float gravityMoveChance;
  float spreadMoveChance;
  int spreadDistanceCells;
  Color baseColor;
  ColorShaderFn colorShader;
  OutlineMode outlineMode;
  Color outlineColor;
  int outlineThicknessCells;
  StackShape stackShape;
  MergeBehavior mergeBehavior;
  float mergeOverlapRatio;
  int mergeAttachReachBias;
} BlockProperties;

typedef struct {
  CellType type;
  Color color;
  float timeAlive;
} PixelCell;

typedef struct {
  bool active;
  int anchorX;
  int anchorY;
  int mass;
} BulbNode;

extern int cellSizePixels;
extern int drawBrushRadiusPixels;
extern int gravitySpeedPixelsPerStep;
extern int bulbStemBulbRatioPercent;
extern int bulbMergeOverlapPercent;

extern int gridCols;
extern int gridRows;
extern int allocatedCols;
extern int allocatedRows;

extern PixelCell *currentGrid;
extern PixelCell *nextGrid;
extern int uiActiveControlIndex;
extern bool uiPointerCaptured;
extern bool bulbNeedsRebuild;
extern float bulbRebuildAccumulator;
extern BulbNode bulbNodes[MAX_BULB_NODES];

extern const BlockProperties BlockRegistry[];

#define GET_CELL(grid, x, y) (grid[(y) * allocatedCols + (x)])
#define SET_CELL(grid, x, y, cType, cColor, cTime)                             \
  (grid[(y) * allocatedCols + (x)] = (PixelCell){cType, cColor, cTime})

Color GetCellColor(CellType type);
Color GetShaderOscillatedColor(int x, int y, float timeAlive, Color baseColor);
Color GetBulbShaderColor(int x, int y, float timeAlive, Color baseColor);

void ResizeGrid(int targetCols, int targetRows);
void InitGrid(void);
void ClearNextGrid(void);
void SwapGrids(void);

PixelCell InspectCell(int x, int y);
PixelCell InspectFutureCell(int x, int y);
CellType GetResolvedCellType(int x, int y);

bool IsSolid(CellType type);
bool IsImmutable(CellType type);
bool RollChance(float chance);

void ClearAllBulbs(void);
void RebuildBulbs(void);
void AddMassToBulbNode(int index, int delta);
int MergeBulbNodeWithNearest(int index);
int FindBulbNodeByReach(int x, int y, int extraReach);
int FindNearestBulbNodeIndex(int x, int y);
void MergeBulbClusterAt(int index);
bool TryAbsorbBulbSeed(int x, int y);

int ClampInt(int value, int minValue, int maxValue);
int GetGravitySpeedInCells(void);
int GetGravitySpeedInCellsForMass(float mass);

int GetUIControlSettingsCount(void);
Rectangle GetUIControlsPanelRect(int settingCount);
void UpdateUIControls(void);
void RenderUIControls(void);

void ProcessPhysics(float deltaTime);
void RenderWorld(void);
void ShutdownRenderer(void);
float GetShaderClockSeconds(void);
void MarkActiveRowRange(int minY, int maxY);
void HandleUserInputs(void);
void UpdateSimulation(void);
void ExecuteFrame(void);

#endif
