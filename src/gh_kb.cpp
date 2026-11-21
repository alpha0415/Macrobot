#include <Geode/Geode.hpp>

#include "gg_kbdata.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace geode::prelude;

// Keyboard typing sound for every input (real touches and macro playback).
namespace Kb {

namespace {

bool g_loaded = false;
bool g_on = false;
bool g_onlyPlay = false;
int g_vol = 60;   // percent
bool g_extracted = false;
std::filesystem::path g_dir;

void ensureLoaded() {
    if (g_loaded) return;
    g_loaded = true;
    auto m = Mod::get();
    g_on = m->getSavedValue<bool>("kb_on", false);
    g_vol = m->getSavedValue<int>("kb_vol", 60);
    g_onlyPlay = m->getSavedValue<bool>("kb_only_play", false);
}

void ensureFiles() {
    if (g_extracted) return;
    g_extracted = true;
    g_dir = std::filesystem::path(Mod::get()->getSaveDir().string()) / "kb";
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    auto write = [&](KbData::Clip const& c) {
        auto p = g_dir / c.name;
        if (std::filesystem::exists(p, ec) && std::filesystem::file_size(p, ec) == c.size) return;
        std::ofstream out(p, std::ios::binary);
        out.write(reinterpret_cast<char const*>(c.data), static_cast<std::streamsize>(c.size));
    };
    for (auto const& c : KbData::press) write(c);
    for (auto const& c : KbData::release) write(c);
}

} // namespace

bool on() {
    ensureLoaded();
    return g_on;
}

void setOn(bool v) {
    ensureLoaded();
    g_on = v;
    Mod::get()->setSavedValue<bool>("kb_on", v);
}

bool onlyPlay() {
    ensureLoaded();
    return g_onlyPlay;
}

void setOnlyPlay(bool v) {
    ensureLoaded();
    g_onlyPlay = v;
    Mod::get()->setSavedValue<bool>("kb_only_play", v);
}

int vol() {
    ensureLoaded();
    return g_vol;
}

void setVol(int v) {
    ensureLoaded();
    g_vol = std::clamp(v, 10, 100);
    Mod::get()->setSavedValue<int>("kb_vol", g_vol);
}

void click(bool down, bool macroPlaying) {
    ensureLoaded();
    if (!g_on) return;
    if (g_onlyPlay && !macroPlaying) return;
    ensureFiles();
    constexpr int n = 6;
    static int lastP = -1, lastR = -1;
    int& last = down ? lastP : lastR;
    int i = std::rand() % n;
    if (i == last) i = (i + 1) % n;
    last = i;
    auto const& c = down ? KbData::press[i] : KbData::release[i];
    float speed = 0.95f + (std::rand() % 11) * 0.01f;   // natural pitch variation
    float volume = (g_vol / 100.f) * (down ? 1.f : 0.8f) * (0.85f + (std::rand() % 16) * 0.01f);
    FMODAudioEngine::sharedEngine()->playEffect((g_dir / c.name).string(), speed, 0.f, volume);
}

} // namespace Kb
