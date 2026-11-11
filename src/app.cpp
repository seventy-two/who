#include "app.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

#include "imgui_internal.h"
#include "net.h"
#include "qrcodegen/qrcodegen.hpp"
#include "waformat.h"

namespace who {

namespace fs = std::filesystem;
using json = nlohmann::json;
using Microsoft::WRL::ComPtr;

namespace {

int64_t nowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{ft.dwLowDateTime, ft.dwHighDateTime};
    return int64_t(u.QuadPart / 10000) - 11644473600000LL;
}

std::tm localTm(int64_t ms) {
    std::time_t t = std::time_t(ms / 1000);
    std::tm tm{};
    localtime_s(&tm, &t);
    return tm;
}

int dayNumber(int64_t ms) {
    std::tm tm = localTm(ms);
    return (tm.tm_year + 1900) * 400 + tm.tm_yday;
}

std::string clock(int64_t ms) {
    std::tm tm = localTm(ms);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

// listTime is a chat row's time: today's clock, Yesterday, a weekday this
// week, else the date.
std::string listTime(int64_t ms) {
    if (ms <= 0) return "";
    int64_t now = nowMs();
    int d = dayNumber(now) - dayNumber(ms);
    if (d == 0) return clock(ms);
    if (d == 1) return "Yesterday";
    std::tm tm = localTm(ms);
    char buf[32];
    if (d < 7 && d > 0) {
        static const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        return days[tm.tm_wday];
    }
    std::snprintf(buf, sizeof(buf), "%02d/%02d/%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year % 100);
    return buf;
}

std::string dayLabel(int64_t ms) {
    int d = dayNumber(nowMs()) - dayNumber(ms);
    if (d == 0) return "Today";
    if (d == 1) return "Yesterday";
    std::tm tm = localTm(ms);
    static const char* months[] = {"January", "February", "March", "April", "May", "June",
                                   "July", "August", "September", "October", "November", "December"};
    static const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    char buf[64];
    if (d > 0 && d < 7) return days[tm.tm_wday];
    std::snprintf(buf, sizeof(buf), "%d %s %d", tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900);
    return buf;
}

std::string sizeText(int64_t b) {
    char buf[32];
    if (b >= 1 << 20) std::snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
    else if (b >= 1024) std::snprintf(buf, sizeof(buf), "%d KB", int(b / 1024));
    else std::snprintf(buf, sizeof(buf), "%d B", int(b));
    return buf;
}

std::string mmss(double s) {
    int t = int(s + 0.5);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
    return buf;
}

std::string unbase64(const std::string& in) {
    std::string out;
    unsigned v = 0;
    int bits = 0;
    for (char c : in) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else continue;
        v = v << 6 | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((v >> bits) & 0xFF);
        }
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string str(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int64_t num(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<int64_t>() : 0;
}

bool flag(const json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

// A colour per group member, so names are told apart at a glance.
ImU32 nameColour(const std::string& name) {
    static const unsigned palette[] = {0x5AA8FF, 0x3FB950, 0xD9A23A, 0xF8645A, 0xA78BFA, 0x2DD4BF, 0xF472B6, 0xFB923C};
    unsigned h = 2166136261u;
    for (unsigned char c : name) h = (h ^ c) * 16777619u;
    return U32(Hex(palette[h % 8]));
}

void openPath(const std::string& path) {
    ShellExecuteW(nullptr, L"open", Widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

namespace icon2 {
const char* Search = "\xEE\x9C\xA1";
const char* Check = "\xEE\x9C\xBE";     // U+E73E
const char* Pin = "\xEE\x9C\x98";       // U+E718
const char* MuteBell = "\xEE\x9D\x8F";  // U+E74F Mute
const char* Archive = "\xEE\x9F\xB8";   // U+E7B8
const char* Doc = "\xEE\x9F\x83";       // U+E7C3 Page
const char* Image = "\xEE\xAB\xA9";     // U+EB9F Photo2
const char* Back = "\xEE\x9C\xAB";
const char* Copy = "\xEE\xA3\x88";      // U+E8C8
const char* Save = "\xEE\x9D\x8E";      // U+E74E
const char* Location = "\xEE\xA0\x87";  // U+E707 MapPin
}  // namespace icon2

}  // namespace

// Preview is a message's one-line summary, as in reply bars.
std::string Preview(const Message& m) {
    static const std::map<std::string, std::string> labels = {
        {"image", "Photo"}, {"video", "Video"}, {"gif", "GIF"}, {"sticker", "Sticker"}, {"voice", "Voice message"},
        {"audio", "Audio"}, {"document", "Document"}, {"location", "Location"}, {"contact", "Contact"}, {"poll", "Poll"},
        {"deleted", "This message was deleted"}};
    auto it = labels.find(m.kind);
    std::string text = m.text;
    for (char& c : text)
        if (c == '\n') c = ' ';
    if (it == labels.end()) return text;
    return text.empty() ? it->second : it->second + ": " + text;
}

bool Chat::Muted() const { return mutedUntil == -1 || mutedUntil > nowMs(); }

App::App(HWND hwnd, ID3D11Device* device, std::function<void()> wake) : hwnd_(hwnd), wake_(wake) {
    if (const wchar_t* d = _wgetenv(L"WHO_DATA"); d && *d) {
        dir_ = Narrow(d);
    } else {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) dir_ = Narrow(p) + "\\who";
        CoTaskMemFree(p);
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(dir_), ec);
    loadConfig();
    images_ = std::make_unique<ImageCache>(device, wake);
    fonts_ = LoadFonts();
    ApplyTheme(true, scale_);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring engineExe = (fs::path(exe).parent_path() / L"who-engine.exe").wstring();
    engine_ = std::make_unique<Engine>(engineExe, Widen(dir_), wake);
    if (const wchar_t* d = _wgetenv(L"WHO_DEMO"); d && *d) {
        demo();
        return;
    }
    engine_->Start();
    if (!cfg_.open.empty()) open_ = cfg_.open;
}

// demo fills Who with made-up chats for checking the layout (WHO_DEMO=1);
// it starts no engine and touches no network but emoji images.
void App::demo() {
    demo_ = true;
    state_ = "connected";
    synced_ = true;
    int64_t now = nowMs();
    auto chat = [&](const char* jid, const char* name, bool group, int unread, int64_t ago, const char* last, bool pinned,
                    bool muted) {
        Chat c;
        c.jid = jid;
        c.name = name;
        c.group = group;
        c.unread = unread;
        c.lastTs = now - ago;
        c.lastText = last;
        c.pinned = pinned;
        c.mutedUntil = muted ? -1 : 0;
        chats_.push_back(c);
    };
    chat("1@g.us", "Allotment Society \xF0\x9F\x8C\xB1", true, 0, 60000, "Courgettes everywhere again", true, false);
    chat("2@s.whatsapp.net", "Priya Raman", false, 0, 3600000, "See you at 6!", false, false);
    chat("3@s.whatsapp.net", "Grandad", false, 1, 86400000, "Photo", false, false);
    chat("4@g.us", "Climbing Wednesdays", true, 12, 2 * 86400000, "Who's got the rope this week?", false, true);
    chat("5@s.whatsapp.net", "~Marcus", false, 0, 3 * 86400000, "\xF0\x9F\x98\x82\xF0\x9F\x98\x82", false, false);
    chat("6@g.us", "Book club", true, 0, 9 * 86400000, "Next one's The Long Way Down", false, false);
    open_ = "1@g.us";
    auto& c = convos_[open_];
    c.loaded = true;
    c.more = false;
    auto msg = [&](const char* sender, const char* name, bool mine, int64_t ago, const char* kind, const char* text) -> Message& {
        Message m;
        m.chat = open_;
        m.id = std::to_string(c.messages.size());
        m.sender = sender;
        m.senderName = name;
        m.fromMe = mine;
        m.ts = now - ago;
        m.kind = kind;
        m.text = text;
        m.status = mine ? "read" : "";
        c.messages.push_back(m);
        return c.messages.back();
    };
    msg("a", "Dot Wheeler", false, 26 * 3600000LL, "text", "Morning all! Water butt by plot 9 is *empty* again");
    msg("b", "Sam Okafor", false, 26 * 3600000LL - 60000, "text", "That was me, sorry \xF0\x9F\x99\x88 I'll fill it tonight");
    msg("me", "You", true, 25 * 3600000LL, "text", "Anyone want rhubarb? I've got _far_ too much");
    Message& v = msg("a", "Dot Wheeler", false, 40 * 60000, "voice", "");
    v.seconds = 23;
    Message& d = msg("b", "Sam Okafor", false, 25 * 60000, "document", "");
    d.fileName = "sowing-calendar.pdf";
    d.size = 241000;
    Message& r = msg("me", "You", true, 10 * 60000, "text", "Courgettes everywhere again \xF0\x9F\xA5\x92 swap you some for the rhubarb? https://example.org/recipes");
    r.replyId = "2";
    r.replyName = "You";
    r.replyText = "Anyone want rhubarb? I've got far too much";
    r.reactions = {{"\xF0\x9F\x91\x8D", {"Dot Wheeler", "Sam Okafor"}}, {"\xE2\x9D\xA4\xEF\xB8\x8F", {"Lena Brook"}}};
    r.status = "delivered";
    msg("c", "Lena Brook", false, 60000, "text", "\xF0\x9F\x98\x82\xF0\x9F\x98\x82");
    msg("a", "Dot Wheeler", false, 30000, "deleted", "");
    notices_.push_front({"1@g.us", "Allotment Society", "Lena Brook", "\xF0\x9F\x98\x82\xF0\x9F\x98\x82", now - 60000, true});
    notices_.push_front({"3@s.whatsapp.net", "Grandad", "Grandad", "Photo", now - 3600000, false});
    unseen_ = 2;
    typing_["2@s.whatsapp.net"] = {"Priya Raman is typing", 1e9};
}

App::~App() {
    if (!open_.empty()) drafts_[open_] = compose_;
    cfg_.open = open_;
    saveConfig();
}

void App::loadConfig() {
    std::ifstream in(fs::u8path(dir_) / "config.json");
    if (!in) return;
    try {
        json j = json::parse(in);
        if (j.contains("window")) {
            cfg_.x = j["window"].value("x", 0);
            cfg_.y = j["window"].value("y", 0);
            cfg_.w = j["window"].value("w", 0);
            cfg_.h = j["window"].value("h", 0);
            cfg_.placed = cfg_.w > 0 && cfg_.h > 0;
        }
        cfg_.open = j.value("open", "");
    } catch (...) {
    }
}

void App::saveConfig() {
    json j;
    if (cfg_.placed) j["window"] = {{"x", cfg_.x}, {"y", cfg_.y}, {"w", cfg_.w}, {"h", cfg_.h}};
    j["open"] = open_;
    std::ofstream out(fs::u8path(dir_) / "config.json");
    out << j.dump(2) << "\n";
}

bool App::Place(RECT& r) const {
    if (!cfg_.placed) return false;
    r = RECT{cfg_.x, cfg_.y, cfg_.x + cfg_.w, cfg_.y + cfg_.h};
    return true;
}

void App::SavePlace(const RECT& r) {
    cfg_.placed = true;
    cfg_.x = r.left;
    cfg_.y = r.top;
    cfg_.w = r.right - r.left;
    cfg_.h = r.bottom - r.top;
    saveConfig();
}

bool App::Visible() const { return IsWindowVisible(hwnd_) && !IsIconic(hwnd_); }

void App::ShowWindow() {
    ::ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

int App::Unread() const {
    int n = 0;
    for (const auto& c : chats_)
        if (!c.Muted() && !c.archived) n += c.unread;
    return n;
}

void App::SetScale(float scale) {
    scale_ = scale;
    ApplyTheme(true, scale_);
    for (auto& [jid, c] : convos_)
        for (auto& m : c.messages) m.layoutWidth = -1;
}

// testInput plays WHO_INPUT, a demo-mode test script of mouse steps fed to
// ImGui one a frame ("move x y", "down", "up", "wait n"), so clicks can be
// tested without the real cursor.
static void testInput(const std::string& logPath) {
    static std::vector<std::string> steps;
    static size_t next = 0;
    static int waiting = 0;
    static bool read = false;
    static float mx = -1, my = -1;
    if (!read) {
        read = true;
        if (const char* s = std::getenv("WHO_INPUT")) {
            std::string cur;
            for (char ch : std::string(s) + ";") {
                if (ch == ';') {
                    if (!cur.empty()) steps.push_back(cur);
                    cur.clear();
                } else {
                    cur += ch;
                }
            }
        }
    }
    if (steps.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDoubleClickTime = 3.0f;
    if (mx >= 0) io.AddMousePosEvent(mx, my);  // over the backend's real cursor
    if (waiting > 0) {
        waiting--;
        return;
    }
    if (next >= steps.size()) return;
    std::string st = steps[next++];
    float x, y;
    int n;
    if (std::sscanf(st.c_str(), " move %f %f", &x, &y) == 2) {
        mx = x;
        my = y;
        io.AddMousePosEvent(x, y);
    } else if (st.find("down") != std::string::npos) {
        io.AddMouseButtonEvent(0, true);
    } else if (st.find("up") != std::string::npos) {
        io.AddMouseButtonEvent(0, false);
    } else if (std::sscanf(st.c_str(), " wait %d", &n) == 1) {
        waiting = n;
    } else if (st.find("shot") != std::string::npos) {
        // Marks the log so a capture can be matched to a step.
    }
    if (FILE* f = std::fopen(logPath.c_str(), "ab")) {
        std::fprintf(f, "input: %s\n", st.c_str());
        std::fclose(f);
    }
}

bool App::BeforeFrame() {
    if (demo_) testInput(dir_ + "\\input.log");
    images_->Tick();
    if (LoadNeededFonts(fonts_)) {
        for (auto& [jid, c] : convos_)
            for (auto& m : c.messages) m.layoutWidth = -1;
        return true;
    }
    return false;
}

bool App::Animating() const {
    if (demo_ && std::getenv("WHO_INPUT")) return true;
    return voice_.Playing() || images_->Animating() || state_ == "qr" || !typing_.empty() ||
           (!note_.empty() && ImGui::GetTime() < noteUntil_);
}

bool App::InCaption(float x, float y) const {
    if (x >= barMax_.x || y >= barMax_.y) return false;
    for (const auto& [a, b] : hot_)
        if (x >= a.x && y >= a.y && x < b.x && y < b.y) return false;
    return true;
}

// ---------------------------------------------------------------- events

Chat App::parseChat(const json& j) {
    Chat c;
    c.jid = str(j, "jid");
    c.name = str(j, "name");
    c.group = flag(j, "group");
    c.unread = int(num(j, "unread"));
    c.lastTs = num(j, "lastTs");
    c.lastText = str(j, "lastText");
    c.lastSender = str(j, "lastSender");
    c.lastFromMe = flag(j, "lastFromMe");
    c.pinned = flag(j, "pinned");
    c.mutedUntil = num(j, "mutedUntil");
    c.archived = flag(j, "archived");
    NoteText(c.name);
    NoteText(c.lastText);
    return c;
}

Message App::parseMessage(const json& j) {
    Message m;
    m.chat = str(j, "chat");
    m.id = str(j, "id");
    m.sender = str(j, "sender");
    m.senderName = str(j, "senderName");
    m.fromMe = flag(j, "fromMe");
    m.ts = num(j, "ts");
    m.kind = str(j, "kind");
    m.text = str(j, "text");
    m.fileName = str(j, "fileName");
    m.mime = str(j, "mime");
    m.seconds = int(num(j, "seconds"));
    m.width = int(num(j, "width"));
    m.height = int(num(j, "height"));
    m.size = num(j, "size");
    m.replyId = str(j, "replyId");
    m.replyName = str(j, "replyName");
    m.replyText = str(j, "replyText");
    m.status = str(j, "status");
    m.edited = flag(j, "edited");
    m.media = str(j, "media");
    std::string thumb = str(j, "thumb");
    if (!thumb.empty()) {
        m.thumb = unbase64(thumb);
        std::string key = "thumb:" + m.chat + "/" + m.id;
        std::lock_guard<std::mutex> lock(filesMu_);
        if (!filesReady_.count(key)) {
            images_->Provide(key, m.thumb);
            filesReady_.insert(key);
        }
    }
    if (auto it = j.find("reactions"); it != j.end() && it->is_object())
        for (auto& [emoji, who] : it->items()) {
            std::vector<std::string> names;
            for (auto& w : who)
                if (w.is_string()) names.push_back(w.get<std::string>());
            m.reactions.push_back({emoji, names});
        }
    NoteText(m.text);
    NoteText(m.senderName);
    NoteText(m.replyText);
    return m;
}

void App::putMessage(Message m) {
    auto it = convos_.find(m.chat);
    if (it == convos_.end() || !it->second.loaded) return;  // loaded when opened
    auto& msgs = it->second.messages;
    for (auto& old : msgs)
        if (old.id == m.id) {
            if (m.media.empty()) m.media = old.media;
            old = std::move(m);
            return;
        }
    bool atEnd = msgs.empty() || m.ts >= msgs.back().ts;
    msgs.push_back(std::move(m));
    if (!atEnd)
        std::stable_sort(msgs.begin(), msgs.end(), [](const Message& a, const Message& b) { return a.ts < b.ts; });
}

void App::Pump() {
    std::vector<json> events;
    engine_->Drain(events);
    for (const auto& e : events) onEvent(e);
    images_->Upload();

    // Online while the window is in front (as WhatsApp Web does), so
    // typing shows; otherwise the phone keeps getting notifications.
    bool front = GetForegroundWindow() == hwnd_ && Visible();
    if (front != cfg_.online && state_ == "connected") {
        cfg_.online = front;
        engine_->Send({{"t", "online"}, {"on", front}});
        if (front && !open_.empty()) markRead(open_);
    }
    // Typing stops being shown after a while without word.
    double now = ImGui::GetTime();
    for (auto it = typing_.begin(); it != typing_.end();)
        it = it->second.second < now ? typing_.erase(it) : std::next(it);
    if (typingSent_ && now - lastTypingSent_ > 5) {
        engine_->Send({{"t", "typing"}, {"chat", open_}, {"on", false}});
        typingSent_ = false;
    }
}

void App::onEvent(const json& e) {
    std::string t = str(e, "t");
    if (t == "state") {
        state_ = str(e, "state");
        stateError_ = str(e, "error");
        if (state_ == "qr") {
            qr_ = str(e, "qr");
            qrUntil_ = ImGui::GetTime() + double(num(e, "seconds"));
        }
        if (state_ == "connected") {
            me_ = str(e, "me");
            myName_ = str(e, "name");
            engine_->Send({{"t", "chats"}});
            cfg_.online = false;  // sent again by Pump
            if (!open_.empty()) loadMessages(open_, false);
        }
        if (state_ == "loggedout") {
            chats_.clear();
            convos_.clear();
            open_.clear();
        }
    } else if (t == "chats") {
        chats_.clear();
        if (e.contains("chats") && e["chats"].is_array())
            for (const auto& c : e["chats"]) chats_.push_back(parseChat(c));
        if (!open_.empty() && !convos_.count(open_)) loadMessages(open_, false);
    } else if (t == "chat") {
        Chat c = parseChat(e["chat"]);
        bool found = false;
        for (auto& o : chats_)
            if (o.jid == c.jid) {
                o = c;
                found = true;
            }
        if (!found) chats_.push_back(c);
    } else if (t == "messages") {
        std::string chat = str(e, "chat");
        auto& c = convos_[chat];
        std::vector<Message> got;
        for (const auto& m : e["messages"]) got.push_back(parseMessage(m));
        bool older = num(e, "before") > 0;
        c.loading = false;
        c.more = flag(e, "more");
        if (older) {
            c.keepFromBottom = 1;  // remembered by the drawing code
            got.insert(got.end(), std::make_move_iterator(c.messages.begin()), std::make_move_iterator(c.messages.end()));
            c.messages = std::move(got);
        } else {
            c.messages = std::move(got);
            c.toBottom = true;
        }
        c.loaded = true;
    } else if (t == "message") {
        Message m = parseMessage(e["message"]);
        bool isNew = flag(e, "new");
        std::string chat = m.chat;
        bool mine = m.fromMe;
        auto it = convos_.find(chat);
        bool atBottom = it != convos_.end() && it->second.toBottom;
        putMessage(std::move(m));
        if (chat == open_ && isNew) {
            convos_[chat].toBottom = atBottom || mine || convos_[chat].toBottom;
            if (!mine && GetForegroundWindow() == hwnd_ && Visible()) markRead(chat);
        }
        typing_.erase(chat);
    } else if (t == "receipt") {
        auto it = convos_.find(str(e, "chat"));
        if (it != convos_.end()) {
            std::string status = str(e, "status");
            for (const auto& id : e["ids"])
                for (auto& m : it->second.messages)
                    if (m.id == id.get<std::string>()) m.status = status;
        }
    } else if (t == "typing") {
        std::string chat = str(e, "chat");
        if (str(e, "state") == "composing") {
            std::string who = str(e, "name");
            if (!who.empty() && who[0] == '~') who.erase(0, 1);
            typing_[chat] = {str(e, "media") == "audio" ? who + " is recording audio" : who + " is typing",
                             ImGui::GetTime() + 8};
        } else {
            typing_.erase(chat);
        }
    } else if (t == "notify") {
        Notice n{str(e, "chat"), str(e, "chatName"), str(e, "sender"), str(e, "text"), nowMs(), flag(e, "group")};
        if (!flag(e, "muted")) notify(n);
    } else if (t == "media") {
        int req = int(num(e, "req"));
        auto it = mediaReqs_.find(req);
        if (it == mediaReqs_.end()) return;
        MediaReq r = it->second;
        mediaReqs_.erase(it);
        std::string key = r.chat + "/" + r.id;
        if (e.contains("error")) {
            mediaErrors_[key] = str(e, "error");
            mediaAsked_.erase(key);
            if (r.action != 0) {
                note_ = str(e, "error");
                noteUntil_ = ImGui::GetTime() + 5;
            }
            return;
        }
        std::string path = str(e, "path");
        auto c = convos_.find(r.chat);
        if (c != convos_.end())
            for (auto& m : c->second.messages)
                if (m.id == r.id) {
                    m.media = path;
                    mediaArrived(m, path, r.action);
                }
    } else if (t == "history") {
        synced_ = true;
        // Newly synced messages: refresh the open chat if it's near empty.
        if (!open_.empty() && convos_[open_].messages.size() < 20) loadMessages(open_, false);
    } else if (t == "names") {
        engine_->Send({{"t", "chats"}});
    } else if (t == "error") {
        note_ = str(e, "error");
        noteUntil_ = ImGui::GetTime() + 6;
    } else if (t == "fatal" || t == "stopped") {
        state_ = "stopped";
        stateError_ = t == "fatal" ? str(e, "error") : engine_->Error();
    }
}

void App::notify(const Notice& n) {
    // Not for the chat being looked at.
    if (n.chat == open_ && GetForegroundWindow() == hwnd_ && Visible()) return;
    notices_.push_front(n);
    while (notices_.size() > 50) notices_.pop_back();
    unseen_++;
    if (!Visible()) {
        FLASHWINFO fi{sizeof(fi), hwnd_, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&fi);
    }
}

void App::openChat(const std::string& jid) {
    if (!open_.empty()) drafts_[open_] = compose_;
    if (typingSent_) {
        engine_->Send({{"t", "typing"}, {"chat", open_}, {"on", false}});
        typingSent_ = false;
    }
    open_ = jid;
    std::snprintf(compose_, sizeof(compose_), "%s", drafts_[jid].c_str());
    replyTo_.clear();
    pendingFiles_.clear();
    focusComposer_ = true;
    auto& c = convos_[jid];
    if (!c.loaded) loadMessages(jid, false);
    c.toBottom = true;
    markRead(jid);
    // Notices from this chat are seen now.
    for (auto it = notices_.begin(); it != notices_.end();) it = it->chat == jid ? notices_.erase(it) : std::next(it);
    unseen_ = std::min<int>(unseen_, int(notices_.size()));
}

void App::loadMessages(const std::string& jid, bool older) {
    auto& c = convos_[jid];
    if (c.loading) return;
    c.loading = true;
    int64_t before = older && !c.messages.empty() ? c.messages.front().ts : 0;
    engine_->Send({{"t", "messages"}, {"chat", jid}, {"before", before}, {"limit", 80}});
}

void App::markRead(const std::string& jid) {
    for (auto& c : chats_)
        if (c.jid == jid && c.unread > 0) {
            c.unread = 0;
            engine_->Send({{"t", "read"}, {"chat", jid}});
        }
}

void App::send() {
    std::string text = compose_;
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    while (!text.empty() && (text.front() == '\n' || text.front() == ' ')) text.erase(0, 1);
    if (!pendingFiles_.empty()) {
        sendFiles(pendingFiles_);
        pendingFiles_.clear();
        return;
    }
    if (text.empty() || open_.empty()) return;
    json cmd = {{"t", "send"}, {"chat", open_}, {"text", text}};
    if (!replyTo_.empty()) cmd["reply"] = replyTo_;
    engine_->Send(cmd);
    compose_[0] = 0;
    replyTo_.clear();
    drafts_[open_].clear();
    if (typingSent_) {
        engine_->Send({{"t", "typing"}, {"chat", open_}, {"on", false}});
        typingSent_ = false;
    }
    convos_[open_].toBottom = true;
}

void App::sendFiles(const std::vector<std::string>& paths) {
    if (open_.empty()) return;
    std::string caption = compose_;
    for (size_t i = 0; i < paths.size(); i++) {
        json cmd = {{"t", "sendfile"}, {"chat", open_}, {"path", paths[i]}};
        if (i == 0 && !caption.empty()) cmd["caption"] = caption;
        if (i == 0 && !replyTo_.empty()) cmd["reply"] = replyTo_;
        engine_->Send(cmd);
    }
    compose_[0] = 0;
    replyTo_.clear();
    convos_[open_].toBottom = true;
    note_ = paths.size() == 1 ? "Sending " + fs::u8path(paths[0]).filename().u8string() + "\xE2\x80\xA6"
                              : "Sending " + std::to_string(paths.size()) + " files\xE2\x80\xA6";
    noteUntil_ = ImGui::GetTime() + 3;
}

void App::DropFiles(const std::vector<std::string>& paths) {
    if (open_.empty() || paths.empty()) return;
    pendingFiles_ = paths;
    focusComposer_ = true;
}

void App::pickFiles() {
    wchar_t buf[8192] = {};
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = ARRAYSIZE(buf);
    ofn.lpstrFilter = L"All files\0*.*\0Images\0*.jpg;*.jpeg;*.png;*.webp\0";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::string> paths;
    std::wstring dir = buf;
    const wchar_t* p = buf + dir.size() + 1;
    if (!*p) {
        paths.push_back(Narrow(dir));
    } else {
        while (*p) {
            std::wstring name = p;
            paths.push_back(Narrow(dir + L"\\" + name));
            p += name.size() + 1;
        }
    }
    DropFiles(paths);
}

// pasteFromClipboard takes copied files, or a copied picture (saved as a
// PNG first), as attachments.
void App::pasteFromClipboard() {
    if (!OpenClipboard(hwnd_)) return;
    std::vector<std::string> paths;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        HDROP drop = HDROP(h);
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t p[MAX_PATH];
            if (DragQueryFileW(drop, i, p, MAX_PATH)) paths.push_back(Narrow(p));
        }
    } else if (HANDLE h = GetClipboardData(CF_DIB)) {
        auto* bi = static_cast<BITMAPINFOHEADER*>(GlobalLock(h));
        if (bi && (bi->biBitCount == 32 || bi->biBitCount == 24) && bi->biCompression != BI_JPEG) {
            int w = bi->biWidth, hgt = std::abs(bi->biHeight);
            bool bottomUp = bi->biHeight > 0;
            int bpp = bi->biBitCount / 8;
            int stride = ((w * bi->biBitCount + 31) / 32) * 4;
            const BYTE* bits = reinterpret_cast<const BYTE*>(bi) + bi->biSize +
                               (bi->biCompression == BI_BITFIELDS ? 12 : 0) + bi->biClrUsed * 4;
            std::vector<BYTE> rgba(size_t(w) * hgt * 4);
            for (int y = 0; y < hgt; y++) {
                const BYTE* row = bits + size_t(bottomUp ? hgt - 1 - y : y) * stride;
                for (int x = 0; x < w; x++) {
                    BYTE* o = &rgba[(size_t(y) * w + x) * 4];
                    o[0] = row[x * bpp];
                    o[1] = row[x * bpp + 1];
                    o[2] = row[x * bpp + 2];
                    o[3] = 255;
                }
            }
            ComPtr<IWICImagingFactory> wic;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
            std::wstring out = Widen(dir_) + L"\\media\\pasted-" + std::to_wstring(nowMs()) + L".png";
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapEncoder> enc;
            ComPtr<IWICBitmapFrameEncode> frame;
            if (wic && SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(out.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
                SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
                frame->SetSize(w, hgt);
                WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
                frame->SetPixelFormat(&fmt);
                frame->WritePixels(hgt, w * 4, UINT(rgba.size()), rgba.data());
                frame->Commit();
                enc->Commit();
                paths.push_back(Narrow(out));
            }
        }
        if (bi) GlobalUnlock(h);
    }
    CloseClipboard();
    if (!paths.empty()) DropFiles(paths);
}

void App::wantMedia(const Message& m, int action) {
    std::string key = m.chat + "/" + m.id;
    if (!m.media.empty() && GetFileAttributesW(Widen(m.media).c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (action) mediaArrived(m, m.media, action);
        return;
    }
    if (action == 0 && (mediaAsked_.count(key) || mediaErrors_.count(key))) return;
    if (action != 0) mediaErrors_.erase(key);
    mediaAsked_.insert(key);
    int req = nextReq_++;
    mediaReqs_[req] = {m.chat, m.id, action};
    engine_->Send({{"t", "media"}, {"req", req}, {"chat", m.chat}, {"id", m.id}});
}

void App::mediaArrived(const Message& m, const std::string& path, int action) {
    switch (action) {
    case 1: openPath(path); break;
    case 2: {
        std::string error;
        if (!voice_.Play(m.chat + "/" + m.id, path, error)) {
            note_ = "Couldn't play it: " + error;
            noteUntil_ = ImGui::GetTime() + 5;
        }
        break;
    }
    case 3: viewer_ = path; break;
    case 4: {
        std::wstring name = Widen(m.fileName.empty() ? fs::u8path(path).filename().u8string() : m.fileName);
        wchar_t buf[MAX_PATH];
        wcsncpy_s(buf, name.c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT;
        if (GetSaveFileNameW(&ofn)) CopyFileW(Widen(path).c_str(), buf, FALSE);
        break;
    }
    }
}

// ---------------------------------------------------------------- images

ImTextureID App::image(const std::string& key) {
    const Texture* t = images_->Get(key);
    return t && t->srv ? (ImTextureID)(intptr_t)t->srv : ImTextureID(0);
}

// fileImage reads a downloaded image off disk (on a thread) into the cache.
ImTextureID App::fileImage(const std::string& path, bool full) {
    std::string key = (full ? "full:" : "file:") + path;
    {
        std::lock_guard<std::mutex> lock(filesMu_);
        if (!filesReady_.count(key)) {
            if (!filesLoading_.count(key)) {
                filesLoading_.insert(key);
                std::thread([this, key, path] {
                    std::ifstream f(fs::u8path(path), std::ios::binary);
                    std::string bytes((std::istreambuf_iterator<char>(f)), {});
                    images_->Provide(key, std::move(bytes));
                    {
                        std::lock_guard<std::mutex> lock(filesMu_);
                        filesReady_.insert(key);
                    }
                    wake_();
                }).detach();
            }
            return ImTextureID(0);
        }
    }
    return image(key);
}

// ---------------------------------------------------------------- drawing

bool App::iconButton(const char* glyph, const char* tip, bool on, float size, bool danger) {
    const Palette& p = Colours();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, danger ? Hex(0xC42B1C) : p.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, danger ? Hex(0xC42B1C) : p.border);
    ImGui::PushStyleColor(ImGuiCol_Text, on ? p.accent : p.muted);
    ImGui::PushFont(fonts_.regular, kSmall + 0.5f);
    bool pressed = ImGui::Button(glyph, ImVec2(size * scale_, size * scale_ * 0.87f));
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    hot_.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    if (tip && *tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    return pressed;
}

void App::Frame() {
    hot_.clear();
    const Palette& p = Colours();
    ImGuiIO& io = ImGui::GetIO();
    const float k = scale_;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("who", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    drawTitle(io.DisplaySize.x);
    float top = barMax_.y, h = io.DisplaySize.y - top;
    if (state_ == "qr" || state_ == "qrtimeout" || state_ == "loggedout" || state_ == "stopped" ||
        (state_ != "connected" && chats_.empty())) {
        drawSignIn(0, top, io.DisplaySize.x, h);
    } else {
        float side = std::clamp(io.DisplaySize.x * 0.3f, 220 * k, 340 * k);
        drawChats(0, top, side, h);
        drawConversation(side, top, io.DisplaySize.x - side, h);
    }
    if (!viewer_.empty()) drawViewer();
    if (!note_.empty() && ImGui::GetTime() < noteUntil_) {
        ImGui::PushFont(fonts_.regular, kSmall);
        ImVec2 ts = ImGui::CalcTextSize(note_.c_str());
        ImVec2 a(io.DisplaySize.x / 2 - ts.x / 2 - 10 * k, io.DisplaySize.y - ts.y - 80 * k);
        ImVec2 b(a.x + ts.x + 20 * k, a.y + ts.y + 10 * k);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(a, b, U32(p.sidebar));
        dl->AddRect(a, b, U32(p.border));
        dl->AddText(ImVec2(a.x + 10 * k, a.y + 5 * k), U32(p.text), note_.c_str());
        ImGui::PopFont();
    }
    ImGui::End();
}

void App::drawTitle(float width) {
    const Palette& p = Colours();
    const float k = scale_;
    const float h = 36 * k;
    barMax_ = ImVec2(width, h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(width, h), U32(p.rail));
    ImGui::SetCursorPos(ImVec2(12 * k, 11 * k));
    ImGui::PushFont(fonts_.bold, 11);
    ImGui::TextColored(p.good, "WHO");
    ImGui::PopFont();
    ImGui::SameLine(0, 10 * k);
    ImGui::PushFont(fonts_.regular, kSmall - 0.5f);
    ImGui::SetCursorPosY(10 * k);
    const char* status = state_ == "connected"   ? ""
                       : state_ == "connecting" ? "connecting\xE2\x80\xA6"
                       : state_ == "offline"    ? "offline, retrying\xE2\x80\xA6"
                       : state_ == "starting"   ? "starting\xE2\x80\xA6"
                                                : "";
    ImGui::TextColored(state_ == "offline" ? p.idle : p.muted, "%s", status);
    ImGui::PopFont();

    // Search: a magnifying glass that opens a box beside it (Ctrl+F too).
    float sx = width - 5 * 28 * k - 4 * k;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
        searchOpen_ = true;
        focusSearch_ = true;
    }
    if (searchOpen_ || filter_[0]) {
        float bw = std::min(240 * k, width * 0.3f);
        ImGui::SetCursorPos(ImVec2(sx - bw - 4 * k, 6 * k));
        ImGui::SetNextItemWidth(bw);
        if (focusSearch_) {
            ImGui::SetKeyboardFocusHere();
            focusSearch_ = false;
            searchOpened_ = ImGui::GetFrameCount();
        }
        ImGui::PushFont(fonts_.regular, kSmall + 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * k, 4 * k));
        ImGui::InputTextWithHint("##filter", "Search chats", filter_, sizeof(filter_));
        ImGui::PopStyleVar();
        ImGui::PopFont();
        hot_.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
        bool active = ImGui::IsItemActive();
        if (active && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            filter_[0] = 0;
            searchOpen_ = false;
        } else if (!active && !filter_[0] && !ImGui::IsItemHovered() && ImGui::GetFrameCount() > searchOpened_ + 3) {
            searchOpen_ = false;  // clicked away with nothing in it
        }
    }
    ImGui::SetCursorPos(ImVec2(sx, 5 * k));
    if (iconButton(icon2::Search, "Search chats (Ctrl+F)", searchOpen_ || filter_[0], 28)) {
        if (searchOpen_ && !filter_[0]) {
            searchOpen_ = false;
        } else {
            searchOpen_ = true;
            focusSearch_ = true;
        }
    }
    ImGui::SameLine(0, 0);
    drawBell();
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Settings, "Settings", false, 28)) ImGui::OpenPopup("##settings");
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Minimise, "Minimise", false, 28)) ::ShowWindow(hwnd_, SW_MINIMIZE);
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Close, "Close to the tray (right-click the tray icon to quit)", false, 28, true))
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    if (ImGui::BeginPopup("##settings")) {
        ImGui::PushFont(fonts_.regular, kSmall);
        ImGui::Dummy(ImVec2(220 * k, 1));
        if (!myName_.empty() || !me_.empty())
            ImGui::TextColored(p.muted, "  %s  +%s", myName_.c_str(), me_.c_str());
        ImGui::Separator();
        if (ImGui::Selectable("  Show archived chats", showArchived_)) showArchived_ = !showArchived_;
        if (ImGui::Selectable("  Open the data folder")) openPath(dir_);
        if (state_ == "connected" && ImGui::Selectable("  Log out of WhatsApp")) engine_->Send({{"t", "logout"}});
        ImGui::Separator();
        if (ImGui::Selectable("  Quit Who")) PostMessageW(hwnd_, WM_APP + 5, 0, 0);
        ImGui::PopFont();
        ImGui::EndPopup();
    }
}

void App::drawBell() {
    const Palette& p = Colours();
    const float k = scale_;
    ImVec2 at = ImGui::GetCursorScreenPos();
    if (iconButton(icon::Bell, unseen_ ? "New messages" : "No new messages", unseen_ > 0, 28)) {
        bellOpen_ = !bellOpen_;
        if (bellOpen_) ImGui::OpenPopup("##bell");
    }
    if (unseen_ > 0) {
        // A glowing count, like disky's bell.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        std::string n = unseen_ > 99 ? "99+" : std::to_string(unseen_);
        ImGui::PushFont(fonts_.bold, 9.5f);
        ImVec2 ts = ImGui::CalcTextSize(n.c_str());
        ImVec2 c(at.x + 21 * k, at.y + 6 * k);
        float r = std::max(ts.x / 2 + 3 * k, 7 * k);
        float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 3);
        dl->AddCircleFilled(c, r + 3 * k * pulse, U32(p.good, 0.25f));
        dl->AddRectFilled(ImVec2(c.x - r, c.y - 7 * k), ImVec2(c.x + r, c.y + 7 * k), U32(p.good), 7 * k);
        dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), U32(Hex(0x0B0C0F)), n.c_str());
        ImGui::PopFont();
    }
    ImGui::SetNextWindowSize(ImVec2(340 * k, 0));
    if (ImGui::BeginPopup("##bell")) {
        unseen_ = 0;
        ImGui::PushFont(fonts_.semibold, 11);
        ImGui::TextColored(p.muted, "  NEW MESSAGES");
        ImGui::PopFont();
        if (notices_.empty()) {
            ImGui::PushFont(fonts_.regular, kSmall);
            ImGui::TextColored(p.muted, "  Nothing new.");
            ImGui::PopFont();
        }
        ImGui::PushFont(fonts_.regular, kSmall);
        int i = 0;
        std::string openIt;
        for (const auto& n : notices_) {
            ImGui::PushID(i++);
            ImVec2 a = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            if (ImGui::InvisibleButton("##n", ImVec2(w, 38 * k))) openIt = n.chat;
            bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (hov) dl->AddRectFilled(a, ImVec2(a.x + w, a.y + 38 * k), U32(p.hover));
            std::string title = n.group ? n.chatName + "  \xC2\xB7  " + n.sender : n.chatName;
            Label(dl, fonts_.semibold, kSmall, ImVec2(a.x + 8 * k, a.y + 3 * k), U32(p.text), title, w - 60 * k,
                  [this](const std::string& u) { return image(u); });
            std::string t = listTime(n.ts);
            ImVec2 tsz = ImGui::CalcTextSize(t.c_str());
            dl->AddText(ImVec2(a.x + w - tsz.x - 8 * k, a.y + 3 * k), U32(p.muted), t.c_str());
            Label(dl, fonts_.regular, kSmall, ImVec2(a.x + 8 * k, a.y + 20 * k), U32(p.muted), n.text, w - 16 * k,
                  [this](const std::string& u) { return image(u); });
            ImGui::PopID();
        }
        if (!notices_.empty() && ImGui::Selectable("  Clear")) notices_.clear();
        ImGui::PopFont();
        ImGui::EndPopup();
        if (!openIt.empty()) {
            openChat(openIt);
            ImGui::CloseCurrentPopup();
        }
    } else {
        bellOpen_ = false;
    }
}

