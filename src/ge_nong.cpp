#include <Geode/Geode.hpp>
#include <Geode/modify/MusicDownloadManager.hpp>
#include <Geode/modify/LevelTools.hpp>

#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <map>

using namespace geode::prelude;

// NONG: replace a level song with an audio file from the device.
// Key: official track id (levels without a custom song) or Newgrounds/Music Library song id.
namespace Nong {

namespace {

std::map<std::pair<bool, int>, std::string>& table() {
    static std::map<std::pair<bool, int>, std::string> t;
    return t;
}

bool g_loaded = false;

std::filesystem::path baseDir() {
    return std::filesystem::path(Mod::get()->getSaveDir().string());
}

void save() {
    nlohmann::json j = nlohmann::json::array();
    for (auto& [k, v] : table()) j.push_back({{"official", k.first}, {"id", k.second}, {"file", v}});
    std::error_code ec;
    std::filesystem::create_directories(baseDir(), ec);
    std::ofstream out(baseDir() / "nongs.json");
    out << j.dump(2);
}

void ensureLoaded() {
    if (g_loaded) return;
    g_loaded = true;
    try {
        std::ifstream in(baseDir() / "nongs.json");
        if (!in) return;
        auto j = nlohmann::json::parse(in);
        for (auto& e : j) {
            table()[{e.value("official", false), e.value("id", 0)}] = e.value("file", std::string());
        }
    } catch (...) {}
}

} // namespace

// Absolute path of the replacement file, or "" when the song isn't replaced.
std::string get(bool official, int id) {
    ensureLoaded();
    auto it = table().find({official, id});
    if (it == table().end()) return "";
    std::error_code ec;
    if (!std::filesystem::exists(it->second, ec)) return "";
    return it->second;
}

bool set(bool official, int id, std::string const& srcPath) {
    ensureLoaded();
    std::filesystem::path src(srcPath);
    auto dir = baseDir() / "nong";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto dest = dir / fmt::format("{}{}_{}", official ? "o" : "n", id, src.filename().string());
    std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return false;
    table()[{official, id}] = dest.string();
    save();
    return true;
}

void clear(bool official, int id) {
    ensureLoaded();
    auto it = table().find({official, id});
    if (it == table().end()) return;
    std::error_code ec;
    std::filesystem::remove(it->second, ec);
    table().erase(it);
    save();
}

std::string displayName(bool official, int id) {
    auto p = get(official, id);
    if (p.empty()) return "";
    return std::filesystem::path(p).filename().string();
}

} // namespace Nong

class $modify(NongMusicManager, MusicDownloadManager) {
    std::string pathForSong(int id) {
        auto p = Nong::get(false, id);
        if (!p.empty()) return p;
        return MusicDownloadManager::pathForSong(id);
    }

    bool isSongDownloaded(int id) {
        if (!Nong::get(false, id).empty()) return true;
        return MusicDownloadManager::isSongDownloaded(id);
    }
};

class $modify(NongLevelTools, LevelTools) {
    static std::string getAudioFileName(int id) {
        auto p = Nong::get(true, id);
        if (!p.empty()) return p;
        return LevelTools::getAudioFileName(id);
    }
};
