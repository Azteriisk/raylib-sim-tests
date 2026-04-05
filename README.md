# Cellular Automata Sandbox (raylib + C)

This project is a real-time 2D cellular simulation sandbox, not just a starter template.
It includes:
- Data-driven block definitions via a `BlockRegistry`.
- Runtime-tunable simulation settings (UI sliders).
- Input-to-action mapping for paint tools.
- Property-driven rendering (static color or per-block shader function).
- Advanced stacked growth behavior (current example: bulb cloud growth and merge logic).

Core logic is now split into focused modules.

## Build and Run

### Prerequisites
- `gcc` (or compatible C compiler)
- `make`
- raylib static lib/header present in this repo setup (`lib/libraylib.a`, `lib/raylib.h`)

### Build
```bash
make
```

### Run
```bash
./main
```
On Windows PowerShell:
```powershell
.\main.exe
```

## Current Controls

- `Left Click`: paint `SAND`
- `Right Click`: paint `SHADER_BLOCK`
- `Space`: paint `WATER`
- `E`: erase (paint `EMPTY`, immutable blocks are preserved)
- `B`: paint `BULB_SEED`
- `C`: clear world

UI sliders (in-game):
- `Cell Px`
- `Brush Px`
- `Gravity Px`
- `Stem:Bulb %`
- `Merge %`

## Framework Overview

### 1. Blocks are defined in one registry

`BlockRegistry[]` (in `sim_state.c`) is the source of truth for per-block behavior and visuals.

`BlockProperties` currently supports:
- Physics and occupancy:
  - `isSolid`
  - `isImmutable`
  - `affectedByGravity`
  - `spreadsLikeLiquid`
  - `mass`
  - `gravityMoveChance`
  - `spreadMoveChance`
  - `spreadDistanceCells`
- Rendering:
  - `baseColor`
  - `colorShader` (function pointer; optional)
  - `outlineMode`
  - `outlineColor`
  - `outlineThicknessCells`
- Stack/merge behavior:
  - `stackShape`
  - `mergeBehavior`
  - `mergeOverlapRatio`
  - `mergeAttachReachBias`

### 2. Input actions are binding-driven

`kInputBindings[]` (in `input_ui.c`) maps device input to `CellType` paint output.

### 3. Simulation uses behavior modules

Core update flow:
- Input paint stamping into the grid.
- Physics step (gravity/spread).
- Specialized behavior paths (for example bulb seed absorption into bulb nodes).
- Render pass with optional shader callbacks and optional exterior outlines.

## Adding Custom Inputs

1. Add or reuse a `CellType` in the enum.
2. Add an entry to `kInputBindings[]`:
```c
{INPUT_BIND_KEY, KEY_Q, MY_NEW_BLOCK},
```
3. Update instruction text if you want it shown in the HUD.

## Adding Custom Blocks

1. Add enum value in `CellType`.
2. Add a `BlockRegistry` entry:
```c
[MY_NEW_BLOCK] = {
  .isSolid = true,
  .affectedByGravity = true,
  .baseColor = {180, 120, 90, 255},
  .mass = 1.2f,
  .gravityMoveChance = 1.0f,
},
```
3. Optionally bind it to input (`kInputBindings[]`).

## Adding Custom Behaviors

There are two levels:

- Property-only behaviors:
  - Gravity and spread behavior are already fully property-driven.
  - Outlines and color shader are property-driven.

- Custom systems:
  - Bulb growth/merge uses a dedicated node system (`bulbNodes`).
  - To add a new advanced behavior family, copy that pattern:
    - Add data structure for that family.
    - Add update/absorb/rebuild functions.
    - Gate usage through new `BlockProperties` fields.

## Adding Custom Color Shaders

Shader callback signature:
```c
typedef Color (*ColorShaderFn)(int x, int y, float timeAlive, Color baseColor);
```

Implement a function:
```c
Color GetMyBlockShader(int x, int y, float timeAlive, Color baseColor) {
  // Use baseColor as your anchor and modulate over space/time.
  return baseColor;
}
```

Attach it in `BlockRegistry`:
```c
[MY_SHADER_BLOCK] = {
  .baseColor = {120, 200, 255, 255},
  .colorShader = GetMyBlockShader,
},
```

If `colorShader == NULL`, the engine renders `baseColor` directly.

## Notes on Architecture

- `sim.h`: shared types, globals, constants, and public function interfaces.
- `main.c`: app entrypoint and Win32 live-resize hook.
- `sim_state.c`: global state, block registry, grid memory/state helpers, utility math.
- `physics_render.c`: movement rules, per-cell physics step, render pipeline, color shader functions.
- `input_ui.c`: key/mouse bindings, slider UI, paint brush logic, frame tick orchestration.
- `bulb.c`: bulb growth system, merge logic, and stacked-shape rebuild.

The framework is data-driven at the block-property level, while advanced families (like bulb growth) stay modular and can be expanded similarly.
