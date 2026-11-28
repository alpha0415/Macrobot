#include "fw_bot.hpp"
#include "fw_window.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <cmath>

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

$on_mod(Loaded) {
    Bot::get().cfg.load();
}

// ---------------------------------------------------------------------------
// GJBaseGameLayer: input recording / playback, TPS bypass, frame stepper
// ---------------------------------------------------------------------------

namespace Layout { extern bool on; extern bool building; bool isOfficial(int id); std::string modify(std::string const& s); }

// true only while the bot or a hack is really in use (never during normal play)
static bool botActive() {
    auto& b = Bot::get();
    if (!b.cfg.safeMode) return false;
    return b.state != BotState::None || b.cfg.noclip || b.cfg.speedOn ||
           Layout::on || b.cfg.tpsOn || b.cfg.stepper || b.cfg.autoclick || b.cfg.instantRespawn;
}

namespace Kb { void click(bool down, bool macroPlaying); }

// ---- Turbo clicks (high CPS while recording) and the frame stepper HUD -------
namespace Turbo {
    bool on = false;        // setting
    int cps = 30;           // clicks per second
    bool held = false;      // finger is on the screen
    bool isDown = false;    // current virtual button state
    bool p1 = true;
    int start = 0;
    bool inj = false;       // we are injecting a click ourselves
}

namespace {
class StepMenu : public CCMenu {
public:
    void onStep(CCObject*) { Bot::get().stepReq += 1; }
    void onStep10(CCObject*) { Bot::get().stepReq += 10; }
    static StepMenu* make() {
        auto m = new StepMenu();
        if (m && m->init()) {
            m->autorelease();
            m->setPosition({0.f, 0.f});
            m->setTag(9912);
            m->setZOrder(1001);
            auto ws = CCDirector::get()->getWinSize();
            auto mk = [&](char const* t, SEL_MenuHandler h, float y) {
                auto sp = ButtonSprite::create(t, "bigFont.fnt", "GJ_button_01.png", .8f);
                sp->setScale(.55f);
                auto b = CCMenuItemSpriteExtra::create(sp, m, h);
                b->setPosition({ws.width - 46.f, y});
                m->addChild(b);
            };
            mk("Step", menu_selector(StepMenu::onStep), ws.height / 2 + 22.f);
            mk("x10", menu_selector(StepMenu::onStep10), ws.height / 2 - 22.f);
            return m;
        }
        delete m;
        return nullptr;
    }
};
}

namespace Step {
    // Shows the small Step / x10 buttons only while the stepper is on.
    void sync() {
        auto pl = PlayLayer::get();
        if (!pl) return;
        auto cur = pl->getChildByTag(9912);
        bool want = Bot::get().cfg.stepper;
        if (want && !cur) pl->addChild(StepMenu::make());
        else if (!want && cur) cur->removeFromParent();
    }
}

// ---- CPS meter (top-left) -----------------------------------------------------
namespace CpsMeter {
    bool on = true;
    std::deque<double> times;   // level time of recent clicks
    double first = -1.0, last = -1.0;
    int total = 0;
    float best = 0.f;

    void reset() { times.clear(); first = last = -1.0; total = 0; best = 0.f; }
    void click(double t) {
        times.push_back(t);
        if (first < 0.0) first = t;
        last = t;
        ++total;
    }
    float now(double t) {
        while (!times.empty() && times.front() < t - 1.0) times.pop_front();
        return static_cast<float>(times.size());
    }
}

