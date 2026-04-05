#include "sim.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLenum;
typedef unsigned char GLubyte;
#if defined(_WIN32)
__declspec(dllimport) const GLubyte *__stdcall glGetString(GLenum name);
#else
const GLubyte *glGetString(GLenum name);
#endif

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

typedef enum { MOVE_EMPTY = 0, MOVE_NON_SOLID, MOVE_SOLID } MoveCellState;

#define ACTIVE_ROW_TTL 6

static Texture2D gWorldTexture = {0};
static Color *gWorldPixels = NULL;
static Color *gPrevWorldPixels = NULL;
static Color *gUploadPixels = NULL;
static int gUploadCapacity = 0;
static int gWorldCols = 0;
static int gWorldRows = 0;
static float gShaderClockSeconds = 0.0f;
static bool gGpuInfoLogged = false;
static char gGpuVendor[96] = "UnknownVendor";
static char gGpuRenderer[160] = "UnknownRenderer";
static char gGpuVersion[96] = "UnknownVersion";

static unsigned char *gActiveRowTtl = NULL;
static unsigned char *gNextActiveRowTtl = NULL;
static int gActiveRowCapacity = 0;
static bool gPhysicsBootstrapped = false;

static bool IsColorEqual(Color a, Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static Color GetCpuRenderedColor(PixelCell cell, int x, int y,
                                 float shaderTimeSeconds) {
  const BlockProperties *renderProps = &BlockRegistry[cell.type];
  Color drawColor = cell.color;
  if (renderProps->colorShader != NULL) {
    float localShaderTime = shaderTimeSeconds;
    if (cell.timeAlive > 0.0f) {
      localShaderTime -= cell.timeAlive;
      if (localShaderTime < 0.0f) {
        localShaderTime = 0.0f;
      }
    }
    drawColor = renderProps->colorShader(x, y, localShaderTime,
                                         renderProps->baseColor);
  }

  if (renderProps->outlineMode == OUTLINE_EXTERIOR) {
    bool leftBoundary = (x == 0) || (GET_CELL(currentGrid, x - 1, y).type != cell.type);
    bool rightBoundary =
        (x == gridCols - 1) || (GET_CELL(currentGrid, x + 1, y).type != cell.type);
    bool topBoundary =
        (y == gridRows - 1) || (GET_CELL(currentGrid, x, y + 1).type != cell.type);
    bool bottomBoundary = (y == 0) || (GET_CELL(currentGrid, x, y - 1).type != cell.type);
    if (leftBoundary || rightBoundary || topBoundary || bottomBoundary) {
      drawColor = renderProps->outlineColor.a > 0 ? renderProps->outlineColor
                                                  : (Color){255, 30, 180, 255};
    }
  }
  return drawColor;
}

static void LogGpuInfoOnce(void) {
  if (gGpuInfoLogged) {
    return;
  }

  const GLubyte *vendorBytes = glGetString(GL_VENDOR);
  const GLubyte *rendererBytes = glGetString(GL_RENDERER);
  const GLubyte *versionBytes = glGetString(GL_VERSION);
  const char *vendor =
      vendorBytes != NULL ? (const char *)vendorBytes : "UnknownVendor";
  const char *renderer =
      rendererBytes != NULL ? (const char *)rendererBytes : "UnknownRenderer";
  const char *version =
      versionBytes != NULL ? (const char *)versionBytes : "UnknownVersion";
  strncpy(gGpuVendor, vendor, sizeof(gGpuVendor) - 1);
  strncpy(gGpuRenderer, renderer, sizeof(gGpuRenderer) - 1);
  strncpy(gGpuVersion, version, sizeof(gGpuVersion) - 1);
  gGpuVendor[sizeof(gGpuVendor) - 1] = '\0';
  gGpuRenderer[sizeof(gGpuRenderer) - 1] = '\0';
  gGpuVersion[sizeof(gGpuVersion) - 1] = '\0';
  TraceLog(LOG_INFO, "GPU vendor: %s", vendor);
  TraceLog(LOG_INFO, "GPU renderer: %s", renderer);
  TraceLog(LOG_INFO, "OpenGL version: %s", version);
  gGpuInfoLogged = true;
}

static void EnsureActiveRows(int rows) {
  if (rows <= gActiveRowCapacity) {
    return;
  }
  unsigned char *newCurrent = (unsigned char *)malloc((size_t)rows);
  unsigned char *newNext = (unsigned char *)malloc((size_t)rows);
  if (newCurrent == NULL || newNext == NULL) {
    free(newCurrent);
    free(newNext);
    return;
  }
  if (gActiveRowTtl != NULL && gActiveRowCapacity > 0) {
    memcpy(newCurrent, gActiveRowTtl, (size_t)gActiveRowCapacity);
  }
  if (gNextActiveRowTtl != NULL && gActiveRowCapacity > 0) {
    memcpy(newNext, gNextActiveRowTtl, (size_t)gActiveRowCapacity);
  }
  if (rows > gActiveRowCapacity) {
    memset(newCurrent + gActiveRowCapacity, 0, (size_t)(rows - gActiveRowCapacity));
    memset(newNext + gActiveRowCapacity, 0, (size_t)(rows - gActiveRowCapacity));
  }
  free(gActiveRowTtl);
  free(gNextActiveRowTtl);
  gActiveRowTtl = newCurrent;
  gNextActiveRowTtl = newNext;
  gActiveRowCapacity = rows;
}

void MarkActiveRowRange(int minY, int maxY) {
  if (gridRows <= 0) {
    return;
  }
  EnsureActiveRows(gridRows);
  if (gActiveRowTtl == NULL) {
    return;
  }

  if (minY < 0) {
    minY = 0;
  }
  if (maxY >= gridRows) {
    maxY = gridRows - 1;
  }
  if (minY > maxY) {
    return;
  }

  for (int y = minY; y <= maxY; y++) {
    gActiveRowTtl[y] = ACTIVE_ROW_TTL;
  }
}

static void MarkNextActiveRowRange(int minY, int maxY) {
  if (gNextActiveRowTtl == NULL || gridRows <= 0) {
    return;
  }
  if (minY < 0) {
    minY = 0;
  }
  if (maxY >= gridRows) {
    maxY = gridRows - 1;
  }
  if (minY > maxY) {
    return;
  }
  for (int y = minY; y <= maxY; y++) {
    gNextActiveRowTtl[y] = ACTIVE_ROW_TTL;
  }
}

static void EnsureWorldTexture(int cols, int rows) {
  if (cols < 1 || rows < 1) {
    return;
  }
  if (gWorldTexture.id > 0 && gWorldCols == cols && gWorldRows == rows &&
      gWorldPixels != NULL && gPrevWorldPixels != NULL) {
    return;
  }

  if (gWorldTexture.id > 0) {
    UnloadTexture(gWorldTexture);
    gWorldTexture.id = 0;
  }
  free(gWorldPixels);
  free(gPrevWorldPixels);
  gWorldPixels = NULL;
  gPrevWorldPixels = NULL;

  gWorldPixels = (Color *)malloc((size_t)cols * (size_t)rows * sizeof(Color));
  gPrevWorldPixels =
      (Color *)malloc((size_t)cols * (size_t)rows * sizeof(Color));
  if (gWorldPixels == NULL || gPrevWorldPixels == NULL) {
    free(gWorldPixels);
    free(gPrevWorldPixels);
    gWorldPixels = NULL;
    gPrevWorldPixels = NULL;
    gWorldCols = 0;
    gWorldRows = 0;
    return;
  }

  int pixelCount = cols * rows;
  for (int i = 0; i < pixelCount; i++) {
    gWorldPixels[i] = RAYWHITE;
    gPrevWorldPixels[i] = RAYWHITE;
  }

  Image img = GenImageColor(cols, rows, RAYWHITE);
  gWorldTexture = LoadTextureFromImage(img);
  UnloadImage(img);
  if (gWorldTexture.id > 0) {
    SetTextureFilter(gWorldTexture, TEXTURE_FILTER_POINT);
    SetTextureWrap(gWorldTexture, TEXTURE_WRAP_CLAMP);
  }
  gWorldCols = cols;
  gWorldRows = rows;
}

static MoveCellState GetMoveCellState(int x, int y) {
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

static MoveCellState GetProcessedRowMoveCellState(int x, int y) {
  PixelCell futureCell = InspectFutureCell(x, y);
  if (futureCell.type == EMPTY) {
    return MOVE_EMPTY;
  }
  return IsSolid(futureCell.type) ? MOVE_SOLID : MOVE_NON_SOLID;
}

static bool TryScatter(int x, int sourceY, int targetY, int maxDistance,
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
      MoveCellState state = targetRowProcessed
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
    }
  }

  return false;
}