void App::drawSignIn(float x, float y, float w, float h) {
    const Palette& p = Colours();
    const float k = scale_;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::BeginChild("##signin", ImVec2(w, h));
    float cw = std::min(w - 40 * k, 560 * k);
    ImGui::SetCursorPos(ImVec2((w - cw) / 2, 40 * k));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw);
    ImGui::PushFont(fonts_.semibold, kH2);
    if (state_ == "qr" || state_ == "qrtimeout" || state_ == "loggedout") ImGui::TextUnformatted("Link Who to your WhatsApp");
    else if (state_ == "stopped") ImGui::TextUnformatted("The WhatsApp engine stopped");
    else ImGui::TextUnformatted("Connecting to WhatsApp\xE2\x80\xA6");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 6 * k));
    ImGui::PushFont(fonts_.regular, kBody);
    if (state_ == "qr") {
        ImGui::TextColored(p.muted, "On your phone, open WhatsApp, go to Settings, then Linked devices, tap Link a device "
                                    "and point the camera at this code. Who signs in as a linked device, like WhatsApp Web; "
                                    "your phone stays the main one.");
        ImGui::Dummy(ImVec2(0, 12 * k));
        try {
            auto code = qrcodegen::QrCode::encodeText(qr_.c_str(), qrcodegen::QrCode::Ecc::LOW);
            int n = code.getSize();
            float cell = std::floor(std::min(cw, 320 * k) / (n + 8));
            float side = cell * (n + 8);
            ImVec2 a = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(a, ImVec2(a.x + side, a.y + side), IM_COL32(255, 255, 255, 255));
            for (int yy = 0; yy < n; yy++)
                for (int xx = 0; xx < n; xx++)
                    if (code.getModule(xx, yy))
                        dl->AddRectFilled(ImVec2(a.x + (xx + 4) * cell, a.y + (yy + 4) * cell),
                                          ImVec2(a.x + (xx + 5) * cell, a.y + (yy + 5) * cell), IM_COL32(0, 0, 0, 255));
            ImGui::Dummy(ImVec2(side, side));
            double left = qrUntil_ - ImGui::GetTime();
            if (left > 0) ImGui::TextColored(p.muted, "A new code comes in %d seconds.", int(left));
        } catch (...) {
            ImGui::TextColored(p.bad, "Couldn't draw the code.");
        }
    } else if (state_ == "qrtimeout" || state_ == "loggedout") {
        ImGui::TextColored(p.muted, state_ == "loggedout" ? "Who was logged out (from the phone, or here). Restart Who to link it again."
                                                          : "The codes ran out. Restart Who to get new ones.");
    } else if (state_ == "stopped") {
        ImGui::TextColored(p.bad, "%s", stateError_.c_str());
        ImGui::TextColored(p.muted, "Its log is engine.log in the data folder.");
        if (ImGui::Button("Open the data folder")) openPath(dir_);
    } else {
        if (!stateError_.empty()) ImGui::TextColored(p.idle, "%s", stateError_.c_str());
        ImGui::TextColored(p.muted, "Your chats load once it's connected; the first time, the phone sends recent history, "
                                    "which can take a minute.");
    }
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::EndChild();
}

