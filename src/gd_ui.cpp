namespace Win { extern int statBuckets[6]; }
#include "fw_bot.hpp"
#include "json.hpp"

#include <fstream>
#include <functional>

#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/utils/async.hpp>

#include <algorithm>

namespace {

enum Act {
    A_REC = 1, A_PLAY, A_SAVE, A_LOAD, A_IMPORT, A_CLEAR,
    A_NOCLIP = 20, A_SAFE, A_INSTANT, A_NODEATH, A_AUTOSAVE, A_SPEEDON, A_TPSON, A_ACCURACY,
    A_STEPPER = 40, A_AUTOCLICK, A_ACP1, A_ACP2,
    A_SPEED_DEC = 60, A_SPEED_INC, A_TPS_DEC, A_TPS_INC, A_OFF_DEC, A_OFF_INC,
    A_ACH_DEC, A_ACH_INC, A_ACR_DEC, A_ACR_INC,
    A_WINDOW = 45, A_WIN_DEC = 70, A_WIN_INC,
    A_PAGE_PREV = 90, A_PAGE_NEXT, A_LISTS,
};

CCLabelBMFont* makeLabel(char const* text, float scale, CCPoint pos, CCNode* parent, CCPoint anchor = {0.f, 0.5f}) {
    auto l = CCLabelBMFont::create(text, "bigFont.fnt");
    l->setScale(scale);
    l->setAnchorPoint(anchor);
    l->setPosition(pos);
    parent->addChild(l);
    return l;
}

} // namespace

void startRun() {
    // Used after record/play toggles from inside the menu: resume and restart from the beginning.
    auto pl = PlayLayer::get();
    if (!pl) return;
    if (auto pause = CCScene::get()->getChildByType<PauseLayer>(0)) pause->onResume(nullptr);
    Loader::get()->queueInMainThread([] {
        if (auto p = PlayLayer::get()) p->resetLevelFromStart();
    });
}

// ---------------------------------------------------------------------------
// Save popup
// ---------------------------------------------------------------------------

class SavePopup : public geode::Popup {
protected:
    TextInput* m_name = nullptr;
    TextInput* m_author = nullptr;

    bool setup() {
        this->setTitle("Save Macro");
        auto size = m_mainLayer->getContentSize();

        m_name = TextInput::create(260.f, "Macro name", "bigFont.fnt");
        m_name->setPosition({size.width / 2, size.height - 62.f});
        m_name->setMaxCharCount(40);
        m_mainLayer->addChild(m_name);

        m_author = TextInput::create(260.f, "Author (optional)", "bigFont.fnt");
        m_author->setPosition({size.width / 2, size.height - 104.f});
        m_author->setMaxCharCount(30);
        m_mainLayer->addChild(m_author);

        auto lvl = Bot::get().macro.levelName;
        if (!lvl.empty()) m_name->setString(lvl);

        makeLabel(Bot::get().info().c_str(), .3f, {size.width / 2, size.height - 138.f}, m_mainLayer, {.5f, .5f});

        auto spr = ButtonSprite::create("Save");
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(SavePopup::onSave));
        btn->setPosition({size.width / 2, 28.f});
        m_buttonMenu->addChild(btn);
        return true;
    }

    void onSave(CCObject*) {
        auto& b = Bot::get();
        std::string name = m_name->getString();
        if (name.empty()) name = "macro";
        if (b.saveMacro(name, m_author->getString(), "")) {
            Notification::create("Macro saved", NotificationIcon::Success, 1.5f)->show();
            this->onClose(nullptr);
        } else {
            FLAlertLayer::create("Error", "Nothing to save (record a macro first) or the file could not be written.", "OK")->show();
        }
    }

