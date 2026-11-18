#include "fw_window.hpp"
#include "fw_bot.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cmath>

namespace Win {
    bool simulating = false;
    bool simDied = false;
    bool simHitPortal = false;
    float playerDt = 0.25f;
}

namespace {

constexpr int kEarly = 8;      // search at most this many ticks earlier
constexpr int kLate = 8;       // ... and this many ticks later
constexpr int kMargin = 24;    // ticks the ghost must survive after the last shifted input
constexpr int kMaxSims = 40;   // safety cap per input

PlayerObject* g_ghost = nullptr;
CCLayer* g_ghostLayer = nullptr;
std::vector<float> g_ticks;        // window size in ticks per input index, <= 0 means unknown
std::vector<unsigned char> g_cap;  // 1 = the search hit its limit
size_t g_computeIdx = 0;
int g_lastSoundFrame = -1000;

constexpr uint64_t bit(GameObjectType t) { return 1ull << static_cast<int>(t); }

constexpr uint64_t kColliding =
    bit(GameObjectType::Solid) | bit(GameObjectType::Hazard) | bit(GameObjectType::Slope) |
    bit(GameObjectType::CollisionObject) | bit(GameObjectType::AnimatedHazard);

// Orbs and pads are simulated too (their activation state is saved/restored around each run).
constexpr uint64_t kOrbPad =
    bit(GameObjectType::YellowJumpPad) | bit(GameObjectType::PinkJumpPad) | bit(GameObjectType::GravityPad) |
    bit(GameObjectType::RedJumpPad) | bit(GameObjectType::SpiderPad) |
    bit(GameObjectType::YellowJumpRing) | bit(GameObjectType::PinkJumpRing) | bit(GameObjectType::GravityRing) |
    bit(GameObjectType::RedJumpRing) | bit(GameObjectType::GreenRing) | bit(GameObjectType::DropRing) |
    bit(GameObjectType::CustomRing) | bit(GameObjectType::SpiderOrb);

struct Saved {
    EffectGameObject* obj;
    bool activated, p1, p2;
};
std::vector<Saved> g_saved;

// Portals that change too much to preview: reaching one ends the preview as "safe".
constexpr uint64_t kStop =
    bit(GameObjectType::ShipPortal) | bit(GameObjectType::CubePortal) | bit(GameObjectType::BallPortal) |
    bit(GameObjectType::UfoPortal) | bit(GameObjectType::WavePortal) | bit(GameObjectType::RobotPortal) |
    bit(GameObjectType::SpiderPortal) | bit(GameObjectType::SwingPortal) | bit(GameObjectType::DualPortal) |
    bit(GameObjectType::SoloPortal) | bit(GameObjectType::RegularSizePortal) | bit(GameObjectType::MiniSizePortal) |
    bit(GameObjectType::InverseMirrorPortal) | bit(GameObjectType::NormalMirrorPortal) |
    bit(GameObjectType::TeleportPortal) | bit(GameObjectType::TeleportOrb) |
    bit(GameObjectType::InverseGravityPortal) | bit(GameObjectType::NormalGravityPortal) |
    bit(GameObjectType::GravityTogglePortal);

struct Ev {
    int k;
    bool down;
};

bool ensureGhost(GJBaseGameLayer* gl) {
    if (g_ghost) return true;
    g_ghostLayer = CCLayer::create();
    g_ghostLayer->retain();
    g_ghost = PlayerObject::create(1, 1, gl, g_ghostLayer, true);
    if (!g_ghost) return false;
    g_ghost->retain();
    g_ghost->setPosition({0.f, 105.f});
    g_ghost->setVisible(false);
    g_ghostLayer->addChild(g_ghost);
    return true;
}

void copyState(PlayerObject* ghost, PlayerObject* real) {
    ghost->setVisible(false);
    ghost->resetTouchedRings(true);
    ghost->copyAttributes(real);
    ghost->setScale(real->getScale());
    ghost->setContentSize(real->getContentSize());
    ghost->m_gravityMod = real->m_gravityMod;
    ghost->m_isOnGround = real->m_isOnGround;
    ghost->m_isUpsideDown = real->m_isUpsideDown;
    ghost->m_vehicleSize = real->m_vehicleSize;
    ghost->m_playerSpeed = real->m_playerSpeed;
    ghost->m_isDashing = real->m_isDashing;
    ghost->m_lastActivatedPortal = real->m_lastActivatedPortal;
    ghost->m_holdingButtons = real->m_holdingButtons;
    ghost->m_jumpBuffered = real->m_jumpBuffered;
    ghost->m_wasJumpBuffered = real->m_wasJumpBuffered;
    ghost->m_stateJumpBuffered = real->m_stateJumpBuffered;
    ghost->m_stateRingJump = real->m_stateRingJump;
    ghost->m_wasRobotJump = real->m_wasRobotJump;
    ghost->setPosition(real->getPosition());
    ghost->setRotation(real->getRotation());
    ghost->m_lastGroundedPos = real->m_lastGroundedPos;
    ghost->m_yVelocity = real->m_yVelocity;
    ghost->m_fallSpeed = real->m_fallSpeed;
    ghost->m_yVelocityBeforeSlope = real->m_yVelocityBeforeSlope;
    ghost->m_groundYVelocity = real->m_groundYVelocity;
    ghost->m_speedMultiplier = real->m_speedMultiplier;
    ghost->m_gravity = real->m_gravity;
    ghost->m_isGoingLeft = real->m_isGoingLeft;
    ghost->m_rotationSpeed = real->m_rotationSpeed;
    ghost->m_isRotating = real->m_isRotating;
    ghost->m_totalTime = real->m_totalTime;
    ghost->m_objectSnappedTo = real->m_objectSnappedTo;
    ghost->m_maybeLastGroundObject = real->m_maybeLastGroundObject;
    ghost->m_touchedRings = real->m_touchedRings;
    ghost->m_ringJumpedSincePress = real->m_ringJumpedSincePress;
    ghost->m_touchedRing = real->m_touchedRing;
    ghost->m_touchedPad = real->m_touchedPad;
    ghost->m_isDead = false;
    ghost->m_isLocked = real->m_isLocked;
}

void stepGhost(GJBaseGameLayer* gl, float dt) {
    g_ghost->update(dt);
    g_ghost->updateRotation(dt);
    g_ghost->m_collisionLogTop->removeAllObjects();
    g_ghost->m_collisionLogBottom->removeAllObjects();
    g_ghost->m_collisionLogLeft->removeAllObjects();
    g_ghost->m_collisionLogRight->removeAllObjects();
    gl->checkCollisions(g_ghost, dt, false);
}

// Run the ghost from the real player's current state, applying the given inputs
// (k = number of physics steps from now). True if it is still alive after `horizon` steps.
bool survives(GJBaseGameLayer* gl, std::vector<Ev> const& events, int horizon) {
    copyState(g_ghost, gl->m_player1);

    float dt = Win::playerDt;
    if (!(dt > 0.0001f && dt < 4.f)) dt = 0.25f;

    bool savedDebug = gl->m_isDebugDrawEnabled;
    gl->m_isDebugDrawEnabled = false;
    g_saved.clear();
    Win::simulating = true;
    Win::simDied = false;
    Win::simHitPortal = false;

    bool safe = false;
    size_t ei = 0;
    for (int k = 0; k < horizon; ++k) {
        while (ei < events.size() && events[ei].k <= k) {
            if (events[ei].down) g_ghost->pushButton(PlayerButton::Jump);
            else g_ghost->releaseButton(PlayerButton::Jump);
            ++ei;
        }
        stepGhost(gl, dt);
        if (g_ghost->m_isDead) Win::simDied = true;
        if (Win::simDied) break;
        if (Win::simHitPortal) {
            safe = true;
            break;
        }
    }

    Win::simulating = false;
    for (auto& sv : g_saved) {
        sv.obj->m_activated = sv.activated;
        sv.obj->m_activatedByPlayer1 = sv.p1;
        sv.obj->m_activatedByPlayer2 = sv.p2;
    }
    g_saved.clear();
    gl->m_isDebugDrawEnabled = savedDebug;
    return safe || !Win::simDied;
}

std::string formatFrames(float ticks, bool capped) {
    float frames = ticks / 4.f;   // 240 TPS -> 60 FPS
    std::string s = fmt::format("{:.2f}", frames);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s + (capped ? "f+" : "f");
}

void computeOne(GJBaseGameLayer* gl, size_t idx, int frame) {
    auto& b = Bot::get();
    auto const& inputs = b.macro.inputs;
    int target = inputs[idx].frame;
    int base = target - frame;               // step of the target input if it is not shifted
    if (base < 1) return;
    int earlyCap = std::min(kEarly, base);   // can't start before "now"
    int horizon = base + kLate + kMargin;

    // first macro input after `frame`
    size_t start = std::lower_bound(inputs.begin(), inputs.end(), frame + 1,
                                    [](MInput const& a, int v) { return a.frame < v; }) - inputs.begin();

    auto build = [&](int shift) {
        std::vector<Ev> ev;
        for (size_t j = start; j < inputs.size(); ++j) {
            int k = inputs[j].frame - frame;
            if (k > horizon) break;
            if (inputs[j].p2) continue;
            if (j == idx) k += shift;
            ev.push_back({std::max(0, k), inputs[j].down});
        }
        std::stable_sort(ev.begin(), ev.end(), [](Ev const& a, Ev const& c) { return a.k < c.k; });
        return ev;
    };

    int sims = 0;
    auto ok = [&](int shift) {
        ++sims;
        return survives(gl, build(shift), horizon);
    };

    if (!ok(0)) return;   // the preview itself doesn't match the real run: don't guess

    int late = 0;
    while (late < kLate && sims < kMaxSims && ok(late + 1)) ++late;
    int early = 0;
    while (early < earlyCap && sims < kMaxSims && ok(-(early + 1))) ++early;

    g_ticks[idx] = static_cast<float>(1 + early + late);
    g_cap[idx] = (late >= kLate || early >= kEarly) ? 1 : 0;
}

} // namespace