void App::drawChats(float x, float y, float w, float h) {
    const Palette& p = Colours();
    const float k = scale_;
    ImDrawList* wdl = ImGui::GetWindowDrawList();
    // Darker than the conversation, like disky's rail, with a line between.
    wdl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), U32(p.rail));
    wdl->AddLine(ImVec2(x + w - 1, y), ImVec2(x + w - 1, y + h), U32(p.border));
    float listTop = y + 4 * k;
    ImGui::SetCursorPos(ImVec2(x, listTop));
    ImGui::BeginChild("##chats", ImVec2(w, y + h - listTop));
    std::vector<const Chat*> rows;
    std::string f = lower(filter_);
    int archived = 0, archivedUnread = 0;
    for (const auto& c : chats_) {
        if (c.lastTs == 0 && c.unread == 0 && !c.group) continue;  // contacts never talked to
        if (c.archived && f.empty()) {
            archived++;
            archivedUnread += c.unread;
            if (!showArchived_) continue;
        }
        if (!f.empty() && lower(c.name).find(f) == std::string::npos) continue;
        rows.push_back(&c);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Chat* a, const Chat* b) {
        if (a->pinned != b->pinned) return a->pinned;
        return a->lastTs > b->lastTs;
    });
    const float rowH = 46 * k;
    float cw = w - ImGui::GetStyle().ScrollbarSize * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto imageFor = [this](const std::string& u) { return image(u); };
    if (archived && f.empty()) {
        ImVec2 a = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##archived", ImVec2(cw, 30 * k))) showArchived_ = !showArchived_;
        if (ImGui::IsItemHovered()) dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + 30 * k), U32(p.hover));
        ImGui::PushFont(fonts_.regular, kSmall + 0.5f);
        dl->AddText(ImVec2(a.x + 14 * k, a.y + 8 * k), U32(p.muted), icon2::Archive);
        std::string t = std::string(showArchived_ ? "Hide archived" : "Archived") + "  (" + std::to_string(archived) + ")";
        dl->AddText(ImVec2(a.x + 38 * k, a.y + 7 * k), U32(p.muted), t.c_str());
        if (archivedUnread && !showArchived_) {
            std::string n = std::to_string(archivedUnread);
            dl->AddText(ImVec2(a.x + cw - ImGui::CalcTextSize(n.c_str()).x - 14 * k, a.y + 7 * k), U32(p.good), n.c_str());
        }
        ImGui::PopFont();
    }
    if (rows.empty() && !f.empty()) {
        ImGui::SetCursorPosX(14 * k);
        ImGui::TextColored(p.muted, "No chats match.");
    }
    if (rows.empty() && f.empty() && !synced_) {
        ImGui::SetCursorPosX(14 * k);
        ImGui::PushFont(fonts_.regular, kSmall);
        ImGui::TextColored(p.muted, "Waiting for your chats from the phone\xE2\x80\xA6");
        ImGui::PopFont();
    }
    ImGuiListClipper clip;
    clip.Begin(int(rows.size()), rowH);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const Chat& c = *rows[i];
            ImGui::PushID(c.jid.c_str());
            ImVec2 a = ImGui::GetCursorScreenPos();
            bool sel = c.jid == open_;
            if (ImGui::InvisibleButton("##row", ImVec2(cw, rowH))) openChat(c.jid);
            bool hov = ImGui::IsItemHovered();
            if (sel || hov) dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + rowH), U32(sel ? p.selected : p.hover));
            if (sel) dl->AddRectFilled(a, ImVec2(a.x + 2 * k, a.y + rowH), U32(p.good));
            float tx = a.x + 14 * k, right = a.x + cw - 10 * k;
            // Time on the right of the name; unread count under it.
            ImGui::PushFont(fonts_.regular, kSmall - 0.5f);
            std::string t = listTime(c.lastTs);
            ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            dl->AddText(ImVec2(right - ts.x, a.y + 7 * k), U32(c.unread && !c.Muted() ? p.good : p.muted), t.c_str());
            ImGui::PopFont();
            float nameRight = right - ts.x - 8 * k;
            Label(dl, fonts_.semibold, kBody - 0.5f, ImVec2(tx, a.y + 5 * k), U32(p.text), c.name, nameRight - tx, imageFor);
            float pr = right;
            if (c.unread > 0) {
                std::string n = c.unread > 999 ? "999+" : std::to_string(c.unread);
                ImGui::PushFont(fonts_.bold, 10.5f);
                ImVec2 ns = ImGui::CalcTextSize(n.c_str());
                float bw = std::max(ns.x + 10 * k, 18 * k);
                ImVec2 b0(right - bw, a.y + 25 * k), b1(right, a.y + 25 * k + 16 * k);
                dl->AddRectFilled(b0, b1, U32(c.Muted() ? p.muted : p.good), 8 * k);
                dl->AddText(ImVec2(b0.x + (bw - ns.x) / 2, b0.y + (16 * k - ns.y) / 2), U32(Hex(0x0B0C0F)), n.c_str());
                ImGui::PopFont();
                pr = b0.x - 6 * k;
            }
            ImGui::PushFont(fonts_.regular, kSmall);
            if (c.Muted()) {
                ImVec2 ms = ImGui::CalcTextSize(icon2::MuteBell);
                dl->AddText(ImVec2(pr - ms.x, a.y + 26 * k), U32(p.muted), icon2::MuteBell);
                pr -= ms.x + 6 * k;
            }
            if (c.pinned) {
                ImVec2 ms = ImGui::CalcTextSize(icon2::Pin);
                dl->AddText(ImVec2(pr - ms.x, a.y + 26 * k), U32(p.muted), icon2::Pin);
                pr -= ms.x + 6 * k;
            }
            ImGui::PopFont();
            std::string preview;
            auto ty = typing_.find(c.jid);
            ImU32 pc = U32(p.muted);
            if (ty != typing_.end()) {
                preview = c.group ? ty->second.first + "\xE2\x80\xA6" : (ty->second.first.find("recording") != std::string::npos ? "recording audio\xE2\x80\xA6" : "typing\xE2\x80\xA6");
                pc = U32(p.good);
            } else {
                preview = c.lastFromMe ? "You: " + c.lastText : c.lastSender.empty() ? c.lastText : c.lastSender + ": " + c.lastText;
            }
            Label(dl, fonts_.regular, kSmall, ImVec2(tx, a.y + 25 * k), pc, preview, pr - tx, imageFor);
            ImGui::PopID();
        }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void App::drawConversation(float x, float y, float w, float h) {
    const Palette& p = Colours();
    const float k = scale_;
    const Chat* chat = nullptr;
    for (const auto& c : chats_)
        if (c.jid == open_) chat = &c;
    if (!chat) {
        ImGui::SetCursorPos(ImVec2(x + w / 2 - 80 * k, y + h / 2));
        ImGui::TextColored(p.muted, "Pick a chat on the left.");
        return;
    }
    ImDrawList* wdl = ImGui::GetWindowDrawList();
    auto imageFor = [this](const std::string& u) { return image(u); };
    // Header: name, and who's typing or that it's a group.
    const float headH = 44 * k;
    wdl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + headH), U32(p.bg));
    wdl->AddLine(ImVec2(x, y + headH), ImVec2(x + w, y + headH), U32(p.border));
    Label(wdl, fonts_.semibold, kBody + 1, ImVec2(x + 16 * k, y + 6 * k), U32(p.heading), chat->name, w - 32 * k, imageFor);
    {
        std::string sub;
        ImU32 sc = U32(p.muted);
        auto ty = typing_.find(chat->jid);
        if (ty != typing_.end()) {
            sub = ty->second.first + "\xE2\x80\xA6";
            sc = U32(p.good);
        } else if (chat->group) {
            sub = "Group";
        } else if (chat->jid.find("@s.whatsapp.net") != std::string::npos) {
            sub = "+" + chat->jid.substr(0, chat->jid.find('@'));
        }
        if (chat->Muted()) sub += sub.empty() ? "Muted" : "  \xC2\xB7  Muted";
        Label(wdl, fonts_.regular, kSmall, ImVec2(x + 16 * k, y + 25 * k), sc, sub, w - 32 * k, imageFor);
    }

    // Composer height follows its text, up to six lines.
    int lines = 1;
    for (const char* c = compose_; *c; c++) lines += *c == '\n';
    lines = std::clamp(lines, 1, 6);
    float lineH = ImGui::GetFontSize() > 0 ? (kBody * k * 1.35f) : 20 * k;
    float extra = (!replyTo_.empty() ? 36 * k : 0) + (!pendingFiles_.empty() ? 32 * k : 0);
    float compH = lines * lineH + 30 * k + extra;
    float listY = y + headH + 1, listH = h - headH - 1 - compH;

    Conversation& conv = convos_[open_];
    ImGui::SetCursorPos(ImVec2(x, listY));
    ImGui::BeginChild("##msgs", ImVec2(w, listH), ImGuiChildFlags_None);
    // Older messages when scrolled to the top.
    if (ImGui::GetScrollY() <= 0 && conv.loaded && !conv.loading && ImGui::GetScrollMaxY() > 0) {
        if (conv.more) {
            conv.keepFromBottom = ImGui::GetScrollMaxY();
            loadMessages(open_, true);
        }
    }
    float inner = w - ImGui::GetStyle().ScrollbarSize - 24 * k;
    ImGui::SetCursorPos(ImVec2(12 * k, 10 * k));
    ImGui::BeginGroup();
    if (!conv.loaded) {
        ImGui::TextColored(p.muted, "Loading\xE2\x80\xA6");
    } else {
        if (!conv.more) {
            ImGui::PushFont(fonts_.regular, kSmall);
            float bw = 260 * k;
            ImGui::SetCursorPosX(12 * k + (inner - bw) / 2);
            if (conv.askedPhone) {
                ImGui::TextColored(p.muted, "Asked the phone for older messages\xE2\x80\xA6");
            } else if (ImGui::Button("Ask the phone for older messages", ImVec2(bw, 0))) {
                engine_->Send({{"t", "older"}, {"chat", open_}});
                conv.askedPhone = true;
                conv.more = true;
            }
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, 6 * k));
        }
        int lastDay = -1;
        std::string lastSender;
        int64_t lastTs = 0;
        bool alt = true;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));
        for (auto& m : conv.messages) {
            int day = dayNumber(m.ts);
            if (day != lastDay) {
                // A day's heading, centred.
                std::string d = dayLabel(m.ts);
                ImGui::PushFont(fonts_.semibold, kSmall - 1);
                ImVec2 ds = ImGui::CalcTextSize(d.c_str());
                ImVec2 a = ImGui::GetCursorScreenPos();
                float cx = a.x + inner / 2;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx - ds.x / 2 - 10 * k, a.y + 4 * k),
                                                          ImVec2(cx + ds.x / 2 + 10 * k, a.y + ds.y + 10 * k), U32(p.code), 4 * k);
                ImGui::GetWindowDrawList()->AddText(ImVec2(cx - ds.x / 2, a.y + 7 * k), U32(p.muted), d.c_str());
                ImGui::Dummy(ImVec2(inner, ds.y + 16 * k));
                ImGui::PopFont();
                lastDay = day;
                lastSender.clear();
            }
            bool first = m.sender != lastSender || m.ts - lastTs > 10 * 60 * 1000;
            if (first) alt = !alt;
            ImGui::PushID(m.id.c_str());
            drawMessage(m, conv, inner, first, *chat, alt);
            ImGui::PopID();
            lastSender = m.sender;
            lastTs = m.ts;
        }
        ImGui::PopStyleVar();
        if (conv.messages.empty()) ImGui::TextColored(p.muted, "No messages here yet.");
    }
    ImGui::Dummy(ImVec2(0, 8 * k));
    ImGui::EndGroup();
    // Keep to the bottom for new messages; keep place after loading older.
    if (conv.toBottom) {
        ImGui::SetScrollHereY(1.0f);
        if (conv.loaded) conv.toBottom = false;
    } else if (conv.keepFromBottom >= 0 && !conv.loading) {
        ImGui::SetScrollY(ImGui::GetScrollMaxY() - conv.keepFromBottom);
        conv.keepFromBottom = -1;
    }
    bool nearBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40 * k;
    ImGui::EndChild();
    // New messages follow the bottom only when the view is there already.
    if (conv.messages.size() != conv.drawnCount) {
        if (nearBottom && conv.keepFromBottom < 0) conv.toBottom = true;
        conv.drawnCount = conv.messages.size();
    }
    drawComposer(x, listY + listH, w, compH);
}

