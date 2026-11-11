// Who's window: chats down the left, the open conversation on the right,
// a bell in the title bar for new messages. Drawn with Dear ImGui.
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "engine.h"
#include "images.h"
#include "richtext.h"
#include "theme.h"
#include "voice.h"

namespace who {

struct Chat {
    std::string jid, name, lastText, lastSender;
    bool group = false, pinned = false, archived = false, lastFromMe = false;
    int unread = 0;
    int64_t lastTs = 0, mutedUntil = 0;
    bool Muted() const;
};

struct Message {
    std::string chat, id, sender, senderName, kind, text, fileName, mime, status;
    std::string replyId, replyName, replyText, media, thumb;  // thumb: JPEG bytes
    bool fromMe = false, edited = false;
    int64_t ts = 0, size = 0;
    int seconds = 0, width = 0, height = 0;
    std::vector<std::pair<std::string, std::vector<std::string>>> reactions;
    // Laid out text, for this width.
    Layout layout;
    float layoutWidth = -1;
    bool jumbo = false;
};

std::string Preview(const Message& m);

struct Conversation {
    std::vector<Message> messages;  // oldest first
    bool loading = false, loaded = false, more = true;
    bool askedPhone = false;
    bool toBottom = true;  // scroll to the newest next frame
    float keepFromBottom = -1;  // after loading older, keep the view where it was
    size_t drawnCount = 0;      // messages last drawn, to spot new ones
};

struct Notice {
    std::string chat, chatName, sender, text;
    int64_t ts = 0;
    bool group = false;
};

class App {
public:
    App(HWND hwnd, ID3D11Device* device, std::function<void()> wake);
    ~App();

    void Frame();
    void Pump();
    bool BeforeFrame();
    bool Animating() const;
    bool InCaption(float x, float y) const;
    void SetScale(float scale);

    bool Place(RECT& r) const;
    void SavePlace(const RECT& r);
    void ShowWindow();  // from the tray
    int Unread() const;  // unread messages in chats that aren't muted
    void DropFiles(const std::vector<std::string>& paths);
    bool Visible() const;

private:
    void demo();
    // Engine events.
    void onEvent(const nlohmann::json& e);
    Chat parseChat(const nlohmann::json& j);
    Message parseMessage(const nlohmann::json& j);
    void putMessage(Message m);
    void openChat(const std::string& jid);
    void loadMessages(const std::string& jid, bool older);
    void markRead(const std::string& jid);
    void send();
    void sendFiles(const std::vector<std::string>& paths);
    void pasteFromClipboard();
    void pickFiles();
    void wantMedia(const Message& m, int action);  // 0 just fetch, 1 open, 2 play, 3 view, 4 save
    void mediaArrived(const Message& m, const std::string& path, int action);
    void loadConfig();
    void saveConfig();
    void notify(const Notice& n);
    void dropNotices(const std::string& jid);  // that chat's been seen

    // Drawing.
    void drawTitle(float width);
    void drawChats(float x, float y, float w, float h);
    void drawConversation(float x, float y, float w, float h);
    void drawMessage(Message& m, const Conversation& c, float width, bool firstOfRun, const Chat& chat, bool alt);
    void drawMedia(Message& m, float maxW);
    void drawComposer(float x, float y, float w, float h);
    void drawSignIn(float x, float y, float w, float h);
    void drawViewer();
    void drawBell();
    bool iconButton(const char* glyph, const char* tip, bool on = false, float size = 30, bool danger = false);
    ImTextureID image(const std::string& key);
    ImTextureID fileImage(const std::string& path, bool full = false);

    HWND hwnd_;
    bool demo_ = false;
    std::function<void()> wake_;
    std::string dir_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<ImageCache> images_;
    VoicePlayer voice_;
    Fonts fonts_;
    float scale_ = 1;

    // Engine state.
    std::string state_ = "starting", qr_, me_, myName_, stateError_;
    double qrUntil_ = 0;
    bool synced_ = false;
    std::vector<Chat> chats_;
    std::map<std::string, Conversation> convos_;
    std::string open_;            // open chat
    bool showArchived_ = false;
    char filter_[128] = {};
    bool searchOpen_ = false, focusSearch_ = false;
    int searchOpened_ = 0;  // frame it opened, so it isn't closed before it takes focus
    std::map<std::string, std::pair<std::string, double>> typing_;  // chat -> who, until
    double lastTypingSent_ = 0;
    bool typingSent_ = false;

    // Composer.
    std::map<std::string, std::string> drafts_;
    char compose_[8192] = {};
    std::string replyTo_;
    std::vector<std::string> pendingFiles_;  // dropped or pasted, waiting for Send
    bool focusComposer_ = false;

    // Media.
    int nextReq_ = 1;
    struct MediaReq {
        std::string chat, id;
        int action;
    };
    std::map<int, MediaReq> mediaReqs_;
    std::set<std::string> mediaAsked_;  // chat/id
    std::map<std::string, std::string> mediaErrors_;
    std::mutex filesMu_;
    std::set<std::string> filesReady_, filesLoading_;  // image keys provided from disk
    std::string viewer_;  // a full image being shown
    std::string playing_;  // chat/id of the voice message

    // Bell.
    std::deque<Notice> notices_;
    int unseen_ = 0;
    bool bellOpen_ = false;

    // Window.
    struct Config {
        bool placed = false;
        int x = 0, y = 0, w = 0, h = 0;
        std::string open;
        bool online = false;
        bool hideChats = false;  // the chat list folded away
    } cfg_;
    ImVec2 barMax_;
    std::vector<std::pair<ImVec2, ImVec2>> hot_;
    std::string note_;
    double noteUntil_ = 0;
};

}  // namespace who
