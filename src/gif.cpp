#include "gif.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace who {

GifCanvas::GifCanvas(int width, int height)
    : width_(width), height_(height), pixels_(size_t(width) * height * 4, 0) {}

void GifCanvas::clip(const GifFrame& f, int& x0, int& y0, int& x1, int& y1) const {
    x0 = std::clamp(f.left, 0, width_);
    y0 = std::clamp(f.top, 0, height_);
    x1 = std::clamp(f.left + f.width, 0, width_);
    y1 = std::clamp(f.top + f.height, 0, height_);
}

void GifCanvas::Paint(const GifFrame& f) {
    int x0, y0, x1, y1;
    if (haveLast_) {
        clip(last_, x0, y0, x1, y1);
        if (last_.disposal == 2) {
            for (int y = y0; y < y1; y++)
                std::memset(&pixels_[(size_t(y) * width_ + x0) * 4], 0, size_t(x1 - x0) * 4);
        } else if (last_.disposal == 3 && saved_.size() == pixels_.size()) {
            pixels_ = saved_;
        }
    }
    if (f.disposal == 3) saved_ = pixels_;

    clip(f, x0, y0, x1, y1);
    if (f.rgba.size() >= size_t(f.width) * f.height * 4) {
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                const unsigned char* s = &f.rgba[(size_t(y - f.top) * f.width + (x - f.left)) * 4];
                if (s[3] == 0) continue;
                std::memcpy(&pixels_[(size_t(y) * width_ + x) * 4], s, 4);
            }
    }
    last_ = GifFrame{f.left, f.top, f.width, f.height, f.delayMs, f.disposal, {}};
    haveLast_ = true;
}

int GifDelayMs(int centiseconds) { return centiseconds <= 1 ? 100 : centiseconds * 10; }

int GifFrameAt(const std::vector<int>& delaysMs, double ms) {
    if (delaysMs.size() < 2) return 0;
    double total = 0;
    for (int d : delaysMs) total += d;
    if (total <= 0) return 0;
    double t = std::fmod(std::max(0.0, ms), total);
    for (size_t i = 0; i < delaysMs.size(); i++) {
        if (t < delaysMs[i]) return int(i);
        t -= delaysMs[i];
    }
    return int(delaysMs.size()) - 1;
}

bool IsGifUrl(const std::string& url) {
    std::string path = url.substr(0, url.find_first_of("?#"));
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".gif";
}

}  // namespace who