static bool ApplyGravity(int x, int y, const BlockProperties *props, int *nextX,
                         int *nextY) {
  if (!RollChance(props->gravityMoveChance)) {
    return false;
  }

  int gravityStepCells = GetGravitySpeedInCellsForMass(props->mass);
  int openCellsSeen = 0;

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

  return TryScatter(x, y, y - 1, 1, nextX, nextY);
}

Color GetShaderOscillatedColor(int x, int y, float timeAlive, Color baseColor) {
  float waveR = sinf(timeAlive * 3.0f + x * 0.1f) * 0.5f + 0.5f;
  float waveG = sinf(timeAlive * 2.0f + y * 0.1f) * 0.5f + 0.5f;
  float waveB = sinf(timeAlive * 4.0f) * 0.5f + 0.5f;
  float r = (float)baseColor.r * 0.45f + waveR * 145.0f;
  float g = (float)baseColor.g * 0.45f + waveG * 145.0f;
  float b = (float)baseColor.b * 0.45f + waveB * 145.0f;
  if (r > 255.0f)
    r = 255.0f;
  if (g > 255.0f)
    g = 255.0f;
  if (b > 255.0f)
    b = 255.0f;
  return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
}

Color GetBulbShaderColor(int x, int y, float timeAlive, Color baseColor) {
  float pulse = sinf(timeAlive * 2.5f + x * 0.18f - y * 0.11f) * 0.5f + 0.5f;
  float shade = 0.68f + pulse * 0.58f;
  float r = (float)baseColor.r * shade;
  float g = (float)baseColor.g * shade;
  float b = (float)baseColor.b * shade;
  if (r > 255.0f)
    r = 255.0f;
  if (g > 255.0f)
    g = 255.0f;
  if (b > 255.0f)
    b = 255.0f;
  unsigned char a = baseColor.a > 0 ? baseColor.a : 255;
  return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, a};
}

