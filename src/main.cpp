#include <SDL.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <random>

// ImGui
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

// ----------------- Basic Types -----------------
namespace hl
{
    constexpr int ARENA_W = 40;
    constexpr int ARENA_H = 24;

    enum class CellKind : uint8_t
    {
        Empty = 0,
        Wall = 1,
        Symbol = 2
    };
    enum class Piece : uint8_t
    {
        Rock = 0,
        Paper = 1,
        Scissors = 2
    };

    struct PlayerId
    {
        uint8_t v{0};
    };

    struct Cell
    {
        CellKind kind{CellKind::Empty};
        PlayerId owner{0};
        Piece piece{Piece::Rock};
    };

    struct Grid
    {
        std::array<Cell, ARENA_W * ARENA_H> cells{}; // zero-initialized
        static constexpr int idx(int x, int y) { return y * ARENA_W + x; }
        Cell &at(int x, int y) { return cells[idx(x, y)]; }
        const Cell &at(int x, int y) const { return cells[idx(x, y)]; }
        void clear() { cells.fill(Cell{}); }
    };

    struct GameConfig
    {
        int pairs_per_tick{240};
        int ticks_per_second{15};
    };

    struct PlayerState
    {
        PlayerId id{0};
        Piece current{Piece::Rock};
        uint16_t last_rot_tick{0};
        // Minimal AI fields; more later
        uint8_t tick_losses{0};
        uint8_t rot_period{0};
        uint8_t accel_ctr{0};
    };

    struct AlbertConfig
    {
        int rotation_average{58}; // Average rotation interval (default: 58 ticks)
        int rotation_half_interval{43}; // Half interval size (default: 43, gives range 15-100)
    };

    enum class Phase
    {
        Ready,
        Playing,
        Lost,
        Won,
        GameWon
    };

    struct GameState
    {
        Grid grid{};
        GameConfig cfg{};
        uint16_t tick{0};
        uint16_t rng16{0xACE1};
        std::vector<PlayerState> players; // 0 = human
        int current_level{1};
        Phase phase{Phase::Ready};
        int last_battles{0}; // Track battles for debugging
        bool use_system_rng{false}; // Option to use std::mt19937 instead of LFSR
        std::mt19937 system_rng{std::random_device{}()}; // System RNG
        int last_attempts{0}; // Total pair attempts
        int last_same_player{0}; // Same player pairs
        int last_wall_empty{0}; // Wall/empty pairs
        AlbertConfig albert_config; // Add Albert configuration
    };
}

// ----------------- Utility -----------------
static uint16_t lfsr16_step(uint16_t &s)
{
    // taps: 16,14,13,11 (poly 0xB400)
    uint16_t bit = ((s >> 0) ^ (s >> 2) ^ (s >> 3) ^ (s >> 5)) & 1u;
    s = (s >> 1) | (bit << 15);
    return s;
}

static uint32_t rngu(hl::GameState &gs)
{
    if (gs.use_system_rng) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        return gen() & 0xFFFF; // Return 16-bit value like LFSR
    } else {
        return lfsr16_step(gs.rng16);
    }
}

static bool in_bounds(int x, int y)
{
    return x >= 0 && x < hl::ARENA_W && y >= 0 && y < hl::ARENA_H;
}

static std::pair<int, int> pick_neighbor(int x, int y, uint16_t r)
{
    // Pick one of 4 neighbors: N, E, S, W
    int dir = r & 3;
    switch (dir)
    {
    case 0:
        return {x, y - 1}; // North
    case 1:
        return {x + 1, y}; // East
    case 2:
        return {x, y + 1}; // South
    case 3:
        return {x - 1, y}; // West
    }
    return {x, y}; // shouldn't happen
}

