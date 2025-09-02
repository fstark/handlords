# Retro Game Design Document

## Game Title

**Handlords**

## Concept

A retro-inspired game built around the core idea of
**rock-paper-scissors**, expanded into a system where the computer
manages several contenders instead of simple one-on-one matches.

## Arena

-   Fullscreen playfield: **40 x 24 cells** (classic old-school screen
    size).
-   Each cell can contain:
    -   Empty space
    -   Wall
    -   Player symbol (Rock, Paper, or Scissors)
    -   Opponent symbol (Rock, Paper, or Scissors)
-   All player symbols are identical (all Rock, or all Paper, or all
    Scissors).
-   Each opponent also has uniform symbols of their type.

## Game Loop

-   The game runs in **ticks** (\~1/15 s).
-   Each tick, **240 adjacent symbol pairs** are resolved.

## Game Flow

1.  **Ready -- Level 1**: Load **Level 1** and wait for keypress to
    begin.
2.  **Play -- Level N**: Run the tick loop; check the victory condition
    each tick.
3.  **If Lost**: Display **"You Lost"** and return to the **Level 1
    ready screen**.
4.  **If Won and N \< MaxLevels**: Advance to **Level N+1 ready
    screen**, wait for keypress, then play.
5.  **If Won and N == MaxLevels**: Display **"You Won"** and return to
    the **Level 1 ready screen**.

-   The winner is declared when **all opponents have been eliminated**
    (no symbols left for them).
-   A **time-based score** will be recorded internally, but not
    displayed to the player.

## Combat Resolution Rules

1.  If one of them is a **wall** → nothing happens.
2.  If both are **empty** → nothing happens.
3.  If one is **empty** and the other is a symbol → the symbol is copied
    into the empty space.
4.  If two symbols from the **same player/opponent** meet → nothing
    happens.
5.  If two **same symbols** from different players meet → 50/50 chance
    for one to win.
6.  If two **different symbols** from different players meet → outcome
    decided by Rock-Paper-Scissors rules:
    -   Rock beats Scissors
    -   Scissors beats Paper
    -   Paper beats Rock

## Player & Opponent Actions

-   The **player** can press **Space** at any time → all their symbols
    rotate (Rock → Paper → Scissors → Rock).
-   The **computer-controlled opponents** can also perform the same
    rotation action on their own sets of symbols.

## Levels

-   **Level 1**: A wall surrounds the outer arena border. The **left
    side** is filled with player hands, and the **right side** is filled
    with opponent 1's hands.
-   **Level 2**: Same as Level 1, but against **opponent 2**.
-   **Level 3**: Same as Level 1, but against **opponent 3**.
-   **Level 4**: Same as Level 1, but against **opponent 4**.
-   **Level 5**: A wall surrounds the outer arena border. The **player**
    starts with a single symbol in the **bottom-left corner**. Opponents
    **B, C, and D** each start with a single symbol in the other three
    corners.

## Opponent Roster & AI

-   **Albert**: Simplest of the lot. Every few ticks (randomly between
    15 and 100), Albert rotates all of his symbols to the **next type**
    (Rock → Paper → Scissors → Rock).
-   **Beatrix**: **Fixed Rhythm Opponent**. Rotates to the **next type**
    at a fixed cadence, e.g., every **60 ticks** (\~4 seconds).
    Implementation: if `tick - last_rot[Beatrix] >= ROT_PERIOD_BX` then
    rotate; set `last_rot[Beatrix] = tick`. Deterministic; no RNG
    required.
-   **Chloe**: **Reactive Reverse Opponent**. Prefers stability but
    reacts to losses; rotates to the **previous type** (Rock ← Paper ←
    Scissors ← Rock) with a probability that increases with **cells lost
    this tick**.
    -   Needs: `tick_losses[p]` (u8) per player (reset at tick start;
        increment on each lost cell).
    -   Impl (per tick): `loss = tick_losses[Chloe]` (cap at 15).
        Threshold `T = BASE + (loss << SHIFT)`; if `(rng16 & 0x3F) < T`
        then rotate **previous**; `last_rot[Chloe] = tick`.
-   **Dimitri**: **Accelerating Metronome**. Starts slow and
    **accelerates** his rotation cadence over time within limits.
    -   Params: `ROT_START_DM` (u8), `ROT_MIN_DM` (u8), `ACCEL_EVERY_DM`
        (u8 ticks), `ACCEL_STEP_DM` (u8).
    -   Needs per-player state: `rot_period[p]` (u8), `accel_ctr[p]`
        (u8).
    -   Init: `rot_period[Dimitri] = ROT_START_DM`,
        `accel_ctr[Dimitri] = 0`.
    -   Per tick:
        1.  `accel_ctr++`; if `accel_ctr >= ACCEL_EVERY_DM` then
            `accel_ctr = 0; if rot_period > ROT_MIN_DM then rot_period -= ACCEL_STEP_DM`.
        2.  If `tick - last_rot[Dimitri] >= rot_period[Dimitri]` then
            rotate to **next type**; set `last_rot[Dimitri] = tick`.
    -   Deterministic; no RNG required.

## AI-Accessible Game Stats (Minimal Set)

### Core

-   `tick` (u16) -- global tick counter
-   `last_rot[p]` (u16) -- tick of last rotation for player `p`
-   `rng16` (u16) -- global random number generator state

### Player Info

-   `current_player` (u8) -- index of player being processed
-   `player_symbol[p]` (u8; 0=Rock,1=Paper,2=Scissors) -- current symbol
    type of each player
-   `tick_losses[p]` (u8) -- **cells lost this tick** by player `p`
    (reset at tick start)
-   `rot_period[p]` (u8) -- current rotation cadence (Dimitri)
-   `accel_ctr[p]` (u8) -- acceleration counter (Dimitri)