public:
    static SavePopup* create() {
        auto ret = new SavePopup();
        if (ret->init(330.f, 200.f) && ret->setup()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Load popup (paged list, 5 per page)
// ---------------------------------------------------------------------------

class LoadPopup : public geode::Popup {
protected:
    std::vector<std::filesystem::path> m_files;
    int m_page = 0;
    CCNode* m_list = nullptr;
    bool m_auto = false;

    bool setup() {
        this->setTitle("Load Macro");
        m_list = CCNode::create();
        m_mainLayer->addChild(m_list);
        rescan();
        rebuild();
        return true;
    }

    void rescan() {
        m_files.clear();
        std::error_code ec;
        auto dir = m_auto ? Bot::get().autoDir() : Bot::get().macroDir();
        for (auto const& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            auto ext = e.path().extension().string();
            if (ext == ".json" || ext == ".gdr") m_files.push_back(e.path());
        }
        std::sort(m_files.begin(), m_files.end());
        int pages = std::max(1, (int)((m_files.size() + 4) / 5));
        m_page = std::clamp(m_page, 0, pages - 1);
    }

    void rebuild() {
        m_list->removeAllChildren();
        auto size = m_mainLayer->getContentSize();

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_list->addChild(menu);

        makeLabel(m_auto ? "Folder: autosaves" : "Folder: macros", .3f, {16.f, size.height - 34.f}, m_list);

        for (int i = 0; i < 5; i++) {
            size_t idx = m_page * 5 + i;
            if (idx >= m_files.size()) break;
            float y = size.height - 62.f - i * 30.f;
            auto name = m_files[idx].filename().string();
            if (name.size() > 34) name = name.substr(0, 31) + "...";

            auto spr = ButtonSprite::create(name.c_str(), 270, true, "bigFont.fnt", "GJ_button_04.png", 24.f, .5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(LoadPopup::onPick));
            btn->setTag((int)idx);
            btn->setPosition({size.width / 2 - 22.f, y});
            menu->addChild(btn);

            auto xs = ButtonSprite::create("X", 22, true, "bigFont.fnt", "GJ_button_06.png", 24.f, .5f);
            auto del = CCMenuItemSpriteExtra::create(xs, this, menu_selector(LoadPopup::onDelete));
            del->setTag((int)idx);
            del->setPosition({size.width - 36.f, y});
            menu->addChild(del);
        }
        if (m_files.empty()) {
            makeLabel("No macros here yet", .45f, {size.width / 2, size.height / 2}, m_list, {.5f, .5f});
        }

        int pages = std::max(1, (int)((m_files.size() + 4) / 5));
        makeLabel(fmt::format("{}/{}", m_page + 1, pages).c_str(), .4f, {size.width / 2, 20.f}, m_list, {.5f, .5f});

        auto mk = [&](char const* t, SEL_MenuHandler cb, CCPoint p, int tag = 0) {
            auto s = ButtonSprite::create(t, "bigFont.fnt", "GJ_button_01.png", .8f);
            s->setScale(.6f);
            auto b = CCMenuItemSpriteExtra::create(s, this, cb);
            b->setPosition(p);
            b->setTag(tag);
            menu->addChild(b);
        };
        mk("<", menu_selector(LoadPopup::onPage), {size.width / 2 - 50.f, 20.f}, -1);
        mk(">", menu_selector(LoadPopup::onPage), {size.width / 2 + 50.f, 20.f}, 1);
        mk(m_auto ? "Macros" : "Autosaves", menu_selector(LoadPopup::onToggleFolder), {70.f, 20.f});
        mk("Import", menu_selector(LoadPopup::onImport), {size.width - 56.f, 20.f});
    }

    void onPage(CCObject* s) {
        m_page += static_cast<CCNode*>(s)->getTag();
        rescan();
        rebuild();
    }

    void onToggleFolder(CCObject*) {
        m_auto = !m_auto;
        m_page = 0;
        rescan();
        rebuild();
    }

    void onPick(CCObject* s) {
        size_t idx = static_cast<CCNode*>(s)->getTag();
        if (idx >= m_files.size()) return;
        auto& b = Bot::get();
        if (b.loadMacro(m_files[idx])) {
            b.applyTimeScale();
            Notification::create("Loaded: " + b.info(), NotificationIcon::Success, 2.f)->show();
            this->onClose(nullptr);
        } else {
            FLAlertLayer::create("Error", "Could not read this macro file.", "OK")->show();
        }
    }

    void onDelete(CCObject* s) {
        size_t idx = static_cast<CCNode*>(s)->getTag();
        if (idx >= m_files.size()) return;
        auto path = m_files[idx];
        Ref<LoadPopup> self(this);
        createQuickPopup("Delete", "Delete <cy>" + path.filename().string() + "</c>?", "No", "Yes",
                         [self, path](FLAlertLayer*, bool yes) {
                             if (!yes) return;
                             std::error_code ec;
                             std::filesystem::remove(path, ec);
                             self->rescan();
                             self->rebuild();
                         });
    }

    void onImport(CCObject*) {
        auto dest = Bot::get().macroDir();
        async::spawn(
            file::pick(file::PickMode::OpenFile, file::FilePickOptions {
                .filters = { file::FilePickOptions::Filter {
                    .description = "Macro Files",
                    .files = { "*.json", "*.gdr" },
                }}
            }),
            [dest](Result<std::optional<asp::fs::Path>> result) {
                if (!result.isOk() || !result.unwrap().has_value()) return;
                std::filesystem::path src(result.unwrap().value().string());
                std::error_code ec;
                std::filesystem::copy_file(src, dest / src.filename(),
                                           std::filesystem::copy_options::overwrite_existing, ec);
                Notification::create(ec ? "Import failed" : "Imported - reopen Load to see it",
                                     ec ? NotificationIcon::Error : NotificationIcon::Success, 2.f)->show();
            }
        );
    }

public:
    static LoadPopup* create() {
        auto ret = new LoadPopup();
        if (ret->init(380.f, 250.f) && ret->setup()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};


// ---------------------------------------------------------------------------
// Macro lists ("Top 1", "Impossible levels", ...). A list only stores macro file names,
// so the same macro can sit in any number of lists, and even twice in the same list.
// ---------------------------------------------------------------------------

namespace {
struct MacroList {
    std::string name;
    std::vector<std::string> items;
};

std::filesystem::path listsFile() {
    return std::filesystem::path(Mod::get()->getSaveDir().string()) / "lists.json";
}

std::vector<MacroList> loadLists() {
    std::vector<MacroList> out;
    try {
        std::ifstream in(listsFile());
        if (!in) return out;
        auto j = nlohmann::json::parse(in);
        for (auto& l : j.value("lists", nlohmann::json::array())) {
            MacroList ml;
            ml.name = l.value("name", std::string("list"));
            for (auto& it : l.value("items", nlohmann::json::array())) {
                if (it.is_string()) ml.items.push_back(it.get<std::string>());
            }
            out.push_back(std::move(ml));
        }
    } catch (...) {}
    return out;
}

void saveLists(std::vector<MacroList> const& lists) {
    nlohmann::json j;
    j["lists"] = nlohmann::json::array();
    for (auto const& l : lists) j["lists"].push_back({{"name", l.name}, {"items", l.items}});
    std::ofstream out(listsFile());
    out << j.dump(2);
}

std::vector<std::string> macroFileNames() {
    std::vector<std::string> names;
    std::error_code ec;
    for (auto const& e : std::filesystem::directory_iterator(Bot::get().macroDir(), ec)) {
        if (!e.is_regular_file()) continue;
        auto ext = e.path().extension().string();
        if (ext == ".json" || ext == ".gdr") names.push_back(e.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string shorten(std::string n, size_t max) {
    if (n.size() > max) n = n.substr(0, max - 3) + "...";
    return n;
}

CCMenuItemSpriteExtra* smallButton(CCMenu* menu, CCObject* target, char const* t, SEL_MenuHandler cb, CCPoint p, int tag = 0) {
    auto s = ButtonSprite::create(t, "bigFont.fnt", "GJ_button_01.png", .8f);
    s->setScale(.6f);
    auto b = CCMenuItemSpriteExtra::create(s, target, cb);
    b->setPosition(p);
    b->setTag(tag);
    menu->addChild(b);
    return b;
}

} // namespace

// New list: ask for a name
class NewListPopup : public geode::Popup {
protected:
    TextInput* m_name = nullptr;
    std::function<void()> m_done;

    bool setup() {
        this->setTitle("New List");
        auto size = m_mainLayer->getContentSize();
        m_name = TextInput::create(260.f, "List name (e.g. Top 1)", "bigFont.fnt");
        m_name->setPosition({size.width / 2, size.height - 70.f});
        m_name->setMaxCharCount(30);
        m_mainLayer->addChild(m_name);
        auto spr = ButtonSprite::create("Create");
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(NewListPopup::onCreate));
        btn->setPosition({size.width / 2, 28.f});
        m_buttonMenu->addChild(btn);
        return true;
    }

    void onCreate(CCObject*) {
        std::string name = m_name->getString();
        if (name.empty()) name = "My list";
        auto lists = loadLists();
        lists.push_back({name, {}});
        saveLists(lists);
        if (m_done) m_done();
        this->onClose(nullptr);
    }

public:
    static NewListPopup* create(std::function<void()> done) {
        auto ret = new NewListPopup();
        ret->m_done = std::move(done);
        if (ret->init(330.f, 170.f) && ret->setup()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// Pick a macro to append to a list (can be done repeatedly, duplicates are fine)
class AddToListPopup : public geode::Popup {
protected:
    int m_list = 0;
    int m_page = 0;
    std::vector<std::string> m_files;
    CCNode* m_view = nullptr;
    std::function<void()> m_changed;

    bool setup() {
        this->setTitle("Add macro to list");
        m_view = CCNode::create();
        m_mainLayer->addChild(m_view);
        m_files = macroFileNames();
        rebuild();
        return true;
    }

    void rebuild() {
        m_view->removeAllChildren();
        auto size = m_mainLayer->getContentSize();
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_view->addChild(menu);

        int pages = std::max(1, (int)((m_files.size() + 4) / 5));
        m_page = std::clamp(m_page, 0, pages - 1);

        for (int i = 0; i < 5; i++) {
            size_t idx = m_page * 5 + i;
            if (idx >= m_files.size()) break;
            float y = size.height - 62.f - i * 30.f;
            auto spr = ButtonSprite::create(shorten(m_files[idx], 34).c_str(), 330, true, "bigFont.fnt", "GJ_button_04.png", 24.f, .5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AddToListPopup::onPick));
            btn->setTag((int)idx);
            btn->setPosition({size.width / 2, y});
            menu->addChild(btn);
        }
        if (m_files.empty()) makeLabel("No saved macros yet", .45f, {size.width / 2, size.height / 2}, m_view, {.5f, .5f});
        makeLabel("Tap a macro to add it (you can add the same one again)", .26f, {16.f, size.height - 34.f}, m_view);
        makeLabel(fmt::format("{}/{}", m_page + 1, pages).c_str(), .4f, {size.width / 2, 20.f}, m_view, {.5f, .5f});
        smallButton(menu, this, "<", menu_selector(AddToListPopup::onPage), {size.width / 2 - 50.f, 20.f}, -1);
        smallButton(menu, this, ">", menu_selector(AddToListPopup::onPage), {size.width / 2 + 50.f, 20.f}, 1);
    }

    void onPage(CCObject* s) {
        m_page += static_cast<CCNode*>(s)->getTag();
        rebuild();
    }

    void onPick(CCObject* s) {
        size_t idx = static_cast<CCNode*>(s)->getTag();
        if (idx >= m_files.size()) return;
        auto lists = loadLists();
        if (m_list < 0 || m_list >= (int)lists.size()) return;
        lists[m_list].items.push_back(m_files[idx]);
        saveLists(lists);
        Notification::create(fmt::format("Added ({} in list)", lists[m_list].items.size()),
                             NotificationIcon::Success, 1.2f)->show();
        if (m_changed) m_changed();
    }

public:
    static AddToListPopup* create(int list, std::function<void()> changed) {
        auto ret = new AddToListPopup();
        ret->m_list = list;
        ret->m_changed = std::move(changed);
        if (ret->init(380.f, 250.f) && ret->setup()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// One list: tap an entry to load that macro, X removes the entry
class ListPopup : public geode::Popup {
protected:
    int m_list = 0;
    int m_page = 0;
    CCNode* m_view = nullptr;

    bool setup() {
        auto lists = loadLists();
        this->setTitle(m_list < (int)lists.size() ? lists[m_list].name.c_str() : "List");
        m_view = CCNode::create();
        m_mainLayer->addChild(m_view);
        rebuild();
        return true;
    }

    void rebuild() {
        m_view->removeAllChildren();
        auto size = m_mainLayer->getContentSize();
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_view->addChild(menu);

        auto lists = loadLists();
        if (m_list < 0 || m_list >= (int)lists.size()) return;
        auto const& items = lists[m_list].items;
        int pages = std::max(1, (int)((items.size() + 4) / 5));
        m_page = std::clamp(m_page, 0, pages - 1);

        for (int i = 0; i < 5; i++) {
            size_t idx = m_page * 5 + i;
            if (idx >= items.size()) break;
            float y = size.height - 62.f - i * 30.f;
            auto label = fmt::format("{}. {}", idx + 1, items[idx]);
            auto spr = ButtonSprite::create(shorten(label, 26).c_str(), 220, true, "bigFont.fnt", "GJ_button_04.png", 24.f, .5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ListPopup::onPick));
            btn->setTag((int)idx);
            btn->setPosition({16.f + 110.f + 8.f, y});
            menu->addChild(btn);

            auto ups = ButtonSprite::create("^", 22, true, "bigFont.fnt", "GJ_button_02.png", 24.f, .5f);
            auto up = CCMenuItemSpriteExtra::create(ups, this, menu_selector(ListPopup::onUp));
            up->setTag((int)idx);
            up->setPosition({size.width - 108.f, y});
            menu->addChild(up);

            auto dns = ButtonSprite::create("v", 22, true, "bigFont.fnt", "GJ_button_02.png", 24.f, .5f);
            auto dn = CCMenuItemSpriteExtra::create(dns, this, menu_selector(ListPopup::onDown));
            dn->setTag((int)idx);
            dn->setPosition({size.width - 72.f, y});
            menu->addChild(dn);

            auto xs = ButtonSprite::create("X", 22, true, "bigFont.fnt", "GJ_button_06.png", 24.f, .5f);
            auto del = CCMenuItemSpriteExtra::create(xs, this, menu_selector(ListPopup::onRemove));
            del->setTag((int)idx);
            del->setPosition({size.width - 36.f, y});
            menu->addChild(del);
        }
        if (items.empty()) makeLabel("Empty list - press Add", .45f, {size.width / 2, size.height / 2}, m_view, {.5f, .5f});
        makeLabel("Tap = load   ^ v = reorder   X = remove", .26f, {16.f, size.height - 34.f}, m_view);
        makeLabel(fmt::format("{}/{}", m_page + 1, pages).c_str(), .4f, {size.width / 2, 20.f}, m_view, {.5f, .5f});
        smallButton(menu, this, "<", menu_selector(ListPopup::onPage), {size.width / 2 - 50.f, 20.f}, -1);
        smallButton(menu, this, ">", menu_selector(ListPopup::onPage), {size.width / 2 + 50.f, 20.f}, 1);
        smallButton(menu, this, "Add", menu_selector(ListPopup::onAdd), {size.width - 56.f, 20.f});
    }

    void onPage(CCObject* s) {
        m_page += static_cast<CCNode*>(s)->getTag();
        rebuild();
    }

    void onAdd(CCObject*) {
        Ref<ListPopup> self(this);
        AddToListPopup::create(m_list, [self] { self->rebuild(); })->show();
    }

    void onPick(CCObject* s) {
        size_t idx = static_cast<CCNode*>(s)->getTag();
        auto lists = loadLists();
        if (m_list >= (int)lists.size() || idx >= lists[m_list].items.size()) return;
        auto& b = Bot::get();
        if (b.loadMacro(Bot::get().macroDir() / lists[m_list].items[idx])) {
            b.applyTimeScale();
            Notification::create("Loaded: " + b.info(), NotificationIcon::Success, 2.f)->show();
            this->onClose(nullptr);
        } else {
            FLAlertLayer::create("Error", "Could not read this macro (was the file deleted?).", "OK")->show();
        }
    }

    void move(size_t idx, int dir) {
        auto lists = loadLists();
        if (m_list >= (int)lists.size()) return;
        auto& items = lists[m_list].items;
        if (idx >= items.size()) return;
        long to = (long)idx + dir;
        if (to < 0 || to >= (long)items.size()) return;
        std::swap(items[idx], items[to]);
        saveLists(lists);
        rebuild();
    }

    void onUp(CCObject* s) { move(static_cast<CCNode*>(s)->getTag(), -1); }
    void onDown(CCObject* s) { move(static_cast<CCNode*>(s)->getTag(), 1); }

    void onRemove(CCObject* s) {
        size_t idx = static_cast<CCNode*>(s)->getTag();
        auto lists = loadLists();
        if (m_list >= (int)lists.size() || idx >= lists[m_list].items.size()) return;
        lists[m_list].items.erase(lists[m_list].items.begin() + idx);
        saveLists(lists);
        rebuild();
    }

public:
    static ListPopup* create(int list) {
        auto ret = new ListPopup();
        ret->m_list = list;
        if (ret->init(380.f, 250.f) && ret->setup()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// All lists
class ListsPopup : public geode::Popup {
protected:
    int m_page = 0;
    CCNode* m_view = nullptr;

    bool setup() {
        this->setTitle("Macro Lists");
        m_view = CCNode::create();
        m_mainLayer->addChild(m_view);
        rebuild();
        return true;
    }

    void rebuild() {
        m_view->removeAllChildren();
        auto size = m_mainLayer->getContentSize();
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_view->addChild(menu);

        auto lists = loadLists();
        int pages = std::max(1, (int)((lists.size() + 4) / 5));
        m_page = std::clamp(m_page, 0, pages - 1);

        for (int i = 0; i < 5; i++) {
            size_t idx = m_page * 5 + i;
            if (idx >= lists.size()) break;
            float y = size.height - 62.f - i * 30.f;
            auto label = fmt::format("{} ({})", lists[idx].name, lists[idx].items.size());
            auto spr = ButtonSprite::create(shorten(label, 34).c_str(), 270, true, "bigFont.fnt", "GJ_button_04.png", 24.f, .5f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ListsPopup::onOpen));
            btn->setTag((int)idx);
            btn->setPosition({size.width / 2 - 22.f, y});
            menu->addChild(btn);

            auto xs = ButtonSprite::create("X", 22, true, "bigFont.fnt", "GJ_button_06.png", 24.f, .5f);
            auto del = CCMenuItemSpriteExtra::create(xs, this, menu_selector(ListsPopup::onDelete));
            del->setTag((int)idx);
            del->setPosition({size.width - 36.f, y});
            menu->addChild(del);
        }
        if (lists.empty()) makeLabel("No lists yet - press New", .45f, {size.width / 2, size.height / 2}, m_view, {.5f, .5f});
        makeLabel(fmt::format("{}/{}", m_page + 1, pages).c_str(), .4f, {size.width / 2, 20.f}, m_view, {.5f, .5f});
        smallButton(menu, this, "<", menu_selector(ListsPopup::onPage), {size.width / 2 - 50.f, 20.f}, -1);
        smallButton(menu, this, ">", menu_selector(ListsPopup::onPage), {size.width / 2 + 50.f, 20.f}, 1);
        smallButton(menu, this, "New", menu_selector(ListsPopup::onNew), {size.width - 56.f, 20.f});
    }

    void onPage(CCObject* s) {
        m_page += static_cast<CCNode*>(s)->getTag();
        rebuild();
    }

    void onNew(CCObject*) {
        Ref<ListsPopup> self(this);
        NewListPopup::create([self] { self->rebuild(); })->show();
    }

    void onOpen(CCObject* s) {
        ListPopup::create(static_cast<CCNode*>(s)->getTag())->show();
    }

    void onDelete(CCObject* s) {
        size_t idx = static_cast<CCNode*>(s)->getTag();
        auto lists = loadLists();
        if (idx >= lists.size()) return;
        Ref<ListsPopup> self(this);
        createQuickPopup("Delete list", "Delete list <cy>" + lists[idx].name + "</c>? (the macros stay)", "No", "Yes",
                         [self, idx](FLAlertLayer*, bool yes) {
                             if (!yes) return;
                             auto l = loadLists();
                             if (idx < l.size()) l.erase(l.begin() + idx);
                             saveLists(l);
                             self->rebuild();
                         });
    }

public:
    static ListsPopup* create() {
        auto ret = new ListsPopup();
        if (ret->init(380.f, 250.f) && ret->setup()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Main menu popup
// ---------------------------------------------------------------------------

class BotPopup : public geode::Popup {
protected:
    int m_page = 0;   // 0 Macro, 1 Hacks, 2 Tools
    CCNode* m_content = nullptr;

    bool setup() {
        this->setTitle("Macro Bot Mobile");
        m_content = CCNode::create();
        m_mainLayer->addChild(m_content);
        rebuild();
        return true;
    }

    CCMenuItemToggler* toggle(CCMenu* menu, char const* label, CCPoint pos, bool on, int tag) {
        auto t = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(BotPopup::onToggle), .55f);
        t->toggle(on);
        t->setTag(tag);
        t->setPosition(pos);
        menu->addChild(t);
        makeLabel(label, .38f, {pos.x + 18.f, pos.y}, m_content);
        return t;
    }

    void stepper(CCMenu* menu, char const* label, CCPoint pos, std::string value, int decTag, int incTag) {
        makeLabel(label, .38f, {pos.x - 8.f, pos.y}, m_content);
        auto mk = [&](char const* t, int tag, float dx) {
            auto s = ButtonSprite::create(t, "bigFont.fnt", "GJ_button_01.png", .8f);
            s->setScale(.5f);
            auto b = CCMenuItemSpriteExtra::create(s, this, menu_selector(BotPopup::onAction));
            b->setTag(tag);
            b->setPosition({pos.x + dx, pos.y});
            menu->addChild(b);
        };
        mk("-", decTag, 100.f);
        makeLabel(value.c_str(), .38f, {pos.x + 140.f, pos.y}, m_content, {.5f, .5f});
        mk("+", incTag, 180.f);
    }

    void button(CCMenu* menu, char const* text, CCPoint pos, int tag, float scale = .6f) {
        auto s = ButtonSprite::create(text, "bigFont.fnt", "GJ_button_01.png", .8f);
        s->setScale(scale);
        auto b = CCMenuItemSpriteExtra::create(s, this, menu_selector(BotPopup::onAction));
        b->setTag(tag);
        b->setPosition(pos);
        menu->addChild(b);
    }

    void rebuild() {
        m_content->removeAllChildren();
        auto& b = Bot::get();
        auto& c = b.cfg;
        auto size = m_mainLayer->getContentSize();

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_content->addChild(menu);

        float left = 26.f;
        float top = size.height - 56.f;
        float row = 26.f;

        static char const* pages[] = {"Macro", "Hacks", "Tools", "Window"};
        makeLabel(pages[m_page], .5f, {size.width / 2, size.height - 38.f}, m_content, {.5f, .5f});
        button(menu, "<", {size.width / 2 - 70.f, size.height - 38.f}, A_PAGE_PREV, .45f);
        button(menu, ">", {size.width / 2 + 70.f, size.height - 38.f}, A_PAGE_NEXT, .45f);

        if (m_page == 0) {
            toggle(menu, "Record", {left, top - 4.f}, b.state == BotState::Record, A_REC);
            toggle(menu, "Play", {left, top - 4.f - row}, b.state == BotState::Play, A_PLAY);
            toggle(menu, "Auto-save recordings", {left, top - 4.f - row * 2}, c.autoSave, A_AUTOSAVE);

            button(menu, "Save", {size.width - 140.f, top - 4.f}, A_SAVE);
            button(menu, "Load", {size.width - 70.f, top - 4.f}, A_LOAD);
            button(menu, "Clear", {size.width - 105.f, top - 4.f - row}, A_CLEAR);
            button(menu, "Lists", {size.width - 105.f, top - 4.f - row * 2}, A_LISTS);

            makeLabel(b.info().c_str(), .3f, {left, 54.f}, m_content);
            auto dir = b.macroDir().string();
            makeLabel(("Folder: " + dir).c_str(), .2f, {left, 36.f}, m_content);
            makeLabel("Record/Play restart the level from the beginning.", .26f, {left, 20.f}, m_content);
        } else if (m_page == 1) {
            float x2 = size.width / 2 + 6.f;
            toggle(menu, "Noclip", {left, top}, c.noclip, A_NOCLIP);
            toggle(menu, "Safe mode (no clear)", {left, top - row}, c.safeMode, A_SAFE);
            toggle(menu, "Instant respawn", {left, top - row * 2}, c.instantRespawn, A_INSTANT);
            toggle(menu, "No death effect", {left, top - row * 3}, c.noDeathEffect, A_NODEATH);
            stepper(menu, "Speed", {left, top - row * 4 - 6.f}, fmt::format("{:.1f}x", c.speed), A_SPEED_DEC, A_SPEED_INC);

            toggle(menu, "Speedhack", {x2, top}, c.speedOn, A_SPEEDON);
            toggle(menu, "TPS bypass", {x2, top - row}, c.tpsOn, A_TPSON);

            static char const* acc[] = {"Vanilla", "Input Fixes", "Frame Fixes"};
            button(menu, acc[std::clamp(c.accuracy, 0, 2)], {x2 + 50.f, top - row * 2}, A_ACCURACY, .45f);
            makeLabel("Accuracy", .3f, {x2 - 8.f, top - row * 2 - 14.f}, m_content);
        } else if (m_page == 2) {
            stepper(menu, "Speed", {left, top}, fmt::format("{:.2f}x", c.speed), A_SPEED_DEC, A_SPEED_INC);
            stepper(menu, "TPS", {left, top - row}, fmt::format("{:.0f}", c.tps), A_TPS_DEC, A_TPS_INC);
            stepper(menu, "Frame offset", {left, top - row * 2}, std::to_string(c.offset), A_OFF_DEC, A_OFF_INC);

            toggle(menu, "Autoclicker", {left, top - row * 3 - 2.f}, c.autoclick, A_AUTOCLICK);
            toggle(menu, "P1", {left + 290.f, top - row * 3 - 2.f}, c.acP1, A_ACP1);

            stepper(menu, "Click hold", {left, top - row * 4 - 4.f}, std::to_string(c.acHold), A_ACH_DEC, A_ACH_INC);
            stepper(menu, "Click release", {left, top - row * 5 - 4.f}, std::to_string(c.acRel), A_ACR_DEC, A_ACR_INC);
            toggle(menu, "P2", {left + 290.f, top - row * 4 - 4.f}, c.acP2, A_ACP2);
        } else {
            toggle(menu, "Frame window (ring + sound)", {left, top}, c.windowOn, A_WINDOW);
            stepper(menu, "Ring max (f@60)", {left, top - row * 1 - 6.f}, std::to_string(c.windowMax), A_WIN_DEC, A_WIN_INC);
            makeLabel("Window sizes seen this run (60 FPS frames):", .28f, {left, top - row * 2 - 4.f}, m_content);
            static char const* names[6] = {"0.25f", "0.5f", "0.75f", "1f", "1.25-2f", "2f+"};
            for (int i = 0; i < 6; ++i) {
                float x = left + (i % 3) * 120.f;
                float y = top - row * 3 - 4.f - (i / 3) * 18.f;
                makeLabel(fmt::format("{}: {}", names[i], Win::statBuckets[i]).c_str(), .34f, {x, y}, m_content);
            }
            makeLabel("Counts reset when a run restarts from the start.", .24f, {left, top - row * 3 - 52.f}, m_content);
        }
    }

    void onToggle(CCObject* s) {
        auto& b = Bot::get();
        auto& c = b.cfg;
        switch (static_cast<CCNode*>(s)->getTag()) {
            case A_REC: {
                bool wasRec = b.state == BotState::Record;
                if (wasRec) {
                    b.setState(BotState::None);
                    rebuild();
                } else {
                    b.setState(BotState::Record);
                    this->onClose(nullptr);
                    startRun();
                }
                return;
            }
            case A_PLAY: {
                if (b.state == BotState::Play) {
                    b.setState(BotState::None);
                    rebuild();
                } else if (b.macro.empty()) {
                    Notification::create("Load a macro first", NotificationIcon::Error, 1.5f)->show();
                    rebuild();
                } else {
                    b.setState(BotState::Play);
                    b.applyTimeScale();
                    this->onClose(nullptr);
                    startRun();
                }
                return;
            }
            case A_NOCLIP: c.noclip = !c.noclip; break;
            case A_SAFE: c.safeMode = !c.safeMode; break;
            case A_INSTANT: c.instantRespawn = !c.instantRespawn; break;
            case A_NODEATH: c.noDeathEffect = !c.noDeathEffect; break;
            case A_AUTOSAVE: c.autoSave = !c.autoSave; break;
            case A_SPEEDON: c.speedOn = !c.speedOn; b.applyTimeScale(); break;
            case A_TPSON: c.tpsOn = !c.tpsOn; break;
            case A_STEPPER: c.stepper = !c.stepper; b.stepReq = 0; break;
            case A_AUTOCLICK: c.autoclick = !c.autoclick; break;
            case A_WINDOW: c.windowOn = !c.windowOn; break;
            case A_ACP1: c.acP1 = !c.acP1; break;
            case A_ACP2: c.acP2 = !c.acP2; break;
            default: break;
        }
        c.save();
        b.refreshHud();
    }

    void onAction(CCObject* s) {
        auto& b = Bot::get();
        auto& c = b.cfg;
        switch (static_cast<CCNode*>(s)->getTag()) {
            case A_PAGE_PREV: m_page = (m_page + 3) % 4; break;
            case A_PAGE_NEXT: m_page = (m_page + 1) % 4; break;
            case A_SAVE:
                if (b.macro.empty()) {
                    FLAlertLayer::create("Nothing to save", "Record a macro first.", "OK")->show();
                    return;
                }
                SavePopup::create()->show();
                return;
            case A_LOAD: LoadPopup::create()->show(); return;
            case A_LISTS: ListsPopup::create()->show(); return;
            case A_CLEAR: b.macro.clear(); b.setState(BotState::None); break;
            case A_ACCURACY: c.accuracy = (c.accuracy + 1) % 3; break;
            case A_SPEED_DEC: c.speed = std::max(0.1f, std::round((c.speed - 0.1f) * 10.f) / 10.f); b.applyTimeScale(); break;
            case A_SPEED_INC: c.speed = std::min(10.f, std::round((c.speed + 0.1f) * 10.f) / 10.f); b.applyTimeScale(); break;
            case A_TPS_DEC: c.tps = std::max(1.f, c.tps - 10.f); break;
            case A_TPS_INC: c.tps = std::min(10000.f, c.tps + 10.f); break;
            case A_OFF_DEC: c.offset = std::max(-8, c.offset - 1); break;
            case A_OFF_INC: c.offset = std::min(8, c.offset + 1); break;
            case A_ACH_DEC: c.acHold = std::max(1, c.acHold - 1); break;
            case A_ACH_INC: c.acHold = std::min(240, c.acHold + 1); break;
            case A_ACR_DEC: c.acRel = std::max(1, c.acRel - 1); break;
            case A_ACR_INC: c.acRel = std::min(240, c.acRel + 1); break;
            case A_WIN_DEC: c.windowMax = std::max(1, c.windowMax - 1); break;
            case A_WIN_INC: c.windowMax = std::min(8, c.windowMax + 1); break;
            default: break;
        }
        c.save();
        rebuild();
    }

public:
    static BotPopup* create() {
        auto ret = new BotPopup();
        if (ret->init(400.f, 250.f) && ret->setup()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

void openBotMenu() {
    if (auto p = BotPopup::create()) p->show();
}