// ----------------- Level Init -----------------
static void load_level1(hl::GameState &gs)
{
    using namespace hl;
    gs.grid.clear();
    // Border walls
    for (int x = 0; x < ARENA_W; ++x)
    {
        gs.grid.at(x, 0).kind = CellKind::Wall;
        gs.grid.at(x, ARENA_H - 1).kind = CellKind::Wall;
    }
    for (int y = 0; y < ARENA_H; ++y)
    {
        gs.grid.at(0, y).kind = CellKind::Wall;
        gs.grid.at(ARENA_W - 1, y).kind = CellKind::Wall;
    }
    // Left half player(0), right half opponent(1)
    for (int y = 1; y < ARENA_H - 1; ++y)
    {
        for (int x = 1; x < ARENA_W - 1; ++x)
        {
            if (x < ARENA_W / 2)
            {
                gs.grid.at(x, y).kind = hl::CellKind::Symbol;
                gs.grid.at(x, y).owner = hl::PlayerId{0};
                gs.grid.at(x, y).piece = gs.players[0].current;
            }
            else
            {
                gs.grid.at(x, y).kind = hl::CellKind::Symbol;
                gs.grid.at(x, y).owner = hl::PlayerId{1};
                gs.grid.at(x, y).piece = gs.players[1].current;
            }
        }
    }
}

// ----------------- AI Update -----------------
static void update_albert_ai(hl::GameState &gs, hl::PlayerState &player)
{
    using namespace hl;
    
    // Albert: rotate based on configurable interval
    if (player.rot_period == 0) {
        // Initialize random rotation period using configured parameters
        uint16_t r = rngu(gs);
        int min_interval = gs.albert_config.rotation_average - gs.albert_config.rotation_half_interval;
        int max_interval = gs.albert_config.rotation_average + gs.albert_config.rotation_half_interval;
        // Ensure minimum of 1 tick
        min_interval = std::max(1, min_interval);
        int range = max_interval - min_interval + 1;
        player.rot_period = min_interval + (r % range);
    }
    
    // Check if it's time to rotate
    if (gs.tick - player.last_rot_tick >= player.rot_period) {
        // Rotate to next piece
        player.current = static_cast<Piece>((static_cast<int>(player.current) + 1) % 3);
        player.last_rot_tick = gs.tick;
        
        // Update all Albert's symbols on the grid
        for (int y = 0; y < ARENA_H; ++y) {
            for (int x = 0; x < ARENA_W; ++x) {
                auto &cell = gs.grid.at(x, y);
                if (cell.kind == CellKind::Symbol && cell.owner.v == player.id.v) {
                    cell.piece = player.current;
                }
            }
        }
        
        // Pick new random interval for next rotation using configured parameters
        uint16_t r = rngu(gs);
        int min_interval = gs.albert_config.rotation_average - gs.albert_config.rotation_half_interval;
        int max_interval = gs.albert_config.rotation_average + gs.albert_config.rotation_half_interval;
        // Ensure minimum of 1 tick
        min_interval = std::max(1, min_interval);
        int range = max_interval - min_interval + 1;
        player.rot_period = min_interval + (r % range);
    }
}