// drawMessage draws one message as disky does: names in a column on the
// left (at the start of each run), the message beside them, every other
// run on a band, the row lit while hovered.
void App::drawMessage(Message& m, const Conversation&, float width, bool firstOfRun, const Chat& chat, bool alt) {
    const Palette& p = Colours();
    const float k = scale_;
    constexpr float kNames = 130;
    auto imageFor = [this](const std::string& u) { return image(u); };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);

    ImVec2 o = ImGui::GetCursorScreenPos();
    const float left = (16 + kNames + 12) * k;
    const float timeW = 74 * k;
    // Reactions sit on the right, by the time: room for their chips.
    float chipsW = 0;
    for (const auto& [emoji, who] : m.reactions) chipsW += 24 * k + (who.size() > 1 ? 12 * k : 0) + 4 * k;
    const float contentW = std::max(120 * k, width - left - timeW - chipsW);
    float y = o.y + (firstOfRun ? 6 : 1) * k;
    std::string name = m.fromMe ? "You" : m.senderName;
    if (!name.empty() && name[0] == '~') name.erase(0, 1);
    ImU32 colour = m.fromMe ? U32(p.good) : nameColour(m.sender);

    // The message replied to, in a line above.
    if (!m.replyId.empty()) {
        ImGui::PushFont(fonts_.regular, kSmall);
        float rx = o.x + left, ry = y;
        dl->AddText(ImVec2(rx, ry), U32(p.muted), "\xE2\x86\xB1");  // the hooked arrow
        rx += 16 * k;
        std::string rn = m.replyName;
        if (!rn.empty() && rn[0] == '~') rn.erase(0, 1);
        ImU32 rc = rn == "You" ? U32(p.good) : nameColour(rn);
        rx += Label(dl, fonts_.semibold, kSmall, ImVec2(rx, ry), rc, "@" + rn, 0, imageFor) + 6 * k;
        Label(dl, fonts_.regular, kSmall, ImVec2(rx, ry), U32(p.muted), m.replyText, o.x + left + contentW - rx, imageFor);
        ImGui::PopFont();
        y += 19 * k;
    }

    // The name, right-aligned in its column, at the start of a run.
    float textY = y + (kBody * 1.3f * k - kBody * k) / 2;
    if (firstOfRun) {
        float nw = Label(nullptr, fonts_.semibold, kBody, ImVec2(0, 0), 0, name, kNames * k, imageFor);
        Label(dl, fonts_.semibold, kBody, ImVec2(o.x + 16 * k + kNames * k - nw, textY), colour, name, kNames * k, imageFor);
    }

    // The content: attachment, then text.
    ImGui::SetCursorScreenPos(ImVec2(o.x + left, y));
    ImGui::BeginGroup();
    bool hasText = !m.text.empty() && m.kind != "deleted";
    bool attached = m.kind != "text";
    if (attached) {
        drawMedia(m, std::min(contentW, 420 * k));
        if (hasText) ImGui::Dummy(ImVec2(0, 2 * k));
    }
    if (hasText) {
        if (m.layoutWidth != contentW) {
            bool jumbo = false;
            m.layout = LayOut(ParseWhatsApp(m.text, &jumbo), contentW, fonts_, Resolver{}, scale_, jumbo, kBody);
            m.layoutWidth = contentW;
        }
        ImVec2 a = ImGui::GetCursorScreenPos();
        Hit hit = Draw(m.layout, a, imageFor, {});
        ImGui::Dummy(ImVec2(std::max(m.layout.width, 1.0f), m.layout.height));
        if (hit.piece && !hit.piece->link.empty()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) openPath(hit.piece->link);
        }
        if (m.edited) {
            ImGui::PushFont(fonts_.regular, kSmall - 1);
            ImGui::TextColored(p.muted, "(edited)");
            ImGui::PopFont();
        }
    }
    ImGui::EndGroup();
    float bottom = std::max(ImGui::GetItemRectMax().y, textY + kBody * k) + (firstOfRun ? 3 : 2) * k;

    // The row: band, hover, and right-click for its menu.
    ImVec2 r0(o.x - 12 * k, o.y), r1(o.x + width + 12 * k, bottom);
    ImVec2 mouse = ImGui::GetMousePos();
    bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && mouse.y >= r0.y && mouse.y < r1.y &&
               !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    // Time on the right: always at a run's start, else while hovered; own
    // messages get ticks (one sent, two delivered, two blue read).
    if (firstOfRun || hov) {
        ImGui::PushFont(fonts_.regular, kSmall - 1);
        std::string t = clock(m.ts);
        ImVec2 ts = ImGui::CalcTextSize(t.c_str());
        float tx = o.x + width - ts.x - (m.fromMe ? 18 * k : 0);
        dl->AddText(ImVec2(tx, textY + 1 * k), U32(p.muted), t.c_str());
        if (m.fromMe) {
            ImU32 tc = m.status == "read" || m.status == "played" ? U32(p.accent) : U32(p.muted);
            float cx = tx + ts.x + 4 * k;
            dl->AddText(ImVec2(cx, textY + 1 * k), tc, icon2::Check);
            if (m.status == "delivered" || m.status == "read" || m.status == "played")
                dl->AddText(ImVec2(cx + 5 * k, textY + 1 * k), tc, icon2::Check);
        }
        ImGui::PopFont();
    }
    // Reactions as small chips on the right, left of the time.
    if (!m.reactions.empty()) {
        float rx = o.x + width - timeW - chipsW + 8 * k, ry = textY - 2 * k;
        ImVec2 start(rx, ry);
        std::string tip;
        for (const auto& [emoji, who] : m.reactions) {
            int n = int(who.size());
            for (const auto& w : who) tip += (tip.empty() ? "" : "\n") + emoji + "  " + w;
            float cw = 24 * k + (n > 1 ? 12 * k : 0);
            bool mine = std::find(who.begin(), who.end(), "You") != who.end();
            dl->AddRectFilled(ImVec2(rx, ry), ImVec2(rx + cw, ry + 20 * k), U32(mine ? p.selected : p.sidebar), 4 * k);
            dl->AddRect(ImVec2(rx, ry), ImVec2(rx + cw, ry + 20 * k), U32(mine ? p.accent : p.border), 4 * k);
            std::string file;
            EmojiAt(emoji, 0, &file);
            if (ImTextureID t = image("https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + file + ".png"))
                dl->AddImage(t, ImVec2(rx + 5 * k, ry + 3 * k), ImVec2(rx + 19 * k, ry + 17 * k));
            if (n > 1) {
                ImGui::PushFont(fonts_.regular, kSmall - 1.5f);
                dl->AddText(ImVec2(rx + 21 * k, ry + 3 * k), U32(p.muted), std::to_string(n).c_str());
                ImGui::PopFont();
            }
            rx += cw + 4 * k;
        }
        if (ImGui::IsMouseHoveringRect(start, ImVec2(rx, ry + 20 * k))) ImGui::SetTooltip("%s", tip.c_str());
    }
    split.SetCurrentChannel(dl, 0);
    if (alt) dl->AddRectFilled(r0, r1, U32(p.band));
    if (hov) dl->AddRectFilled(r0, r1, U32(p.hover));
    split.Merge(dl);

    ImGui::SetCursorScreenPos(r0);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##row", ImVec2(r1.x - r0.x, std::max(1.0f, r1.y - r0.y)), ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##msg");
    if (ImGui::BeginPopup("##msg")) {
        // Quick reactions first, then the rest.
        static const char* quick[] = {"\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x82", "\xF0\x9F\x98\xAE",
                                      "\xF0\x9F\x98\xA2", "\xF0\x9F\x99\x8F"};
        for (int i = 0; i < 6; i++) {
            if (i) ImGui::SameLine(0, 2 * k);
            ImGui::PushID(i);
            ImVec2 a = ImGui::GetCursorScreenPos();
            float s = 28 * k;
            if (ImGui::InvisibleButton("##r", ImVec2(s, s))) {
                engine_->Send({{"t", "react"}, {"chat", m.chat}, {"id", m.id}, {"emoji", quick[i]}});
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + s, a.y + s), U32(p.hover), 4 * k);
            std::string file;
            EmojiAt(quick[i], 0, &file);
            if (ImTextureID t = image("https://cdn.jsdelivr.net/gh/jdecked/twemoji@15.1.0/assets/72x72/" + file + ".png"))
                ImGui::GetWindowDrawList()->AddImage(t, ImVec2(a.x + 4 * k, a.y + 4 * k), ImVec2(a.x + s - 4 * k, a.y + s - 4 * k));
            ImGui::PopID();
        }
        ImGui::PushFont(fonts_.regular, kSmall + 0.5f);
        ImGui::Separator();
        if (ImGui::Selectable("  Reply")) {
            replyTo_ = m.id;
            focusComposer_ = true;
        }
        if (hasText && ImGui::Selectable("  Copy text")) ImGui::SetClipboardText(m.text.c_str());
        for (const auto& [emoji, who] : m.reactions)
            if (std::find(who.begin(), who.end(), "You") != who.end() &&
                ImGui::Selectable(("  Remove your reaction " + emoji).c_str()))
                engine_->Send({{"t", "react"}, {"chat", m.chat}, {"id", m.id}, {"emoji", ""}});
        if ((m.kind == "image" || m.kind == "sticker" || m.kind == "video" || m.kind == "gif" || m.kind == "document" ||
             m.kind == "voice" || m.kind == "audio") &&
            ImGui::Selectable("  Save as\xE2\x80\xA6"))
            wantMedia(m, 4);
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    (void)chat;
    ImGui::SetCursorScreenPos(ImVec2(o.x, bottom));
    ImGui::Dummy(ImVec2(width, 0));
}

void App::drawMedia(Message& m, float maxW) {
    const Palette& p = Colours();
    const float k = scale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string key = m.chat + "/" + m.id;
    auto err = mediaErrors_.find(key);
    if (m.kind == "image" || m.kind == "sticker" || m.kind == "video" || m.kind == "gif") {
        float side = m.kind == "sticker" ? 140 * k : std::min(maxW, 300 * k);
        float iw = float(m.width > 0 ? m.width : 4), ih = float(m.height > 0 ? m.height : 3);
        float s = std::min(side / iw, (m.kind == "sticker" ? 140 * k : 320 * k) / ih);
        float w = std::max(60 * k, iw * s), h = std::max(40 * k, ih * s);
        ImVec2 a = ImGui::GetCursorScreenPos();
        bool still = m.kind == "image" || m.kind == "sticker";
        if (still && m.media.empty()) wantMedia(m, 0);  // pictures come down on their own
        ImTextureID tex = 0;
        if (still && !m.media.empty()) tex = fileImage(m.media);
        if (!tex && !m.thumb.empty()) tex = image("thumb:" + m.chat + "/" + m.id);
        if (tex) dl->AddImage(tex, a, ImVec2(a.x + w, a.y + h));
        else dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), U32(p.rail), 4 * k);
        if (!still) {
            // A play mark: videos open in the system's player.
            ImVec2 c(a.x + w / 2, a.y + h / 2);
            dl->AddCircleFilled(c, 18 * k, IM_COL32(0, 0, 0, 150));
            ImGui::PushFont(fonts_.regular, kBody);
            ImVec2 ts = ImGui::CalcTextSize(icon::Play);
            dl->AddText(ImVec2(c.x - ts.x / 2 + 1 * k, c.y - ts.y / 2 + 1 * k), IM_COL32(255, 255, 255, 230), icon::Play);
            ImGui::PopFont();
            if (m.seconds > 0) {
                std::string d = mmss(m.seconds);
                ImGui::PushFont(fonts_.regular, kSmall - 1);
                dl->AddText(ImVec2(a.x + 6 * k, a.y + h - 18 * k), IM_COL32(255, 255, 255, 230), d.c_str());
                ImGui::PopFont();
            }
        }
        if (err != mediaErrors_.end() && still && m.media.empty()) {
            ImGui::PushFont(fonts_.regular, kSmall - 1);
            dl->AddText(ImVec2(a.x + 6 * k, a.y + 6 * k), U32(p.idle), "not available");
            ImGui::PopFont();
        }
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##media", ImVec2(w, h))) wantMedia(m, still ? 3 : 1);
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (m.kind == "video" || m.kind == "gif") ImGui::SetTooltip("Open in your video player");
        }
        ImGui::Dummy(ImVec2(0, 2 * k));
        return;
    }
    if (m.kind == "voice" || m.kind == "audio") {
        bool isThis = voice_.Key() == key;
        bool playing = isThis && voice_.Playing();
        ImVec2 a = ImGui::GetCursorScreenPos();
        float w = std::min(maxW, 260 * k);
        if (m.kind == "voice") {
            if (iconButton(playing ? icon::Pause : icon::Play, playing ? "Pause" : "Play", true, 30)) {
                if (isThis && (voice_.Playing() || voice_.Paused())) voice_.Pause(playing);
                else wantMedia(m, 2);
            }
        } else if (iconButton(icon::Open, "Open in your music player", true, 30)) {
            wantMedia(m, 1);
        }
        ImGui::SameLine(0, 6 * k);
        // Progress, clickable to seek while it plays.
        float bx = ImGui::GetCursorScreenPos().x, bw = a.x + w - bx - 44 * k, by = a.y + 13 * k;
        double frac = isThis && voice_.Duration() > 0 ? voice_.Position() / voice_.Duration() : 0;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + 3 * k), U32(p.border), 2 * k);
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * float(frac), by + 3 * k), U32(p.good), 2 * k);
        if (isThis) dl->AddCircleFilled(ImVec2(bx + bw * float(frac), by + 1.5f * k), 5 * k, U32(p.good));
        ImGui::InvisibleButton("##seek", ImVec2(bw, 26 * k));
        if (isThis && ImGui::IsItemClicked()) voice_.Seek((ImGui::GetIO().MousePos.x - bx) / bw);
        std::string d = isThis ? mmss(voice_.Position()) : mmss(m.seconds);
        ImGui::PushFont(fonts_.regular, kSmall - 1);
        dl->AddText(ImVec2(bx + bw + 8 * k, a.y + 7 * k), U32(p.muted), d.c_str());
        ImGui::PopFont();
        if (err != mediaErrors_.end()) {
            ImGui::PushFont(fonts_.regular, kSmall - 1);
            ImGui::TextColored(p.idle, "%s", "not available any more");
            ImGui::PopFont();
        }
        return;
    }
    // One-line kinds: an icon and a label; documents open on click.
    const char* glyph = m.kind == "document" ? icon2::Doc : m.kind == "location" ? icon2::Location : icon::Chat;
    std::string label, sub;
    if (m.kind == "document") {
        label = m.fileName.empty() ? "Document" : m.fileName;
        sub = sizeText(m.size);
    } else if (m.kind == "location") {
        label = "Location";
    } else if (m.kind == "contact") {
        label = "Contact card";
    } else if (m.kind == "poll") {
        label = "Poll (vote on the phone)";
    } else if (m.kind == "deleted") {
        label = m.fromMe ? "You deleted this message" : "This message was deleted";
    } else {
        label = "A message Who can't show (see the phone)";
    }
    ImVec2 a = ImGui::GetCursorScreenPos();
    float w = std::min(maxW, std::max(200 * k, Label(nullptr, fonts_.regular, kSmall, ImVec2(0, 0), 0, label, 0, nullptr) + 40 * k));
    bool clickable = m.kind == "document";
    if (clickable) {
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##doc", ImVec2(w, sub.empty() ? 22 * k : 36 * k))) wantMedia(m, 1);
        if (ImGui::IsItemHovered()) {
            dl->AddRectFilled(a, ImVec2(a.x + w, a.y + 36 * k), U32(p.hover), 4 * k);
            ImGui::SetTooltip("Open (right-click the message to save it)");
        }
        ImGui::SetCursorScreenPos(a);
    }
    ImGui::PushFont(fonts_.regular, kBody);
    dl->AddText(ImVec2(a.x + 2 * k, a.y + 3 * k), U32(m.kind == "deleted" ? p.muted : p.accent), glyph);
    ImGui::PopFont();
    ImGui::PushFont(m.kind == "deleted" ? fonts_.italic : fonts_.regular, kSmall + 0.5f);
    Label(dl, m.kind == "deleted" ? fonts_.italic : fonts_.regular, kSmall + 0.5f, ImVec2(a.x + 26 * k, a.y + 2 * k),
          U32(m.kind == "deleted" ? p.muted : p.text), label, w - 30 * k, [this](const std::string& u) { return image(u); });
    if (!sub.empty()) dl->AddText(ImVec2(a.x + 26 * k, a.y + 19 * k), U32(p.muted), sub.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, sub.empty() ? 22 * k : 36 * k));
    if (m.kind == "document" && err != mediaErrors_.end()) {
        ImGui::PushFont(fonts_.regular, kSmall - 1);
        ImGui::TextColored(p.idle, "not available any more");
        ImGui::PopFont();
    }
}

