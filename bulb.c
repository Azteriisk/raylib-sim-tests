#include "sim.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BULB_BUCKET_SIZE 16

static int *gBulbBucketHeads = NULL;
static int gBulbBucketCols = 0;
static int gBulbBucketRows = 0;
static int gBulbBucketCapacity = 0;
static int gBulbBucketNext[MAX_BULB_NODES];
static int gBulbMaxReach = 6;
static bool gBulbBucketsDirty = true;

static void InvalidateBulbBuckets(void) { gBulbBucketsDirty = true; }

static void GetBulbShapeAxesFromMass(int mass, int *bodyRadiusX,
                                     int *bodyRadiusY) {
  int safeMass = mass < 1 ? 1 : mass;
  float stemRatio = (float)ClampInt(bulbStemBulbRatioPercent, 10, 90) / 100.0f;
  float bulbBudget = (float)safeMass * (1.0f - stemRatio);
  float size = sqrtf(fmaxf(1.0f, bulbBudget));
  int rx = 4 + (int)floorf(size * 1.45f);
  int ry = 3 + (int)floorf(size * 0.95f);
  int maxRy = (int)floorf((float)rx * 0.85f);
  if (maxRy < 3) {
    maxRy = 3;
  }
  if (ry > maxRy) {
    ry = maxRy;
  }
  *bodyRadiusX = rx;
  *bodyRadiusY = ry;
}

static float GetBulbOutlineLength(const BulbNode *node) {
  int rx = 0;
  int ry = 0;
  GetBulbShapeAxesFromMass(node->mass, &rx, &ry);
  float a = (float)rx;
  float b = (float)ry;
  const float pi = 3.14159265f;
  return 2.0f * pi * sqrtf((a * a + b * b) * 0.5f);
}

static int GetBulbMergeReach(const BulbNode *node) {
  float outline = GetBulbOutlineLength(node);
  int reach = (int)floorf(outline * 0.12f);
  if (reach < 6) {
    reach = 6;
  }
  int maxDimension = gridCols > gridRows ? gridCols : gridRows;
  if (maxDimension < 6) {
    maxDimension = 6;
  }
  if (reach > maxDimension) {
    reach = maxDimension;
  }
  return reach;
}

static void EnsureBulbBuckets(void) {
  int cols = (gridCols + BULB_BUCKET_SIZE - 1) / BULB_BUCKET_SIZE;
  int rows = (gridRows + BULB_BUCKET_SIZE - 1) / BULB_BUCKET_SIZE;
  if (cols < 1) {
    cols = 1;
  }
  if (rows < 1) {
    rows = 1;
  }

  int required = cols * rows;
  if (required > gBulbBucketCapacity) {
    int *newHeads =
        (int *)realloc(gBulbBucketHeads, (size_t)required * sizeof(int));
    if (newHeads == NULL) {
      return;
    }
    gBulbBucketHeads = newHeads;
    gBulbBucketCapacity = required;
  }

  if (cols != gBulbBucketCols || rows != gBulbBucketRows) {
    gBulbBucketCols = cols;
    gBulbBucketRows = rows;
    gBulbBucketsDirty = true;
  }
}

static void RebuildBulbBuckets(void) {
  EnsureBulbBuckets();
  if (gBulbBucketHeads == NULL || gBulbBucketCols < 1 || gBulbBucketRows < 1) {
    return;
  }

  int bucketCount = gBulbBucketCols * gBulbBucketRows;
  for (int i = 0; i < bucketCount; i++) {
    gBulbBucketHeads[i] = -1;
  }
  gBulbMaxReach = 6;

  for (int i = 0; i < MAX_BULB_NODES; i++) {
    gBulbBucketNext[i] = -1;
    if (!bulbNodes[i].active) {
      continue;
    }
    if (bulbNodes[i].anchorX < 0 || bulbNodes[i].anchorX >= gridCols ||
        bulbNodes[i].anchorY < 0 || bulbNodes[i].anchorY >= gridRows) {
      continue;
    }

    int bx = bulbNodes[i].anchorX / BULB_BUCKET_SIZE;
    int by = bulbNodes[i].anchorY / BULB_BUCKET_SIZE;
    if (bx < 0) {
      bx = 0;
    } else if (bx >= gBulbBucketCols) {
      bx = gBulbBucketCols - 1;
    }
    if (by < 0) {
      by = 0;
    } else if (by >= gBulbBucketRows) {
      by = gBulbBucketRows - 1;
    }

    int bucketIndex = by * gBulbBucketCols + bx;
    gBulbBucketNext[i] = gBulbBucketHeads[bucketIndex];
    gBulbBucketHeads[bucketIndex] = i;

    int reach = GetBulbMergeReach(&bulbNodes[i]);
    if (reach > gBulbMaxReach) {
      gBulbMaxReach = reach;
    }
  }

  gBulbBucketsDirty = false;
}