// ----------------- Combat Resolution -----------------
static void resolve_pair(hl::GameState &gs, int x, int y, int nx, int ny)
{
    using namespace hl;

    if (!in_bounds(nx, ny))
        return;

    Cell &a = gs.grid.at(x, y);
    Cell &b = gs.grid.at(nx, ny);

    // Rule 1: If one is a wall, nothing happens
    if (a.kind == CellKind::Wall || b.kind == CellKind::Wall)
        return;

    // Rule 2: If both are empty, nothing happens
    if (a.kind == CellKind::Empty && b.kind == CellKind::Empty)
        return;

    // Rule 3: If one is empty and other is symbol, copy symbol to empty
    if (a.kind == CellKind::Empty && b.kind == CellKind::Symbol)
    {
        a = b; // copy symbol to empty space
        return;
    }
    if (b.kind == CellKind::Empty && a.kind == CellKind::Symbol)
    {
        b = a; // copy symbol to empty space
        return;
    }

    // Rule 4: If both are symbols from same player, nothing happens
    if (a.kind == CellKind::Symbol && b.kind == CellKind::Symbol)
    {
        if (a.owner.v == b.owner.v)
            return;

        // Rule 5: Same symbols from different players - 50/50 chance
        if (a.piece == b.piece)
        {
            uint16_t r = rngu(gs);
            if (r & 1)
            {
                // a wins, b loses
                PlayerId loser = b.owner;
                b = a; // a wins
                if (loser.v < gs.players.size())
                    gs.players[loser.v].tick_losses++;
            }
            else
            {
                // b wins, a loses
                PlayerId loser = a.owner;
                a = b; // b wins
                if (loser.v < gs.players.size())
                    gs.players[loser.v].tick_losses++;
            }
            return;
        }

        // Rule 6: Different symbols - Rock-Paper-Scissors rules
        bool a_wins = false;

        if (a.piece == Piece::Rock && b.piece == Piece::Scissors)
            a_wins = true;
        else if (a.piece == Piece::Scissors && b.piece == Piece::Paper)
            a_wins = true;
        else if (a.piece == Piece::Paper && b.piece == Piece::Rock)
            a_wins = true;

        if (a_wins)
        {
            // a wins, b loses
            PlayerId loser = b.owner;
            b = a; // a wins
            if (loser.v < gs.players.size())
                gs.players[loser.v].tick_losses++;
        }
        else
        {
            // b wins, a loses
            PlayerId loser = a.owner;
            a = b; // b wins
            if (loser.v < gs.players.size())
                gs.players[loser.v].tick_losses++;
        }
    }
}

static void resolve_pairs(hl::GameState &gs, int count)
{
    // Random pair selection strategy
    int battles_count = 0;
    int same_player_count = 0;
    int wall_empty_count = 0;
    
    for (int i = 0; i < count; ++i)
    {
        // Pick a random cell
        uint16_t r1 = rngu(gs);
        uint16_t r2 = rngu(gs);
        int x = r1 % hl::ARENA_W;
        int y = r2 % hl::ARENA_H;

        // Pick a random neighbor
        uint16_t r3 = rngu(gs);
        auto [nx, ny] = pick_neighbor(x, y, r3);

        // Count interaction types
        if (in_bounds(nx, ny)) {
            const auto &a = gs.grid.at(x, y);
            const auto &b = gs.grid.at(nx, ny);
            
            if (a.kind == hl::CellKind::Wall || b.kind == hl::CellKind::Wall || 
                a.kind == hl::CellKind::Empty || b.kind == hl::CellKind::Empty) {
                wall_empty_count++;
            } else if (a.kind == hl::CellKind::Symbol && b.kind == hl::CellKind::Symbol) {
                if (a.owner.v == b.owner.v) {
                    same_player_count++;
                } else {
                    battles_count++;
                }
            }
        }

        resolve_pair(gs, x, y, nx, ny);
    }
    
    // Store stats for debug display
    gs.last_battles = battles_count;
    gs.last_attempts = count;
    gs.last_same_player = same_player_count;
    gs.last_wall_empty = wall_empty_count;
}

