# Handlords (PC Test Build) — Technical Design

**Target:** C++17, Linux, ImGui (no sound). Purpose: faithful prototype of the gameplay rules and AI to iterate on balance and UX.

---

## 1) High-level Goals

* Run the original tick-based simulation at **15 Hz** and resolve **240 adjacent pairs** per tick.
* Render a 40×24 arena using **ImGui**.
* Implement game flow: Level 1 → … → Level 5 → loop to Level 1; per-level ready screen; win/lose messages.
* Implement four AIs: **Albert, Beatrix, Chloe, Dimitri** (as specified in the design doc).
* Keep the code modular so later we can port logic to Z80/6502.

---

## 2) Tech Stack & Build

* **Tooling:** CMake (≥3.16), g++/clang++, vcpkg or system packages.
* **Libraries:** ImGui + backend (SDL2 + OpenGL3, or GLFW + OpenGL3). Choose SDL2 for input simplicity.
* **Executable:** `handlords_pc`

### Directory Layout

```
handlords/
  CMakeLists.txt
  external/ (imgui, imgui_backends, SDL2 headers if needed)
  src/
    main.cpp
    app/            (App bootstrap, rendering)
    core/           (game logic, tick, grid, rules)
    ai/             (opponent behaviors)
    levels/         (level definitions)
    util/           (rng, timers)
```

---

## 3) Core Data Structures

> Keep simple, flat data for speed and easy future porting.

```cpp
// Symbols and owners
enum class Piece : uint8_t { Rock=0, Paper=1, Scissors=2 };
struct PlayerId { uint8_t v; };  // 0 = human, 1..N = AI opponents

// Cell kinds
enum class CellKind : uint8_t { Empty=0, Wall=1, Symbol=2 };

struct Cell {
    CellKind kind;      // Empty/Wall/Symbol
    PlayerId owner;     // valid if kind==Symbol
    Piece    piece;     // valid if kind==Symbol
};

constexpr int ARENA_W = 40;
constexpr int ARENA_H = 24;

struct Grid {
    std::array<Cell, ARENA_W*ARENA_H> cells; // row-major
    Cell& at(int x,int y); // bounds-checked in debug
    const Cell& at(int x,int y) const;
};

struct LevelSpec {
    int id;
    // Initialization callback fills grid and sets per-player starting piece
    std::function<void(Grid&, std::vector<PlayerId>& players, std::vector<Piece>& startingPiece)> init;
};

struct GameConfig {
    int pairs_per_tick = 240;
    int ticks_per_second = 15;
};

struct PlayerState {
    PlayerId id;
    Piece    current;    // the player’s current piece (all symbols match this)
    uint16_t last_rot_tick = 0;
    // Optional for AIs below
    uint8_t  tick_losses = 0;   // Chloe
    uint8_t  rot_period = 0;    // Dimitri
    uint8_t  accel_ctr  = 0;    // Dimitri
};

struct GameState {
    Grid grid;
    GameConfig cfg;
    uint16_t tick = 0;          // global tick counter
    uint16_t rng16 = 0xACE1;    // LFSR state

    std::vector<PlayerState> players; // index 0 = human
    int current_level = 1;
    enum class Phase { Ready, Playing, Lost, Won, GameWon } phase = Phase::Ready;
};
```

---

## 4) Systems & Interfaces

### 4.1 PRNG (16-bit LFSR)

```cpp
uint16_t lfsr16_step(uint16_t& s); // taps 16,14,13,11 (poly 0xB400)
uint32_t rngu(GameState& gs);      // returns 0..65535, advances gs.rng16
```

### 4.2 Pair Selection

Two interchangeable strategies (select at startup via config or compile-time):

```cpp
// Random neighbor sampling
struct PairSelectorRandom {
    std::vector<std::pair<int,int>> select_pairs(const GameState& gs, int count);
};

// Checkerboard sweep with jittered start
struct PairSelectorSweep {
    int phase = 0; // flip each tick
    std::vector<std::pair<int,int>> select_pairs(const GameState& gs, int count);
};
```

`select_pairs` returns a vector of **cell indices** (or (x,y)) where the neighbor direction is chosen internally (N/E/S/W).

### 4.3 Combat Resolution

```cpp
// Applies one interaction between cell A and its chosen neighbor B
void resolve_pair(GameState& gs, int x, int y, int nx, int ny);

// Applies N interactions per tick
void resolve_pairs(GameState& gs, int count); // count=cfg.pairs_per_tick
```

Rules implemented exactly as spec: walls/empties noop; copy to empty; same owner noop; same piece diff owners 50/50; different pieces per RPS.

### 4.4 Rotation

```cpp
enum class RotDir { Next, Prev };
void rotate_all_of_player(GameState& gs, PlayerState& p, RotDir dir=RotDir::Next);
```

Updates the player’s `current` and **sweeps the grid** to change all owned symbols to the new piece.

### 4.5 AI Interface & Opponents

```cpp
struct IAI {
    virtual ~IAI() = default;
    virtual void update(GameState& gs, PlayerState& self) = 0; // called once per tick
};

struct AlbertAI   : IAI { void update(GameState&, PlayerState&) override; };
struct BeatrixAI  : IAI { void update(GameState&, PlayerState&) override; };
struct ChloeAI    : IAI { void update(GameState&, PlayerState&) override; };
struct DimitriAI  : IAI { void update(GameState&, PlayerState&) override; };
```

**Albert**: every 15–100 ticks, rotate **Next** (use RNG to pick interval).
**Beatrix**: rotate **Next** every fixed period (e.g., 60 ticks).
**Chloe**: with probability that rises with `tick_losses`, rotate **Prev**; reset `tick_losses` at tick start.
**Dimitri**: rotate **Next** when `tick - last_rot_tick >= rot_period`; reduce `rot_period` over time down to `ROT_MIN`.