static bool ApplyPixelRules(int x, int y, float deltaTime, PixelCell cell,
                            int *outX, int *outY) {
  cell.timeAlive += deltaTime;
  int nextX = x;
  int nextY = y;
  const BlockProperties *props = &BlockRegistry[cell.type];

  if (cell.type == BULB_SEED) {
    if (ApplyGravity(x, y, props, &nextX, &nextY)) {
      GET_CELL(nextGrid, nextX, nextY) = cell;
      *outX = nextX;
      *outY = nextY;
      return false;
    }

    if (TryAbsorbBulbSeed(x, y)) {
      *outX = -1;
      *outY = -1;
      return true;
    }

    GET_CELL(nextGrid, x, y) = cell;
    *outX = x;
    *outY = y;
    return false;
  }

  if (props->affectedByGravity) {
    if (!ApplyGravity(x, y, props, &nextX, &nextY)) {
      if (props->spreadsLikeLiquid && RollChance(props->spreadMoveChance)) {
        int spreadDistance =
            props->spreadDistanceCells < 1 ? 1 : props->spreadDistanceCells;
        TryScatter(x, y, y, spreadDistance, &nextX, &nextY);
      }
    }
  }

  GET_CELL(nextGrid, nextX, nextY) = cell;
  *outX = nextX;
  *outY = nextY;
  return false;
}