// ----------------- Rendering -----------------
static void draw_grid_imgui(const hl::GameState &gs)
{
    // Set up the Arena window to be large and prominent - FirstUseEver allows user to move/resize
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1000, 700), ImGuiCond_FirstUseEver);
    
    ImGui::Begin("Arena", nullptr, ImGuiWindowFlags_NoCollapse);
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    // Show debug info about window size
    ImGui::Text("Window size: %.0f x %.0f", avail.x, avail.y);
    ImGui::Text("Window pos: %.0f, %.0f", ImGui::GetWindowPos().x, ImGui::GetWindowPos().y);
    ImGui::Text("Cursor pos: %.0f, %.0f", ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y);

    // Compute cell size to fit grid while preserving aspect
    const float cell_w = avail.x / hl::ARENA_W;
    const float cell_h = avail.y / hl::ARENA_H;
    const float cell = std::max(8.0f, std::min(cell_w, cell_h)); // Minimum 8 pixels per cell

    ImGui::Text("Cell size: %.1f pixels", cell);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    auto color_for_player = [](uint8_t pid) -> ImU32
    {
        // Simple palette: up to 4 players
        static const ImU32 k[] = {
            IM_COL32(80, 200, 120, 255), // human - greenish
            IM_COL32(220, 80, 80, 255),  // opponent 1 - red
            IM_COL32(80, 120, 220, 255), // opponent 2 - blue
            IM_COL32(220, 200, 80, 255), // opponent 3 - yellow
        };
        return k[pid % 4];
    };

    const ImU32 wall_col = IM_COL32(80, 80, 80, 255);
    const ImU32 empty_bg = IM_COL32(25, 25, 28, 255);

    // Background
    dl->AddRectFilled(origin, ImVec2(origin.x + cell * hl::ARENA_W, origin.y + cell * hl::ARENA_H), empty_bg);

    // Cells
    for (int y = 0; y < hl::ARENA_H; ++y)
    {
        for (int x = 0; x < hl::ARENA_W; ++x)
        {
            const auto &c = gs.grid.at(x, y);
            ImVec2 p0(origin.x + x * cell, origin.y + y * cell);
            ImVec2 p1(p0.x + cell - 1.0f, p0.y + cell - 1.0f);
            switch (c.kind)
            {
            case hl::CellKind::Empty:
                break;
            case hl::CellKind::Wall:
                dl->AddRectFilled(p0, p1, wall_col);
                break;
            case hl::CellKind::Symbol:
            {
                ImU32 col = color_for_player(c.owner.v);
                dl->AddRectFilled(p0, p1, col);
                
                // Add text character to show piece type
                const char* piece_char = "R"; // Default to Rock
                if (c.piece == hl::Piece::Paper)
                    piece_char = "P";
                else if (c.piece == hl::Piece::Scissors)
                    piece_char = "S";
                
                // Calculate text position (centered in cell)
                float font_size = std::max(8.0f, cell * 0.6f);
                ImVec2 text_pos(p0.x + cell * 0.5f - font_size * 0.3f, p0.y + cell * 0.5f - font_size * 0.5f);
                
                // Add white text with black outline for visibility
                ImU32 text_color = IM_COL32(255, 255, 255, 255);
                ImU32 outline_color = IM_COL32(0, 0, 0, 255);
                
                // Simple outline effect by drawing text at offset positions
                for (int dx = -1; dx <= 1; dx++) {
                    for (int dy = -1; dy <= 1; dy++) {
                        if (dx != 0 || dy != 0) {
                            dl->AddText(ImVec2(text_pos.x + dx, text_pos.y + dy), outline_color, piece_char);
                        }
                    }
                }
                dl->AddText(text_pos, text_color, piece_char);
            }
            break;
            }
        }
    }

    ImGui::End();
}