static void cpsOverlay(GJBaseGameLayer* gl) {
    auto pl = PlayLayer::get();
    if (!pl || static_cast<GJBaseGameLayer*>(pl) != gl) return;
    auto& b = Bot::get();
    auto label = static_cast<CCLabelBMFont*>(pl->getChildByTag(9913));
    if (!CpsMeter::on) {
        if (label) label->removeFromParent();
        return;
    }
    if (!label) {
        label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setTag(9913);
        label->setAnchorPoint({0.f, 1.f});
        label->setScale(.4f);
        label->setPosition({8.f, CCDirector::get()->getWinSize().height - 6.f});
        label->setZOrder(1000);
        pl->addChild(label);
    }
    double t = gl->m_gameState.m_levelTime;
    float n = CpsMeter::now(t);
    CpsMeter::best = std::max(CpsMeter::best, n);
    float avg = (CpsMeter::total >= 2 && CpsMeter::last > CpsMeter::first)
        ? static_cast<float>((CpsMeter::total - 1) / (CpsMeter::last - CpsMeter::first)) : 0.f;
    label->setString(fmt::format("CPS {:.0f}  avg {:.1f}  max {:.0f}", n, avg, CpsMeter::best).c_str());
}

class $modify(BotBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& b = Bot::get();
        if (!Bot::inLevel(this) || (b.state == BotState::None && !b.cfg.autoclick)) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        // Same frame twice (half ticks etc.): let the game run, don't handle the macro again.
        if (b.state != BotState::None) {
            int before = Bot::frameNow();
            if (b.previousFrame == before && before != 0) {
                return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        int frame = Bot::frameNow();
        b.previousFrame = frame;

        if (b.state == BotState::Record) b.handleRecording(this, frame);
        else if (b.state == BotState::Play) b.handlePlaying(this, frame);

        // Turbo clicks: while the finger is held during a recording, keep clicking at the chosen CPS.
        if (b.state == BotState::Record && Turbo::on && Turbo::held && m_player1 && !m_player1->m_isDead) {
            double tps = b.effectiveTps();
            double cps = std::min<double>(std::max(1, Turbo::cps), tps / 2.0);
            int el = frame - Turbo::start;
            if (el > 0) {
                long phase = static_cast<long>(el * 2.0 * cps / tps);
                bool want = (phase % 2) == 0;
                if (want != Turbo::isDown) {
                    Turbo::inj = true;
                    this->handleButton(want, 1, Turbo::p1);
                    Turbo::inj = false;
                    Turbo::isDown = want;
                }
            }
        }
        if (b.state != BotState::Record) Turbo::held = false;

        if (b.cfg.autoclick && b.state != BotState::Play && m_player1 && !m_player1->m_isDead) {
            b.autoclickFrame(this, frame);
        }
    }

    // No mirror-portal screen flip while recording (so the screen doesn't keep turning).
    // Playback and normal play are untouched.
    void toggleFlipped(bool flip, bool noEffects) {
        auto& b = Bot::get();
        if (b.state == BotState::Record && !noEffects && Bot::inLevel(this)) return;
        GJBaseGameLayer::toggleFlipped(flip, noEffects);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& b = Bot::get();
        if (!Bot::inLevel(this)) return GJBaseGameLayer::handleButton(down, button, isPlayer1);

        // While a macro plays, ignore real touches; only injected inputs go through.
        if (b.state == BotState::Play && !b.injecting) return;

        if (b.state == BotState::Record && Turbo::on && !Turbo::inj && button == 1 && m_player1 && !m_player1->m_isDead) {
            if (down) {
                Turbo::held = true;
                Turbo::isDown = true;
                Turbo::p1 = isPlayer1;
                Turbo::start = Bot::frameNow();
            } else {
                Turbo::held = false;
                if (!Turbo::isDown) return;   // turbo already released the button
                Turbo::isDown = false;
            }
        }

        if (b.state == BotState::Record && m_player1 && !m_player1->m_isDead) {
            int f = Bot::frameNow();
            if (b.cfg.accuracy >= 1) b.recordFix(this, f);
            bool two = m_levelSettings && m_levelSettings->m_twoPlayerMode;
            b.macro.inputs.push_back({f, button, two ? !isPlayer1 : false, down});
        }

        if (down && button == 1) {
            CpsMeter::click(m_gameState.m_levelTime);
        }
        Kb::click(down, b.state == BotState::Play);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // TPS bypass: same stepping logic as the stock function, with a custom step size.
    double getModifiedDelta(float dt) {
        auto& b = Bot::get();
        if (!b.cfg.tpsOn || !Bot::inLevel(this) || std::fabs(b.cfg.tps - 240.f) < 0.01f) {
            return GJBaseGameLayer::getModifiedDelta(dt);
        }
        float step = 1.f / std::max(1.f, b.cfg.tps);

        if (m_resumeTimer > 0) {
            m_resumeTimer--;
            dt = 0.f;
        }
        float warp = std::min(1.0f, m_gameState.m_timeWarp);
        double total = dt + m_extraDelta;
        double steps = std::round(total / (warp * step));
        double out = steps * warp * step;
        m_extraDelta = total - out;
        return out;
    }

    // Frame stepper: freeze the simulation, advance only when asked.
    void update(float dt) {
        auto& b = Bot::get();
        if (b.cfg.stepper && b.state != BotState::None && Bot::inLevel(this)) {
            auto pl = PlayLayer::get();
            if (pl->m_isPaused || (m_player1 && m_player1->m_isDead) || m_gameState.m_currentProgress < 3) {
                return GJBaseGameLayer::update(dt);
            }
            if (b.stepReq > 0) {
                b.stepReq--;
                GJBaseGameLayer::update(1.f / b.effectiveTps());
            }
            return;
        }
        GJBaseGameLayer::update(dt);
        cpsOverlay(this);
    }
};

// ---------------------------------------------------------------------------
// PlayLayer: HUD, reset handling, noclip, safe mode, instant respawn
// ---------------------------------------------------------------------------

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        // Layout mode: build the level from a stripped copy of its string, then put the original back.
        bool robtop = false;
#ifdef GEODE_IS_ANDROID
        robtop = Layout::isOfficial(level->m_levelID.value());
#endif
        Layout::on = Mod::get()->getSavedValue<bool>("layout_mode", false) && !robtop;
        std::string oldString = level->m_levelString;
        if (Layout::on) {
            level->m_levelString = Layout::modify(oldString);
            Layout::building = true;
        }
        bool ok = PlayLayer::init(level, dontCreateObjects);
        Layout::building = false;
        if (Layout::on) level->m_levelString = oldString;
        if (!ok) return false;

        auto& b = Bot::get();
        Turbo::on = Mod::get()->getSavedValue<bool>("turbo_on", false);
        Turbo::cps = Mod::get()->getSavedValue<int>("turbo_cps", 30);
        Turbo::held = false;
        CpsMeter::on = Mod::get()->getSavedValue<bool>("cps_meter", true);
        CpsMeter::reset();
        b.setState(BotState::None);   // never start a run automatically; use the HUD button
        b.cfg.stepper = false;
        b.cfg.autoclick = false;
        b.stepReq = 0;

        b.hudRec = nullptr;
        b.hudPlay = nullptr;
        b.hudStep = nullptr;
        b.hudStep10 = nullptr;
        b.applyTimeScale();   // keep speedhack active across levels
        return true;
    }

    void onBotMenu(CCObject*) {
        if (!m_isPaused) this->pauseGame(false);
        Loader::get()->queueInMainThread([] { openBotMenu(); });
    }

    void onBotRec(CCObject*) {
        auto& b = Bot::get();
        if (b.state == BotState::Record) {
            b.setState(BotState::None);
            Notification::create(fmt::format("Recording stopped ({} inputs)", b.macro.inputs.size()),
                                 NotificationIcon::Success, 1.5f)->show();
            return;
        }
        b.setState(BotState::Record);
        this->resetLevelFromStart();
    }

    void onBotPlay(CCObject*) {
        auto& b = Bot::get();
        if (b.state == BotState::Play) {
            b.setState(BotState::None);
            return;
        }
        if (b.macro.empty()) {
            Notification::create("No macro loaded. Open BOT menu -> Load", NotificationIcon::Error, 2.f)->show();
            return;
        }
        b.setState(BotState::Play);
        b.applyTimeScale();
        this->resetLevelFromStart();
    }

    void onBotStep(CCObject*) { Bot::get().stepReq += 1; }
    void onBotStep10(CCObject*) { Bot::get().stepReq += 10; }

    void resetLevel() {
        PlayLayer::resetLevel();
        Turbo::held = false;
        Turbo::isDown = false;
        CpsMeter::reset();
        auto& b = Bot::get();
        b.onReset(Bot::frameNow());
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (Win::simulating) {
            // preview ghost: only record that it died, never a real death
            if (Win::isGhost(player) && obj != m_anticheatSpike) Win::simDied = true;
            return;
        }
        auto& b = Bot::get();
        if (b.cfg.noclip && obj != m_anticheatSpike) return;

        PlayLayer::destroyPlayer(player, obj);

        if (b.cfg.instantRespawn && m_player1 && m_player1->m_isDead && b.skipDelayed == false) {
            b.skipDelayed = true;
            b.skipDelayedAt = nowSec();
            Loader::get()->queueInMainThread([] {
                if (auto pl = PlayLayer::get()) pl->resetLevel();
            });
        }
    }

    void delayedResetLevel() {
        auto& b = Bot::get();
        if (b.skipDelayed) {
            bool fresh = nowSec() - b.skipDelayedAt < 3.0;
            b.skipDelayed = false;
            if (fresh) return;   // we already respawned instantly
        }
        PlayLayer::delayedResetLevel();
    }

    // Safe mode (like xdBot): the level-complete screen shows, but the game treats the run
    // as a test run so nothing is saved. Only active while the bot / hacks are in use.
    void levelComplete() {
        if (Win::simulating) return;
        auto& b = Bot::get();
        if (!botActive()) return PlayLayer::levelComplete();

        bool wasRecording = b.state == BotState::Record;
        if (b.state != BotState::None) b.setState(BotState::None);
        if (wasRecording) {
            Notification::create(
                fmt::format("Run finished - {} inputs recorded. Open BOT menu to save.", b.macro.inputs.size()),
                NotificationIcon::Success, 3.f)->show();
        }
        bool oldTest = m_isTestMode;
        m_isTestMode = true;
        PlayLayer::levelComplete();
        m_isTestMode = oldTest;
    }

    void onQuit() {
        Win::clear();
        auto& b = Bot::get();
        b.setState(BotState::None);
        b.cfg.stepper = false;
        b.cfg.autoclick = false;
        b.hudRec = nullptr;
        b.hudPlay = nullptr;
        b.hudStep = nullptr;
        b.hudStep10 = nullptr;
        PlayLayer::onQuit();
    }
};