void ProcessPhysics(float deltaTime) {
  EnsureActiveRows(gridRows);
  bool hasActiveRows = false;
  if (gActiveRowTtl != NULL) {
    for (int y = 0; y < gridRows; y++) {
      if (gActiveRowTtl[y] > 0) {
        hasActiveRows = true;
        break;
      }
    }
  }
  if (!hasActiveRows && gPhysicsBootstrapped) {
    if (bulbNeedsRebuild) {
      bulbRebuildAccumulator += deltaTime;
      float rebuildStep = 1.0f / BULB_REBUILD_HZ;
      if (bulbRebuildAccumulator >= rebuildStep) {
        RebuildBulbs();
        bulbRebuildAccumulator = 0.0f;
        MarkActiveRowRange(0, gridRows - 1);
      }
    } else {
      bulbRebuildAccumulator = 0.0f;
    }
    return;
  }

  bool fullScan = !hasActiveRows;

  for (int y = 0; y < gridRows; y++) {
    PixelCell *dst = &nextGrid[y * allocatedCols];
    PixelCell *src = &currentGrid[y * allocatedCols];
    if (fullScan || (gActiveRowTtl != NULL && gActiveRowTtl[y] > 0)) {
      memset(dst, 0, gridCols * sizeof(PixelCell));
    } else {
      memcpy(dst, src, gridCols * sizeof(PixelCell));
    }
  }

  if (gNextActiveRowTtl != NULL) {
    memset(gNextActiveRowTtl, 0, (size_t)gridRows);
  }

  for (int y = 0; y < gridRows; y++) {
    if (!fullScan && (gActiveRowTtl == NULL || gActiveRowTtl[y] == 0)) {
      continue;
    }
    for (int x = 0; x < gridCols; x++) {
      PixelCell cell = GET_CELL(currentGrid, x, y);
      if (cell.type == EMPTY) {
        continue;
      }

      const BlockProperties *props = &BlockRegistry[cell.type];
      if (cell.type != BULB_SEED && !props->affectedByGravity &&
          !props->spreadsLikeLiquid) {
        GET_CELL(nextGrid, x, y) = cell;
        continue;
      }

      int outX = x;
      int outY = y;
      bool consumed = ApplyPixelRules(x, y, deltaTime, cell, &outX, &outY);
      if (props->affectedByGravity || props->spreadsLikeLiquid ||
          cell.type == BULB_SEED) {
        int minY = y - 1;
        int maxY = y + 1;
        if (!consumed && outY >= 0) {
          if (outY - 1 < minY) {
            minY = outY - 1;
          }
          if (outY + 1 > maxY) {
            maxY = outY + 1;
          }
        }
        MarkNextActiveRowRange(minY, maxY);
      }
    }
  }

  if (gActiveRowTtl != NULL) {
    if (fullScan) {
      for (int y = 0; y < gridRows; y++) {
        gActiveRowTtl[y] = gNextActiveRowTtl[y];
      }
    } else {
      for (int y = 0; y < gridRows; y++) {
        unsigned char decay = gActiveRowTtl[y] > 0 ? gActiveRowTtl[y] - 1 : 0;
        if (gNextActiveRowTtl[y] > decay) {
          decay = gNextActiveRowTtl[y];
        }
        gActiveRowTtl[y] = decay;
      }
    }
  }
  gPhysicsBootstrapped = true;

  SwapGrids();
  if (bulbNeedsRebuild) {
    bulbRebuildAccumulator += deltaTime;
    float rebuildStep = 1.0f / BULB_REBUILD_HZ;
    if (bulbRebuildAccumulator >= rebuildStep) {
      RebuildBulbs();
      bulbRebuildAccumulator = 0.0f;
      MarkActiveRowRange(0, gridRows - 1);
    }
  } else {
    bulbRebuildAccumulator = 0.0f;
  }
}

