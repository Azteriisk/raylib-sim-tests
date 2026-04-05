#include "sim.h"

#include <math.h>
#include <stdlib.h>

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

static const InputBinding kInputBindings[] = {
    {INPUT_BIND_MOUSE_BUTTON, MOUSE_BUTTON_LEFT, SAND},
    {INPUT_BIND_MOUSE_BUTTON, MOUSE_BUTTON_RIGHT, SHADER_BLOCK},
    {INPUT_BIND_KEY, KEY_SPACE, WATER},
    {INPUT_BIND_KEY, KEY_E, EMPTY},
    {INPUT_BIND_KEY, KEY_B, BULB_SEED},
};

static float GetPlacedCellPhaseSeconds(CellType type) {
  if (BlockRegistry[type].colorShader != NULL) {
    return GetShaderClockSeconds();
  }
  return 0.0f;
}

static int GetUIControlSettings(UIControlSetting settings[5]) {
  settings[0] = (UIControlSetting){.label = "Cell Px",
                                   .value = &cellSizePixels,
                                   .minValue = 1,
                                   .maxValue = 8};
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
  settings[4] = (UIControlSetting){.label = "Merge %",
                                   .value = &bulbMergeOverlapPercent,
                                   .minValue = 5,
                                   .maxValue = 90};
  return 5;
}

int GetUIControlSettingsCount(void) { return 5; }

Rectangle GetUIControlsPanelRect(int settingCount) {
  float rowHeight = 24.0f;
  float panelHeight = 12.0f + rowHeight * settingCount + 8.0f;
  return (Rectangle){8.0f, 8.0f, 340.0f, panelHeight};
}

static Rectangle GetUIControlTrackRect(int index) {
  float rowHeight = 24.0f;
  float y = 26.0f + index * rowHeight;
  return (Rectangle){116.0f, y, 170.0f, 6.0f};
}

static Rectangle GetUIControlHitRect(int index) {
  Rectangle track = GetUIControlTrackRect(index);
  return (Rectangle){track.x - 6.0f, track.y - 8.0f, track.width + 12.0f,
                     track.height + 16.0f};
}

static float GetUIControlT(const UIControlSetting *setting) {
  int range = setting->maxValue - setting->minValue;
  if (range <= 0) {
    return 0.0f;
  }
  return (float)(*setting->value - setting->minValue) / (float)range;
}

static int GetSliderValueFromMouse(const UIControlSetting *setting,
                                   Rectangle track, float mouseX) {
  float t = (mouseX - track.x) / track.width;
  if (t < 0.0f)
    t = 0.0f;
  if (t > 1.0f)
    t = 1.0f;
  int range = setting->maxValue - setting->minValue;
  return setting->minValue + (int)roundf(t * (float)range);
}

void UpdateUIControls(void) {
  UIControlSetting settings[5];
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
    *settings[i].value = ClampInt(*settings[i].value, settings[i].minValue,
                                  settings[i].maxValue);
  }

  uiPointerCaptured =
      (uiActiveControlIndex >= 0) ||
      (CheckCollisionPointRec(mouse, panelRect) && (leftDown || rightDown));
}

void RenderUIControls(void) {
  UIControlSetting settings[5];
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

static bool IsBindingActive(const InputBinding *binding) {
  switch (binding->triggerType) {
  case INPUT_BIND_MOUSE_BUTTON:
    return IsMouseButtonDown(binding->triggerCode);
  case INPUT_BIND_KEY:
    return IsKeyDown(binding->triggerCode);
  default:
    return false;
  }
}

static const InputBinding *GetActiveInputBinding(void) {
  int count = sizeof(kInputBindings) / sizeof(kInputBindings[0]);
  for (int i = 0; i < count; i++) {
    if (IsBindingActive(&kInputBindings[i])) {
      return &kInputBindings[i];
    }
  }
  return NULL;
}

static void PlaceDrawnCell(int x, int y, CellType type) {
  PixelCell existing = GET_CELL(currentGrid, x, y);
  if (IsImmutable(existing.type)) {
    return;
  }

  if (type == BULB_SEED && existing.type == BULB_BLOCK) {
    int nearest = FindBulbNodeByReach(x, y, 6);
    if (nearest < 0) {
      nearest = FindNearestBulbNodeIndex(x, y);
    }
    if (nearest >= 0) {
      AddMassToBulbNode(nearest, 1);
      MergeBulbClusterAt(nearest);
      MarkActiveRowRange(y - 2, y + 2);
    }
    return;
  }

  if (existing.type == EMPTY || IsSolid(existing.type) ||
      !BlockRegistry[type].affectedByGravity) {
    SET_CELL(currentGrid, x, y, type, GetCellColor(type),
             GetPlacedCellPhaseSeconds(type));
    MarkActiveRowRange(y - 2, y + 2);
    return;
  }

  for (int scanY = y - 1; scanY >= 0; scanY--) {
    PixelCell scanned = GET_CELL(currentGrid, x, scanY);
    if (IsImmutable(scanned.type)) {
      break;
    }
    if (IsSolid(scanned.type)) {
      break;
    }
    if (scanned.type == EMPTY) {
      SET_CELL(currentGrid, x, scanY, type, GetCellColor(type),
               GetPlacedCellPhaseSeconds(type));
      int minY = y < scanY ? y : scanY;
      int maxY = y > scanY ? y : scanY;
      MarkActiveRowRange(minY - 2, maxY + 2);
      return;
    }
  }
}

static int GetBrushRadiusInCells(void) {
  int clampedPixels = drawBrushRadiusPixels < 0 ? 0 : drawBrushRadiusPixels;
  int size = cellSizePixels < 1 ? 1 : cellSizePixels;
  if (clampedPixels == 0) {
    return 0;
  }
  return (clampedPixels + size - 1) / size;
}

static void SpawnFilledCircleAtGrid(int centerX, int centerY, int radius,
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

static void SpawnLineBetweenGrids(int x0, int y0, int x1, int y1,
                                  CellType type) {
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

void HandleUserInputs(void) {
  int mx = GetMouseX();
  int my = GetMouseY();

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
  if (activeBinding == NULL) {
    lastGridX = gridX;
    lastGridY = gridY;
  }

  if (activeBinding != NULL) {
    SpawnLineBetweenGrids(lastGridX, lastGridY, gridX, gridY,
                          activeBinding->resultCellType);
  }

  lastGridX = gridX;
  lastGridY = gridY;
}

void UpdateSimulation(void) {
  float deltaTime = GetFrameTime();
  if (deltaTime > 0.1f) {
    deltaTime = 0.016f;
  }

  static float physicsAccumulator = 0.0f;
  float tickRate = 1.0f / PHYSICS_TICK_RATE;
  physicsAccumulator += deltaTime;

  while (physicsAccumulator >= tickRate) {
    HandleUserInputs();
    ProcessPhysics(tickRate);
    physicsAccumulator -= tickRate;
  }
}

void ExecuteFrame(void) {
  if (IsKeyPressed(KEY_C)) {
    InitGrid();
  }

  UpdateUIControls();

  gridCols = GetScreenWidth() / cellSizePixels;
  gridRows = GetScreenHeight() / cellSizePixels;
  if (gridCols < 1)
    gridCols = 1;
  if (gridRows < 1)
    gridRows = 1;
  ResizeGrid(gridCols, gridRows);

  UpdateSimulation();
  RenderWorld();
}
