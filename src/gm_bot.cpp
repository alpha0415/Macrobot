#include <limits>
#include "fw_bot.hpp"
#include "json.hpp"
#include "fw_window.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <ctime>
#include <map>

using json = nlohmann::json;

// ---- Cfg ------------------------------------------------------------------

void Cfg::load() {
    auto m = Mod::get();
    noclip = m->getSavedValue<bool>("noclip", false);
    safeMode = m->getSavedValue<bool>("safe_mode", true);
    instantRespawn = m->getSavedValue<bool>("instant_respawn", false);
    noDeathEffect = m->getSavedValue<bool>("no_death_effect", false);
    autoSave = m->getSavedValue<bool>("auto_save", false);
    acP1 = m->getSavedValue<bool>("ac_p1", true);
    acP2 = m->getSavedValue<bool>("ac_p2", false);
    speed = m->getSavedValue<double>("speed", 1.0);
    tps = m->getSavedValue<double>("tps", 240.0);
    offset = m->getSavedValue<int>("offset", 0);
    accuracy = m->getSavedValue<int>("accuracy", 2);
    acHold = m->getSavedValue<int>("ac_hold", 2);
    acRel = m->getSavedValue<int>("ac_rel", 2);
    windowOn = m->getSavedValue<bool>("window_on", false);
    windowMax = m->getSavedValue<int>("window_max", 1);
    // speedhack / tps / stepper / autoclicker always start disabled
}

void Cfg::save() const {
    auto m = Mod::get();
    m->setSavedValue<bool>("noclip", noclip);
    m->setSavedValue<bool>("safe_mode", safeMode);
    m->setSavedValue<bool>("instant_respawn", instantRespawn);
    m->setSavedValue<bool>("no_death_effect", noDeathEffect);
    m->setSavedValue<bool>("auto_save", autoSave);
    m->setSavedValue<bool>("ac_p1", acP1);
    m->setSavedValue<bool>("ac_p2", acP2);
    m->setSavedValue<double>("speed", speed);
    m->setSavedValue<double>("tps", tps);
    m->setSavedValue<int>("offset", offset);
    m->setSavedValue<int>("accuracy", accuracy);
    m->setSavedValue<int>("ac_hold", acHold);
    m->setSavedValue<int>("ac_rel", acRel);
    m->setSavedValue<bool>("window_on", windowOn);
    m->setSavedValue<int>("window_max", windowMax);
}

// ---- Bot ------------------------------------------------------------------

Bot& Bot::get() {
    static Bot inst;
    return inst;
}

int Bot::frameNow() {
    auto pl = PlayLayer::get();
    if (!pl) return 0;
    auto& b = Bot::get();
    int f = static_cast<int>(std::llround(pl->m_gameState.m_levelTime * b.effectiveTps())) + 1;
    f -= b.cfg.offset;
    return f < 0 ? 0 : f;
}

bool Bot::inLevel(GJBaseGameLayer* gl) {
    auto pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == gl;
}

void Bot::captureLevelInfo() {
    auto pl = PlayLayer::get();
    if (!pl || !pl->m_level) return;
    macro.levelId = pl->m_level->m_levelID.value();
    macro.levelName = pl->m_level->m_levelName;
    macro.fps = effectiveTps();
}

void Bot::setState(BotState s) {
    if (state == BotState::Record && s != BotState::Record) {
        if (cfg.autoSave && !macro.empty()) saveMacro("auto", "", "", true);
    }
    state = s;
    previousFrame = -1;
    curAction = 0;
    curFix = 0;
    desyncShown = false;
    restoreHeld = false;
    if (s == BotState::Record) {
        macro.clear();
        captureLevelInfo();
    }
    if (s == BotState::Play) {
        restoreHeld = true;
    }
    refreshHud();
}