void RenderWorld(void) {
  BeginDrawing();
  ClearBackground(RAYWHITE);
  EnsureWorldTexture(gridCols, gridRows);
  LogGpuInfoOnce();
  float frameDt = GetFrameTime();
  if (frameDt <= 0.0f) {
    frameDt = 1.0f / PHYSICS_TICK_RATE;
  }
  gShaderClockSeconds += frameDt;
  float shaderTime = gShaderClockSeconds;

  if (gWorldTexture.id > 0 && gWorldPixels != NULL && gPrevWorldPixels != NULL &&
      gWorldCols == gridCols && gWorldRows == gridRows) {
    int dirtyMinX = gWorldCols;
    int dirtyMinY = gWorldRows;
    int dirtyMaxX = -1;
    int dirtyMaxY = -1;
    for (int y = 0; y < gridRows; y++) {
      int py = gridRows - 1 - y;
      for (int x = 0; x < gridCols; x++) {
        PixelCell cell = GET_CELL(currentGrid, x, y);
        Color drawColor = RAYWHITE;
        if (cell.type != EMPTY) {
          drawColor = GetCpuRenderedColor(cell, x, y, shaderTime);
        }
        int idx = py * gWorldCols + x;
        gWorldPixels[idx] = drawColor;
        if (!IsColorEqual(drawColor, gPrevWorldPixels[idx])) {
          if (x < dirtyMinX) {
            dirtyMinX = x;
          }
          if (py < dirtyMinY) {
            dirtyMinY = py;
          }
          if (x > dirtyMaxX) {
            dirtyMaxX = x;
          }
          if (py > dirtyMaxY) {
            dirtyMaxY = py;
          }
        }
      }
    }

    if (dirtyMaxX >= dirtyMinX && dirtyMaxY >= dirtyMinY) {
      int dirtyWidth = dirtyMaxX - dirtyMinX + 1;
      int dirtyHeight = dirtyMaxY - dirtyMinY + 1;
      int needed = dirtyWidth * dirtyHeight;
      if (needed > gUploadCapacity) {
        Color *newUpload =
            (Color *)realloc(gUploadPixels, (size_t)needed * sizeof(Color));
        if (newUpload != NULL) {
          gUploadPixels = newUpload;
          gUploadCapacity = needed;
        }
      }

      if (gUploadPixels != NULL) {
        for (int row = 0; row < dirtyHeight; row++) {
          memcpy(&gUploadPixels[row * dirtyWidth],
                 &gWorldPixels[(dirtyMinY + row) * gWorldCols + dirtyMinX],
                 (size_t)dirtyWidth * sizeof(Color));
        }
        UpdateTextureRec(gWorldTexture,
                         (Rectangle){(float)dirtyMinX, (float)dirtyMinY,
                                     (float)dirtyWidth, (float)dirtyHeight},
                         gUploadPixels);
      }
    }

    Color *swap = gPrevWorldPixels;
    gPrevWorldPixels = gWorldPixels;
    gWorldPixels = swap;

    int drawWidth = gridCols * cellSizePixels;
    int drawHeight = gridRows * cellSizePixels;
    int drawY = GetScreenHeight() - drawHeight;
    DrawTexturePro(gWorldTexture,
                   (Rectangle){0, 0, (float)gWorldCols, (float)gWorldRows},
                   (Rectangle){0, (float)drawY, (float)drawWidth,
                               (float)drawHeight},
                   (Vector2){0, 0}, 0.0f, WHITE);
  }

  int settingCount = GetUIControlSettingsCount();
  Rectangle panelRect = GetUIControlsPanelRect(settingCount);
  RenderUIControls();
  DrawText("Left Click: Sand  |  Right Click: Shader Block  |  Space: Water  | "
           " E: Erase  |  B: Bulb Seed  |  C: Clear",
           10, (int)(panelRect.y + panelRect.height + 8.0f), 16, DARKGRAY);
  DrawFPS(GetScreenWidth() - 100, 30);
  DrawText(TextFormat("GPU: %s", gGpuRenderer), 10,
           (int)(panelRect.y + panelRect.height + 30.0f), 12, GRAY);
  EndDrawing();
}

void ShutdownRenderer(void) {
  if (gWorldTexture.id > 0) {
    UnloadTexture(gWorldTexture);
    gWorldTexture.id = 0;
  }
  free(gWorldPixels);
  free(gPrevWorldPixels);
  free(gUploadPixels);
  gWorldPixels = NULL;
  gPrevWorldPixels = NULL;
  gUploadPixels = NULL;
  gUploadCapacity = 0;
  gWorldCols = 0;
  gWorldRows = 0;
  gShaderClockSeconds = 0.0f;
  gGpuInfoLogged = false;
  strncpy(gGpuVendor, "UnknownVendor", sizeof(gGpuVendor) - 1);
  strncpy(gGpuRenderer, "UnknownRenderer", sizeof(gGpuRenderer) - 1);
  strncpy(gGpuVersion, "UnknownVersion", sizeof(gGpuVersion) - 1);
  gGpuVendor[sizeof(gGpuVendor) - 1] = '\0';
  gGpuRenderer[sizeof(gGpuRenderer) - 1] = '\0';
  gGpuVersion[sizeof(gGpuVersion) - 1] = '\0';

  free(gActiveRowTtl);
  free(gNextActiveRowTtl);
  gActiveRowTtl = NULL;
  gNextActiveRowTtl = NULL;
  gActiveRowCapacity = 0;
  gPhysicsBootstrapped = false;
}

float GetShaderClockSeconds(void) { return gShaderClockSeconds; }