// ----------------- Debug UI -----------------
static void draw_debug_ui(hl::GameState &gs)
{
    // Position the debug window to the right of the arena - FirstUseEver allows user to move/resize
    ImGui::SetNextWindowPos(ImVec2(1020, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 400), ImGuiCond_FirstUseEver);
    
    ImGui::Begin("Game State");

    const char *phase_names[] = {"Ready", "Playing", "Lost", "Won", "GameWon"};
    ImGui::Text("Phase: %s", phase_names[static_cast<int>(gs.phase)]);
    ImGui::Text("Tick: %d", gs.tick);
    ImGui::Text("Level: %d", gs.current_level);
    ImGui::Text("RNG: 0x%04X", gs.rng16);
    ImGui::Text("Battles this tick: %d", gs.last_battles);
    
    // RNG selection checkbox
    ImGui::Separator();
    ImGui::Checkbox("Use System RNG", &gs.use_system_rng);
    ImGui::Text("(LFSR may have poor distribution)");

    ImGui::Separator();
    ImGui::Text("Players:");
    for (size_t i = 0; i < gs.players.size(); ++i)
    {
        const auto &p = gs.players[i];
        const char *piece_names[] = {"Rock", "Paper", "Scissors"};
        ImGui::Text("Player %d: %s (losses: %d)",
                    p.id.v, piece_names[static_cast<int>(p.current)], p.tick_losses);
    }
    
    // Count symbols for each player
    ImGui::Separator();
    ImGui::Text("Symbol counts:");
    int counts[4] = {0, 0, 0, 0}; // support up to 4 players
    for (int y = 0; y < hl::ARENA_H; ++y) {
        for (int x = 0; x < hl::ARENA_W; ++x) {
            const auto &c = gs.grid.at(x, y);
            if (c.kind == hl::CellKind::Symbol && c.owner.v < 4) {
                counts[c.owner.v]++;
            }
        }
    }
    for (size_t i = 0; i < gs.players.size(); ++i) {
        ImGui::Text("Player %zu: %d symbols", i, counts[i]);
    }

    if (gs.phase == hl::Phase::Ready)
    {
        ImGui::Separator();
        ImGui::Text("Press SPACE to start!");
    }
    else if (gs.phase == hl::Phase::Playing)
    {
        ImGui::Separator();
        ImGui::Text("Press SPACE to rotate your piece!");
    }
    else if (gs.phase == hl::Phase::Won)
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "YOU WON!");
        ImGui::Text("Press SPACE to restart level");
    }
    else if (gs.phase == hl::Phase::Lost)
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "YOU LOST!");
        ImGui::Text("Press SPACE to restart level");
    }

    // Enhanced pair statistics
    ImGui::Separator();
    ImGui::Text("Per-tick statistics:");
    ImGui::Text("Battles: %d", gs.last_battles);
    ImGui::Text("Same player pairs: %d", gs.last_same_player);
    ImGui::Text("Wall/empty pairs: %d", gs.last_wall_empty);
    ImGui::Text("Total attempts: %d", gs.last_attempts);

    // Legend for the arena graphics
    ImGui::Separator();
    ImGui::Text("Arena Legend:");
    ImGui::Text("Dark gray = Walls (borders)");
    ImGui::Text("Black = Empty space");
    ImGui::TextColored(ImVec4(0.31f, 0.78f, 0.47f, 1.0f), "Green = Your territory (Player 0)");
    ImGui::TextColored(ImVec4(0.86f, 0.31f, 0.31f, 1.0f), "Red = Opponent territory (Player 1)");
    ImGui::Separator();
    ImGui::Text("Symbol characters:");
    ImGui::Text("R = Rock");
    ImGui::Text("P = Paper");
    ImGui::Text("S = Scissors");

    ImGui::End();
}