// ---------------------------------------------------------------------------
// Misc hooks
// ---------------------------------------------------------------------------

class $modify(BotPlayer, PlayerObject) {
    void playDeathEffect() {
        if (Bot::get().cfg.noDeathEffect && PlayLayer::get()) return;
        PlayerObject::playDeathEffect();
    }
};

class $modify(BotLevel, GJGameLevel) {
    void savePercentage(int percent, bool practice, int clicks, int attempts, bool valid) {
        if (botActive()) return;   // keep progress/stats untouched while hacking
        GJGameLevel::savePercentage(percent, practice, clicks, attempts, valid);
    }
};

class $modify(BotPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = ButtonSprite::create("BOT", "bigFont.fnt", "GJ_button_01.png", .8f);
        spr->setScale(.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(BotPause::onBotMenuPause));

        auto winSize = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({winSize.width - 36.f, winSize.height - 24.f});
        menu->addChild(btn);
        this->addChild(menu);
    }

    void onBotMenuPause(CCObject*) { openBotMenu(); }
};

// Speedhack: scale the delta time that drives every scheduled update.
class $modify(BotScheduler, CCScheduler) {
    void update(float dt) {
        auto& b = Bot::get();
        if (b.cfg.speedOn && PlayLayer::get()) {
            dt *= std::clamp(b.cfg.speed, 0.05f, 10.f);
        }
        CCScheduler::update(dt);
    }
};
