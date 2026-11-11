// Thumbnails for list items: downloaded and decoded on a background thread,
// turned into textures on the UI thread. Animated GIFs are decoded whole
// only when they're to play.
#pragma once

#include <d3d11.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace who {

struct Texture {
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0, height = 0;
};

class ImageCache {
public:
    // wake is called from the background thread when an image is ready.
    ImageCache(ID3D11Device* device, std::function<void()> wake);
    ~ImageCache();

    // Get returns the image's texture, or nullptr while it loads or if it
    // failed. The first call starts loading it.
    const Texture* Get(const std::string& url);

    // Animated gives the frame of the GIF at url that's due at now
    // (seconds), or nullptr while it loads or if it failed. It plays from
    // the start when it wasn't asked for the frame before, so each hover
    // starts it over. The first call starts loading it.
    const Texture* Animated(const std::string& url, double now);
    // Animating says an animation was asked for last frame, so frames
    // should keep coming.
    bool Animating() const { return animating_; }
    // Tick marks a new frame. Call it once at the start of each.
    void Tick();

    // Upload turns decoded images into textures. Call it on the UI thread.
    void Upload();

    // Provide supplies an image's bytes for a key that isn't a URL, such as
    // album art read from Windows. Any thread may call it; call it before
    // the first Get of that key.
    void Provide(const std::string& key, std::string bytes);

private:
    struct Job {
        std::string url;
        bool animated = false;
    };
    struct Decoded {
        std::string url;
        bool animated = false;
        int width = 0, height = 0;
        std::vector<unsigned char> rgba;  // empty if it failed
        // An animation's frames, after the first (in rgba), and how long
        // each shows.
        std::vector<std::vector<unsigned char>> more;
        std::vector<int> delays;
    };
    struct Entry {
        bool done = false;
        Texture tex;
        unsigned lastUsed = 0;
    };

    struct Anim {
        bool done = false;
        std::vector<Texture> frames;
        std::vector<int> delays;  // ms
        size_t bytes = 0;
        double start = 0;
        unsigned lastFrame = 0, lastUsed = 0;
    };

    void run();
    void evict();
    void evictAnims();
    void enqueue(const std::string& url, bool animated);
    Texture upload(int width, int height, const unsigned char* rgba);

    ID3D11Device* device_;
    std::function<void()> wake_;
    std::unordered_map<std::string, Entry> entries_;
    unsigned tick_ = 0;
    std::unordered_map<std::string, Anim> anims_;
    unsigned frame_ = 0;
    bool animating_ = false, animatingNow_ = false;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::unordered_map<std::string, std::string> provided_;  // bytes by key, until decoded
    std::vector<Decoded> decoded_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace who