namespace Win {

bool isGhost(PlayerObject* p) {
    return p && p == g_ghost;
}

void sync(size_t inputCount, int frame) {
    auto& b = Bot::get();
    g_ticks.assign(inputCount, -1.f);
    g_cap.assign(inputCount, 0);
    g_computeIdx = std::lower_bound(b.macro.inputs.begin(), b.macro.inputs.end(), frame + 1,
                                    [](MInput const& a, int v) { return a.frame < v; }) - b.macro.inputs.begin();
    g_lastSoundFrame = -1000;
}

void clear() {
    simulating = false;
    if (g_ghost) {
        g_ghost->removeFromParent();
        g_ghost->release();
        g_ghost = nullptr;
    }
    if (g_ghostLayer) {
        g_ghostLayer->release();
        g_ghostLayer = nullptr;
    }
    g_ticks.clear();
    g_cap.clear();
    g_computeIdx = 0;
}

void compute(GJBaseGameLayer* gl, int frame) {
    auto& b = Bot::get();
    if (!b.cfg.windowOn || b.state != BotState::Play) return;
    if (!gl->m_player1 || gl->m_player1->m_isDead || gl->m_gameState.m_isDualMode) return;
    auto const& inputs = b.macro.inputs;
    if (g_ticks.size() != inputs.size()) sync(inputs.size(), frame);
    if (!ensureGhost(gl)) return;

    while (g_computeIdx < inputs.size() && inputs[g_computeIdx].frame <= frame + kEarly) {
        size_t idx = g_computeIdx++;
        if (inputs[idx].frame <= frame || inputs[idx].p2) continue;
        computeOne(gl, idx, frame);
    }
}

void onInput(GJBaseGameLayer* gl, size_t idx, int frame) {
    auto& b = Bot::get();
    if (!b.cfg.windowOn || idx >= g_ticks.size()) return;
    float ticks = g_ticks[idx];
    if (ticks <= 0.f || !gl->m_player1 || !gl->m_objectLayer) return;
    if (ticks > b.cfg.windowMax * 4.f) return;   // not frame perfect enough

    // colour by window size
    ccColor4F col;
    if (ticks <= 1.f) col = {1.f, 0.25f, 0.25f, 1.f};          // 0.25 frame
    else if (ticks <= 2.f) col = {1.f, 0.6f, 0.2f, 1.f};       // 0.5 frame
    else if (ticks <= 4.f) col = {1.f, 0.9f, 0.2f, 1.f};       // 1 frame
    else col = {0.4f, 1.f, 0.4f, 1.f};

    auto holder = CCNode::create();
    holder->setPosition(gl->m_player1->getPosition());
    holder->setZOrder(2000);

    auto ring = CCDrawNode::create();
    ring->drawCircle({0.f, 0.f}, 18.f, {0.f, 0.f, 0.f, 0.f}, 2.5f, col, 32);
    holder->addChild(ring);

    auto label = CCLabelBMFont::create(formatFrames(ticks, g_cap[idx] != 0).c_str(), "bigFont.fnt");
    label->setScale(0.4f);
    label->setPosition({0.f, -30.f});
    label->setColor({static_cast<GLubyte>(col.r * 255), static_cast<GLubyte>(col.g * 255), static_cast<GLubyte>(col.b * 255)});
    holder->addChild(label);

    gl->m_objectLayer->addChild(holder);
    ring->runAction(CCScaleTo::create(0.35f, 1.7f));
    label->runAction(CCFadeOut::create(0.5f));
    holder->runAction(CCSequence::create(CCDelayTime::create(0.5f), CCRemoveSelf::create(), nullptr));

    if (frame - g_lastSoundFrame >= 6) {
        g_lastSoundFrame = frame;
        FMODAudioEngine::sharedEngine()->playEffect("playSound_01.ogg", 1.7f, 0.f, 1.f);
    }
}

} // namespace Win