static void EnsureBulbBucketsReady(void) {
  int cols = (gridCols + BULB_BUCKET_SIZE - 1) / BULB_BUCKET_SIZE;
  int rows = (gridRows + BULB_BUCKET_SIZE - 1) / BULB_BUCKET_SIZE;
  if (cols < 1) {
    cols = 1;
  }
  if (rows < 1) {
    rows = 1;
  }
  if (cols != gBulbBucketCols || rows != gBulbBucketRows) {
    gBulbBucketsDirty = true;
  }
  if (gBulbBucketsDirty) {
    RebuildBulbBuckets();
  }
}

static void StampBulbCell(PixelCell *grid, int x, int y, CellType type) {
  if (x < 0 || x >= gridCols || y < 0 || y >= gridRows) {
    return;
  }
  PixelCell existing = GET_CELL(grid, x, y);
  if (IsImmutable(existing.type)) {
    return;
  }
  SET_CELL(grid, x, y, type, GetCellColor(type), 0);
}

static void StampBulbShape(PixelCell *grid, const BulbNode *node,
                           CellType blockType) {
  const BlockProperties *stackProps = &BlockRegistry[blockType];
  if (stackProps->stackShape == STACK_SHAPE_NONE) {
    StampBulbCell(grid, node->anchorX, node->anchorY, blockType);
    return;
  }

  int mass = node->mass < 1 ? 1 : node->mass;
  float stemRatio = (float)ClampInt(bulbStemBulbRatioPercent, 10, 90) / 100.0f;
  float stemBudget = (float)mass * stemRatio;
  int stemHeight = 2 + (int)floorf(sqrtf(stemBudget) * 0.55f);
  int stemHalfWidth = 1 + (int)floorf(sqrtf(stemBudget) * 0.20f);
  int capRadiusX = 0;
  int capRadiusY = 0;
  GetBulbShapeAxesFromMass(mass, &capRadiusX, &capRadiusY);
  int minStemHeight = capRadiusY / 2;
  if (minStemHeight < 2) {
    minStemHeight = 2;
  }
  if (stemHeight < minStemHeight) {
    stemHeight = minStemHeight;
  }
  int maxStemHalfWidth = capRadiusX - 2;
  if (maxStemHalfWidth < 1) {
    maxStemHalfWidth = 1;
  }
  if (stemHalfWidth > maxStemHalfWidth) {
    stemHalfWidth = maxStemHalfWidth;
  }

  int stemTopY = node->anchorY + stemHeight;
  int stemBottomY = node->anchorY + 1;
  for (int y = stemBottomY; y <= stemTopY; y++) {
    float t =
        stemHeight > 0 ? (float)(y - stemBottomY) / (float)stemHeight : 0.0f;
    int taperedHalfWidth =
        stemHalfWidth + (int)floorf(t * ((float)capRadiusX * 0.18f));
    for (int x = node->anchorX - taperedHalfWidth;
         x <= node->anchorX + taperedHalfWidth; x++) {
      StampBulbCell(grid, x, y, blockType);
    }
  }

  int centerX = node->anchorX;
  int centerY = stemTopY + (int)floorf((float)capRadiusY * 0.85f);
  for (int dy = -capRadiusY; dy <= capRadiusY; dy++) {
    for (int dx = -capRadiusX; dx <= capRadiusX; dx++) {
      float nx = capRadiusX > 0 ? (float)dx / (float)capRadiusX : 0.0f;
      float ny = capRadiusY > 0 ? (float)dy / (float)capRadiusY : 0.0f;
      if (ny < -0.55f) {
        continue;
      }
      if (nx * nx + ny * ny <= 1.0f) {
        StampBulbCell(grid, centerX + dx, centerY + dy, blockType);
      }
    }
  }

  int skirtRadiusX = capRadiusX + capRadiusX / 3 + 1;
  int skirtRadiusY = capRadiusY / 2;
  if (skirtRadiusY < 2) {
    skirtRadiusY = 2;
  }
  int skirtCenterY = stemTopY + (int)floorf((float)skirtRadiusY * 0.35f);
  for (int dy = -skirtRadiusY; dy <= skirtRadiusY; dy++) {
    for (int dx = -skirtRadiusX; dx <= skirtRadiusX; dx++) {
      float nx = skirtRadiusX > 0 ? (float)dx / (float)skirtRadiusX : 0.0f;
      float ny = skirtRadiusY > 0 ? (float)dy / (float)skirtRadiusY : 0.0f;
      if (ny < -0.25f) {
        continue;
      }
      if (nx * nx + ny * ny <= 1.0f) {
        StampBulbCell(grid, centerX + dx, skirtCenterY + dy, blockType);
      }
    }
  }

  // Guarantee contiguity between stem and crown/skirt across integer rounding
  // boundaries (prevents detached stems at some radius/mass combinations).
  int capLowestY = centerY - (int)floorf((float)capRadiusY * 0.55f);
  int skirtLowestY = skirtCenterY - (int)floorf((float)skirtRadiusY * 0.25f);
  int neckTopY = capLowestY < skirtLowestY ? capLowestY : skirtLowestY;
  if (neckTopY < stemTopY) {
    neckTopY = stemTopY;
  }
  int neckHalfWidth = stemHalfWidth + (int)floorf((float)capRadiusX * 0.15f);
  if (neckHalfWidth < 1) {
    neckHalfWidth = 1;
  }
  for (int y = stemTopY; y <= neckTopY; y++) {
    for (int x = centerX - neckHalfWidth; x <= centerX + neckHalfWidth; x++) {
      StampBulbCell(grid, x, y, blockType);
    }
  }
}

