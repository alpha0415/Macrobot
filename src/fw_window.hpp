#pragma once

#include <Geode/Geode.hpp>
#include <cstddef>

using namespace geode::prelude;

// Frame window: for every macro input, find out (by simulating a ghost player
// ahead of time) how many physics ticks the input could have been placed
// earlier / later without dying, then show a ring + sound + "N f" label
// (in 60 FPS frames) when the window is small enough.
namespace Win {
    extern bool simulating;     // a ghost preview is running
    extern bool simDied;        // the ghost died during the preview
    extern bool simHitPortal;   // the ghost reached something the preview can't handle
    extern float playerDt;      // dt the game hands the real player's physics step

    bool isGhost(PlayerObject* p);

    // Forget old results (new run / respawn); inputs before `frame` are skipped.
    void sync(size_t inputCount, int frame);
    // Drop the ghost player (leaving the level).
    void clear();

    // Called right after the macro injected input `idx`.
    void onInput(GJBaseGameLayer* gl, size_t idx, int frame);
    // Called once per tick after inputs were injected; precomputes upcoming windows.
    void compute(GJBaseGameLayer* gl, int frame);
}