// ---------------------------------------------------------------------------
// Sandbox: while the ghost runs, only solids/hazards matter and nothing the
// ghost touches may have visible or lasting side effects.
// ---------------------------------------------------------------------------

class $modify(WinLayer, GJBaseGameLayer) {
    void collisionCheckObjects(PlayerObject* player, std::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!Win::simulating || !Win::isGhost(player)) {
            return GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
        }
        static std::vector<GameObject*> relevant;
        relevant.clear();
        bool haveRect = false;
        CCRect ghostRect;
        auto& list = *objects;
        for (int i = 0; i < objectCount; ++i) {
            auto obj = list[i];
            if (!obj) continue;
            auto type = static_cast<unsigned>(obj->m_objectType);
            uint64_t b = type < 64 ? (1ull << type) : 0;
            if (kColliding & b) {
                relevant.push_back(obj);
                continue;
            }
            if (kOrbPad & b) {
                relevant.push_back(obj);
                auto eo = static_cast<EffectGameObject*>(obj);
                bool known = false;
                for (auto& sv : g_saved) if (sv.obj == eo) { known = true; break; }
                if (!known) g_saved.push_back({eo, eo->m_activated, eo->m_activatedByPlayer1, eo->m_activatedByPlayer2});
                continue;
            }
            if (kStop & b) {
                if (obj == player->m_lastActivatedPortal) continue;
                if (!haveRect) {
                    ghostRect = player->getObjectRect();
                    haveRect = true;
                }
                if (ghostRect.intersectsRect(obj->getObjectRect())) Win::simHitPortal = true;
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (Win::simulating) {
            Win::simHitPortal = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (Win::simulating) {
            if (Win::isGhost(player)) GJBaseGameLayer::flipGravity(player, flip, true);
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (Win::simulating) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void updateDebugDraw() {
        if (Win::simulating) return;
        GJBaseGameLayer::updateDebugDraw();
    }
};

class $modify(WinPlayer, PlayerObject) {
    // Remember the dt the game gives the real player's step so the ghost uses the same units.
    void update(float dt) {
        if (!Win::simulating && dt > 0.f && !Win::isGhost(this)) Win::playerDt = dt;
        PlayerObject::update(dt);
    }

    void incrementJumps() {
        if (Win::simulating) return;
        PlayerObject::incrementJumps();
    }

    void playBumpEffect(int objectType, GameObject* obj) {
        if (Win::simulating) return;
        PlayerObject::playBumpEffect(objectType, obj);
    }

    void spawnCircle() {
        if (Win::simulating) return;
        PlayerObject::spawnCircle();
    }

    void animatePlatformerJump(float scale) {
        if (Win::simulating) return;
        PlayerObject::animatePlatformerJump(scale);
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (Win::simulating) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }
};

class $modify(WinPlayLayer, PlayLayer) {
    void playEndAnimationToPos(CCPoint position) {
        if (Win::simulating) return;
        PlayLayer::playEndAnimationToPos(position);
    }
};
