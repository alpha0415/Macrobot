#pragma once

#include <Geode/Geode.hpp>
#include <filesystem>
#include <string>
#include <vector>

using namespace geode::prelude;

// ---- macro data -----------------------------------------------------------

struct MInput {
    int frame = 0;
    int btn = 1;
    bool p2 = false;
    bool down = false;
};

// Per-frame player snapshot used by the "Frame Fixes" accuracy mode.
struct MFix {
    int frame = 0;
    float x1 = 0, y1 = 0, r1 = 0;
    double v1 = 0;
    bool has2 = false;
    float x2 = 0, y2 = 0, r2 = 0;
    double v2 = 0;
};

struct Macro {
    std::vector<MInput> inputs;
    std::vector<MFix> fixes;
    std::string name = "untitled";
    std::string author;
    std::string description;
    std::string levelName;
    int levelId = 0;
    float fps = 240.f;
    unsigned int seed = 0;

    void clear() {
        inputs.clear();
        fixes.clear();
    }
    bool empty() const { return inputs.empty(); }
    int lastFrame() const { return inputs.empty() ? 0 : inputs.back().frame; }
};

// ---- settings -------------------------------------------------------------

struct Cfg {
    bool noclip = false;
    bool safeMode = true;       // level can never be completed
    bool instantRespawn = false;
    bool noDeathEffect = false;
    bool autoSave = false;
    bool stepper = false;
    bool autoclick = false;
    bool acP1 = true;
    bool acP2 = false;
    bool speedOn = false;
    bool tpsOn = false;
    float speed = 1.f;
    float tps = 240.f;
    int offset = 0;
    int accuracy = 2;           // 0 Vanilla, 1 Input Fixes, 2 Frame Fixes
    int fixInterval = 4;        // record a snapshot every N steps (Frame Fixes)
    int acHold = 2;
    int acRel = 2;

    void load();
    void save() const;
};

enum class BotState { None, Record, Play };

class Bot {
public:
    static Bot& get();

    BotState state = BotState::None;
    Macro macro;
    Cfg cfg;

    int lastPlayed = -1;
    bool restoreHeld = false;
    bool injecting = false;
    int stepReq = 0;
    bool skipDelayed = false;
    double skipDelayedAt = 0.0;

    Ref<CCMenuItemSpriteExtra> hudRec;
    Ref<CCMenuItemSpriteExtra> hudPlay;
    Ref<CCMenuItemSpriteExtra> hudStep;
    Ref<CCMenuItemSpriteExtra> hudStep10;

    static int frame();
    static bool inLevel(GJBaseGameLayer* gl);

    void setState(BotState s);
    void onReset(int frame);
    void playFrame(GJBaseGameLayer* gl, int f);
    void recordFix(GJBaseGameLayer* gl, int f);
    void autoclickFrame(GJBaseGameLayer* gl, int f);

    float effectiveTps() const { return cfg.tpsOn ? cfg.tps : 240.f; }
    void applyTimeScale();
    void refreshHud();

    std::filesystem::path macroDir();
    std::filesystem::path autoDir();
    bool saveMacro(std::string const& name, std::string const& author, std::string const& desc, bool autosave = false);
    bool loadMacro(std::filesystem::path const& path);
    void captureLevelInfo();
    std::string info() const;
};

// UI (ui.cpp)
void openBotMenu();
void startRun();