// Called right after PlayLayer::resetLevel() (death, restart, practice respawn).
void Bot::onReset(int f) {
    previousFrame = -1;
    if (f <= 1) desyncShown = false;
    if (state == BotState::Record) {
        // Drop everything that happened after the point we respawned at.
        macro.inputs.erase(
            std::remove_if(macro.inputs.begin(), macro.inputs.end(), [&](MInput const& i) { return i.frame >= f; }),
            macro.inputs.end());
        macro.fixes.erase(
            std::remove_if(macro.fixes.begin(), macro.fixes.end(), [&](MFix const& x) { return x.frame >= f; }),
            macro.fixes.end());
        if (f > 0) {
            // The game releases every button on respawn: mirror that in the macro.
            std::map<std::pair<bool, int>, bool> held;
            for (auto const& i : macro.inputs) held[{i.p2, i.btn}] = i.down;
            for (auto const& [key, down] : held) {
                if (down) macro.inputs.push_back({f, key.second, key.first, false});
            }
        }
    } else if (state == BotState::Play) {
        syncCursors(f);
        restoreHeld = f > 1;
    }
}

void Bot::recordFix(GJBaseGameLayer* gl, int f) {
    if (!macro.fixes.empty() && macro.fixes.back().frame == f) return;
    if (!gl->m_player1) return;
    MFix x;
    x.frame = f;
    x.x1 = gl->m_player1->getPositionX();
    x.y1 = gl->m_player1->getPositionY();
    x.r1 = gl->m_player1->getRotation();
    x.v1 = gl->m_player1->m_yVelocity;
    if (gl->m_levelSettings && gl->m_levelSettings->m_twoPlayerMode && gl->m_player2) {
        x.has2 = true;
        x.x2 = gl->m_player2->getPositionX();
        x.y2 = gl->m_player2->getPositionY();
        x.r2 = gl->m_player2->getRotation();
        x.v2 = gl->m_player2->m_yVelocity;
    }
    macro.fixes.push_back(x);
}

void Bot::syncCursors(int frame) {
    Win::sync(macro.inputs.size(), frame);
    curAction = std::lower_bound(macro.inputs.begin(), macro.inputs.end(), frame,
                                 [](MInput const& a, int v) { return a.frame < v; }) - macro.inputs.begin();
    curFix = std::upper_bound(macro.fixes.begin(), macro.fixes.end(), frame,
                              [](int v, MFix const& a) { return v < a.frame; }) - macro.fixes.begin();
}

// Runs right AFTER the game processed its own commands for this tick (same order xdBot uses).
void Bot::handlePlaying(GJBaseGameLayer* gl, int frame) {
    if (!gl->m_player1) return;
    if (gl->m_player1->m_isDead) {
        gl->m_player1->releaseAllButtons();
        if (gl->m_player2) gl->m_player2->releaseAllButtons();
        return;
    }

    auto press = [&](bool down, int btn, bool p2) {
        injecting = true;
        gl->handleButton(down, btn, !p2);
        injecting = false;
    };

    if (restoreHeld) {
        restoreHeld = false;
        std::map<std::pair<bool, int>, bool> held;
        for (size_t i = 0; i < curAction && i < macro.inputs.size(); i++) {
            held[{macro.inputs[i].p2, macro.inputs[i].btn}] = macro.inputs[i].down;
        }
        for (auto const& [k, down] : held) {
            if (down) press(true, k.second, k.first);
        }
    }

    while (curAction < macro.inputs.size() && frame >= macro.inputs[curAction].frame) {
        auto const& in = macro.inputs[curAction];
        press(in.down, in.btn, in.p2);
        Win::onInput(gl, curAction, frame);
        curAction++;
    }

    while (curFix < macro.fixes.size() && frame >= macro.fixes[curFix].frame) {
        auto const& fx = macro.fixes[curFix];
        if (fx.frame == frame) {
            // Diagnostic: report the first frame where the replay differs from the recording.
            float dx = gl->m_player1->getPositionX() - fx.x1;
            float dy = gl->m_player1->getPositionY() - fx.y1;
            if (!desyncShown && (std::fabs(dx) > 0.05f || std::fabs(dy) > 0.05f)) {
                desyncShown = true;
                Notification::create(fmt::format("DESYNC frame {}  dx={:.2f} dy={:.2f}", frame, dx, dy),
                                     NotificationIcon::Error, 5.f)->show();
            }
        }
        if (cfg.accuracy >= 1) {
            gl->m_player1->setPosition({fx.x1, fx.y1});
            gl->m_player1->setRotation(fx.r1);
            if (!std::isnan(fx.v1)) gl->m_player1->m_yVelocity = fx.v1;
            if (fx.has2 && gl->m_player2) {
                gl->m_player2->setPosition({fx.x2, fx.y2});
                gl->m_player2->setRotation(fx.r2);
                if (!std::isnan(fx.v2)) gl->m_player2->m_yVelocity = fx.v2;
            }
        }
        curFix++;
    }

    Win::compute(gl, frame);
}