void App::drawComposer(float x, float y, float w, float h) {
    const Palette& p = Colours();
    const float k = scale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), U32(p.bg));
    dl->AddLine(ImVec2(x, y), ImVec2(x + w, y), U32(p.border));
    float cy = y + 8 * k;
    auto imageFor = [this](const std::string& u) { return image(u); };
    if (!replyTo_.empty()) {
        const Message* r = nullptr;
        for (const auto& m : convos_[open_].messages)
            if (m.id == replyTo_) r = &m;
        ImVec2 a(x + 12 * k, cy);
        dl->AddRectFilled(a, ImVec2(x + w - 12 * k, a.y + 30 * k), U32(p.code), 3 * k);
        dl->AddRectFilled(a, ImVec2(a.x + 3 * k, a.y + 30 * k), U32(p.good));
        std::string who = r ? (r->fromMe ? "You" : r->senderName) : "";
        if (!who.empty() && who[0] == '~') who.erase(0, 1);
        std::string what = r ? Preview(*r) : "";
        Label(dl, fonts_.semibold, kSmall - 1, ImVec2(a.x + 9 * k, a.y + 1 * k), U32(p.good), "Replying to " + who, w - 80 * k, imageFor);
        Label(dl, fonts_.regular, kSmall - 1, ImVec2(a.x + 9 * k, a.y + 15 * k), U32(p.muted), what, w - 80 * k, imageFor);
        ImGui::SetCursorScreenPos(ImVec2(x + w - 42 * k, a.y + 2 * k));
        if (iconButton(icon::Dismiss, "Cancel the reply (Esc)", false, 26)) replyTo_.clear();
        cy += 36 * k;
    }
    if (!pendingFiles_.empty()) {
        std::string names;
        for (const auto& f : pendingFiles_) names += (names.empty() ? "" : ", ") + fs::u8path(f).filename().u8string();
        ImGui::SetCursorScreenPos(ImVec2(x + 12 * k, cy));
        ImGui::PushFont(fonts_.regular, kSmall);
        dl->AddText(ImVec2(x + 14 * k, cy + 4 * k), U32(p.accent), icon::Attach);
        Label(dl, fonts_.regular, kSmall, ImVec2(x + 34 * k, cy + 3 * k), U32(p.text),
              names + "  (Enter sends; the text becomes the caption)", w - 110 * k, imageFor);
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(x + w - 42 * k, cy - 1 * k));
        if (iconButton(icon::Dismiss, "Don't send these", false, 26)) pendingFiles_.clear();
        cy += 32 * k;
    }
    ImGui::SetCursorScreenPos(ImVec2(x + 8 * k, cy));
    if (iconButton(icon::Attach, "Attach files (or paste, or drop them here)", false, 30)) pickFiles();
    ImGui::SameLine(0, 4 * k);
    float inputW = w - 8 * k - 34 * k - 12 * k;
    float inputH = y + h - cy - 10 * k;
    if (focusComposer_) {
        ImGui::SetKeyboardFocusHere();
        focusComposer_ = false;
    }
    // Enter sends, Shift+Enter starts a new line: with Shift held the box
    // takes Enter as a new line, otherwise Enter reports and sends.
    bool shift = ImGui::GetIO().KeyShift;
    ImGuiInputTextFlags fl = shift ? 0 : (ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
    ImGui::PushFont(fonts_.regular, kBody);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * k, 7 * k));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, p.input);
    // Without EnterReturnsTrue (Shift held) it returns true on any edit, so
    // only an Enter press without Shift sends.
    bool enter = ImGui::InputTextMultiline("##compose", compose_, sizeof(compose_), ImVec2(inputW, inputH), fl) && !shift &&
                 (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
    bool active = ImGui::IsItemActive();
    if (ImGui::IsItemEdited() && compose_[0] && !open_.empty()) {
        double now = ImGui::GetTime();
        if (!typingSent_ || now - lastTypingSent_ > 4) {
            engine_->Send({{"t", "typing"}, {"chat", open_}, {"on", true}});
            typingSent_ = true;
        }
        lastTypingSent_ = now;
    }
    if (!compose_[0] && !active) {
        ImVec2 a = ImGui::GetItemRectMin();
        dl->AddText(ImVec2(a.x + 10 * k, a.y + 7 * k), U32(p.muted), "Message (Enter sends, Shift+Enter new line)");
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (enter) {
        send();
        focusComposer_ = true;
    }
    if (active && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (!replyTo_.empty()) replyTo_.clear();
        else pendingFiles_.clear();
    }
    // Pasting a picture or files attaches them.
    if (active && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V) &&
        (IsClipboardFormatAvailable(CF_HDROP) || (IsClipboardFormatAvailable(CF_DIB) && !IsClipboardFormatAvailable(CF_UNICODETEXT))))
        pasteFromClipboard();
}