static bool AddBulbMassAt(int anchorX, int anchorY) {
  const BlockProperties *stackProps = &BlockRegistry[BULB_BLOCK];
  int nearbyIndex =
      FindBulbNodeByReach(anchorX, anchorY, stackProps->mergeAttachReachBias);
  if (nearbyIndex >= 0) {
    AddMassToBulbNode(nearbyIndex, 1);
    MergeBulbClusterAt(nearbyIndex);
    return true;
  }

  for (int i = 0; i < MAX_BULB_NODES; i++) {
    if (!bulbNodes[i].active) {
      bulbNodes[i] = (BulbNode){
          .active = true, .anchorX = anchorX, .anchorY = anchorY, .mass = 1};
      bulbNeedsRebuild = true;
      InvalidateBulbBuckets();
      MergeBulbClusterAt(i);
      return true;
    }
  }
  return false;
}

void ClearAllBulbs(void) {
  memset(bulbNodes, 0, sizeof(bulbNodes));
  bulbNeedsRebuild = false;
  bulbRebuildAccumulator = 0.0f;
  InvalidateBulbBuckets();
}

void AddMassToBulbNode(int index, int delta) {
  if (index < 0 || index >= MAX_BULB_NODES || !bulbNodes[index].active) {
    return;
  }
  if (delta > 0 && bulbNodes[index].mass > INT_MAX - delta) {
    bulbNodes[index].mass = INT_MAX;
  } else {
    bulbNodes[index].mass += delta;
  }
  if (bulbNodes[index].mass < 1) {
    bulbNodes[index].mass = 1;
  }
  int reach = GetBulbMergeReach(&bulbNodes[index]);
  if (reach > gBulbMaxReach) {
    gBulbMaxReach = reach;
  }
  bulbNeedsRebuild = true;
}