// ----------------- Tuning UI -----------------
static void draw_tuning_ui(hl::GameState &gs)
{
    // Position the tuning window to the left of other windows
    ImGui::SetNextWindowPos(ImVec2(10, 450), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 300), ImGuiCond_FirstUseEver);
    
    ImGui::Begin("Game Tuning");

    // Combat statistics section
    ImGui::Text("Combat Performance (per 15 ticks):");
    ImGui::Separator();
    
    // Calculate combats per 15 ticks (1 second at 15 Hz)
    static int combat_history[15] = {0}; // Ring buffer for last 15 ticks
    static int history_index = 0;
    
    // Update history
    combat_history[history_index] = gs.last_battles;
    history_index = (history_index + 1) % 15;
    
    // Calculate total combats in last 15 ticks
    int total_combats = 0;
    for (int i = 0; i < 15; i++) {
        total_combats += combat_history[i];
    }
    
    ImGui::Text("Combats per second: %d", total_combats);
    ImGui::Text("Current tick battles: %d", gs.last_battles);
    ImGui::Text("Efficiency: %.1f%%", gs.last_attempts > 0 ? (float)gs.last_battles / gs.last_attempts * 100.0f : 0.0f);
    
    ImGui::Separator();
    
    // Game parameters section
    ImGui::Text("Game Parameters:");
    ImGui::SliderInt("Pairs per tick", &gs.cfg.pairs_per_tick, 50, 500);
    ImGui::SliderInt("Ticks per second", &gs.cfg.ticks_per_second, 5, 30);
    
    if (ImGui::Button("Reset to Default")) {
        gs.cfg.pairs_per_tick = 240;
        gs.cfg.ticks_per_second = 15;
    }
    
    ImGui::Separator();
    
    // Albert AI section
    ImGui::Text("Albert AI (Player 1):");
    
    // Configuration controls
    ImGui::SliderInt("Rotation Average", &gs.albert_config.rotation_average, 10, 200);
    ImGui::SliderInt("Half Interval Size", &gs.albert_config.rotation_half_interval, 5, 100);
    
    // Display current interval range
    int min_interval = std::max(1, gs.albert_config.rotation_average - gs.albert_config.rotation_half_interval);
    int max_interval = gs.albert_config.rotation_average + gs.albert_config.rotation_half_interval;
    ImGui::Text("Current interval range: %d - %d ticks", min_interval, max_interval);
    
    if (gs.players.size() > 1) {
        auto &albert = gs.players[1];
        
        ImGui::Text("Current piece: %s", 
                   albert.current == hl::Piece::Rock ? "Rock" :
                   albert.current == hl::Piece::Paper ? "Paper" : "Scissors");
        
        ImGui::Text("Last rotation tick: %d", albert.last_rot_tick);
        ImGui::Text("Next rotation in: %d ticks", 
                   albert.rot_period > 0 ? 
                   (int)albert.rot_period - (int)(gs.tick - albert.last_rot_tick) : 0);
        
        // Manual controls for testing
        if (ImGui::Button("Force Albert Rotation")) {
            albert.current = static_cast<hl::Piece>((static_cast<int>(albert.current) + 1) % 3);
            albert.last_rot_tick = gs.tick;
            
            // Reset rotation period to get new random interval with current config
            albert.rot_period = 0;
            
            // Update all Albert's symbols on the grid
            for (int y = 0; y < hl::ARENA_H; ++y) {
                for (int x = 0; x < hl::ARENA_W; ++x) {
                    auto &cell = gs.grid.at(x, y);
                    if (cell.kind == hl::CellKind::Symbol && cell.owner.v == 1) {
                        cell.piece = albert.current;
                    }
                }
            }
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Reset Albert Timer")) {
            albert.rot_period = 0; // Will reinitialize on next AI update
        }
        
        if (ImGui::Button("Reset Albert Config")) {
            gs.albert_config.rotation_average = 58;
            gs.albert_config.rotation_half_interval = 43;
            albert.rot_period = 0; // Will reinitialize with new config
        }
        
        // Display AI parameters (read-only for now)
        ImGui::Text("Rotation interval: 15-100 ticks (random)");
        ImGui::Text("Current interval: %d ticks", albert.rot_period);
    }

    ImGui::End();
}

// ----------------- App Bootstrap -----------------
static bool init_sdl(SDL_Window **outWin, SDL_Renderer **outRen)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0)
        return false;
    SDL_Window *win = SDL_CreateWindow("Handlords", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1400, 800, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win)
        return false;
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
        return false;
    *outWin = win;
    *outRen = ren;
    return true;
}

static void init_imgui(SDL_Window *win, SDL_Renderer *ren)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io; // Suppress unused warning
    
    ImGui::StyleColorsDark();
    
    // Remove all scaling - let's see if this fixes the coordinate issues
    // ImGuiStyle& style = ImGui::GetStyle();
    // style.ScaleAllSizes(1.2f); // Commented out
    
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
}