// Frame Fixes: store a snapshot after every tick (inputs store their own snapshot too).
void Bot::handleRecording(GJBaseGameLayer* gl, int frame) {
    if (!gl->m_player1 || gl->m_player1->m_isDead) return;
    if (cfg.accuracy >= 2 && cfg.fixInterval > 0 && frame % cfg.fixInterval == 0) recordFix(gl, frame);
}

void Bot::autoclickFrame(GJBaseGameLayer* gl, int f) {
    int hold = std::max(1, cfg.acHold);
    int rel = std::max(1, cfg.acRel);
    int phase = f % (hold + rel);
    if (phase != 0 && phase != hold) return;
    bool down = phase == 0;
    if (cfg.acP1) gl->handleButton(down, 1, true);
    if (cfg.acP2 && gl->m_levelSettings && gl->m_levelSettings->m_twoPlayerMode) gl->handleButton(down, 1, false);
}

void Bot::applyTimeScale() {
    // Speedhack is applied by the CCScheduler::update hook in main.cpp.
}

void Bot::refreshHud() {
    auto tint = [](CCMenuItemSpriteExtra* b, bool on, ccColor3B c) {
        if (!b || !b->getParent()) return;
        if (auto spr = typeinfo_cast<CCSprite*>(b->getNormalImage())) spr->setColor(on ? c : ccColor3B{255, 255, 255});
    };
    tint(hudRec, state == BotState::Record, {255, 70, 70});
    tint(hudPlay, state == BotState::Play, {80, 255, 80});
    for (auto b : {hudStep.data(), hudStep10.data()}) {
        if (b && b->getParent()) b->setVisible(cfg.stepper);
    }
}

// ---- files ----------------------------------------------------------------