int FindBulbNodeByReach(int x, int y, int extraReach) {
  int bestIndex = -1;
  long long bestDistSq = LLONG_MAX;
  EnsureBulbBucketsReady();

  if (gBulbBucketHeads != NULL && gBulbBucketCols > 0 && gBulbBucketRows > 0) {
    int queryReach = gBulbMaxReach + extraReach;
    if (queryReach < 1) {
      queryReach = 1;
    }

    int minX = x - queryReach;
    int maxX = x + queryReach;
    int minY = y - queryReach;
    int maxY = y + queryReach;
    int minBx = minX / BULB_BUCKET_SIZE;
    int maxBx = maxX / BULB_BUCKET_SIZE;
    int minBy = minY / BULB_BUCKET_SIZE;
    int maxBy = maxY / BULB_BUCKET_SIZE;
    if (minBx < 0) {
      minBx = 0;
    }
    if (minBy < 0) {
      minBy = 0;
    }
    if (maxBx >= gBulbBucketCols) {
      maxBx = gBulbBucketCols - 1;
    }
    if (maxBy >= gBulbBucketRows) {
      maxBy = gBulbBucketRows - 1;
    }

    for (int by = minBy; by <= maxBy; by++) {
      for (int bx = minBx; bx <= maxBx; bx++) {
        int nodeIndex = gBulbBucketHeads[by * gBulbBucketCols + bx];
        while (nodeIndex >= 0) {
          if (bulbNodes[nodeIndex].active) {
            int dx = bulbNodes[nodeIndex].anchorX - x;
            int dy = bulbNodes[nodeIndex].anchorY - y;
            long long distSq = (long long)dx * (long long)dx +
                               (long long)dy * (long long)dy;
            int reach = GetBulbMergeReach(&bulbNodes[nodeIndex]) + extraReach;
            if (reach < 1) {
              reach = 1;
            }
            long long reachSq = (long long)reach * (long long)reach;
            if (distSq <= reachSq && distSq < bestDistSq) {
              bestDistSq = distSq;
              bestIndex = nodeIndex;
            }
          }
          nodeIndex = gBulbBucketNext[nodeIndex];
        }
      }
    }
  } else {
    for (int i = 0; i < MAX_BULB_NODES; i++) {
      if (!bulbNodes[i].active) {
        continue;
      }
      int dx = bulbNodes[i].anchorX - x;
      int dy = bulbNodes[i].anchorY - y;
      long long distSq = (long long)dx * (long long)dx +
                         (long long)dy * (long long)dy;
      int reach = GetBulbMergeReach(&bulbNodes[i]) + extraReach;
      if (reach < 1) {
        reach = 1;
      }
      long long reachSq = (long long)reach * (long long)reach;
      if (distSq <= reachSq && distSq < bestDistSq) {
        bestDistSq = distSq;
        bestIndex = i;
      }
    }
  }
  return bestIndex;
}

int FindNearestBulbNodeIndex(int x, int y) {
  int bestIndex = -1;
  long long bestDistSq = LLONG_MAX;
  for (int i = 0; i < MAX_BULB_NODES; i++) {
    if (!bulbNodes[i].active) {
      continue;
    }
    int dx = bulbNodes[i].anchorX - x;
    int dy = bulbNodes[i].anchorY - y;
    long long distSq = (long long)dx * (long long)dx +
                       (long long)dy * (long long)dy;
    if (distSq < bestDistSq) {
      bestDistSq = distSq;
      bestIndex = i;
    }
  }
  return bestIndex;
}