static void shutdown_imgui()
{
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

// ----------------- Game Flow -----------------
static void step_fixed(hl::GameState &gs)
{
    using namespace hl;

    switch (gs.phase)
    {
    case Phase::Ready:
        // Wait for key to start - handled in input
        break;

    case Phase::Playing:
    {
        gs.tick++;

        // Reset tick losses at START of tick
        for (auto &player : gs.players)
        {
            player.tick_losses = 0;
        }

        // Resolve pairs
        resolve_pairs(gs, gs.cfg.pairs_per_tick);

        // Check win/lose conditions
        int player_counts[4] = {0, 0, 0, 0};
        for (int y = 0; y < hl::ARENA_H; ++y) {
            for (int x = 0; x < hl::ARENA_W; ++x) {
                const auto &c = gs.grid.at(x, y);
                if (c.kind == CellKind::Symbol && c.owner.v < 4) {
                    player_counts[c.owner.v]++;
                }
            }
        }
        
        // Check if any player has won (controls all territory)
        if (player_counts[0] == 0 && player_counts[1] > 0) {
            gs.phase = Phase::Lost;
        } else if (player_counts[1] == 0 && player_counts[0] > 0) {
            gs.phase = Phase::Won;
        }

        // Run AI updates
        for (size_t i = 1; i < gs.players.size(); ++i) {
            if (i == 1) {
                // Player 1 is Albert
                update_albert_ai(gs, gs.players[i]);
            }
            // TODO: Add other AIs (Beatrix, Chloe, Dimitri) later
        }
        break;
    }

    case Phase::Lost:
    case Phase::Won:
    case Phase::GameWon:
        // Wait for key to continue - handled in input
        break;
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv; // Suppress unused parameter warnings
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    if (!init_sdl(&window, &renderer))
        return 1;
    init_imgui(window, renderer);

    // Game state
    hl::GameState gs;
    gs.players = {hl::PlayerState{hl::PlayerId{0}, hl::Piece::Rock},
                  hl::PlayerState{hl::PlayerId{1}, hl::Piece::Scissors}};
    load_level1(gs);

    auto last = std::chrono::high_resolution_clock::now();
    double acc = 0.0;
    const double fixed_dt = 1.0 / gs.cfg.ticks_per_second;

    bool running = true;
    while (running)
    {
        // Handle events
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT)
                running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                running = false;

            // Game input
            if (e.type == SDL_KEYDOWN)
            {
                if (gs.phase == hl::Phase::Ready && e.key.keysym.sym == SDLK_SPACE)
                {
                    gs.phase = hl::Phase::Playing;
                }
                else if (gs.phase == hl::Phase::Playing && e.key.keysym.sym == SDLK_SPACE)
                {
                    // Rotate player 0's piece
                    auto &player = gs.players[0];
                    player.current = static_cast<hl::Piece>((static_cast<int>(player.current) + 1) % 3);
                    player.last_rot_tick = gs.tick;

                    // Update all player 0's symbols on the grid
                    for (int y = 0; y < hl::ARENA_H; ++y)
                    {
                        for (int x = 0; x < hl::ARENA_W; ++x)
                        {
                            auto &cell = gs.grid.at(x, y);
                            if (cell.kind == hl::CellKind::Symbol && cell.owner.v == 0)
                            {
                                cell.piece = player.current;
                            }
                        }
                    }
                }
                else if ((gs.phase == hl::Phase::Won || gs.phase == hl::Phase::Lost) && e.key.keysym.sym == SDLK_SPACE)
                {
                    // Restart the level
                    gs.phase = hl::Phase::Ready;
                    gs.tick = 0;
                    // Reset player pieces to default
                    gs.players[0].current = hl::Piece::Rock;
                    gs.players[1].current = hl::Piece::Scissors;
                    // Reset player stats
                    for (auto &player : gs.players) {
                        player.tick_losses = 0;
                        player.last_rot_tick = 0;
                        player.rot_period = 0; // Reset AI timers
                    }
                    // Reload the level
                    load_level1(gs);
                }
            }
        }

        // Timing
        auto now = std::chrono::high_resolution_clock::now();
        acc += std::chrono::duration<double>(now - last).count();
        last = now;

        // New frame
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Fixed-step simulation
        while (acc >= fixed_dt)
        {
            step_fixed(gs);
            acc -= fixed_dt;
        }

        // UI
        draw_grid_imgui(gs);
        draw_debug_ui(gs);
        draw_tuning_ui(gs);

        // Render ImGui to SDL2 renderer
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    shutdown_imgui();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
