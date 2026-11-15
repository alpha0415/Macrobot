#include "xd_bot.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>

#include <algorithm>
#include <chrono>
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

        if (b.cfg.autoclick && b.state != BotState::Play && m_player1 && !m_player1->m_isDead) {
            b.autoclickFrame(this, frame);
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& b = Bot::get();
        if (!Bot::inLevel(this)) return GJBaseGameLayer::handleButton(down, button, isPlayer1);

        // While a macro plays, ignore real touches; only injected inputs go through.
        if (b.state == BotState::Play && !b.injecting) return;

        if (b.state == BotState::Record && m_player1 && !m_player1->m_isDead) {
            int f = Bot::frameNow();
            if (b.cfg.accuracy >= 1) b.recordFix(this, f);
            bool two = m_levelSettings && m_levelSettings->m_twoPlayerMode;
            b.macro.inputs.push_back({f, button, two ? !isPlayer1 : false, down});
        }

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
        if (b.cfg.stepper && Bot::inLevel(this)) {
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
    }
};

// ---------------------------------------------------------------------------
// PlayLayer: HUD, reset handling, noclip, safe mode, instant respawn
// ---------------------------------------------------------------------------

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;

        auto& b = Bot::get();
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
        auto& b = Bot::get();
        b.onReset(Bot::frameNow());
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
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

    // Safe mode: the level can never be completed.
    void levelComplete() {
        auto& b = Bot::get();
        if (!b.cfg.safeMode) return PlayLayer::levelComplete();

        bool wasRecording = b.state == BotState::Record;
        if (b.state != BotState::None) b.setState(BotState::None);
        if (wasRecording) {
            Notification::create(
                fmt::format("Run finished - {} inputs recorded. Open BOT menu to save.", b.macro.inputs.size()),
                NotificationIcon::Success, 3.f)->show();
        } else {
            Notification::create("Level completion blocked", NotificationIcon::Info, 1.5f)->show();
        }
        Loader::get()->queueInMainThread([] {
            if (auto pl = PlayLayer::get()) pl->resetLevel();
        });
    }

    void onQuit() {
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
        if (Bot::get().cfg.safeMode) return;   // keep progress/stats untouched
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