### 4.6 Level Loader

```cpp
LevelSpec make_level1();
LevelSpec make_level2();
LevelSpec make_level3();
LevelSpec make_level4();
LevelSpec make_level5();

void load_level(GameState& gs, int level_id);
```

Level 1–4: outer wall; left half player(0), right half opponent(N).
Level 5: outer wall; player(0) at bottom-left single cell; opponents B,C,D at the other corners.

### 4.7 Victory / Loss Check

```cpp
bool is_level_won(const GameState& gs);   // "all opponents eliminated"
bool is_level_lost(const GameState& gs);  // optionally: human eliminated
```

### 4.8 Input & Flow Control

```cpp
void handle_input_ready(GameState& gs);   // wait for key to start
void handle_input_play(GameState& gs);    // Space = rotate player(0) Next

void step_ready(GameState& gs);           // transitions to Playing
void step_play(GameState& gs);            // advances tick, AIs, pairs, checks win/lose
void step_lost(GameState& gs);            // show message; key → Level 1 Ready
void step_won(GameState& gs);             // advance N; key → next level Ready
void step_gamewon(GameState& gs);         // show message; key → Level 1 Ready
```

---

## 5) ImGui Integration (Rendering)

* **Arena view:** draw a fixed-size grid; one ImGui window `"Arena"`.
* Each cell as a small filled rect; color by player id (palette), walls as dark gray, empty as background.
* Optional overlay text: tick counter, pairs-per-tick, current level, phase.

```cpp
struct Renderer {
    void draw(const GameState& gs);
};
```

Implementation hint: Use `ImDrawList` (from `ImGui::GetWindowDrawList()`) to draw rectangles aligned to a cell size computed from window size.

---

## 6) Main Loop (PC)

```cpp
int main() {
    App app; // sets up SDL2 + ImGui + GL
    GameState gs; init_game(gs);
    load_level(gs, 1);

    const double fixed_dt = 1.0 / gs.cfg.ticks_per_second; // 1/15
    double acc = 0.0; double last = now();

    while (app.poll()) {
        double t = now(); acc += (t - last); last = t;
        app.new_frame();

        // Phase handling & fixed-step simulation
        while (acc >= fixed_dt) {
            step_fixed(gs); // calls step_ready/play/won/lost based on gs.phase
            acc -= fixed_dt;
        }

        // Draw
        app.render(gs);
        app.present();
    }
}
```

`step_fixed(gs)` does:

* If `Ready`: wait for key; on key → `Playing`.
* If `Playing`: `gs.tick++`; reset `tick_losses` for all players; run AIs; resolve `cfg.pairs_per_tick`; test win/lose; set phase.
* If `Lost/Won/GameWon`: show message; on key, transition per spec.

---

## 7) File/Module List

* `src/app/App.cpp/.h` — SDL2 + ImGui bootstrapping, event loop, frame timing.
* `src/app/Renderer.cpp/.h` — grid rendering via ImGui draw lists.
* `src/core/Grid.cpp/.h` — grid access, helpers, flood/scan utilities.
* `src/core/Rules.cpp/.h` — pair selection + combat resolution + rotation.
* `src/core/Game.cpp/.h` — GameState, step functions, flow control, victory/lose checks.
* `src/ai/*.cpp/.h` — `IAI` and the four opponent implementations.
* `src/levels/*.cpp/.h` — level initializers.
* `src/util/Rng.cpp/.h` — LFSR implementation.

---

## 8) Key Functions (Signatures)

```cpp
// Grid helpers
inline int idx(int x,int y) { return y*ARENA_W + x; }
bool in_bounds(int x,int y);

// Pair selection
std::pair<int,int> pick_neighbor(int x,int y, uint16_t r); // returns (nx,ny)

// Rules
void resolve_pairs(GameState& gs, int count);
void resolve_pair(GameState& gs, int x,int y,int nx,int ny);
void rotate_all_of_player(GameState& gs, PlayerState& p, RotDir dir);

// AIs
void update_albert (GameState& gs, PlayerState& self);
void update_beatrix(GameState& gs, PlayerState& self);
void update_chloe  (GameState& gs, PlayerState& self);
void update_dimitri(GameState& gs, PlayerState& self);

// Flow
void step_fixed(GameState& gs);
bool is_level_won(const GameState& gs);
bool is_level_lost(const GameState& gs);

// Levels
void load_level(GameState& gs, int lvl);
```

---

## 9) Test & Debug Aids

* Toggle overlays: show tick, pairs-per-tick, RNG state.
* Hot-reload level: key `R` to reload current level.
* AI sandbox: override human to any AI for testing.
* Determinism toggle (fixed seed) to reproduce bugs.

---

## 10) Milestones / Sub‑Tasks

1. **Scaffold project**: CMake, SDL2+ImGui window, render empty grid.
2. **Grid model & rendering**: draw walls/empties/symbols; palette per player id.
3. **Tick loop**: fixed-step at 15 Hz, counters & accumulator.
4. **Pair selection + rules**: implement 240 interactions per tick.
5. **Rotation & input**: player(0) Space = rotate Next; sweep update.
6. **Game flow**: Ready/Playing/Won/Lost/GameWon screens.
7. **AIs**: Albert → Beatrix → Chloe (add `tick_losses`) → Dimitri (add `rot_period/accel_ctr`).
8. **Levels 1–5**: loaders and transitions.
9. **Debug UI**: overlays & deterministic mode.
10. **Polish pass**: code cleanup, perf check, freezes/edge cases.

---

> This PC build mirrors the rules closely while keeping the code portable and data‑oriented. Later we can split the pure logic from rendering to prep for Z80/6502-specific ports.