void App::drawViewer() {
    const float k = scale_;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 220));
    ImTextureID tex = fileImage(viewer_, true);
    const Texture* t = tex ? images_->Get("full:" + viewer_) : nullptr;
    if (tex && t) {
        float maxW = io.DisplaySize.x - 60 * k, maxH = io.DisplaySize.y - 100 * k;
        float s = std::min({maxW / t->width, maxH / t->height, 1.0f * scale_ * 2});
        float w = t->width * s, h = t->height * s;
        ImVec2 a((io.DisplaySize.x - w) / 2, (io.DisplaySize.y - h) / 2);
        dl->AddImage(tex, a, ImVec2(a.x + w, a.y + h));
    } else {
        dl->AddText(ImVec2(io.DisplaySize.x / 2 - 30 * k, io.DisplaySize.y / 2), IM_COL32(255, 255, 255, 200), "Loading\xE2\x80\xA6");
    }
    const char* help = "Click or Esc to close  \xC2\xB7  O opens it in your viewer";
    ImVec2 hs = ImGui::CalcTextSize(help);
    dl->AddText(ImVec2((io.DisplaySize.x - hs.x) / 2, io.DisplaySize.y - 40 * k), IM_COL32(255, 255, 255, 170), help);
    if (ImGui::IsKeyPressed(ImGuiKey_O)) openPath(viewer_);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) viewer_.clear();
}

}  // namespace who