int MergeBulbNodeWithNearest(int index) {
  if (index < 0 || index >= MAX_BULB_NODES || !bulbNodes[index].active) {
    return -1;
  }
  const BlockProperties *stackProps = &BlockRegistry[BULB_BLOCK];
  if (stackProps->mergeBehavior == MERGE_NONE) {
    return -1;
  }
  float overlapRatio = stackProps->mergeOverlapRatio > 0.0f
                           ? stackProps->mergeOverlapRatio
                           : BULB_MERGE_OVERLAP_RATIO;
  overlapRatio = (float)ClampInt(bulbMergeOverlapPercent, 5, 90) / 100.0f;

  int bestPartner = -1;
  long long bestDistSq = LLONG_MAX;
  for (int i = 0; i < MAX_BULB_NODES; i++) {
    if (i == index || !bulbNodes[i].active) {
      continue;
    }
    int dx = bulbNodes[index].anchorX - bulbNodes[i].anchorX;
    int dy = bulbNodes[index].anchorY - bulbNodes[i].anchorY;
    long long distSq = (long long)dx * (long long)dx +
                       (long long)dy * (long long)dy;
    int reachA = GetBulbMergeReach(&bulbNodes[index]);
    int reachB = GetBulbMergeReach(&bulbNodes[i]);
    int minReach = reachA < reachB ? reachA : reachB;
    int requiredOverlap = (int)floorf((float)minReach * overlapRatio);
    if (requiredOverlap < 1) {
      requiredOverlap = 1;
    }
    int mergeDistance = reachA + reachB - requiredOverlap;
    if (mergeDistance < 1) {
      mergeDistance = 1;
    }

    long long mergeDistanceSq =
        (long long)mergeDistance * (long long)mergeDistance;
    if (distSq <= mergeDistanceSq && distSq < bestDistSq) {
      bestDistSq = distSq;
      bestPartner = i;
    }
  }

  if (bestPartner < 0) {
    return -1;
  }

  int massA = bulbNodes[index].mass;
  int massB = bulbNodes[bestPartner].mass;
  long long totalMass = (long long)massA + (long long)massB;
  if (totalMass > INT_MAX) {
    totalMass = INT_MAX;
  }
  long long weightedX = (long long)bulbNodes[index].anchorX * (long long)massA +
                        (long long)bulbNodes[bestPartner].anchorX *
                            (long long)massB;
  long long weightedY = (long long)bulbNodes[index].anchorY * (long long)massA +
                        (long long)bulbNodes[bestPartner].anchorY *
                            (long long)massB;
  int denominator = massA + massB;
  if (denominator < 1) {
    denominator = 1;
  }
  bulbNodes[index].anchorX = (int)(weightedX / denominator);
  bulbNodes[index].anchorY = (int)(weightedY / denominator);
  bulbNodes[index].mass = (int)totalMass;
  bulbNodes[bestPartner].active = false;
  bulbNeedsRebuild = true;
  InvalidateBulbBuckets();
  return index;
}

void MergeBulbClusterAt(int index) {
  int current = index;
  while (current >= 0) {
    int mergedIndex = MergeBulbNodeWithNearest(current);
    if (mergedIndex < 0) {
      break;
    }
    current = mergedIndex;
  }
}

void RebuildBulbs(void) {
  CellType stackType = BULB_BLOCK;
  for (int y = 0; y < gridRows; y++) {
    for (int x = 0; x < gridCols; x++) {
      if (GET_CELL(currentGrid, x, y).type == stackType) {
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
      InvalidateBulbBuckets();
      continue;
    }
    StampBulbShape(currentGrid, &bulbNodes[i], stackType);
  }
  bulbNeedsRebuild = false;
}

bool TryAbsorbBulbSeed(int x, int y) {
  int absorbIndex = FindBulbNodeByReach(x, y, 2);
  CellType belowType = GetResolvedCellType(x, y - 1);

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

  if (touchingBulb || belowType == BULB_BLOCK) {
    if (absorbIndex < 0) {
      absorbIndex = FindNearestBulbNodeIndex(x, y);
    }
    if (absorbIndex >= 0) {
      AddMassToBulbNode(absorbIndex, 1);
      MergeBulbClusterAt(absorbIndex);
      return true;
    }
  }

  if (IsSolid(belowType) && belowType != BULB_SEED && belowType != BULB_BLOCK) {
    return AddBulbMassAt(x, y - 1);
  }

  return false;
}