std::filesystem::path Bot::macroDir() {
    auto p = std::filesystem::path(Mod::get()->getSaveDir().string()) / "macros";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

std::filesystem::path Bot::autoDir() {
    auto p = std::filesystem::path(Mod::get()->getSaveDir().string()) / "autosaves";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

static std::string sanitize(std::string s) {
    for (auto& c : s) {
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
    }
    if (s.empty()) s = "macro";
    return s;
}

bool Bot::saveMacro(std::string const& name, std::string const& author, std::string const& desc, bool autosave) {
    if (macro.empty()) return false;

    macro.name = name;
    macro.author = author;
    macro.description = desc;

    json j;
    j["author"] = author;
    j["description"] = desc;
    j["duration"] = macro.lastFrame() / std::max(1.f, macro.fps);
    j["framerate"] = macro.fps;
    j["gameVersion"] = 2.2074;
    j["version"] = 1;
    j["seed"] = macro.seed;
    j["botInfo"] = {{"name", "MacroBotMobile"}, {"version", "1.0.0"}};
    j["level"] = {{"id", macro.levelId}, {"name", macro.levelName}};
    j["inputs"] = json::array();
    for (auto const& i : macro.inputs) {
        json e = {{"frame", i.frame}, {"btn", i.btn}, {"2p", i.p2}, {"down", i.down}};
        if (i.half) e["half"] = true;
        j["inputs"].push_back(e);
    }
    j["fixes"] = json::array();
    for (auto const& x : macro.fixes) {
        json e = {{"frame", x.frame}, {"p1", {x.x1, x.y1, x.r1, x.v1}}};
        if (x.half) e["half"] = true;
        if (x.has2) e["p2"] = {x.x2, x.y2, x.r2, x.v2};
        j["fixes"].push_back(e);
    }

    std::filesystem::path dir = autosave ? autoDir() : macroDir();
    std::string base = sanitize(autosave ? ("auto_" + std::to_string(std::time(nullptr))) : name);
    std::filesystem::path path = dir / (base + ".gdr.json");
    if (!autosave) {
        int n = 1;
        std::error_code ec;
        while (std::filesystem::exists(path, ec)) {
            path = dir / (base + "_" + std::to_string(n++) + ".gdr.json");
        }
    }

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << j.dump();
    return f.good();
}

bool Bot::loadMacro(std::filesystem::path const& path) {
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        json j = json::parse(f, nullptr, true, true);

        Macro m;
        m.author = j.value("author", "");
        m.description = j.value("description", "");
        m.fps = j.value("framerate", 240.f);
        m.seed = j.value("seed", 0u);
        if (j.contains("level") && j["level"].is_object()) {
            m.levelId = j["level"].value("id", 0);
            m.levelName = j["level"].value("name", "");
        }
        m.name = path.filename().string();

        for (auto const& e : j.at("inputs")) {
            MInput i;
            i.frame = e.value("frame", 0);
            i.btn = e.value("btn", 1);
            i.p2 = e.value("2p", false);
            i.down = e.value("down", false);
            i.half = e.value("half", false);
            m.inputs.push_back(i);
        }
        std::stable_sort(m.inputs.begin(), m.inputs.end(), [](MInput const& a, MInput const& b) { return a.frame * 2 + (a.half ? 1 : 0) < b.frame * 2 + (b.half ? 1 : 0); });

        if (j.contains("fixes")) {
            for (auto const& e : j["fixes"]) {
                MFix x;
                x.frame = e.value("frame", 0);
                x.half = e.value("half", false);
                auto p1 = e.at("p1");
                x.x1 = p1.at(0); x.y1 = p1.at(1); x.r1 = p1.at(2);
                x.v1 = p1.size() > 3 ? p1.at(3).get<float>() : std::numeric_limits<float>::quiet_NaN();   // imported macros have no velocity
                if (e.contains("p2")) {
                    auto p2 = e["p2"];
                    x.has2 = true;
                    x.x2 = p2.at(0); x.y2 = p2.at(1); x.r2 = p2.at(2);
                    x.v2 = p2.size() > 3 ? p2.at(3).get<float>() : std::numeric_limits<float>::quiet_NaN();
                }
                m.fixes.push_back(x);
            }
            std::stable_sort(m.fixes.begin(), m.fixes.end(), [](MFix const& a, MFix const& b) { return a.frame * 2 + (a.half ? 1 : 0) < b.frame * 2 + (b.half ? 1 : 0); });
        }

        macro = std::move(m);
        Win::sync(macro.inputs.size(), 0);
        if (macro.fps >= 1.f && macro.fps <= 100000.f && std::fabs(macro.fps - 240.f) > 0.01f) {
            cfg.tpsOn = true;
            cfg.tps = macro.fps;
        } else {
            cfg.tpsOn = false;   // normal 240 TPS macro: don't keep an old TPS override
        }
        return !macro.empty();
    } catch (...) {
        return false;
    }
}

std::string Bot::info() const {
    if (macro.empty()) return "No macro loaded";
    float secs = macro.lastFrame() / std::max(1.f, macro.fps);
    return fmt::format("{}: {} inputs, {:.1f}s @ {:.0f} TPS", macro.name, macro.inputs.size(), secs, macro.fps);
}
